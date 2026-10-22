/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_EXTENSIONS_H
#define PLAYERBOTS_EXTENSIONS_H

#include <functional>
#include <string>

#include "NamedObjectContext.h"

class Action;
class Engine;
class Player;
class PlayerbotAI;
class Quest;
class Strategy;
class Trigger;
class UntypedValue;

// Extension points for other AzerothCore modules that add bot behaviour on top of playerbots without patching it:
// their own strategies, actions, triggers and values, a say in a random bot's default strategies, a per-bot tick,
// a veto on quests, and the telemetry state label. Register from the module's script loader (Add<module>Scripts),
// which runs before playerbots builds its shared contexts and before any bot logs in; the lists are not changed
// after that, so map threads read them without locking.
namespace PlayerbotsExtensions
{
    template <class T>
    using ContextFactory = std::function<NamedObjectContext<T>*()>;

    // Named-object contexts added to every bot's shared lists, after the built-in ones. The factory is called once
    // per shared list (the base list and each class's), and the list owns what it returns.
    void AddStrategyContext(ContextFactory<Strategy> factory);
    void AddActionContext(ContextFactory<Action> factory);
    void AddTriggerContext(ContextFactory<Trigger> factory);
    void AddValueContext(ContextFactory<UntypedValue> factory);

    // Called at the end of AiFactory::AddDefaultNonCombatStrategies, after playerbots has added its own (including
    // the configured RandomBotNonCombatStrategies), to add, remove or swap strategies.
    using StrategiesHook = std::function<void(Player* player, PlayerbotAI* botAI, Engine* engine)>;
    void OnNonCombatStrategies(StrategiesHook hook);
    // The same at the end of AiFactory::AddDefaultCombatStrategies, for the combat engine.
    void OnCombatStrategies(StrategiesHook hook);

    // Called every PlayerbotAI::UpdateAI tick of a bot on its map thread, right after telemetry.
    using BotUpdateHook = std::function<void(PlayerbotAI* botAI)>;
    void OnBotUpdate(BotUpdateHook hook);

    // A quest a giver offers the bot (new rpg quest handling): any filter that returns false turns it down.
    using QuestFilter = std::function<bool(Player* bot, Quest const* quest)>;
    void AddQuestFilter(QuestFilter filter);

    // Telemetry: `state` may set the sample's state label instead of the rpg status (the first provider that
    // returns true wins); `arm` puts the bot's samples in the extension's A/B arm (flag SAMPLE_EXT_ARM).
    struct TelemetryProvider
    {
        std::function<bool(Player const* bot, std::string& state)> state;
        std::function<bool(Player const* bot)> arm;
    };
    void AddTelemetryProvider(TelemetryProvider provider);

    // Chat another module answers itself (mod-cortex-chat). `claims`: a real player's whisper, party or guild
    // message to `bot` is chat, not a command, so playerbots doesn't run it (nor refuse it) for that bot; world
    // thread. `silences`: the bot leaves out its canned reply (ChatReplyAction) to a real player's message of this
    // chat type; the bot's map thread.
    struct ChatFilter
    {
        std::function<bool(Player* bot, Player* from, uint32 type, std::string const& msg)> claims;
        std::function<bool(Player* bot, uint32 type)> silences;
    };
    void AddChatFilter(ChatFilter filter);

    // A random bot that comes back from death the way a player does (mod-cortex): it runs to its corpse, takes the
    // spirit healer at the nearest graveyard only when the corpse can't be reached or isn't safe, and is never
    // resurrected and moved by the random bot manager (its "revive" timer, the "died 5 times" shortcut). Any
    // filter returning true opts the bot in. Called from map threads and the world thread.
    using CorpseRunFilter = std::function<bool(Player const* bot)>;
    void AddCorpseRunFilter(CorpseRunFilter filter);

    // Called by playerbots at the matching points.
    void BuildStrategyContexts(SharedNamedObjectContextList<Strategy>& contexts);
    void BuildActionContexts(SharedNamedObjectContextList<Action>& contexts);
    void BuildTriggerContexts(SharedNamedObjectContextList<Trigger>& contexts);
    void BuildValueContexts(SharedNamedObjectContextList<UntypedValue>& contexts);
    void NonCombatStrategies(Player* player, PlayerbotAI* botAI, Engine* engine);
    void CombatStrategies(Player* player, PlayerbotAI* botAI, Engine* engine);
    void BotUpdate(PlayerbotAI* botAI);
    bool AllowsQuest(Player* bot, Quest const* quest);
    bool TelemetryState(Player const* bot, std::string& state);
    bool TelemetryArm(Player const* bot);
    bool ClaimsChat(Player* bot, Player* from, uint32 type, std::string const& msg);
    bool SilencesChatReply(Player* bot, uint32 type);
    bool RunsToCorpse(Player const* bot);
}

#endif
