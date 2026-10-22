/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include "CortexNeeds.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "BotAuction.h"
#include "BotProfessions.h"
#include "BotTelemetry.h"
#include "BudgetValues.h"
#include "Config.h"
#include "Cortex.h"
#include "CortexQuests.h"
#include "CortexTravel.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "HunterPetFood.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "Timer.h"
#include "Trainer.h"
#include "WorldSession.h"

namespace
{
    bool sEnabled = true;
    float sVendorRange = 1500.0f;
    float sTrainerRange = 3000.0f;
    bool sFood = true;
    bool sMail = true;
    bool sTrainerTrips = false;  // Cortex.Bots.NewRpgTrainer: trainer, profession and inn trips at all
    uint32 sMinAmmo = 200;       // Cortex.Bots.NewRpgUpkeepAmmo

    using Need = CortexNeedsAction::Need;

    constexpr uint32 VENDOR_CHECK_MS = 10 * IN_MILLISECONDS;
    constexpr uint32 FOOD_CHECK_MS = 2 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 TRAINER_CHECK_MS = 30 * IN_MILLISECONDS;
    constexpr uint32 TRAINER_RECHECK_MS = 20 * MINUTE * IN_MILLISECONDS;  // nothing (affordable) to learn: money comes in
    constexpr uint32 PROFESSION_CHECK_MS = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 INN_CHECK_MS = 5 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 MAIL_CHECK_MS = 2 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 AUCTION_CHECK_MS = MINUTE * IN_MILLISECONDS;
    constexpr uint32 RETRY_MS = 30 * IN_MILLISECONDS;  // after a failed errand: the next remembered place, or fallback
    // Memory had nothing: playerbots' action has the need for this long (its own trip runs to its end).
    constexpr uint32 VENDOR_FALLBACK_MS = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 TRAINER_FALLBACK_MS = 30 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 AUCTION_FALLBACK_MS = 30 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 FOOD_DONE_MS = 30 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 HANDOVER_MS = MINUTE * IN_MILLISECONDS;  // upkeep does the business at the vendor
    constexpr uint32 HANDOVER_REPEAT_MS = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 HANDOVER_HOLD_MS = 30 * IN_MILLISECONDS;  // upkeep walking to a vendor in sight it prefers

    constexpr uint32 ERRAND_MS = 20 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 STUCK_MS = 3 * MINUTE * IN_MILLISECONDS;  // no 10 yd closer in this long: unreachable
    constexpr float PROGRESS = 10.0f;
    constexpr float TARGET_SEARCH = 60.0f;          // the NPC or mailbox is looked for from this close
    constexpr float TARGET_NEAR_SPOT = 40.0f;       // and counts as the remembered one this close to the spot
    constexpr float APPROACH = 30.0f;
    constexpr float ARRIVE = 10.0f;                 // at the spot with nobody there: gone
    constexpr uint32 ABSENT_SKIP_MS = 6 * HOUR * IN_MILLISECONDS;
    constexpr uint32 UNREACH_SKIP_MS = 2 * HOUR * IN_MILLISECONDS;
    constexpr uint32 TIMEOUT_SKIP_MS = HOUR * IN_MILLISECONDS;
    // Not at its spot: a walker (a vendor on its round of the town or the road) is looked for this far around, and
    // followed this many times before it counts as gone.
    constexpr float WALKER_SEARCH = 200.0f;
    constexpr uint8 MAX_RELOCATIONS = 3;
    // Like the trainer trips' marks (bots 0059): a place several Cortex bots gave up on as unreachable is skipped by
    // all of them for a while.
    constexpr uint8 UNREACH_STRIKES = 3;
    constexpr uint32 UNREACH_BLOCK_SECONDS = 6 * HOUR;
    constexpr uint32 MAX_REACH_CHECKS = 2;  // trip plans per pick for places in another region
    constexpr float OTHER_REGION_FACTOR = 1.5f;
    constexpr float OTHER_REGION_EXTRA = 300.0f;

    constexpr uint32 WANT_SELL = 0x1;
    constexpr uint32 WANT_REPAIR = 0x2;
    constexpr uint32 WANT_AMMO = 0x4;
    constexpr uint32 WANT_PET_FOOD = 0x8;
    constexpr uint32 WANT_WATER = 0x10;
    constexpr uint32 PET_FOOD_LOW = 3;  // as upkeep: a trip for pet food below this many pieces
    // As upkeep (bots 0058): a mana user makes a trip for water below WATER_LOW drinks that suit its level, a drink
    // suiting it up to WATER_LEVEL_SPAN levels above its required level.
    constexpr uint32 WATER_LOW = 5;
    constexpr uint32 WATER_LEVEL_SPAN = 15;
    constexpr uint32 DRINK_CATEGORY = 59;  // spell category of drinks (11 is food)

    constexpr uint32 FOOD_MIN = 5;
    constexpr float FOOD_RANGE = 500.0f;
    constexpr uint8 FOOD_MIN_LEVEL = 5;
    constexpr float MAIL_RANGE = 250.0f;
    constexpr float AUCTION_SPOT_MATCH = 10.0f;  // a catalog auctioneer this close to the remembered one is it

    // As the trainer trips (bots 0016, 0048, 0057).
    constexpr float PROFESSION_PASSING_DISTANCE = 600.0f;
    constexpr uint32 SKILL_CAP_MARGIN = 5;
    constexpr float INN_RANGE = 250.0f;
    constexpr float INN_REBIND_DISTANCE = 1000.0f;
    constexpr uint8 INN_MIN_LEVEL = 5;
    constexpr uint32 INN_BIND_COOLDOWN_SECONDS = 2 * HOUR;

    struct Strike
    {
        uint8 count = 0;
        uint32 until = 0;  // unix seconds
    };
    std::mutex sStrikeMutex;
    std::unordered_map<uint64, Strike> sStrikes;  // memory key (entry, cell) -> strikes

    std::once_flag sClassTrainersOnce;
    std::vector<uint32> sClassTrainers;

    uint32 UnixNow() { return static_cast<uint32>(GameTime::GetGameTime().count()); }
    bool Due(uint32 now, uint32 at) { return static_cast<int32>(now - at) >= 0; }
    float Dist2d(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }
    uint8 Index(Need need) { return static_cast<uint8>(need); }

    char const* NeedName(Need need)
    {
        switch (need)
        {
            case Need::VENDOR: return "vendor";
            case Need::FOOD: return "food";
            case Need::TRAINER: return "trainer";
            case Need::PROFESSION: return "profession";
            case Need::INN: return "inn";
            case Need::MAIL: return "mail";
            case Need::AUCTION: return "ah";
            default: return "none";
        }
    }

    bool Blocked(uint64 key)
    {
        std::lock_guard<std::mutex> lock(sStrikeMutex);
        auto it = sStrikes.find(key);
        return it != sStrikes.end() && UnixNow() < it->second.until;
    }

    // True when this strike blocks the place for everyone.
    bool AddStrike(uint64 key)
    {
        std::lock_guard<std::mutex> lock(sStrikeMutex);
        Strike& strike = sStrikes[key];
        if (++strike.count < UNREACH_STRIKES)
            return false;
        strike.count = 0;
        strike.until = UnixNow() + UNREACH_BLOCK_SECONDS;
        return true;
    }

    // Spells the bot could learn from this trainer now, and what they cost (as the trainer trips count them).
    uint32 Learnable(Player* bot, Trainer::Trainer const* trainer, uint32& cost)
    {
        cost = 0;
        uint32 count = 0;
        for (Trainer::Spell const& spell : trainer->GetSpells())
        {
            if (!PlayerbotFactory::IsTrainerSpellAllowedForBot(bot, trainer, &spell) ||
                !trainer->CanTeachSpell(bot, &spell))
                continue;
            ++count;
            cost += spell.MoneyCost;
        }
        return count;
    }

    // A class trainer of the bot's class anywhere: only to tell whether there is something new to learn when memory
    // has no trainer (a player knows a level brings new ranks; where to learn them is what it doesn't know).
    Trainer::Trainer* AnyClassTrainer(Player* bot)
    {
        std::call_once(sClassTrainersOnce,
                       []()
                       {
                           for (auto const& [entry, info] : *sObjectMgr->GetCreatureTemplates())
                           {
                               if (!(info.npcflag & UNIT_NPC_FLAG_TRAINER_CLASS))
                                   continue;
                               Trainer::Trainer* trainer = sObjectMgr->GetTrainer(entry);
                               if (trainer && trainer->GetTrainerType() == Trainer::Type::Class)
                                   sClassTrainers.push_back(entry);
                           }
                       });
        for (uint32 entry : sClassTrainers)
        {
            Trainer::Trainer* trainer = sObjectMgr->GetTrainer(entry);
            if (trainer && trainer->IsTrainerValidForPlayer(bot))
                return trainer;
        }
        return nullptr;
    }

    bool SellsAmmo(uint32 vendorEntry, uint32 ammoSubclass, uint8 level)
    {
        VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(vendorEntry);
        if (!items)
            return false;
        for (VendorItem const* item : items->m_items)
        {
            ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
            if (proto && proto->Class == ITEM_CLASS_PROJECTILE && proto->SubClass == ammoSubclass &&
                proto->RequiredLevel <= level)
                return true;
        }
        return false;
    }

    bool SellsPetFood(uint32 vendorEntry, Pet* pet, uint8 level)
    {
        VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(vendorEntry);
        if (!items)
            return false;
        for (VendorItem const* item : items->m_items)
        {
            ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
            if (proto && !item->ExtendedCost && proto->RequiredLevel <= level && HunterPetFood::Benefit(pet, proto))
                return true;
        }
        return false;
    }

    bool IsDrink(ItemTemplate const* proto)
    {
        return proto && proto->Class == ITEM_CLASS_CONSUMABLE &&
               (proto->SubClass == ITEM_SUBCLASS_CONSUMABLE || proto->SubClass == ITEM_SUBCLASS_FOOD) &&
               proto->Spells[0].SpellCategory == DRINK_CATEGORY;
    }

    bool DrinkSuits(Player* bot, ItemTemplate const* proto)
    {
        uint32 level = bot->GetLevel();
        return IsDrink(proto) && proto->RequiredLevel <= level && proto->RequiredLevel + WATER_LEVEL_SPAN >= level;
    }

    uint32 DrinkCount(Player* bot)
    {
        uint32 count = 0;
        HunterPetFood::ForEachBagItem(bot,
                                      [&](Item* item)
                                      {
                                          if (DrinkSuits(bot, item->GetTemplate()))
                                              count += item->GetCount();
                                      });
        return count;
    }

    float HomeDistance(Player* bot, float x, float y)
    {
        if (bot->m_homebindMapId != bot->GetMapId())
            return FLT_MAX;
        return std::hypot(bot->m_homebindX - x, bot->m_homebindY - y);
    }

    // The level a profession's next rank needs, from its current cap (Apprentice 75 -> Journeyman at 10 ...); 0 when
    // there is no next rank.
    uint8 NextRankLevel(uint32 maxSkill)
    {
        switch (maxSkill)
        {
            case 75: return 10;
            case 150: return 20;
            case 225: return 35;
            case 300: return 50;
            case 375: return 65;
            default: return 0;
        }
    }
}

void CortexNeeds::LoadConfig()
{
    sEnabled = sConfigMgr->GetOption<bool>("Cortex.Needs", true);
    sVendorRange = std::clamp(sConfigMgr->GetOption<float>("Cortex.Needs.VendorRange", 1500.0f), 100.0f, 5000.0f);
    sTrainerRange = std::clamp(sConfigMgr->GetOption<float>("Cortex.Needs.TrainerRange", 3000.0f), 100.0f, 10000.0f);
    sFood = sConfigMgr->GetOption<bool>("Cortex.Needs.Food", true);
    sMail = sConfigMgr->GetOption<bool>("Cortex.Needs.Mail", true);
    sTrainerTrips = sConfigMgr->GetOption<bool>("Cortex.Bots.NewRpgTrainer", false);
    sMinAmmo = sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgUpkeepAmmo", 200);
    LOG_INFO("server.loading", "Cortex: Needs {}, VendorRange {}, TrainerRange {}, Food {}, Mail {}", sEnabled,
             sVendorRange, sTrainerRange, sFood, sMail);
}

bool CortexNeeds::Enabled() { return sEnabled; }

bool CortexNeedsAction::isUseful()
{
    if (!sEnabled)
        return false;
    CortexPlan* plan = Cortex::Plan(bot);
    CortexMemory* memory = Cortex::Memory(bot);
    if (!plan || !memory || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->GetGroup() ||
        bot->GetMap()->Instanceable() || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;
    if (need != Need::NONE)
        return true;
    // Upkeep took a vendor in sight over the remembered one and walks there: no other errand walks the bot off.
    if (holdUntilMs)
    {
        uint32 now = getMSTime();
        if (!Due(now, holdUntilMs) && Due(now, botAI->rpgInfo.upkeepNextCheckMs))
            return false;
        holdUntilMs = 0;
    }
    // At the pier, aboard or casting: the ride action sees that through first. Hurt: recover first.
    if (CortexTravel::Busy(plan->trip) || botAI->rpgInfo.recoverSinceMs)
        return false;
    return Choose(*plan, *memory, getMSTime());
}

bool CortexNeedsAction::Choose(CortexPlan& /*plan*/, CortexMemory const& memory, uint32 now)
{
    if (ChooseVendor(memory, now) || ChooseTrainer(memory, now) || ChooseProfession(memory, now) ||
        ChooseInn(memory, now))
        return true;
    // Never an errand here: the trip is handed to playerbots' auction action, which runs right below.
    ChooseAuction(memory, now);
    return ChooseMail(memory, now) || ChooseFood(memory, now);
}

// Selling, repairs, ammo, water and pet food: the same needs as upkeep (bots 0006, 0009, 0058), and the same order
// of vendors to try, so the remembered vendor serves the need (repairs, sells the ammo, drinks or pet food). With
// memory serving them, upkeep's own check is kept behind this one so it never looks first.
bool CortexNeedsAction::ChooseVendor(CortexMemory const& memory, uint32 now)
{
    if (!sPlayerbotAIConfig.NewRpgUpkeep || !Due(now, nextCheckMs[Index(Need::VENDOR)]))
        return false;
    nextCheckMs[Index(Need::VENDOR)] = now + VENDOR_CHECK_MS;

    uint32 ammo = 0;
    uint32 wants = VendorWants(ammo);
    if (!wants)
    {
        uint32& upkeepAt = botAI->rpgInfo.upkeepNextCheckMs;
        if (!Due(upkeepAt, now + VENDOR_CHECK_MS))
            upkeepAt = now + VENDOR_CHECK_MS + IN_MILLISECONDS;
        return false;
    }

    uint8 level = static_cast<uint8>(bot->GetLevel());
    bool repair = wants & WANT_REPAIR;
    bool sell = wants & WANT_SELL;
    bool water = wants & WANT_WATER;
    uint32 waterBudget = water ? WaterBudget() : 0;
    Pet* pet = (wants & WANT_PET_FOOD) ? HunterPetFood::HunterPet(bot) : nullptr;
    auto isVendor = [](CortexMemory::Npc const& npc) { return (npc.services & CortexMemory::SERVICE_VENDOR) != 0; };
    auto repairs = [](CortexMemory::Npc const& npc) { return (npc.services & CortexMemory::SERVICE_REPAIR) != 0; };
    auto sellsAmmo = [&](CortexMemory::Npc const& npc)
    { return (npc.services & CortexMemory::SERVICE_AMMO) && SellsAmmo(npc.entry, ammo, level); };
    auto sellsPetFood = [&](CortexMemory::Npc const& npc) { return pet && SellsPetFood(npc.entry, pet, level); };
    auto sellsWater = [&](CortexMemory::Npc const& npc) { return SellsDrink(npc.entry, waterBudget); };

    std::vector<NpcMatch> tries;
    // Low on water and no repair or ammo to buy: a vendor with drinks, which takes the junk as well. Armorers and
    // weaponsmiths (the repair vendors) almost never sell drinks.
    if (water && !repair && !ammo)
        tries.push_back([&](CortexMemory::Npc const& npc) { return isVendor(npc) && sellsWater(npc); });
    if (repair || sell || ammo)
        tries.push_back([&](CortexMemory::Npc const& npc)
                        { return isVendor(npc) && (!repair || repairs(npc)) && (!ammo || sellsAmmo(npc)); });
    if (ammo && repair)  // ammo first: a hunter without it barely fights
        tries.push_back([&](CortexMemory::Npc const& npc) { return isVendor(npc) && sellsAmmo(npc); });
    if (repair || sell)
        tries.push_back([&](CortexMemory::Npc const& npc) { return isVendor(npc) && (!repair || repairs(npc)); });
    if (repair && sell)  // a plain vendor still clears the bags
        tries.push_back(isVendor);
    if (water)
        tries.push_back([&](CortexMemory::Npc const& npc) { return isVendor(npc) && sellsWater(npc); });
    if (pet)
        tries.push_back([&](CortexMemory::Npc const& npc) { return isVendor(npc) && sellsPetFood(npc); });

    for (NpcMatch const& match : tries)
    {
        uint64 npcKey = 0;
        if (CortexMemory::Npc const* npc = NearestNpc(memory, sVendorRange, match, npcKey))
        {
            Start(Need::VENDOR, npcKey, npc->entry, npc->map, npc->x, npc->y, npc->z, static_cast<int32>(wants));
            return true;
        }
    }
    Fallback(Need::VENDOR, VENDOR_FALLBACK_MS, static_cast<int32>(wants));
    return false;
}

// Out of food and drink: buy some at a food vendor in town. Playerbots has no trip for this; its "buy" at a vendor
// visit (bots 0021) does the buying.
bool CortexNeedsAction::ChooseFood(CortexMemory const& memory, uint32 now)
{
    if (!sFood || !sPlayerbotAIConfig.NewRpgUpkeepBuy || !Due(now, nextCheckMs[Index(Need::FOOD)]))
        return false;
    nextCheckMs[Index(Need::FOOD)] = now + FOOD_CHECK_MS;

    uint32 level = bot->GetLevel();
    if (level < FOOD_MIN_LEVEL || bot->getClass() == CLASS_MAGE || botAI->HasCheat(BotCheatMask::food) ||
        bot->GetMoney() < level * level * 5 || FoodCount() >= FOOD_MIN)
        return false;

    uint64 npcKey = 0;
    CortexMemory::Npc const* npc = NearestNpc(
        memory, FOOD_RANGE, [](CortexMemory::Npc const& n) { return (n.services & CortexMemory::SERVICE_FOOD) != 0; },
        npcKey);
    if (!npc)
        return false;
    Start(Need::FOOD, npcKey, npc->entry, npc->map, npc->x, npc->y, npc->z, static_cast<int32>(FoodCount()));
    return true;
}

// New class spells or ranks the bot can afford, at the nearest class trainer it remembers.
bool CortexNeedsAction::ChooseTrainer(CortexMemory const& memory, uint32 now)
{
    if (!sTrainerTrips || !Due(now, nextCheckMs[Index(Need::TRAINER)]))
        return false;
    nextCheckMs[Index(Need::TRAINER)] = now + TRAINER_CHECK_MS;

    uint8 level = static_cast<uint8>(bot->GetLevel());
    if (checkedLevel == level && !Due(now, trainerRecheckMs))
        return false;

    uint8 classId = bot->getClass();
    uint64 npcKey = 0;
    CortexMemory::Npc const* npc = NearestNpc(
        memory, sTrainerRange,
        [&](CortexMemory::Npc const& n)
        {
            if (!(n.services & (CortexMemory::SERVICE_TRAINER | CortexMemory::SERVICE_CLASS_TRAINER)) ||
                (n.trainerClass && n.trainerClass != classId))
                return false;
            Trainer::Trainer* trainer = sObjectMgr->GetTrainer(n.entry);
            return trainer && trainer->GetTrainerType() == Trainer::Type::Class && trainer->IsTrainerValidForPlayer(bot);
        },
        npcKey);

    Trainer::Trainer* trainer = npc ? sObjectMgr->GetTrainer(npc->entry) : AnyClassTrainer(bot);
    uint32 cost = 0;
    uint32 count = trainer ? Learnable(bot, trainer, cost) : 0;
    // Something to learn, and at least the cheapest of it affordable (the trainer action learns what the budget allows)
    if (!count || bot->GetMoney() < cost / count)
    {
        checkedLevel = level;
        trainerRecheckMs = now + TRAINER_RECHECK_MS;
        return false;
    }
    if (!npc)
    {
        checkedLevel = level;
        trainerRecheckMs = now + TRAINER_FALLBACK_MS;
        Fallback(Need::TRAINER, TRAINER_FALLBACK_MS, static_cast<int32>(count));
        return false;
    }
    Start(Need::TRAINER, npcKey, npc->entry, npc->map, npc->x, npc->y, npc->z, static_cast<int32>(count));
    return true;
}

// A profession to learn, the next rank of one at its cap, or new recipes close by (as the trainer trips decide it,
// bots 0048), at a trainer of that profession the bot remembers.
bool CortexNeedsAction::ChooseProfession(CortexMemory const& memory, uint32 now)
{
    if (!sTrainerTrips || !BotProfessions::Enabled() || !Due(now, nextCheckMs[Index(Need::PROFESSION)]))
        return false;
    nextCheckMs[Index(Need::PROFESSION)] = now + PROFESSION_CHECK_MS;

    std::vector<uint32> skills = BotProfessions::Wanted(bot);
    for (uint32 known : {SKILL_MINING, SKILL_BLACKSMITHING, SKILL_ENGINEERING, SKILL_JEWELCRAFTING, SKILL_SKINNING,
                         SKILL_LEATHERWORKING, SKILL_HERBALISM, SKILL_ALCHEMY, SKILL_TAILORING, SKILL_ENCHANTING,
                         SKILL_INSCRIPTION})
        if (bot->HasSkill(known))
            skills.push_back(known);

    CortexMemory::Npc const* best = nullptr;
    uint64 bestKey = 0;
    uint32 bestSkill = 0;
    float bestDist = sTrainerRange;
    bool unserved = false;  // a profession to learn or a rank due, with no trainer for it in memory
    for (uint32 wanted : skills)
    {
        bool known = bot->HasSkill(wanted);
        uint32 maxSkill = known ? bot->GetMaxSkillValue(wanted) : 0;
        bool capped = known && bot->GetSkillValue(wanted) + SKILL_CAP_MARGIN >= maxSkill;
        uint8 rankLevel = capped ? NextRankLevel(maxSkill) : 0;
        bool rankDue = rankLevel && bot->GetLevel() >= rankLevel;

        uint64 npcKey = 0;
        CortexMemory::Npc const* npc = NearestNpc(
            memory, bestDist,
            [&](CortexMemory::Npc const& n)
            {
                if (!(n.services & (CortexMemory::SERVICE_TRAINER | CortexMemory::SERVICE_PROFESSION_TRAINER)) ||
                    TrainerSkill(n.entry) != wanted)
                    return false;
                Trainer::Trainer* trainer = sObjectMgr->GetTrainer(n.entry);
                return trainer && trainer->IsTrainerValidForPlayer(bot);
            },
            npcKey);
        if (!npc)
        {
            if (!known || rankDue)
                unserved = true;
            continue;
        }

        float dist = bot->GetExactDist2d(npc->x, npc->y);
        uint32 cost = 0;
        uint32 ranks = 0;
        uint32 count = BotProfessions::Worthwhile(bot, sObjectMgr->GetTrainer(npc->entry), wanted, cost, ranks);
        bool trip = known ? (ranks && capped) || dist <= PROFESSION_PASSING_DISTANCE : ranks > 0;
        if (!count || !trip || dist >= bestDist)
            continue;
        best = npc;
        bestKey = npcKey;
        bestSkill = wanted;
        bestDist = dist;
    }

    if (best)
    {
        Start(Need::PROFESSION, bestKey, best->entry, best->map, best->x, best->y, best->z,
              static_cast<int32>(bestSkill));
        skill = bestSkill;
        return true;
    }
    if (unserved)
        Fallback(Need::PROFESSION, TRAINER_FALLBACK_MS, 0);
    return false;
}

// Home is far away and the bot is in a town (not a capital) whose innkeeper it has seen: make that inn home, as a
// player does at a new quest hub (bots 0057).
bool CortexNeedsAction::ChooseInn(CortexMemory const& memory, uint32 now)
{
    if (!sTrainerTrips || !Due(now, nextCheckMs[Index(Need::INN)]))
        return false;
    nextCheckMs[Index(Need::INN)] = now + INN_CHECK_MS;

    uint32 unixNow = UnixNow();
    if (bot->GetLevel() < INN_MIN_LEVEL || (innBoundAt && unixNow < innBoundAt + INN_BIND_COOLDOWN_SECONDS) ||
        HomeDistance(bot, bot->GetPositionX(), bot->GetPositionY()) < INN_REBIND_DISTANCE)
        return false;
    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId());
    if (!zone || (zone->flags & AREA_FLAG_CAPITAL))
        return false;

    uint64 npcKey = 0;
    CortexMemory::Npc const* npc = NearestNpc(
        memory, INN_RANGE,
        [&](CortexMemory::Npc const& n)
        {
            return (n.services & CortexMemory::SERVICE_INNKEEPER) &&
                   HomeDistance(bot, n.x, n.y) >= INN_REBIND_DISTANCE;
        },
        npcKey);
    if (!npc)
        return false;
    Start(Need::INN, npcKey, npc->entry, npc->map, npc->x, npc->y, npc->z,
          static_cast<int32>(std::min(HomeDistance(bot, npc->x, npc->y), 99999.0f)));
    return true;
}

// Auction mail (sales money, won or expired items) waiting and a mailbox the bot remembers close by: collect it.
bool CortexNeedsAction::ChooseMail(CortexMemory const& memory, uint32 now)
{
    if (!sMail || !BotAuction::Config().enabled || !Due(now, nextCheckMs[Index(Need::MAIL)]))
        return false;
    nextCheckMs[Index(Need::MAIL)] = now + MAIL_CHECK_MS;
    if (BotAuction::State(bot).phase != BotAuction::Phase::NONE || !BotAuction::HasCollectibleMail(bot))
        return false;

    CortexMemory::Object const* best = nullptr;
    uint64 bestKey = 0;
    float bestDist = MAIL_RANGE;
    for (auto const& [objectKey, object] : memory.Objects())
    {
        if (object.kind != CortexMemory::OBJECT_MAILBOX || object.map != bot->GetMapId())
            continue;
        float d = bot->GetExactDist2d(object.x, object.y);
        if (d >= bestDist || !Usable(objectKey, now) ||
            !CortexTravel::SameRegion(bot, object.map, object.x, object.y, object.z, 0))
            continue;
        best = &object;
        bestKey = objectKey;
        bestDist = d;
    }
    if (!best)
        return false;
    Start(Need::MAIL, bestKey, best->entry, best->map, best->x, best->y, best->z, 0);
    return true;
}

// An auction house trip (bots 0011) to an auctioneer the bot remembers: the trip state is filled here and
// playerbots' auction action carries it out. Its own check is kept behind this one; with nothing in memory it is
// let go at once and picks from its catalog.
void CortexNeedsAction::ChooseAuction(CortexMemory const& memory, uint32 now)
{
    BotAuction::Settings const& cfg = BotAuction::Config();
    if (!cfg.enabled || bot->GetLevel() < cfg.minLevel || !Due(now, nextCheckMs[Index(Need::AUCTION)]))
        return;
    BotAuction::TripState& state = BotAuction::State(bot);
    // Not started, or not spread over the first interval yet (the auction action does that on its first look).
    if (state.phase != BotAuction::Phase::NONE || !state.lastTripAt)
        return;
    nextCheckMs[Index(Need::AUCTION)] = now + AUCTION_CHECK_MS;

    time_t t = GameTime::GetGameTime().count();
    state.nextCheckAt = std::max<time_t>(state.nextCheckAt, t + AUCTION_CHECK_MS / IN_MILLISECONDS + 1);
    if (t < state.giveUpUntil || t < state.lastTripAt + static_cast<time_t>(cfg.tripMinutes) * MINUTE)
        return;

    // The auction action's reasons (NewRpgAuctionAction::TripReasons).
    uint32 reasons = 0;
    if (BotAuction::HasCollectibleMail(bot))
        reasons |= 0x1;
    uint64 heldValue = 0;
    uint32 held = 0;
    HunterPetFood::ForEachBagItem(bot,
                                  [&](Item* item)
                                  {
                                      if (!BotAuction::WantsToList(botAI, item))
                                          return;
                                      ++held;
                                      heldValue += static_cast<uint64>(BotAuction::UnitValue(item->GetTemplate())) *
                                                   item->GetCount();
                                  });
    uint64 level = bot->GetLevel();
    if (held && (heldValue >= level * level * 5 ||
                 AI_VALUE(uint8, "bag space") >= sPlayerbotAIConfig.NewRpgUpkeepBagPct))
        reasons |= 0x2;
    uint64 money = bot->GetMoney();
    uint64 spare = money > level * level * 10 ? money - level * level * 10 : 0;
    if (spare * cfg.spendPct / 100 >= level * level * 10 &&
        t >= state.lastTripAt + 2 * static_cast<time_t>(cfg.tripMinutes) * MINUTE)
        reasons |= 0x4;
    if (!reasons)
        return;

    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    std::vector<BotAuction::Spot> const& spots = BotAuction::Spots();
    int32 bestSpot = -1;
    uint32 bestEntry = 0;
    float bestDist = cfg.maxDistance;
    for (auto const& [npcKey, npc] : memory.Npcs())
    {
        if (!(npc.services & CortexMemory::SERVICE_AUCTIONEER) || npc.map != bot->GetMapId() || !botFaction)
            continue;
        float d = bot->GetExactDist2d(npc.x, npc.y);
        if (d >= bestDist || !Usable(npcKey, now) || !CortexTravel::SameRegion(bot, npc.map, npc.x, npc.y, npc.z, 0))
            continue;
        for (size_t i = 0; i < spots.size(); ++i)
        {
            BotAuction::Spot const& spot = spots[i];
            if (spot.map != npc.map || Dist2d(spot.x, spot.y, npc.x, npc.y) > AUCTION_SPOT_MATCH ||
                std::find(cfg.skipZones.begin(), cfg.skipZones.end(), spot.zone) != cfg.skipZones.end())
                continue;
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(spot.factionTemplate);
            if (!faction || botFaction->IsHostileTo(*faction))
                continue;
            bestSpot = static_cast<int32>(i);
            bestEntry = npc.entry;
            bestDist = d;
            break;
        }
    }

    if (bestSpot < 0)
    {
        state.nextCheckAt = 0;
        Fallback(Need::AUCTION, AUCTION_FALLBACK_MS, static_cast<int32>(reasons));
        return;
    }

    BotAuction::Spot const& spot = spots[bestSpot];
    state.spot = bestSpot;
    state.startedAt = t;
    state.bought = 0;
    state.opDone = false;
    state.phase = (reasons & 0x1) ? BotAuction::Phase::TO_MAILBOX : BotAuction::Phase::TO_AUCTIONEER;
    BotTelemetry::instance().Event(botAI, "ah", "trip", static_cast<int32>(bot->GetExactDist(spot.x, spot.y, spot.z)),
                                   static_cast<int32>(reasons), static_cast<int32>(spot.auctioneerSpawn));
    BotTelemetry::instance().Event(botAI, "need_mem", NeedName(Need::AUCTION), static_cast<int32>(bestDist),
                                   static_cast<int32>(bestEntry), static_cast<int32>(reasons));
}

CortexMemory::Npc const* CortexNeedsAction::NearestNpc(CortexMemory const& memory, float range, NpcMatch const& match,
                                                       uint64& keyOut)
{
    uint32 now = getMSTime();
    uint32 mapId = bot->GetMapId();
    struct Candidate
    {
        float cost;
        bool sameRegion;
        uint64 key;
        CortexMemory::Npc const* npc;
    };
    std::vector<Candidate> found;
    for (auto const& [npcKey, npc] : memory.Npcs())
    {
        if (npc.map != mapId)
            continue;
        float d = bot->GetExactDist2d(npc.x, npc.y);
        if (d >= range || !match(npc) || !Usable(npcKey, now))
            continue;
        bool same = CortexTravel::SameRegion(bot, npc.map, npc.x, npc.y, npc.z, 0);
        if (!same && !CortexTravel::Enabled())
            continue;
        found.push_back({same ? d : d * OTHER_REGION_FACTOR + OTHER_REGION_EXTRA, same, npcKey, &npc});
    }
    std::sort(found.begin(), found.end(), [](Candidate const& a, Candidate const& b) { return a.cost < b.cost; });

    uint32 reachChecks = 0;
    for (Candidate const& candidate : found)
    {
        if (!candidate.sameRegion)
        {
            if (reachChecks >= MAX_REACH_CHECKS)
                continue;
            ++reachChecks;
            CortexMemory::Npc const& npc = *candidate.npc;
            if (!CortexTravel::Reachable(bot, npc.map, npc.x, npc.y, 0))
                continue;
        }
        keyOut = candidate.key;
        return candidate.npc;
    }
    return nullptr;
}

bool CortexNeedsAction::Usable(uint64 memoryKey, uint32 now)
{
    auto it = skip.find(memoryKey);
    if (it != skip.end())
    {
        if (!Due(now, it->second))
            return false;
        skip.erase(it);
    }
    return !Blocked(memoryKey);
}

// What upkeep would go to a vendor for (NewRpgUpkeepAction::isUseful): bags full of things to sell, gear worn down,
// a hunter low on ammo (and the ammo subclass), a mana user low on water, or out of pet food.
uint32 CortexNeedsAction::VendorWants(uint32& ammoSubclass)
{
    uint32 wants = 0;
    ammoSubclass = 0;
    if (AI_VALUE(uint8, "bag space") >= sPlayerbotAIConfig.NewRpgUpkeepBagPct && AI_VALUE(bool, "can sell"))
        wants |= WANT_SELL;
    if (AI_VALUE(uint8, "durability") < sPlayerbotAIConfig.NewRpgUpkeepDurabilityPct && AI_VALUE(bool, "can repair"))
        wants |= WANT_REPAIR;
    if (!sPlayerbotAIConfig.NewRpgUpkeepBuy)
        return wants;

    uint32 level = bot->GetLevel();
    if (bot->GetMaxPower(POWER_MANA) && DrinkCount(bot) < WATER_LOW && WaterBudget() >= 2 * level * level)
        wants |= WANT_WATER;
    if (bot->getClass() != CLASS_HUNTER)
        return wants;

    if (Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
    {
        switch (ranged->GetTemplate()->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_GUN:
                ammoSubclass = ITEM_SUBCLASS_BULLET;
                break;
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                ammoSubclass = ITEM_SUBCLASS_ARROW;
                break;
            default:
                break;
        }
        if (ammoSubclass && (AI_VALUE2(uint32, "item count", "ammo") >= sMinAmmo || bot->GetMoney() < level * level))
            ammoSubclass = 0;
        if (ammoSubclass)
            wants |= WANT_AMMO;
    }

    Pet* pet = HunterPetFood::HunterPet(bot);
    if (pet && bot->HasSpell(HunterPetFood::SPELL_FEED_PET) && bot->GetMoney() >= 4 * level * level &&
        HunterPetFood::Count(bot, pet) < PET_FOOD_LOW)
        wants |= WANT_PET_FOOD;
    return wants;
}

// As upkeep: what is free for consumables after repair and ammo, and at least a quarter of the bot's money.
uint32 CortexNeedsAction::WaterBudget()
{
    uint32 spare = AI_VALUE2(uint32, "free money for", static_cast<uint32>(NeedMoneyFor::consumables));
    return std::max<uint32>(spare, bot->GetMoney() / 4);
}

bool CortexNeedsAction::SellsDrink(uint32 vendorEntry, uint32 budget)
{
    auto it = vendorDrinks.find(vendorEntry);
    if (it == vendorDrinks.end())
    {
        std::vector<ItemTemplate const*> drinks;
        if (VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(vendorEntry))
            for (VendorItem const* item : items->m_items)
            {
                ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
                if (proto && !item->ExtendedCost && proto->BuyPrice && IsDrink(proto))
                    drinks.push_back(proto);
            }
        it = vendorDrinks.emplace(vendorEntry, std::move(drinks)).first;
    }
    for (ItemTemplate const* proto : it->second)
        if (DrinkSuits(bot, proto) && proto->BuyPrice <= budget && bot->CanUseItem(proto) == EQUIP_ERR_OK)
            return true;
    return false;
}

// Food and drink in the bags the bot is high enough to use.
uint32 CortexNeedsAction::FoodCount()
{
    uint32 count = 0;
    uint32 level = bot->GetLevel();
    HunterPetFood::ForEachBagItem(bot,
                                  [&](Item* item)
                                  {
                                      ItemTemplate const* proto = item->GetTemplate();
                                      if (proto->Class == ITEM_CLASS_CONSUMABLE &&
                                          proto->SubClass == ITEM_SUBCLASS_FOOD && proto->RequiredLevel <= level)
                                          count += item->GetCount();
                                  });
    return count;
}

// The profession a trainer teaches, as its title says (<Herbalism Trainer>); 0 for other trainers.
uint32 CortexNeedsAction::TrainerSkill(uint32 trainerEntry)
{
    auto it = trainerSkills.find(trainerEntry);
    if (it != trainerSkills.end())
        return it->second;
    Trainer::Trainer* trainer = sObjectMgr->GetTrainer(trainerEntry);
    uint32 taught = trainer && trainer->GetTrainerType() == Trainer::Type::Tradeskill
                        ? BotProfessions::TrainerSkill(trainer)
                        : 0;
    trainerSkills[trainerEntry] = taught;
    return taught;
}

void CortexNeedsAction::Start(Need started, uint64 memoryKey, uint32 targetEntry, uint32 targetMap, float targetX,
                              float targetY, float targetZ, int32 detail)
{
    uint32 now = getMSTime();
    need = started;
    key = memoryKey;
    entry = targetEntry;
    map = targetMap;
    x = targetX;
    y = targetY;
    z = targetZ;
    skill = 0;
    startMs = now;
    seenMs = now;
    seenAwayMs = Cortex::AwayMs(bot);
    best = bot->GetExactDist2d(x, y);
    bestMs = now;
    tripTried = false;
    relocations = 0;
    bool sameRegion = CortexTravel::SameRegion(bot, map, x, y, z, 0);
    std::string info = std::string(NeedName(need)) + (sameRegion ? "" : " trip");
    BotTelemetry::instance().Event(botAI, "need_mem", info, static_cast<int32>(best), static_cast<int32>(entry),
                                   detail);
}

void CortexNeedsAction::End(uint32 cooldownMs)
{
    if (need != Need::NONE)
        nextCheckMs[Index(need)] = getMSTime() + cooldownMs;
    need = Need::NONE;
    key = 0;
    entry = 0;
    skill = 0;
    tripTried = false;
}

void CortexNeedsAction::GiveUp(char const* reason, uint32 skipMs, bool strike)
{
    uint32 now = getMSTime();
    bool blocked = false;
    if (skipMs)
        skip[key] = now + skipMs;
    if (strike)
        blocked = AddStrike(key);
    BotTelemetry::instance().Event(botAI, "need_fail", std::string(NeedName(need)) + " " + reason,
                                   static_cast<int32>(entry),
                                   static_cast<int32>(getMSTimeDiff(startMs, now) / IN_MILLISECONDS), blocked ? 1 : 0);
    End(RETRY_MS);
}

// Memory has no place for the need: playerbots' own action takes it (looking around, a camp, its catalogs).
void CortexNeedsAction::Fallback(Need fallback, uint32 holdMs, int32 detail)
{
    uint32 now = getMSTime();
    nextCheckMs[Index(fallback)] = now + holdMs;
    if (fallback == Need::VENDOR)
        botAI->rpgInfo.upkeepNextCheckMs = now;
    BotTelemetry::instance().Event(botAI, "need_fallbk", NeedName(fallback), detail);
}

bool CortexNeedsAction::Execute(Event /*event*/)
{
    CortexPlan* plan = Cortex::Plan(bot);
    if (!plan || need == Need::NONE)
        return false;

    // Time spent fighting, dead or recovering doesn't count against the timers. The gap between steps doesn't tell
    // it: the bot's own moves set its next AI check seconds out, and an errand that never got closer was never
    // given up on (bots stood 12 yd from a Darnassus vendor for hours, the timers pushed back at every step).
    uint32 now = getMSTime();
    uint32 awayMs = Cortex::AwayMs(bot);
    uint32 away = awayMs - seenAwayMs;
    seenAwayMs = awayMs;
    if (away)
    {
        startMs += away;
        bestMs += away;
    }
    seenMs = now;
    plan->step = "n:walk";
    plan->stepDetail = NeedName(need);
    plan->SetStepTarget(map, x, y, z);

    if (getMSTimeDiff(startMs, now) >= ERRAND_MS)
    {
        GiveUp("timeout", TIMEOUT_SKIP_MS, false);
        return false;
    }
    if (need == Need::VENDOR)
    {
        uint32 ammo = 0;
        if (!VendorWants(ammo))  // sold or repaired on the way (a vendor in passing)
        {
            End(VENDOR_CHECK_MS);
            return false;
        }
    }
    // Off the map the errand was for (a hearth out of a dead end) with no trip of its own under way.
    CortexTrip const& trip = plan->trip;
    bool ownTrip = trip.active && trip.map == map && Dist2d(trip.x, trip.y, x, y) <= 5.0f;
    if (bot->GetMapId() != map && !ownTrip)
    {
        GiveUp("map", 0, false);
        return false;
    }

    WorldObject* target = FindTarget();
    if (target)
    {
        if (bot->IsWithinDistInMap(target, INTERACTION_DISTANCE - 1.0f))
        {
            plan->step = "n:at";
            return AtTarget(target, now);
        }
        // A walker in sight is followed, not its remembered spot.
        Creature* creature = target->ToCreature();
        if (bot->GetExactDist2d(target) <= APPROACH ||
            (creature && creature->GetDefaultMovementType() != IDLE_MOTION_TYPE))
        {
            // In sight but no closer for a while (up on a ledge, behind a rail): unreachable like a remembered spot.
            float d = bot->GetExactDist2d(target);
            if (d + PROGRESS < best)
            {
                best = d;
                bestMs = now;
            }
            if (getMSTimeDiff(bestMs, now) >= STUCK_MS)
            {
                GiveUp("unreach_near", UNREACH_SKIP_MS, true);
                return false;
            }
            plan->step = "n:approach";
            MoveWorldObjectTo(target->GetGUID(), INTERACTION_DISTANCE - 1.0f);
            return true;
        }
    }
    else if (!ownTrip && bot->GetExactDist2d(x, y) <= ARRIVE && CortexTravel::SameRegion(bot, map, x, y, z, 0))
    {
        // Not at its spot: a walker on its round (Lizzarik on the Barrens road, the bread vendors of the capitals)
        // is somewhere else on its way. Go to where it is now.
        Creature* walker = need != Need::MAIL && relocations < MAX_RELOCATIONS
                               ? bot->FindNearestCreature(entry, WALKER_SEARCH)
                               : nullptr;
        if (walker && !walker->IsHostileTo(bot) && !walker->GetTransport() &&
            CortexTravel::SameRegion(bot, map, walker->GetPositionX(), walker->GetPositionY(),
                                     walker->GetPositionZ(), 0))
        {
            ++relocations;
            BotTelemetry::instance().Event(botAI, "need_move", NeedName(need), static_cast<int32>(entry),
                                           static_cast<int32>(bot->GetExactDist2d(walker)), relocations);
            x = walker->GetPositionX();
            y = walker->GetPositionY();
            z = walker->GetPositionZ();
            best = bot->GetExactDist2d(x, y);
            bestMs = now;
            return Walk(*plan, now);
        }
        GiveUp("absent", ABSENT_SKIP_MS, false);
        return false;
    }
    return Walk(*plan, now);
}

// Walks to the remembered place, or takes a trip (CortexTravel) when it lies in another region (up a lift, across
// water). Gives up when walking stops getting closer, the walk's unstick marks it unreachable, or the trip fails.
bool CortexNeedsAction::Walk(CortexPlan& plan, uint32 now)
{
    CortexTrip& trip = plan.trip;
    bool tripTarget = trip.map == map && Dist2d(trip.x, trip.y, x, y) <= 5.0f;
    if (tripTarget)
    {
        if (trip.active)
        {
            bool failed = false;
            TripStep(plan, failed);
            if (failed)
            {
                GiveUp("trip", UNREACH_SKIP_MS, true);
                return false;
            }
            if (trip.active)
                return true;  // waiting at a pier counts as busy too
        }
        else if (trip.failed)  // the ride action gave up on it
        {
            trip.failed = false;
            GiveUp("trip", UNREACH_SKIP_MS, true);
            return false;
        }
        if (trip.finished)
        {
            trip.finished = false;
            best = bot->GetMapId() == map ? bot->GetExactDist2d(x, y) : FLT_MAX;
            bestMs = now;
        }
    }

    if (bot->GetMapId() != map || !CortexTravel::SameRegion(bot, map, x, y, z, 0))
    {
        if (!tripTried && CortexTravel::Enabled() && !CortexTravel::Busy(trip))
        {
            tripTried = true;
            if (StartTrip(plan, map, x, y, z, 0, false, "need"))
            {
                bool failed = false;
                TripStep(plan, failed);
                if (failed)
                {
                    GiveUp("trip", UNREACH_SKIP_MS, true);
                    return false;
                }
                return true;
            }
        }
        GiveUp("noroute", UNREACH_SKIP_MS, false);
        return false;
    }

    float d = bot->GetExactDist2d(x, y);
    if (d + PROGRESS < best)
    {
        best = d;
        bestMs = now;
    }
    WorldPosition dest(map, x, y, z);
    uint32 unreachableMs = sPlayerbotAIConfig.NewRpgUnreachableMinutes * MINUTE * IN_MILLISECONDS;
    if (getMSTimeDiff(bestMs, now) >= STUCK_MS ||
        (sPlayerbotAIConfig.NewRpgWalkUnstick && botAI->rpgInfo.IsUnreachable(dest, unreachableMs)))
    {
        // Walking got nowhere: a trip round it (a lift, a flight) once, before the place counts as unreachable.
        if (!tripTried && CortexTravel::Enabled() && !CortexTravel::Busy(trip))
        {
            tripTried = true;
            if (StartTrip(plan, map, x, y, z, 0, false, "need"))
            {
                best = d;
                bestMs = now;
                bool failed = false;
                TripStep(plan, failed);
                return true;
            }
        }
        GiveUp("unreach", UNREACH_SKIP_MS, true);
        return false;
    }
    MoveFarTo(dest);
    return true;
}

// The remembered NPC (alive, not hostile) or mailbox, once the bot is close enough to see it at its spot.
WorldObject* CortexNeedsAction::FindTarget()
{
    if (bot->GetMapId() != map || bot->GetExactDist2d(x, y) > TARGET_SEARCH)
        return nullptr;
    if (need == Need::MAIL)
    {
        GameObject* mailbox = bot->FindNearestGameObject(entry, TARGET_SEARCH, true);
        return mailbox && mailbox->GetExactDist2d(x, y) <= TARGET_NEAR_SPOT ? mailbox : nullptr;
    }
    Creature* creature = bot->FindNearestCreature(entry, TARGET_SEARCH);
    if (!creature || creature->IsHostileTo(bot) || creature->GetTransport())
        return nullptr;
    // A walker is on its round, not at the spot it was seen.
    if (creature->GetExactDist2d(x, y) > TARGET_NEAR_SPOT && creature->GetDefaultMovementType() == IDLE_MOTION_TYPE)
        return nullptr;
    return creature;
}

bool CortexNeedsAction::AtTarget(WorldObject* target, uint32 now)
{
    if (bot->isMoving())
    {
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
    }
    bot->SetFacingToObject(target);

    switch (need)
    {
        case Need::VENDOR:
        {
            // Handed over here a moment ago and the need is still there: upkeep can't do business with this one.
            if (key == handoverKey && getMSTimeDiff(handoverMs, now) < HANDOVER_REPEAT_MS)
            {
                GiveUp("handover", TIMEOUT_SKIP_MS, false);
                return false;
            }
            handoverKey = key;
            handoverMs = now;
            // Upkeep finds the vendor in sight and does the business (equip, sell, repair, ammo, water, pet food,
            // buy), here and now: left to a later tick, an errand ranked above it (Cortex's next need, the trainer
            // trip) walked the bot off first, and the vendor need was still there on the next visit.
            uint32& upkeepAt = botAI->rpgInfo.upkeepNextCheckMs;
            upkeepAt = now;
            if (!botAI->DoSpecificAction("new rpg upkeep", Event(), true))
            {
                GiveUp("upkeep", TIMEOUT_SKIP_MS, false);  // no business here (and it may be off to a camp)
                return false;
            }
            // Done: upkeep's cooldown runs. Still due: it walks to a vendor in sight it prefers (one with drinks).
            bool walking = Due(getMSTime(), upkeepAt);
            if (walking)
                holdUntilMs = now + HANDOVER_HOLD_MS;
            BotTelemetry::instance().Event(botAI, "need_done", NeedName(need), static_cast<int32>(entry),
                                           static_cast<int32>(getMSTimeDiff(startMs, now) / IN_MILLISECONDS),
                                           walking ? 2 : 1);
            End(HANDOVER_MS);
            return true;
        }
        case Need::FOOD:
            DoFood(target->ToCreature());
            End(FOOD_DONE_MS);
            return true;
        case Need::TRAINER:
            DoTrainer(target->ToCreature());
            checkedLevel = static_cast<uint8>(bot->GetLevel());
            trainerRecheckMs = now + TRAINER_RECHECK_MS;
            End(TRAINER_CHECK_MS);
            return true;
        case Need::PROFESSION:
            DoProfession(target->ToCreature());
            End(PROFESSION_CHECK_MS);
            return true;
        case Need::INN:
            DoInn(target->ToCreature());
            innBoundAt = UnixNow();  // tried: not again for a while either way
            End(INN_CHECK_MS);
            return true;
        case Need::MAIL:
            DoMail(target->ToGameObject());
            End(MAIL_CHECK_MS);
            return true;
        default:
            End(RETRY_MS);
            return false;
    }
}

void CortexNeedsAction::DoFood(Creature* vendor)
{
    if (!vendor)
        return;
    uint32 before = FoodCount();
    uint32 money = bot->GetMoney();
    bot->SetSelection(vendor->GetGUID());
    // As upkeep's business: equip first so upgrades aren't sold, sell, repair, then buy (food and drink among it).
    botAI->DoSpecificAction("equip upgrade", Event(), true);
    botAI->DoSpecificAction("sell", Event("upkeep", "gray"), true);
    botAI->DoSpecificAction("sell", Event("upkeep", "vendor"), true);
    if (vendor->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))
        botAI->DoSpecificAction("repair", Event("upkeep"), true);
    botAI->DoSpecificAction("buy", Event("upkeep", "vendor"), true);
    uint32 after = FoodCount();
    BotTelemetry::instance().Event(botAI, "need_done", NeedName(need), static_cast<int32>(entry),
                                   static_cast<int32>(after) - static_cast<int32>(before),
                                   static_cast<int32>(bot->GetMoney()) - static_cast<int32>(money));
}

void CortexNeedsAction::DoTrainer(Creature* trainer)
{
    if (!trainer)
        return;
    Trainer::Trainer const* data = sObjectMgr->GetTrainer(trainer->GetEntry());
    uint32 cost = 0;
    uint32 before = data ? Learnable(bot, data, cost) : 0;
    uint32 money = bot->GetMoney();
    bot->SetSelection(trainer->GetGUID());
    botAI->DoSpecificAction("trainer", Event("new rpg trainer", "learn"), true);
    uint32 after = data ? Learnable(bot, data, cost) : 0;
    BotTelemetry::instance().Event(botAI, "need_done", NeedName(need), static_cast<int32>(entry),
                                   static_cast<int32>(before > after ? before - after : 0),
                                   static_cast<int32>(money) - static_cast<int32>(bot->GetMoney()));
}

void CortexNeedsAction::DoProfession(Creature* trainer)
{
    if (!trainer)
        return;
    Trainer::Trainer* data = sObjectMgr->GetTrainer(trainer->GetEntry());
    uint32 money = bot->GetMoney();
    bot->SetSelection(trainer->GetGUID());
    uint32 learned = data ? BotProfessions::Learn(bot, trainer, data, skill) : 0;
    BotTelemetry::instance().Event(botAI, "need_done", std::string(NeedName(need)) + " " + std::to_string(skill),
                                   static_cast<int32>(entry), static_cast<int32>(learned),
                                   static_cast<int32>(money) - static_cast<int32>(bot->GetMoney()));
}

void CortexNeedsAction::DoInn(Creature* innkeeper)
{
    if (!innkeeper)
        return;
    float before = HomeDistance(bot, innkeeper->GetPositionX(), innkeeper->GetPositionY());
    bot->SetSelection(innkeeper->GetGUID());
    bot->GetSession()->SendBindPoint(innkeeper);  // the innkeeper's Bind spell, as from its gossip
    bool bound = HomeDistance(bot, innkeeper->GetPositionX(), innkeeper->GetPositionY()) < before;
    BotTelemetry::instance().Event(botAI, "need_done", std::string(NeedName(need)) + (bound ? " bound" : " cant"),
                                   static_cast<int32>(entry), static_cast<int32>(std::min(before, 99999.0f)),
                                   static_cast<int32>(bot->GetZoneId()));
}

void CortexNeedsAction::DoMail(GameObject* mailbox)
{
    if (!mailbox)
        return;
    // Runs on the world thread through the stock mail handlers, as on an auction trip.
    bool queued = BotAuction::QueueMailboxVisit(bot, mailbox->GetGUID());
    BotTelemetry::instance().Event(botAI, "need_done", NeedName(need), static_cast<int32>(entry), queued ? 1 : 0);
}
