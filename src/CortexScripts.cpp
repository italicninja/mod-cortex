/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include "Cortex.h"
#include "CortexBody.h"
#include "CortexActions.h"
#include "CortexMemory.h"
#include "CortexNeeds.h"
#include "CortexQuests.h"
#include "CortexCombatStrategy.h"
#include "CortexStrategy.h"
#include "Engine.h"
#include "Group.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"

namespace
{
    class CortexStrategyContext : public NamedObjectContext<Strategy>
    {
    public:
        CortexStrategyContext()
        {
            creators["cortex"] = [](PlayerbotAI* botAI) { return new CortexStrategy(botAI); };
            creators["cortex combat"] = [](PlayerbotAI* botAI) { return new CortexCombatStrategy(botAI); };
        }
    };

    class CortexActionContext : public NamedObjectContext<Action>
    {
    public:
        CortexActionContext()
        {
            creators["cortex think"] = [](PlayerbotAI* botAI) { return new CortexThinkAction(botAI); };
            creators["cortex step"] = [](PlayerbotAI* botAI) { return new CortexStepAction(botAI); };
            creators["cortex ride"] = [](PlayerbotAI* botAI) { return new CortexRideAction(botAI); };
            creators["cortex attack"] = [](PlayerbotAI* botAI) { return new CortexAttackAction(botAI); };
            creators["cortex tame"] = [](PlayerbotAI* botAI) { return new CortexTameAction(botAI); };
            creators["cortex gather"] = [](PlayerbotAI* botAI) { return new CortexGatherAction(botAI); };
            creators["cortex needs"] = [](PlayerbotAI* botAI) { return new CortexNeedsAction(botAI); };
            creators["cortex regroup"] = [](PlayerbotAI* botAI) { return new CortexRegroupAction(botAI); };
        }
    };
}

void Cortex::BuildStrategyContexts(SharedNamedObjectContextList<Strategy>& contexts)
{
    contexts.Add(new CortexStrategyContext());
}

void Cortex::BuildActionContexts(SharedNamedObjectContextList<Action>& contexts)
{
    contexts.Add(new CortexActionContext());
}

// A random bot in the Cortex arm gets the "cortex" strategy; acting, it drops "grind" and "new rpg" for it. Same
// bots as the body's own random-bot block: not in a battleground, alone or leading its group.
void Cortex::AddNonCombatStrategies(Player* player, PlayerbotAI* /*botAI*/, Engine* engine)
{
    if (!sRandomPlayerbotMgr.IsRandomBot(player) || player->InBattleground())
        return;
    if (player->GetGroup() && player->GetGroup()->GetLeaderGUID() != player->GetGUID())
        return;
    if (!Cortex::InArm(player))
        return;
    if (Cortex::Acting())
    {
        engine->removeStrategy("grind", false);
        engine->removeStrategy("new rpg", false);
    }
    engine->addStrategy("cortex", false);
    Cortex::WatchActions(player, engine);
}

// The arm's combat engine gets "cortex combat" (wands), for the same bots as above.
void Cortex::AddCombatStrategies(Player* player, PlayerbotAI* /*botAI*/, Engine* engine)
{
    if (!sRandomPlayerbotMgr.IsRandomBot(player) || player->InBattleground() || !Cortex::InArm(player) ||
        !Cortex::Acting())
        return;
    if (player->GetGroup() && player->GetGroup()->GetLeaderGUID() != player->GetGUID())
        return;
    engine->addStrategy("cortex combat", false);
}

// A quest the bot gave up on (CortexQuests::Shelve) waits until it has outlevelled it. Class quests are never
// given up on, and one dropped before that rule is taken again.
bool Cortex::AllowsQuest(Player* bot, Quest const* quest)
{
    if (!Cortex::Acting() || CortexQuests::IsClassQuest(quest))
        return true;
    CortexMemory* memory = Cortex::Memory(bot);
    return !memory || !memory->GaveUp(quest->GetQuestId(), static_cast<uint8>(bot->GetLevel()));
}

bool Cortex::TelemetryState(Player const* bot, std::string& state)
{
    char const* goal = Cortex::GoalName(bot);
    if (!goal)
        return false;
    state = std::string("c:") + goal;
    return true;
}

namespace
{
    class CortexPlayerScript : public PlayerScript
    {
    public:
        CortexPlayerScript()
            : PlayerScript("CortexPlayerScript", {PLAYERHOOK_ON_CREATURE_KILL_CREDIT, PLAYERHOOK_ON_LOOT_ITEM})
        {
        }

        void OnPlayerCreatureKillCredit(Player* player, Creature* killed) override
        {
            Cortex::OnKillCredit(player, killed);
        }

        void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid lootGuid) override
        {
            Cortex::OnLoot(player, item, count, lootGuid);
        }
    };

    class CortexWorldScript : public WorldScript
    {
    public:
        CortexWorldScript()
            : WorldScript("CortexWorldScript",
                          {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN})
        {
        }

        void OnAfterConfigLoad(bool /*reload*/) override { Cortex::LoadConfig(); }

        // Loads memory for bots that asked for it.
        void OnUpdate(uint32 diff) override { Cortex::UpdateWorld(diff); }

        // Runs before the body logs the bots out: Addmod_cortexScripts registers this before the body's scripts.
        void OnShutdown() override { Cortex::SaveAll(); }
    };
}

void AddCortexScripts()
{
    new CortexPlayerScript();
    new CortexWorldScript();
}
