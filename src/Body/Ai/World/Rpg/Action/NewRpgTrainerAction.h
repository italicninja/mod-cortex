/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGTRAINERACTION_H
#define PLAYERBOTS_NEWRPGTRAINERACTION_H

#include "NewRpgBaseAction.h"

// Class trainer trip (Playerbots.NewRpgTrainer): with AutoLearnTrainerSpells off, a random bot that can afford new
// spells or ranks walks to the nearest friendly class trainer and learns them (paid, through "trainer").
class NewRpgTrainerAction : public NewRpgBaseAction
{
public:
    NewRpgTrainerAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg trainer") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

#endif
