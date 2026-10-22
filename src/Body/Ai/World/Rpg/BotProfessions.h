/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTPROFESSIONS_H
#define PLAYERBOTS_BOTPROFESSIONS_H

#include <utility>
#include <vector>

#include "Define.h"

class Creature;
class Player;
class SpellInfo;

namespace Trainer
{
    class Trainer;
}

// Professions for random bots (Playerbots.Professions, docs/professions.md): a gathering + crafting pair chosen by
// class fit and by how rare each profession is among the bots, learned and trained at real trainers for real money,
// levelled by gathering and crafting.
namespace BotProfessions
{
    void LoadConfig();
    bool Enabled();
    // Node detours (Playerbots.Professions.Gather): herbalists and miners leave their path for a node they can
    // gather within GatherRange yards, at most once every GatherCooldown seconds.
    bool Gathering();
    float GatherRange();
    uint32 GatherCooldown();
    // Resource tracking (Playerbots.Professions.GatherTracking): herbalists and miners keep Find Herbs or Find
    // Minerals up, and nodes of the tracked kind count as seen without line of sight, out to the visibility range.
    bool GatherTracking();
    // World thread: recounts which professions the online random bots have, every 10 minutes.
    void UpdateWorld(uint32 diff);

    // Primary professions the bot wants and hasn't learned yet (0, 1 or 2 skill ids), once it is old enough.
    std::vector<uint32> Wanted(Player* bot);
    // The profession a tradeskill trainer teaches (most common skill line of its spells), or 0.
    uint32 TrainerSkill(Trainer::Trainer const* trainer);
    // Whether a trainer of this profession has something worth the trip: the profession itself when wanted, the
    // next rank, or recipes that still give skill-ups, that it can afford. `cost` is what all of it costs, `ranks`
    // how many of them are the profession or a rank of it.
    uint32 Worthwhile(Player* bot, Trainer::Trainer const* trainer, uint32 skill, uint32& cost, uint32& ranks);
    // At the trainer: learn the profession or the next rank, then skill-up recipes, cheapest first, within what
    // the bot can spare. Returns the number of spells learned.
    uint32 Learn(Player* bot, Creature* npc, Trainer::Trainer* trainer, uint32 skill);

    // Chance in percent that casting this recipe raises its skill now (0 = grey or not a recipe).
    int32 SkillUpChance(Player* bot, SpellInfo const* spell);
    // Copper the bot can spend on its professions without touching its reserve; `reservePct` of the reserve kept.
    int64 Spare(Player* bot, uint32 reservePct = 100);
    // Share of the reserve kept when buying a missing tool or the reagents of a recipe that makes one
    // (Playerbots.Professions.ToolReservePct): a rod or a pick is worth dipping into it.
    uint32 ToolReservePct();

    // Disenchanting and milling (Playerbots.Professions.Disenchant / .Mill): enchanters disenchant green and blue
    // gear they have no use for, scribes mill herbs, so their recipes have dust, essence and pigment.
    constexpr uint32 SPELL_DISENCHANT = 13262;
    constexpr uint32 SPELL_MILLING = 51005;
    constexpr uint32 MILL_COUNT = 5;  // herbs one Milling cast takes, all from one stack
    bool Disenchanting();
    bool Milling();
    // Whether the herb is one the bot can mill now and whose pigment a recipe it knows uses (directly or through
    // the ink made from it).
    bool MillUseful(Player* bot, uint32 herb);

    // Recipes the bot knows that would raise a skill and that it has the tools for, best chance first, with the
    // reagents it lacks to make each one `crafts` times: what to look for at the auction house. A scribe lacking
    // pigment (or the ink made from it) also gets SPELL_MILLING entries, one per herb it could mill for it, that
    // lack the herbs (in MILL_COUNT lots).
    struct Shopping
    {
        uint32 spellId = 0;
        int32 chance = 0;
        std::vector<std::pair<uint32, uint32>> missing;  // item, count
    };
    std::vector<Shopping> ShoppingList(Player* bot, uint32 crafts);
}

#endif
