/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 * Originally written as part of the wowproject patches to mod-playerbots.
 */

#include "CortexStrategy.h"

#include "Cortex.h"
#include "Player.h"
#include "PlayerbotAI.h"

CortexStrategy::CortexStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

void CortexStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    if (!Cortex::Acting())
        return;

    // A mage makes its own food and water between pulls. Playerbots has the conjure actions and the "no food" / "no
    // drink" triggers but no strategy that joins them, so mages rested without either: 127 of 265 Cortex mage rests on
    // 10-20 ate and drank nothing (57 s on average, against ~25 s with), and needs leave food to mages.
    if (botAI->GetBot()->getClass() == CLASS_MAGE)
    {
        triggers.push_back(new TriggerNode("no drink", {NextAction("conjure water", 11.65f)}));
        triggers.push_back(new TriggerNode("no food", {NextAction("conjure food", 11.64f)}));
    }
}

std::vector<NextAction> CortexStrategy::getDefaultActions()
{
    if (!Cortex::Acting())
        return {};

    // Needs first: "cortex needs" sends them to places memory knows (B2c) and hands them to new-rpg's upkeep,
    // trainer and auction actions below it when memory has none. Then a hunter's taming, node detours,
    // re-planning, the plan's step. The step sits below grind's 4.0 slot, so looting (6-8) and eating (4.x) still
    // come first; a trip to a remembered node sits just above the step. A bot waiting at a pier, aboard or casting a teleport sees that
    // through ("cortex ride") before any errand walks it off.
    // Back from a death, it steps away from the hostiles round its corpse before sitting down ("cortex regroup").
    return {
        NextAction("cortex regroup", 12.1f),
        NextAction("new rpg recover", 12.0f),
        NextAction("cortex ride", 11.9f),
        NextAction("new rpg gear check", 11.6f),
        NextAction("cortex needs", 11.57f),
        NextAction("new rpg trainer", 11.55f),
        NextAction("new rpg upkeep", 11.5f),
        NextAction("new rpg auction", 11.4f),
        NextAction("bot craft", 11.3f),
        NextAction("cortex tame", 11.2f),
        NextAction("new rpg gather", 11.1f),
        NextAction("cortex think", 11.0f),
        NextAction("cortex gather", 3.5f),
        NextAction("cortex step", 3.0f)
    };
}
