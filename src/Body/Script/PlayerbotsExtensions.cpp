/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotsExtensions.h"

#include <vector>

namespace
{
    // Function-local statics: other modules register from their loaders, whose order against this file's static
    // initialisation isn't defined.
    template <class T>
    std::vector<PlayerbotsExtensions::ContextFactory<T>>& Factories()
    {
        static std::vector<PlayerbotsExtensions::ContextFactory<T>> factories;
        return factories;
    }

    std::vector<PlayerbotsExtensions::StrategiesHook>& NonCombatHooks()
    {
        static std::vector<PlayerbotsExtensions::StrategiesHook> hooks;
        return hooks;
    }

    std::vector<PlayerbotsExtensions::StrategiesHook>& CombatHooks()
    {
        static std::vector<PlayerbotsExtensions::StrategiesHook> hooks;
        return hooks;
    }

    std::vector<PlayerbotsExtensions::BotUpdateHook>& UpdateHooks()
    {
        static std::vector<PlayerbotsExtensions::BotUpdateHook> hooks;
        return hooks;
    }

    std::vector<PlayerbotsExtensions::QuestFilter>& QuestFilters()
    {
        static std::vector<PlayerbotsExtensions::QuestFilter> filters;
        return filters;
    }

    std::vector<PlayerbotsExtensions::TelemetryProvider>& TelemetryProviders()
    {
        static std::vector<PlayerbotsExtensions::TelemetryProvider> providers;
        return providers;
    }

    std::vector<PlayerbotsExtensions::ChatFilter>& ChatFilters()
    {
        static std::vector<PlayerbotsExtensions::ChatFilter> filters;
        return filters;
    }

    std::vector<PlayerbotsExtensions::CorpseRunFilter>& CorpseRunFilters()
    {
        static std::vector<PlayerbotsExtensions::CorpseRunFilter> filters;
        return filters;
    }

    template <class T>
    void Build(SharedNamedObjectContextList<T>& contexts)
    {
        for (auto const& factory : Factories<T>())
            if (NamedObjectContext<T>* context = factory())
                contexts.Add(context);
    }
}

void PlayerbotsExtensions::AddStrategyContext(ContextFactory<Strategy> factory)
{
    Factories<Strategy>().push_back(std::move(factory));
}

void PlayerbotsExtensions::AddActionContext(ContextFactory<Action> factory)
{
    Factories<Action>().push_back(std::move(factory));
}

void PlayerbotsExtensions::AddTriggerContext(ContextFactory<Trigger> factory)
{
    Factories<Trigger>().push_back(std::move(factory));
}

void PlayerbotsExtensions::AddValueContext(ContextFactory<UntypedValue> factory)
{
    Factories<UntypedValue>().push_back(std::move(factory));
}

void PlayerbotsExtensions::OnNonCombatStrategies(StrategiesHook hook) { NonCombatHooks().push_back(std::move(hook)); }

void PlayerbotsExtensions::OnCombatStrategies(StrategiesHook hook) { CombatHooks().push_back(std::move(hook)); }

void PlayerbotsExtensions::OnBotUpdate(BotUpdateHook hook) { UpdateHooks().push_back(std::move(hook)); }

void PlayerbotsExtensions::AddQuestFilter(QuestFilter filter) { QuestFilters().push_back(std::move(filter)); }

void PlayerbotsExtensions::AddTelemetryProvider(TelemetryProvider provider)
{
    TelemetryProviders().push_back(std::move(provider));
}

void PlayerbotsExtensions::AddChatFilter(ChatFilter filter) { ChatFilters().push_back(std::move(filter)); }

void PlayerbotsExtensions::AddCorpseRunFilter(CorpseRunFilter filter)
{
    CorpseRunFilters().push_back(std::move(filter));
}

void PlayerbotsExtensions::BuildStrategyContexts(SharedNamedObjectContextList<Strategy>& contexts) { Build(contexts); }

void PlayerbotsExtensions::BuildActionContexts(SharedNamedObjectContextList<Action>& contexts) { Build(contexts); }

void PlayerbotsExtensions::BuildTriggerContexts(SharedNamedObjectContextList<Trigger>& contexts) { Build(contexts); }

void PlayerbotsExtensions::BuildValueContexts(SharedNamedObjectContextList<UntypedValue>& contexts) { Build(contexts); }

void PlayerbotsExtensions::NonCombatStrategies(Player* player, PlayerbotAI* botAI, Engine* engine)
{
    for (auto const& hook : NonCombatHooks())
        hook(player, botAI, engine);
}

void PlayerbotsExtensions::CombatStrategies(Player* player, PlayerbotAI* botAI, Engine* engine)
{
    for (auto const& hook : CombatHooks())
        hook(player, botAI, engine);
}

void PlayerbotsExtensions::BotUpdate(PlayerbotAI* botAI)
{
    for (auto const& hook : UpdateHooks())
        hook(botAI);
}

bool PlayerbotsExtensions::AllowsQuest(Player* bot, Quest const* quest)
{
    for (auto const& filter : QuestFilters())
        if (!filter(bot, quest))
            return false;
    return true;
}

bool PlayerbotsExtensions::TelemetryState(Player const* bot, std::string& state)
{
    for (auto const& provider : TelemetryProviders())
        if (provider.state && provider.state(bot, state))
            return true;
    return false;
}

bool PlayerbotsExtensions::TelemetryArm(Player const* bot)
{
    for (auto const& provider : TelemetryProviders())
        if (provider.arm && provider.arm(bot))
            return true;
    return false;
}

bool PlayerbotsExtensions::ClaimsChat(Player* bot, Player* from, uint32 type, std::string const& msg)
{
    for (auto const& filter : ChatFilters())
        if (filter.claims && filter.claims(bot, from, type, msg))
            return true;
    return false;
}

bool PlayerbotsExtensions::SilencesChatReply(Player* bot, uint32 type)
{
    for (auto const& filter : ChatFilters())
        if (filter.silences && filter.silences(bot, type))
            return true;
    return false;
}

bool PlayerbotsExtensions::RunsToCorpse(Player const* bot)
{
    for (auto const& filter : CorpseRunFilters())
        if (filter(bot))
            return true;
    return false;
}
