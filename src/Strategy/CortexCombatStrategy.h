/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#ifndef MOD_CORTEX_CORTEXCOMBATSTRATEGY_H
#define MOD_CORTEX_CORTEXCOMBATSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

// Combat strategy of the Cortex arm (with Cortex.Act and Cortex.Wand): a priest, mage or warlock with a wand saves
// its mana the way a levelling player does (CortexWandMultiplier).
class CortexCombatStrategy : public Strategy
{
public:
    CortexCombatStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "cortex combat"; }
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

#endif
