/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActionContext.h"
#include "AiObjectContext.h"
#include "PlayerbotsExtensions.h"
#include "Aq20ActionContext.h"
#include "BWLActionContext.h"
#include "ChatActionContext.h"
#include "MCActionContext.h"
#include "OnyActionContext.h"
#include "WorldPacketActionContext.h"

void AiObjectContext::BuildSharedActionContexts(
    SharedNamedObjectContextList<Action>& actionContexts)
{
    actionContexts.Add(new ActionContext());
    actionContexts.Add(new ChatActionContext());
    actionContexts.Add(new WorldPacketActionContext());
    actionContexts.Add(new RaidAq20ActionContext());
    actionContexts.Add(new RaidMcActionContext());
    actionContexts.Add(new RaidBwlActionContext());
    actionContexts.Add(new RaidOnyxiaActionContext());

    // Other modules' contexts (PlayerbotsExtensions), after the built-in ones.
    PlayerbotsExtensions::BuildActionContexts(actionContexts);
}
