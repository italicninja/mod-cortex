/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 * Originally written as part of the wowproject patches to mod-playerbots.
 */

#ifndef MOD_CORTEX_CORTEXSTRATEGY_H
#define MOD_CORTEX_CORTEXSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

// Non-combat strategy of Cortex (docs/bot-ai-overhaul.md §7b). With Cortex.Act off it
// is empty and the arm still runs "new rpg" (A/A). With it on, the arm runs this instead of "new rpg" and "grind".
class CortexStrategy : public Strategy
{
public:
    CortexStrategy(PlayerbotAI* botAI);

    std::string const getName() override { return "cortex"; }
    std::vector<NextAction> getDefaultActions() override;
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

#endif
