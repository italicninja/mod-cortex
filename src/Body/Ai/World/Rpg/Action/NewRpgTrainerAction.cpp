/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgTrainerAction.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BotLevels.h"
#include "BotProfessions.h"
#include "BotTelemetry.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Event.h"
#include "GameEventMgr.h"
#include "GameTime.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Trainer.h"
#include "WorldSession.h"

namespace
{
constexpr time_t CHECK_INTERVAL_SECONDS = 60;
// Nothing to learn (or can't afford it): look again after a level-up, or after this long (money comes in).
constexpr time_t RECHECK_SECONDS = 20 * MINUTE;
constexpr time_t TRIP_TIMEOUT_SECONDS = 30 * MINUTE;
constexpr time_t GIVE_UP_SECONDS = HOUR;
constexpr float MAX_DISTANCE = 3000.0f;
constexpr float APPROACH_DISTANCE = 30.0f;
constexpr float TALK_DISTANCE = 5.0f;
constexpr time_t PROFESSION_CHECK_SECONDS = 10 * MINUTE;
// A known profession's new recipes are picked up in passing; a rank at the skill cap or a new profession is a trip.
constexpr float PROFESSION_PASSING_DISTANCE = 600.0f;
constexpr uint32 SKILL_CAP_MARGIN = 5;
// Hearthstone: a bot whose home is far away makes the inn of the town it's in its home, as a player does at a new
// quest hub (random bots otherwise keep the creation spot, so every unstick hearth sent them back to the start).
constexpr time_t INN_CHECK_SECONDS = 5 * MINUTE;
constexpr time_t INN_BIND_COOLDOWN_SECONDS = 2 * HOUR;
constexpr float INN_RANGE = 250.0f;            // an innkeeper this close: the bot is in that town
constexpr float INN_REBIND_DISTANCE = 1000.0f;  // home at least this far from here (or on another map)
constexpr uint8 INN_MIN_LEVEL = 5;
// A spawn this many bots gave up on as unreachable is skipped by everyone for a while (a vendor in a hut the navmesh
// can't enter), and a limited-stock item a bot found sold out is left until the vendor restocks.
constexpr uint32 UNREACH_STRIKES = 3;
constexpr time_t UNREACH_BLOCK_SECONDS = 6 * HOUR;
constexpr time_t SOLD_OUT_MIN_SECONDS = 15 * MINUTE;
// On the way to a vendor, how often the bot checks whether what it goes for has sold out since it set off.
constexpr time_t STOCK_CHECK_SECONDS = 10;

struct Spot
{
    ObjectGuid::LowType spawn;
    uint32 entry;
    uint32 map;
    float x, y, z;
    uint32 faction;
    uint32 skill;         // profession a tradeskill trainer teaches, 0 for a class trainer
    bool vendor = false;  // a vendor of profession tools and reagents, not a trainer
    bool inn = false;     // an innkeeper, not a trainer
    uint16 event = 0;     // a game event's spawn, only there while it runs (the unnamed "<Class> Trainer"s)
};

// Tools a profession can't work without that vendors sell (a fresh-start bot has none). Engineering's spanner and
// enchanting's rod are made by the profession itself.
struct Tool
{
    uint32 skill;
    uint32 item;
};
constexpr Tool TOOLS[] = {{SKILL_MINING, 2901},      {SKILL_SKINNING, 7005},      {SKILL_BLACKSMITHING, 5956},
                          {SKILL_JEWELCRAFTING, 20815}, {SKILL_INSCRIPTION, 39505}};
// Vendor reagents (vials, thread, flux, rods) for the recipes that need them, bought for up to this many crafts,
// from this share of the money above the reserve.
constexpr uint32 SUPPLY_CRAFTS = 5;
constexpr int64 SUPPLY_SPEND_PCT = 30;

struct TripState
{
    bool going = false;
    int32 spot = -1;
    time_t startedAt = 0;
    time_t nextCheckAt = 0;
    time_t nextTripAt = 0;
    uint8 checkedLevel = 0;  // level at the last "nothing to learn" check
    time_t professionCheckAt = 0;
    std::vector<std::pair<uint32, uint32>> buy;  // at a vendor spot: item, count
    time_t stockCheckAt = 0;                     // next in-transit sold-out check
    time_t innCheckAt = 0;
    time_t boundAt = 0;
};

bool enabled = false;
std::once_flag catalogOnce;
std::vector<Spot> spots;
std::unordered_map<uint32, std::vector<uint32>> vendorSpots;  // tool or reagent -> vendor spots selling it for gold

std::mutex stateMutex;
std::unordered_map<ObjectGuid::LowType, TripState> states;
// Spawns a bot found empty (an event spawn the event list missed): skipped by everyone after.
std::unordered_set<ObjectGuid::LowType> absentSpawns;
struct Unreachable
{
    uint32 strikes = 0;
    time_t until = 0;
};
std::unordered_map<ObjectGuid::LowType, Unreachable> unreachableSpawns;
std::map<std::pair<ObjectGuid::LowType, uint32>, time_t> soldOut;  // vendor spawn, item -> restocked by

time_t Now() { return GameTime::GetGameTime().count(); }

// Whether nobody should go to the spot now: found empty, an event spawn out of its event, or given up on as
// unreachable by several bots lately.
bool Skipped(Spot const& spot)
{
    if (spot.event && !sGameEventMgr->IsActiveEvent(spot.event))
        return true;
    std::lock_guard<std::mutex> lock(stateMutex);
    if (absentSpawns.count(spot.spawn))
        return true;
    auto it = unreachableSpawns.find(spot.spawn);
    return it != unreachableSpawns.end() && Now() < it->second.until;
}

bool SoldOut(Spot const& spot, uint32 item)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    auto it = soldOut.find({spot.spawn, item});
    return it != soldOut.end() && Now() < it->second;
}

// Spawns that only exist while a game event runs (the positive events; a negative one despawns them instead).
std::unordered_map<ObjectGuid::LowType, uint16> EventSpawns()
{
    std::unordered_map<ObjectGuid::LowType, uint16> events;
    auto const& lists = sGameEventMgr->GameEventCreatureGuids;
    int32 count = static_cast<int32>(sGameEventMgr->GetEventMap().size());
    for (int32 i = 0; i < static_cast<int32>(lists.size()); ++i)
    {
        int32 event = i - count + 1;
        if (event <= 0)
            continue;
        for (ObjectGuid::LowType spawn : lists[i])
            events[spawn] = static_cast<uint16>(event);
    }
    return events;
}

void LoadCatalog()
{
    enabled = sConfigMgr->GetOption<bool>("Playerbots.NewRpgTrainer", false);
    if (!enabled)
        return;

    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(data.id);
        CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(data.id);
        if (!trainer || !cInfo)
            continue;
        uint32 skill = 0;
        if (trainer->GetTrainerType() == Trainer::Type::Tradeskill && BotProfessions::Enabled())
            skill = BotProfessions::TrainerSkill(trainer);
        if (trainer->GetTrainerType() != Trainer::Type::Class && !skill)
            continue;

        spots.push_back({spawnId, data.id, data.mapid, data.posX, data.posY, data.posZ, cInfo->faction, skill});
    }

    size_t trainers = spots.size();
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(data.id);
        if (cInfo && (cInfo->npcflag & UNIT_NPC_FLAG_INNKEEPER))
            spots.push_back({spawnId, data.id, data.mapid, data.posX, data.posY, data.posZ, cInfo->faction, 0, false,
                             true});
    }
    size_t inns = spots.size() - trainers;
    trainers = spots.size();
    if (BotProfessions::Enabled())
    {
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(data.id);
            CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(data.id);
            if (!items || !cInfo)
                continue;
            int32 index = -1;
            for (VendorItem const* item : items->m_items)
            {
                // Limited stock too: enchanting supply vendors keep a few Strange Dust and Lesser Magic Essence.
                if (!item || item->ExtendedCost)
                    continue;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->item);
                bool tool = std::any_of(std::begin(TOOLS), std::end(TOOLS),
                                        [&](Tool const& t) { return t.item == item->item; });
                if (!proto || (!tool && proto->Class != ITEM_CLASS_TRADE_GOODS && proto->Class != ITEM_CLASS_REAGENT))
                    continue;
                if (index < 0)
                {
                    index = static_cast<int32>(spots.size());
                    spots.push_back({spawnId, data.id, data.mapid, data.posX, data.posY, data.posZ, cInfo->faction, 0,
                                     true});
                }
                vendorSpots[item->item].push_back(static_cast<uint32>(index));
            }
        }
    }

    std::unordered_map<ObjectGuid::LowType, uint16> events = EventSpawns();
    for (Spot& spot : spots)
    {
        auto it = events.find(spot.spawn);
        if (it != events.end())
            spot.event = it->second;
    }

    LOG_INFO("playerbots",
             ">> Playerbots trainer trips: {} class and profession trainer spawns, {} innkeepers, {} tool and reagent "
             "vendors",
             trainers - inns, inns, spots.size() - trainers);
}

TripState& State(Player* bot)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return states[bot->GetGUID().GetCounter()];
}

Creature* SpawnedCreature(Player* bot, ObjectGuid::LowType spawnId)
{
    auto bounds = bot->GetMap()->GetCreatureBySpawnIdStore().equal_range(spawnId);
    for (auto it = bounds.first; it != bounds.second; ++it)
    {
        if (it->second && it->second->IsInWorld() && it->second->IsAlive())
            return it->second;
    }
    return nullptr;
}

// Whether the vendor at the spot is out of the item now: marked sold out by a bot that took the last of it or found
// none, or, when the vendor's grid is loaded, out by its own limited-stock count (then marked for everyone).
bool OutOfStock(Player* bot, Spot const& spot, uint32 item)
{
    if (SoldOut(spot, item))
        return true;
    Creature* vendor = SpawnedCreature(bot, spot.spawn);
    VendorItemData const* items = vendor ? vendor->GetVendorItems() : nullptr;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    if (!items || !proto)
        return false;
    for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* entry = items->GetItem(slot);
        if (!entry || entry->item != item)
            continue;
        if (!entry->maxcount || vendor->GetVendorItemCurrentCount(entry) >= std::max<uint32>(1, proto->BuyCount))
            return false;
        std::lock_guard<std::mutex> lock(stateMutex);
        soldOut[{spot.spawn, item}] = Now() + std::max<time_t>(entry->incrtime, SOLD_OUT_MIN_SECONDS);
        return true;
    }
    return false;
}

// Spells the bot could learn from this trainer now, and what they cost.
uint32 Learnable(Player* bot, Trainer::Trainer const* trainer, uint32& cost)
{
    cost = 0;
    uint32 count = 0;
    for (Trainer::Spell const& spell : trainer->GetSpells())
    {
        if (!PlayerbotFactory::IsTrainerSpellAllowedForBot(bot, trainer, &spell) || !trainer->CanTeachSpell(bot, &spell))
            continue;

        ++count;
        cost += spell.MoneyCost;
    }
    return count;
}

// Nearest friendly class trainer (skill 0) or trainer of that profession for the bot on its map, or -1.
int32 NearestSpot(Player* bot, uint32 skill = 0)
{
    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    if (!botFaction)
        return -1;

    int32 best = -1;
    float bestDist = MAX_DISTANCE;
    for (size_t i = 0; i < spots.size(); ++i)
    {
        Spot const& spot = spots[i];
        if (spot.map != bot->GetMapId() || spot.skill != skill || spot.vendor || spot.inn)
            continue;

        float dist = bot->GetExactDist(spot.x, spot.y, spot.z);
        // Up a lift (Thunder Bluff's mesas, Undercity) from where the bot is: new rpg can't get there (bots 0064)
        if (dist >= bestDist || Skipped(spot) || BotLevels::LiftOnly(bot, spot.map, spot.x, spot.y, spot.z))
            continue;

        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(spot.faction);
        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(spot.entry);
        if (!faction || !faction->IsFriendlyTo(*botFaction) || !trainer || !trainer->IsTrainerValidForPlayer(bot))
            continue;

        best = static_cast<int32>(i);
        bestDist = dist;
    }
    return best;
}

float HomeDistance(Player* bot, float x, float y)
{
    if (bot->m_homebindMapId != bot->GetMapId())
        return FLT_MAX;
    return std::hypot(bot->m_homebindX - x, bot->m_homebindY - y);
}

// The innkeeper of the town the bot is in, when its home is far from here, it isn't a capital (passing through) and
// it hasn't just moved its home. Or -1.
int32 InnSpot(Player* bot, time_t now, TripState const& state)
{
    if (bot->GetLevel() < INN_MIN_LEVEL || (state.boundAt && now < state.boundAt + INN_BIND_COOLDOWN_SECONDS) ||
        HomeDistance(bot, bot->GetPositionX(), bot->GetPositionY()) < INN_REBIND_DISTANCE)
        return -1;
    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId());
    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    if (!zone || (zone->flags & AREA_FLAG_CAPITAL) || !botFaction)
        return -1;

    int32 best = -1;
    float bestDist = INN_RANGE;
    for (size_t i = 0; i < spots.size(); ++i)
    {
        Spot const& spot = spots[i];
        if (!spot.inn || spot.map != bot->GetMapId())
            continue;
        float dist = bot->GetExactDist(spot.x, spot.y, spot.z);
        if (dist >= bestDist || HomeDistance(bot, spot.x, spot.y) < INN_REBIND_DISTANCE)
            continue;
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(spot.faction);
        if (!faction || !faction->IsFriendlyTo(*botFaction) || Skipped(spot))
            continue;
        best = static_cast<int32>(i);
        bestDist = dist;
    }
    return best;
}

bool HasTool(Player* bot, uint32 item)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    return bot->HasItemCount(item, 1) ||
           (proto && proto->TotemCategory && bot->HasItemTotemCategory(proto->TotemCategory));
}

// The nearest friendly vendor spot selling the item for gold, and not known to be sold out of it, or -1.
int32 NearestVendor(Player* bot, uint32 item)
{
    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    auto it = vendorSpots.find(item);
    if (!botFaction || it == vendorSpots.end())
        return -1;
    int32 best = -1;
    float bestDist = MAX_DISTANCE;
    for (uint32 i : it->second)
    {
        Spot const& spot = spots[i];
        if (spot.map != bot->GetMapId())
            continue;
        float dist = bot->GetExactDist(spot.x, spot.y, spot.z);
        if (dist >= bestDist || Skipped(spot) || SoldOut(spot, item) ||
            BotLevels::LiftOnly(bot, spot.map, spot.x, spot.y, spot.z))
            continue;
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(spot.faction);
        if (!faction || !faction->IsFriendlyTo(*botFaction))
            continue;
        best = static_cast<int32>(i);
        bestDist = dist;
    }
    return best;
}

// Copper for `count` of the item at a vendor, which sells it in stacks of BuyCount.
int64 VendorCost(uint32 item, uint32 count)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    if (!proto)
        return std::numeric_limits<int64>::max();
    uint32 stack = std::max<uint32>(1, proto->BuyCount);
    return static_cast<int64>((count + stack - 1) / stack) * proto->BuyPrice;
}

// A vendor for a tool one of the bot's professions lacks and it can afford, or -1.
int32 ToolSpot(Player* bot, std::vector<std::pair<uint32, uint32>>& buy)
{
    int64 spare = BotProfessions::Spare(bot, BotProfessions::ToolReservePct());
    int32 best = -1;
    float bestDist = MAX_DISTANCE;
    for (Tool const& tool : TOOLS)
    {
        if (!bot->HasSkill(tool.skill) || HasTool(bot, tool.item) || VendorCost(tool.item, 1) > spare)
            continue;
        int32 index = NearestVendor(bot, tool.item);
        if (index < 0)
            continue;
        float dist = bot->GetExactDist(spots[index].x, spots[index].y, spots[index].z);
        if (dist >= bestDist)
            continue;
        best = index;
        bestDist = dist;
        buy = {{tool.item, 1}};
    }
    return best;
}

// Whether the recipe makes a profession tool the bot doesn't have yet (an enchanter's Runed Copper Rod).
bool MakesMissingTool(Player* bot, SpellInfo const* spell)
{
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (spell->Effects[i].Effect != SPELL_EFFECT_CREATE_ITEM)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(spell->Effects[i].ItemType);
        if (proto && proto->TotemCategory && !bot->HasItemTotemCategory(proto->TotemCategory))
            return true;
    }
    return false;
}

// A vendor for the reagents the bot's best skill-up recipe lacks, when everything it lacks is sold by vendors (an
// alchemist with herbs but no vials): enough for what its other reagents allow, up to SUPPLY_CRAFTS, from a share of
// its spare money. A recipe for a missing tool comes first and is made once, from all of it (down to
// ToolReservePct of the reserve), like a bought tool. Herbs for a scribe to mill come in lots of MILL_COUNT. Or -1.
int32 SupplySpot(Player* bot, std::vector<std::pair<uint32, uint32>>& buy, bool& mill)
{
    mill = false;
    int64 spare = BotProfessions::Spare(bot);
    std::vector<BotProfessions::Shopping> list = BotProfessions::ShoppingList(bot, 1);
    std::stable_partition(list.begin(), list.end(),
                          [bot](BotProfessions::Shopping const& entry)
                          {
                              SpellInfo const* spell = sSpellMgr->GetSpellInfo(entry.spellId);
                              return spell && MakesMissingTool(bot, spell);
                          });
    for (BotProfessions::Shopping const& entry : list)
    {
        if (!std::all_of(entry.missing.begin(), entry.missing.end(),
                         [](auto const& missing) { return vendorSpots.count(missing.first); }))
            continue;
        if (entry.spellId == BotProfessions::SPELL_MILLING)
        {
            // Herbs to mill (one herb per entry): as many lots as the share of spare money covers.
            auto [herb, count] = entry.missing.front();
            int64 budget = spare * SUPPLY_SPEND_PCT / 100;
            while (count > BotProfessions::MILL_COUNT && VendorCost(herb, count) > budget)
                count -= BotProfessions::MILL_COUNT;
            if (VendorCost(herb, count) > budget)
                continue;
            int32 index = NearestVendor(bot, herb);
            if (index < 0)
                continue;
            buy = {{herb, count}};
            mill = true;
            return index;
        }
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(entry.spellId);
        if (!spell)
            continue;
        bool tool = MakesMissingTool(bot, spell);
        int64 budget = tool ? BotProfessions::Spare(bot, BotProfessions::ToolReservePct())
                            : spare * SUPPLY_SPEND_PCT / 100;
        uint32 crafts = tool ? 1 : SUPPLY_CRAFTS;
        for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            if (spell->Reagent[i] > 0 && spell->ReagentCount[i] && !vendorSpots.count(spell->Reagent[i]))
                crafts = std::min<uint32>(crafts, bot->GetItemCount(spell->Reagent[i]) / spell->ReagentCount[i]);
        if (!crafts)
            continue;

        // As many crafts as the budget covers.
        std::vector<std::pair<uint32, uint32>> list;
        for (; crafts > 0; --crafts)
        {
            list.clear();
            int64 cost = 0;
            for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            {
                if (spell->Reagent[i] <= 0 || !spell->ReagentCount[i] || !vendorSpots.count(spell->Reagent[i]))
                    continue;
                uint32 item = static_cast<uint32>(spell->Reagent[i]);
                uint32 need = spell->ReagentCount[i] * crafts;
                uint32 have = bot->GetItemCount(item);
                if (have >= need)
                    continue;
                list.push_back({item, need - have});
                cost += VendorCost(item, need - have);
            }
            if (cost <= budget)
                break;
        }
        if (!crafts || list.empty())
            continue;
        int32 index = NearestVendor(bot, list.front().first);
        if (index < 0)
            continue;
        buy = std::move(list);
        return index;
    }
    return -1;
}

// Buys what the vendor has of the list; the number of items bought.
uint32 BuyFromVendor(Player* bot, Creature* vendor, std::vector<std::pair<uint32, uint32>> const& buy)
{
    uint32 bought = 0;
    VendorItemData const* items = vendor->GetVendorItems();
    for (auto const& [item, count] : buy)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
        for (uint32 slot = 0; proto && items && slot < items->GetItemCount(); ++slot)
        {
            VendorItem const* entry = items->GetItem(slot);
            if (!entry || entry->item != item)
                continue;
            // The count is in stacks of BuyCount, and the return value is maxcount != 0, not whether it worked.
            uint32 stack = std::max<uint32>(1, proto->BuyCount);
            uint32 stacks = std::min<uint32>((count + stack - 1) / stack, 255);
            if (entry->maxcount)  // limited stock: what's left of it
            {
                uint32 left = vendor->GetVendorItemCurrentCount(entry);
                if (left / stack <= stacks)  // this takes the last of it, or there's none: others go elsewhere
                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    soldOut[{vendor->GetSpawnId(), item}] =
                        Now() + std::max<time_t>(entry->incrtime, SOLD_OUT_MIN_SECONDS);
                }
                stacks = std::min<uint32>(stacks, left / stack);
            }
            if (!stacks)
                break;
            uint32 had = bot->GetItemCount(item);
            bot->BuyItemFromVendorSlot(vendor->GetGUID(), slot, item, static_cast<uint8>(stacks), NULL_BAG, NULL_SLOT);
            uint32 now = bot->GetItemCount(item);
            bought += now > had ? now - had : 0;
            break;
        }
    }
    return bought;
}

// A profession trainer worth walking to: one for a profession the bot wants to learn, for the next rank of one at
// its skill cap, or, close by, for new recipes. The nearest such, or -1.
int32 ProfessionSpot(Player* bot, uint32& count, uint32& cost)
{
    std::vector<uint32> skills = BotProfessions::Wanted(bot);
    for (uint32 skill : {SKILL_MINING, SKILL_BLACKSMITHING, SKILL_ENGINEERING, SKILL_JEWELCRAFTING, SKILL_SKINNING,
                         SKILL_LEATHERWORKING, SKILL_HERBALISM, SKILL_ALCHEMY, SKILL_TAILORING, SKILL_ENCHANTING,
                         SKILL_INSCRIPTION})
        if (bot->HasSkill(skill))
            skills.push_back(skill);

    int32 best = -1;
    float bestDist = MAX_DISTANCE;
    for (uint32 skill : skills)
    {
        int32 index = NearestSpot(bot, skill);
        if (index < 0)
            continue;
        Spot const& spot = spots[index];
        float dist = bot->GetExactDist(spot.x, spot.y, spot.z);
        uint32 spotCost = 0;
        uint32 ranks = 0;
        uint32 spotCount = BotProfessions::Worthwhile(bot, sObjectMgr->GetTrainer(spot.entry), skill, spotCost, ranks);
        bool capped =
            bot->HasSkill(skill) && bot->GetSkillValue(skill) + SKILL_CAP_MARGIN >= bot->GetMaxSkillValue(skill);
        bool trip = bot->HasSkill(skill) ? (ranks && capped) || dist <= PROFESSION_PASSING_DISTANCE : ranks > 0;
        if (!spotCount || !trip || dist >= bestDist)
            continue;
        best = index;
        bestDist = dist;
        count = spotCount;
        cost = spotCost;
    }
    return best;
}
}  // namespace

bool NewRpgTrainerAction::isUseful()
{
    std::call_once(catalogOnce, LoadCatalog);
    if (!enabled || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->GetGroup() ||
        bot->GetMap()->Instanceable() || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    TripState& state = State(bot);
    if (state.going)
        return true;

    time_t now = Now();
    if (now < state.nextCheckAt || now < state.nextTripAt)
        return false;

    state.nextCheckAt = now + CHECK_INTERVAL_SECONDS;

    if (now >= state.innCheckAt)
    {
        state.innCheckAt = now + INN_CHECK_SECONDS;
        int32 inn = InnSpot(bot, now, state);
        if (inn >= 0)
        {
            Spot const& spot = spots[inn];
            state.going = true;
            state.spot = inn;
            state.startedAt = now;
            BotTelemetry::instance().Event(botAI, "trainer", "inn_trip",
                                           static_cast<int32>(bot->GetExactDist(spot.x, spot.y, spot.z)),
                                           static_cast<int32>(std::min(HomeDistance(bot, spot.x, spot.y), 99999.0f)),
                                           static_cast<int32>(spot.entry));
            return true;
        }
    }

    if (BotProfessions::Enabled() && now >= state.professionCheckAt)
    {
        state.professionCheckAt = now + PROFESSION_CHECK_SECONDS;
        std::vector<std::pair<uint32, uint32>> buy;
        bool tool = true;
        bool mill = false;
        int32 vendorIndex = ToolSpot(bot, buy);
        if (vendorIndex < 0)
        {
            tool = false;
            vendorIndex = SupplySpot(bot, buy, mill);
        }
        if (vendorIndex >= 0)
        {
            Spot const& spot = spots[vendorIndex];
            state.going = true;
            state.spot = vendorIndex;
            state.startedAt = now;
            state.buy = std::move(buy);
            BotTelemetry::instance().Event(botAI, "trainer", tool ? "tool_trip" : (mill ? "mill_trip" : "supply_trip"),
                                           static_cast<int32>(bot->GetExactDist(spot.x, spot.y, spot.z)),
                                           static_cast<int32>(state.buy.front().first),
                                           static_cast<int32>(state.buy.front().second));
            return true;
        }

        uint32 count = 0;
        uint32 cost = 0;
        int32 index = ProfessionSpot(bot, count, cost);
        if (index >= 0)
        {
            Spot const& spot = spots[index];
            state.going = true;
            state.spot = index;
            state.startedAt = now;
            BotTelemetry::instance().Event(botAI, "trainer", "profession_trip",
                                           static_cast<int32>(bot->GetExactDist(spot.x, spot.y, spot.z)),
                                           static_cast<int32>(spot.skill), static_cast<int32>(cost));
            return true;
        }
    }

    if (state.checkedLevel == bot->GetLevel())
        return false;

    int32 spotIndex = NearestSpot(bot);
    if (spotIndex < 0)
        return false;

    Spot const& spot = spots[spotIndex];
    uint32 cost = 0;
    uint32 count = Learnable(bot, sObjectMgr->GetTrainer(spot.entry), cost);
    // Something to learn, and at least the cheapest of it is affordable (TrainerAction learns what the budget allows)
    if (!count || bot->GetMoney() < cost / count)
    {
        state.checkedLevel = bot->GetLevel();
        state.nextTripAt = now + RECHECK_SECONDS;
        return false;
    }

    state.going = true;
    state.spot = spotIndex;
    state.startedAt = now;
    BotTelemetry::instance().Event(botAI, "trainer", "trip", static_cast<int32>(bot->GetExactDist(spot.x, spot.y, spot.z)),
                                   static_cast<int32>(count), static_cast<int32>(cost));
    return true;
}

bool NewRpgTrainerAction::Execute(Event /*event*/)
{
    TripState& state = State(bot);
    time_t now = Now();
    if (!state.going || state.spot < 0 || state.spot >= static_cast<int32>(spots.size()))
    {
        state.going = false;
        return false;
    }

    Spot const& spot = spots[state.spot];
    WorldPosition dest(spot.map, spot.x, spot.y, spot.z);
    uint32 unreachableMs = sPlayerbotAIConfig.NewRpgUnreachableMinutes * MINUTE * IN_MILLISECONDS;
    bool unreachable = sPlayerbotAIConfig.NewRpgWalkUnstick && botAI->rpgInfo.IsUnreachable(dest, unreachableMs);
    if (bot->GetMapId() != spot.map || now > state.startedAt + TRIP_TIMEOUT_SECONDS || unreachable)
    {
        state.going = false;
        state.nextTripAt = now + GIVE_UP_SECONDS;
        bool blocked = false;
        if (unreachable)
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            Unreachable& entry = unreachableSpawns[spot.spawn];
            if (++entry.strikes >= UNREACH_STRIKES)
            {
                entry.strikes = 0;
                entry.until = now + UNREACH_BLOCK_SECONDS;
                blocked = true;
            }
        }
        BotTelemetry::instance().Event(botAI, "trainer", unreachable ? "give_up_unreach" : "give_up_timeout",
                                       static_cast<int32>(now - state.startedAt), static_cast<int32>(spot.entry),
                                       blocked ? 1 : 0);
        return false;
    }

    // On the way to a vendor for limited stock: another bot may have taken the last of it since this one set off.
    // Go to the next vendor that has it, or drop the trip.
    if (spot.vendor && !state.buy.empty() && now >= state.stockCheckAt)
    {
        state.stockCheckAt = now + STOCK_CHECK_SECONDS;
        uint32 item = state.buy.front().first;
        if (OutOfStock(bot, spot, item))
        {
            int32 next = NearestVendor(bot, item);
            float nextDist = next >= 0 ? bot->GetExactDist(spots[next].x, spots[next].y, spots[next].z) : 0.0f;
            BotTelemetry::instance().Event(botAI, "trainer", next >= 0 ? "sold_out_retarget" : "sold_out_abort",
                                           static_cast<int32>(item), static_cast<int32>(now - state.startedAt),
                                           static_cast<int32>(nextDist));
            if (next >= 0)
            {
                state.spot = next;
                state.startedAt = now;
                return true;
            }
            state.going = false;
            state.buy.clear();
            state.nextCheckAt = 0;  // free for other trips; NearestVendor won't pick a sold-out vendor again
            return false;
        }
    }

    Creature* trainer = SpawnedCreature(bot, spot.spawn);
    if (trainer && bot->GetDistance(trainer) <= TALK_DISTANCE)
    {
        if (bot->isMoving())
        {
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
        }
        bot->SetFacingToObject(trainer);
        bot->SetSelection(trainer->GetGUID());

        uint32 moneyBefore = bot->GetMoney();
        if (spot.inn)
        {
            float before = HomeDistance(bot, spot.x, spot.y);
            bot->GetSession()->SendBindPoint(trainer);  // the innkeeper's Bind spell, as from its gossip
            bool bound = HomeDistance(bot, spot.x, spot.y) < before;
            state.going = false;
            state.boundAt = now;  // tried: not again for a while either way
            BotTelemetry::instance().Event(botAI, "trainer", bound ? "inn_bound" : "inn_cant",
                                           static_cast<int32>(std::min(before, 99999.0f)),
                                           static_cast<int32>(bot->GetZoneId()), static_cast<int32>(spot.entry));
            return true;
        }
        if (spot.vendor)
        {
            uint32 bought = BuyFromVendor(bot, trainer, state.buy);
            state.going = false;
            if (!bought)  // bags full, or the money went elsewhere on the way
                state.nextTripAt = now + RECHECK_SECONDS;
            BotTelemetry::instance().Event(botAI, "trainer", bought ? "bought" : "cant_buy",
                                           static_cast<int32>(state.buy.empty() ? 0 : state.buy.front().first),
                                           static_cast<int32>(moneyBefore - bot->GetMoney()),
                                           static_cast<int32>(bought));
            state.buy.clear();
            return true;
        }
        if (spot.skill)
        {
            Trainer::Trainer* trainerData = sObjectMgr->GetTrainer(spot.entry);
            uint32 learned = trainerData ? BotProfessions::Learn(bot, trainer, trainerData, spot.skill) : 0;
            state.going = false;
            BotTelemetry::instance().Event(botAI, "trainer", "profession_done", static_cast<int32>(learned),
                                           static_cast<int32>(moneyBefore - bot->GetMoney()),
                                           static_cast<int32>(spot.skill));
            return true;
        }
        uint32 cost = 0;
        Trainer::Trainer const* trainerData = sObjectMgr->GetTrainer(spot.entry);
        uint32 before = trainerData ? Learnable(bot, trainerData, cost) : 0;
        botAI->DoSpecificAction("trainer", Event("new rpg trainer", "learn"), true);
        uint32 after = trainerData ? Learnable(bot, trainerData, cost) : 0;
        uint32 learned = before > after ? before - after : 0;

        state.going = false;
        state.checkedLevel = bot->GetLevel();
        state.nextTripAt = now + RECHECK_SECONDS;
        BotTelemetry::instance().Event(botAI, "trainer", "done", static_cast<int32>(learned),
                                       static_cast<int32>(moneyBefore - bot->GetMoney()),
                                       static_cast<int32>(now - state.startedAt));
        return true;
    }

    if (trainer && bot->GetDistance(trainer) <= APPROACH_DISTANCE)
        return MoveWorldObjectTo(trainer->GetGUID(), TALK_DISTANCE / 2);

    if (!trainer && bot->GetExactDist(spot.x, spot.y, spot.z) <= TALK_DISTANCE)
    {
        // At the spawn point but the trainer isn't there (an event spawn, or dead): nobody tries it again
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            absentSpawns.insert(spot.spawn);
        }
        state.going = false;
        state.nextCheckAt = 0;  // look for the next trainer right away
        BotTelemetry::instance().Event(botAI, "trainer", "give_up_absent", static_cast<int32>(now - state.startedAt),
                                       static_cast<int32>(spot.entry));
        return false;
    }

    MoveFarTo(dest);
    return true;
}
