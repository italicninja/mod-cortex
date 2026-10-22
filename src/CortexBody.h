/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#ifndef MOD_CORTEX_BODY_H
#define MOD_CORTEX_BODY_H

#include <string>

#include "Cortex.h"
#include "NamedObjectContext.h"

class Action;
class Engine;
class Player;
class PlayerbotAI;
class Quest;
class Strategy;

// What the bot body (src/Body, ported from mod-playerbots) calls in the brain. These were playerbots' extension hooks
// (PlayerbotsExtensions, bots 0053/0066/0069) until the body moved into mod-cortex; now they are plain calls.
// Also called from the body: Cortex::Update (every bot update), Cortex::InArm (telemetry), Cortex::RunsToCorpse
// (revive and the random bot manager), declared in Cortex.h.
namespace Cortex
{
    // Cortex's strategies ("cortex", "cortex combat") and actions, after the body's own contexts.
    void BuildStrategyContexts(SharedNamedObjectContextList<Strategy>& contexts);
    void BuildActionContexts(SharedNamedObjectContextList<Action>& contexts);

    // End of AiFactory's default non-combat / combat strategies: an arm bot gets Cortex's.
    void AddNonCombatStrategies(Player* player, PlayerbotAI* botAI, Engine* engine);
    void AddCombatStrategies(Player* player, PlayerbotAI* botAI, Engine* engine);

    // False for a quest the bot gave up on (new rpg skips it).
    bool AllowsQuest(Player* bot, Quest const* quest);

    // The telemetry "state" for an arm bot ("c:<goal>"); false if Cortex has nothing to say.
    bool TelemetryState(Player const* bot, std::string& state);
}

#endif
