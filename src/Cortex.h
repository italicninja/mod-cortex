/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 * Originally written as part of the wowproject patches to mod-playerbots.
 */

#ifndef MOD_CORTEX_CORTEX_H
#define MOD_CORTEX_CORTEX_H

#include <string>

#include "ObjectGuid.h"

class CortexMemory;
class Engine;
struct CortexPlan;
class Creature;
class Item;
class Player;
class PlayerbotAI;

// Cortex, the player-like bot AI (docs/bot-ai-overhaul.md): the replacement for the new-rpg strategy, rolled out as
// an A/B test. A random bot is in the Cortex arm when its GUID hashes below Ratio, so the split is stable across
// restarts and resets and the report can compare the arms side by side.
namespace Cortex
{
    void LoadConfig();
    bool Enabled();
    // Random bots only; real players' bots and alt bots never join the arm.
    bool InArm(Player const* bot);
    // Arm bots come back from death like a player (Cortex.CorpseRun, playerbots 0066): corpse run, the nearest
    // healer when the corpse can't be reached, no random-bot revive that moves them somewhere else.
    bool RunsToCorpse(Player const* bot);

    // Per bot, every UpdateAI tick on its map thread: perception into memory (B1), periodic save.
    void Update(PlayerbotAI* botAI);
    // World thread: loads memory requested by Update, logs scan cost.
    void UpdateWorld(uint32 diff);
    // At shutdown, after the world loop (no map thread running): writes every loaded memory synchronously.
    void SaveAll();

    // B2: the Cortex arm drops new-rpg and grind and runs Cortex's own actions.
    bool Acting();
    // Cortex.Regroup: "cortex regroup" after a revive.
    bool Regrouping();
    // Cortex.Wand: casters in the acting arm shoot their wand rather than spend their last mana ("cortex combat").
    bool Wands();
    // Cortex.Revisit: with no "!" known, a bot that has levelled goes back to quest givers it last saw at a lower
    // level, where new quests may have opened up.
    bool Revisit();
    // Hunters in the acting arm tame a pet once they know Tame Beast (Cortex.Tame).
    bool Taming();
    // Herbalists and miners in the acting arm walk to nodes they remember (Cortex.Gather); perception remembers
    // the nodes of a bot's gathering skills whenever Gather is on.
    bool Gathering();
    float GatherTripRange();
    // The acting arm walks back to quest sparkles (chests, sacks, crates) it remembers near the quest's area
    // (Cortex.Sparkles); perception remembers them either way.
    bool Sparkles();
    // The bot's memory and plan, once loaded; null for bots outside the arm. Map thread of the bot only.
    CortexMemory* Memory(Player const* bot);
    CortexPlan* Plan(Player const* bot);
    // Goal name for telemetry rows, or null when the bot isn't acting on Cortex.
    char const* GoalName(Player const* bot);
    // What the bot is about, in a few words for chat (mod-cortex-chat): "working on <quest>, <quest>", "handing in
    // <quest>", "looking for new quests", "grinding mobs"; empty when it isn't acting on Cortex. Reads the plan, so
    // only while the bot's map isn't updating (the world thread's session update, where chat is handled).
    std::string Describe(Player const* bot);

    // Milliseconds the bot has spent fighting, dead, in flight or recovering, and on errands (trainer, vendor,
    // auction, crafting, gathering, taming), summed on every AI update: counters that only grow, for timers that
    // shouldn't run while the bot is away. A timer can't tell from the gap between its own steps: a bot's moves set
    // its next AI check seconds out, so a walking bot's steps are as far apart as a fighting one's.
    uint32 AwayMs(Player const* bot);
    uint32 ErrandMs(Player const* bot);

    // Hooks the bot's non-combat engine so the watchdog (Cortex.Watchdog) knows the last action it ran.
    void WatchActions(Player* bot, Engine* engine);

    void OnKillCredit(Player* player, Creature* killed);
    void OnLoot(Player* player, Item* item, uint32 count, ObjectGuid lootGuid);
}

#endif
