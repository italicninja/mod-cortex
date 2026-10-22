/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotProfessions.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Trainer.h"

namespace
{
    constexpr uint32 RECOUNT_MS = 10 * MINUTE * IN_MILLISECONDS;
    constexpr float SCARCITY_MIN = 0.5f;
    constexpr float SCARCITY_MAX = 3.0f;

    // The eleven primary professions, in the order of the counters below.
    constexpr std::array<uint32, 11> PRIMARY = {SKILL_MINING,        SKILL_BLACKSMITHING, SKILL_ENGINEERING,
                                                SKILL_JEWELCRAFTING, SKILL_SKINNING,      SKILL_LEATHERWORKING,
                                                SKILL_HERBALISM,     SKILL_ALCHEMY,       SKILL_TAILORING,
                                                SKILL_ENCHANTING,    SKILL_INSCRIPTION};

    // A gatherer with the crafter it feeds (tailoring + enchanting: cloth and disenchanting feed each other), and
    // how well the pair suits each class: War Pal Hun Rog Pri DK Sha Mag Lock Dru (docs/professions.md).
    struct Pair
    {
        uint32 first;
        uint32 second;
        std::array<float, 10> fit;
    };

    constexpr std::array<Pair, 7> PAIRS = {{
        {SKILL_MINING, SKILL_BLACKSMITHING, {1.0f, 1.0f, .2f, .3f, .1f, 1.0f, .4f, .1f, .1f, .2f}},
        {SKILL_MINING, SKILL_ENGINEERING, {.6f, .3f, .9f, .7f, .3f, .4f, .5f, .3f, .3f, .3f}},
        {SKILL_MINING, SKILL_JEWELCRAFTING, {.4f, .6f, .4f, .4f, .4f, .5f, .5f, .4f, .4f, .4f}},
        {SKILL_SKINNING, SKILL_LEATHERWORKING, {.5f, .3f, 1.0f, 1.0f, .1f, .3f, .9f, .1f, .1f, 1.0f}},
        {SKILL_HERBALISM, SKILL_ALCHEMY, {.5f, .6f, .6f, .7f, .7f, .5f, .7f, .7f, .7f, .8f}},
        {SKILL_TAILORING, SKILL_ENCHANTING, {.1f, .3f, .1f, .1f, 1.0f, .1f, .4f, 1.0f, 1.0f, .4f}},
        {SKILL_HERBALISM, SKILL_INSCRIPTION, {.3f, .4f, .4f, .3f, .6f, .3f, .5f, .6f, .6f, .6f}},
    }};

    bool sEnabled = false;
    uint32 sMinLevel = 5;
    float sAlchemyFloor = 0.15f;
    float sEnchantingFloor = 0.12f;
    bool sGathering = true;
    float sGatherRange = 70.0f;
    uint32 sGatherCooldown = 60;
    bool sGatherTracking = true;
    bool sDisenchant = true;
    bool sMill = true;
    uint32 sToolReservePct = 50;

    std::array<std::atomic<uint32>, PRIMARY.size()> sCounts{};
    std::atomic<uint32> sBots{0};
    uint32 sRecountTimer = 0;

    std::mutex sTrainerMutex;
    std::unordered_map<Trainer::Trainer const*, uint32> sTrainerSkills;

    // Millable herb -> the pigments milling it gives (milling_loot_template, through its reference loot).
    std::once_flag sMillOnce;
    std::unordered_map<uint32, std::vector<uint32>> sMillPigments;

    void LoadMill()
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT m.Entry, m.Item FROM milling_loot_template m WHERE m.Reference = 0 "
            "UNION SELECT m.Entry, r.Item FROM milling_loot_template m "
            "JOIN reference_loot_template r ON r.Entry = m.Reference WHERE m.Reference <> 0 AND r.Reference = 0");
        if (!result)
            return;
        do
        {
            Field* fields = result->Fetch();
            uint32 herb = fields[0].Get<uint32>();
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(herb);
            if (proto && proto->HasFlag(ITEM_FLAG_IS_MILLABLE))
                sMillPigments[herb].push_back(fields[1].Get<uint32>());
        } while (result->NextRow());
        LOG_INFO("playerbots", ">> Playerbots professions: {} millable herbs", sMillPigments.size());
    }

    std::unordered_map<uint32, std::vector<uint32>> const& MillPigments()
    {
        std::call_once(sMillOnce, LoadMill);
        return sMillPigments;
    }

    // Whether the bot can mill the herb now: it knows Milling and has the skill for it.
    bool CanMill(Player* bot, uint32 herb)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(herb);
        return sMill && bot->HasSkill(SKILL_INSCRIPTION) && bot->HasSpell(BotProfessions::SPELL_MILLING) && proto &&
               proto->HasFlag(ITEM_FLAG_IS_MILLABLE) &&
               proto->RequiredSkillRank <= bot->GetSkillValue(SKILL_INSCRIPTION);
    }

    // The items a known recipe makes.
    void Products(SpellInfo const* spell, std::vector<uint32>& out)
    {
        out.clear();
        for (SpellEffectInfo const& effect : spell->Effects)
            if (effect.Effect == SPELL_EFFECT_CREATE_ITEM && effect.ItemType)
                out.push_back(effect.ItemType);
    }

    template <typename Fn>
    void ForEachKnownRecipe(Player* bot, Fn&& fn)
    {
        for (auto const& [spellId, known] : bot->GetSpellMap())
        {
            if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
                continue;
            if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId))
                if (spell->HasEffect(SPELL_EFFECT_CREATE_ITEM))
                    fn(spell);
        }
    }

    // Items with the best chance of a recipe that lacks them, plus, one step up, the reagents of known recipes
    // that make such an item (the pigment of a missing ink).
    std::unordered_map<uint32, int32> Lacking(Player* bot, std::vector<BotProfessions::Shopping> const& list)
    {
        std::unordered_map<uint32, int32> lacking;
        for (BotProfessions::Shopping const& entry : list)
            for (auto const& [item, count] : entry.missing)
                lacking[item] = std::max(lacking[item], entry.chance);
        std::unordered_map<uint32, int32> upstream;
        std::vector<uint32> products;
        ForEachKnownRecipe(bot,
                           [&](SpellInfo const* spell)
                           {
                               Products(spell, products);
                               for (uint32 product : products)
                               {
                                   auto it = lacking.find(product);
                                   if (it == lacking.end())
                                       continue;
                                   for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                                       if (spell->Reagent[i] > 0)
                                       {
                                           int32& chance = upstream[static_cast<uint32>(spell->Reagent[i])];
                                           chance = std::max(chance, it->second);
                                       }
                               }
                           });
        for (auto const& [item, chance] : upstream)
            lacking[item] = std::max(lacking[item], chance);
        return lacking;
    }

    int32 ClassColumn(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR: return 0;
            case CLASS_PALADIN: return 1;
            case CLASS_HUNTER: return 2;
            case CLASS_ROGUE: return 3;
            case CLASS_PRIEST: return 4;
            case CLASS_DEATH_KNIGHT: return 5;
            case CLASS_SHAMAN: return 6;
            case CLASS_MAGE: return 7;
            case CLASS_WARLOCK: return 8;
            case CLASS_DRUID: return 9;
            default: return -1;
        }
    }

    bool IsPrimary(uint32 skill) { return std::find(PRIMARY.begin(), PRIMARY.end(), skill) != PRIMARY.end(); }

    // How much rarer than wanted a profession is among the bots: above 1 when there are too few of it.
    float Scarcity(uint32 skill)
    {
        uint32 bots = sBots.load(std::memory_order_relaxed);
        size_t index = std::find(PRIMARY.begin(), PRIMARY.end(), skill) - PRIMARY.begin();
        float share = bots ? static_cast<float>(sCounts[index].load(std::memory_order_relaxed)) / bots : 0.0f;
        // Each bot has two of eleven, so an even spread is 2/11 per profession; alchemy and enchanting have floors.
        float target = 2.0f / PRIMARY.size();
        if (skill == SKILL_ALCHEMY)
            target = std::max(target, sAlchemyFloor);
        else if (skill == SKILL_ENCHANTING)
            target = std::max(target, sEnchantingFloor);
        return std::clamp(target / std::max(share, 0.01f), SCARCITY_MIN, SCARCITY_MAX);
    }

    // The spell a trainer entry teaches: the entry itself, or the spell a "learn" wrapper casts.
    uint32 Taught(uint32 spellId)
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
            for (SpellEffectInfo const& effect : info->Effects)
                if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell)
                    return effect.TriggerSpell;
        return spellId;
    }

    // The profession a spell belongs to, and whether it is a rank (Apprentice, Journeyman ...) of it.
    uint32 SpellSkill(uint32 spellId, bool& rank)
    {
        rank = false;
        if (SpellLearnSkillNode const* learn = sSpellMgr->GetSpellLearnSkill(spellId))
        {
            rank = true;
            return learn->skill;
        }
        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto it = bounds.first; it != bounds.second; ++it)
            if (IsPrimary(it->second->SkillLine))
                return it->second->SkillLine;
        return 0;
    }

    struct Offer
    {
        Trainer::Spell const* spell;
        uint32 taught;
        bool rank;
        int32 chance;
    };

    // What this trainer would teach the bot for `skill` that is worth paying for.
    std::vector<Offer> Offers(Player* bot, Trainer::Trainer const* trainer, uint32 skill)
    {
        std::vector<Offer> offers;
        bool known = bot->HasSkill(skill);
        if (!known)
        {
            std::vector<uint32> wanted = BotProfessions::Wanted(bot);
            if (std::find(wanted.begin(), wanted.end(), skill) == wanted.end())
                return offers;
        }
        for (Trainer::Spell const& spell : trainer->GetSpells())
        {
            uint32 taught = Taught(spell.SpellId);
            bool rank = false;
            if (SpellSkill(taught, rank) != skill || !trainer->CanTeachSpell(bot, &spell))
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(taught);
            int32 chance = rank ? 100 : (info ? BotProfessions::SkillUpChance(bot, info) : 0);
            // Without the profession only its first rank; with it, ranks and recipes that still teach something.
            if ((!known && !rank) || chance <= 0)
                continue;
            offers.push_back({&spell, taught, rank, chance});
        }
        return offers;
    }
}

void BotProfessions::LoadConfig()
{
    sEnabled = sConfigMgr->GetOption<bool>("Playerbots.Professions.Enable", false);
    sMinLevel = sConfigMgr->GetOption<uint32>("Playerbots.Professions.MinLevel", 5);
    sAlchemyFloor = sConfigMgr->GetOption<float>("Playerbots.Professions.AlchemyFloor", 0.15f);
    sEnchantingFloor = sConfigMgr->GetOption<float>("Playerbots.Professions.EnchantingFloor", 0.12f);
    sGathering = sConfigMgr->GetOption<bool>("Playerbots.Professions.Gather", true);
    sGatherRange = std::clamp(sConfigMgr->GetOption<float>("Playerbots.Professions.GatherRange", 70.0f), 10.0f, 100.0f);
    sGatherCooldown = sConfigMgr->GetOption<uint32>("Playerbots.Professions.GatherCooldown", 60);
    sGatherTracking = sConfigMgr->GetOption<bool>("Playerbots.Professions.GatherTracking", true);
    sDisenchant = sConfigMgr->GetOption<bool>("Playerbots.Professions.Disenchant", true);
    sMill = sConfigMgr->GetOption<bool>("Playerbots.Professions.Mill", true);
    sToolReservePct = std::min<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Professions.ToolReservePct", 50), 100);
}

bool BotProfessions::Enabled() { return sEnabled; }

bool BotProfessions::Gathering() { return sGathering; }

float BotProfessions::GatherRange() { return sGatherRange; }

uint32 BotProfessions::GatherCooldown() { return sGatherCooldown; }

bool BotProfessions::GatherTracking() { return sGatherTracking; }

bool BotProfessions::Disenchanting() { return sEnabled && sDisenchant; }

bool BotProfessions::Milling() { return sEnabled && sMill; }

uint32 BotProfessions::ToolReservePct() { return sToolReservePct; }

bool BotProfessions::MillUseful(Player* bot, uint32 herb)
{
    if (!sEnabled || !CanMill(bot, herb))
        return false;
    auto const& mill = MillPigments();
    auto it = mill.find(herb);
    if (it == mill.end())
        return false;
    // Pigment one of the bot's recipes takes (an ink; scrolls take the ink).
    std::unordered_set<uint32> reagents;
    ForEachKnownRecipe(bot,
                       [&](SpellInfo const* spell)
                       {
                           for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                               if (spell->Reagent[i] > 0)
                                   reagents.insert(static_cast<uint32>(spell->Reagent[i]));
                       });
    return std::any_of(it->second.begin(), it->second.end(),
                       [&](uint32 pigment) { return reagents.count(pigment) > 0; });
}

void BotProfessions::UpdateWorld(uint32 diff)
{
    if (!sEnabled)
        return;
    if (sRecountTimer > diff && sBots.load(std::memory_order_relaxed))
    {
        sRecountTimer -= diff;
        return;
    }
    sRecountTimer = RECOUNT_MS;

    // World thread, between map updates: no map thread is changing a player's skills now.
    std::array<uint32, PRIMARY.size()> counts{};
    uint32 bots = 0;
    std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
    {
        if (!player || !player->IsInWorld() || !sRandomPlayerbotMgr.IsRandomBot(player))
            continue;
        ++bots;
        for (size_t i = 0; i < PRIMARY.size(); ++i)
            if (player->HasSkill(PRIMARY[i]))
                ++counts[i];
    }
    for (size_t i = 0; i < PRIMARY.size(); ++i)
        sCounts[i].store(counts[i], std::memory_order_relaxed);
    sBots.store(std::max<uint32>(bots, 1), std::memory_order_relaxed);
}

std::vector<uint32> BotProfessions::Wanted(Player* bot)
{
    std::vector<uint32> wanted;
    int32 column = ClassColumn(bot->getClass());
    if (!sEnabled || column < 0 || bot->GetLevel() < sMinLevel || !bot->GetFreePrimaryProfessionPoints())
        return wanted;

    std::vector<uint32> known;
    for (uint32 skill : PRIMARY)
        if (bot->HasSkill(skill))
            known.push_back(skill);
    if (known.size() >= 2)
        return wanted;

    // Weighted draw, seeded by the bot so it keeps wanting the same thing from one check to the next.
    std::vector<std::pair<std::vector<uint32>, float>> options;
    for (Pair const& pair : PAIRS)
    {
        float fit = pair.fit[column];
        if (known.empty())
            options.push_back({{pair.first, pair.second}, fit * Scarcity(pair.first) * Scarcity(pair.second)});
        else if (pair.first == known[0])
            options.push_back({{pair.second}, fit * Scarcity(pair.second)});
        else if (pair.second == known[0])
            options.push_back({{pair.first}, fit * Scarcity(pair.first)});
    }
    float total = 0.0f;
    for (auto const& option : options)
        total += option.second;
    if (total <= 0.0f)
        return wanted;

    std::mt19937 rng(bot->GetGUID().GetCounter() * 2654435761u + static_cast<uint32>(known.size()));
    float roll = std::uniform_real_distribution<float>(0.0f, total)(rng);
    for (auto const& [skills, weight] : options)
    {
        roll -= weight;
        if (roll <= 0.0f)
            return skills;
    }
    return options.back().first;
}

uint32 BotProfessions::TrainerSkill(Trainer::Trainer const* trainer)
{
    std::lock_guard<std::mutex> lock(sTrainerMutex);
    auto it = sTrainerSkills.find(trainer);
    if (it != sTrainerSkills.end())
        return it->second;

    std::unordered_map<uint32, uint32> votes;
    for (Trainer::Spell const& spell : trainer->GetSpells())
    {
        bool rank = false;
        if (uint32 skill = SpellSkill(Taught(spell.SpellId), rank))
            ++votes[skill];
    }
    uint32 best = 0;
    uint32 bestVotes = 0;
    for (auto const& [skill, count] : votes)
        if (count > bestVotes)
        {
            best = skill;
            bestVotes = count;
        }
    sTrainerSkills[trainer] = best;
    return best;
}

uint32 BotProfessions::Worthwhile(Player* bot, Trainer::Trainer const* trainer, uint32 skill, uint32& cost,
                                  uint32& ranks)
{
    cost = 0;
    ranks = 0;
    int64 spare = Spare(bot);
    uint32 count = 0;
    for (Offer const& offer : Offers(bot, trainer, skill))
    {
        if (offer.spell->MoneyCost > spare)
            continue;
        ++count;
        ranks += offer.rank ? 1 : 0;
        cost += offer.spell->MoneyCost;
    }
    return count;
}

uint32 BotProfessions::Learn(Player* bot, Creature* npc, Trainer::Trainer* trainer, uint32 skill)
{
    uint32 learned = 0;
    // A new rank opens recipes that need it, so look twice.
    for (uint32 pass = 0; pass < 2; ++pass)
    {
        std::vector<Offer> offers = Offers(bot, trainer, skill);
        std::sort(offers.begin(), offers.end(),
                  [](Offer const& a, Offer const& b)
                  {
                      if (a.rank != b.rank)
                          return a.rank;
                      if (a.chance != b.chance)
                          return a.chance > b.chance;
                      return a.spell->MoneyCost < b.spell->MoneyCost;
                  });
        uint32 before = learned;
        for (Offer const& offer : offers)
        {
            if (offer.spell->MoneyCost > Spare(bot) || bot->HasSpell(offer.taught))
                continue;
            trainer->TeachSpell(npc, bot, offer.spell->SpellId);
            if (bot->HasSpell(offer.taught))
                ++learned;
        }
        if (learned == before)
            break;
    }
    return learned;
}

int32 BotProfessions::SkillUpChance(Player* bot, SpellInfo const* spell)
{
    SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spell->Id);
    for (auto it = bounds.first; it != bounds.second; ++it)
    {
        SkillLineAbilityEntry const* ability = it->second;
        if (!IsPrimary(ability->SkillLine) || !ability->TrivialSkillLineRankHigh)
            continue;
        uint32 value = bot->HasSkill(ability->SkillLine) ? bot->GetSkillValue(ability->SkillLine) : 1;
        uint32 grey = ability->TrivialSkillLineRankHigh;
        uint32 yellow = ability->TrivialSkillLineRankLow;
        // Player::UpdateCraftSkill's curve with the default 100% orange and 0% grey.
        if (grey <= yellow)
            return value < grey ? 100 : 0;
        if (value <= yellow)
            return 100;
        if (value >= grey)
            return 0;
        return static_cast<int32>((grey - value) * 100 / (grey - yellow));
    }
    return 0;
}

int64 BotProfessions::Spare(Player* bot, uint32 reservePct)
{
    // The same reserve BotAuction keeps: a player keeps some money back for repairs and food.
    int64 reserve = static_cast<int64>(bot->GetLevel()) * bot->GetLevel() * 10 * reservePct / 100;
    return std::max<int64>(0, static_cast<int64>(bot->GetMoney()) - reserve);
}

std::vector<BotProfessions::Shopping> BotProfessions::ShoppingList(Player* bot, uint32 crafts)
{
    std::vector<Shopping> list;
    for (auto const& [spellId, known] : bot->GetSpellMap())
    {
        if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
            continue;
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
        if (!spell || !(spell->HasEffect(SPELL_EFFECT_CREATE_ITEM) || spell->HasEffect(SPELL_EFFECT_ENCHANT_ITEM)))
            continue;
        int32 chance = SkillUpChance(bot, spell);
        if (chance <= 0)
            continue;

        bool tools = true;
        for (uint32 i = 0; i < 2; ++i)
            if ((spell->Totem[i] && !bot->HasItemCount(spell->Totem[i], 1)) ||
                (spell->TotemCategory[i] && !bot->HasItemTotemCategory(spell->TotemCategory[i])))
                tools = false;
        if (!tools)
            continue;

        Shopping entry;
        entry.spellId = spellId;
        entry.chance = chance;
        for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            if (spell->Reagent[i] <= 0 || !spell->ReagentCount[i])
                continue;
            uint32 item = static_cast<uint32>(spell->Reagent[i]);
            uint32 need = spell->ReagentCount[i] * crafts;
            uint32 have = bot->GetItemCount(item);
            if (have < need)
                entry.missing.push_back({item, need - have});
        }
        if (!entry.missing.empty())
            list.push_back(std::move(entry));
    }

    // A scribe short of pigment, or of the ink made from it: herbs to mill, in lots of MILL_COUNT, about one
    // Milling per two crafts (a Milling gives two to four pigments).
    if (sMill && bot->HasSkill(SKILL_INSCRIPTION) && bot->HasSpell(SPELL_MILLING) && !list.empty())
    {
        std::unordered_map<uint32, int32> lacking = Lacking(bot, list);
        std::vector<Shopping> mills;
        for (auto const& [herb, pigments] : MillPigments())
        {
            int32 chance = 0;
            for (uint32 pigment : pigments)
            {
                auto it = lacking.find(pigment);
                if (it != lacking.end())
                    chance = std::max(chance, it->second);
            }
            if (chance <= 0 || !CanMill(bot, herb))
                continue;
            uint32 need = MILL_COUNT * std::max<uint32>(1, (crafts + 1) / 2);
            uint32 have = bot->GetItemCount(herb);
            if (have >= need)
                continue;
            Shopping entry;
            entry.spellId = SPELL_MILLING;
            entry.chance = chance;
            entry.missing.push_back({herb, need - have});
            mills.push_back(std::move(entry));
        }
        list.insert(list.end(), mills.begin(), mills.end());
    }
    std::stable_sort(list.begin(), list.end(),
                     [](Shopping const& a, Shopping const& b) { return a.chance > b.chance; });
    return list;
}
