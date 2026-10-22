/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AiObjectContext.h"
#include "PlayerbotsExtensions.h"
#include "Aq20TriggerContext.h"
#include "BWLTriggerContext.h"
#include "ChatTriggerContext.h"
#include "MCTriggerContext.h"
#include "OnyTriggerContext.h"
#include "TriggerContext.h"
#include "WorldPacketTriggerContext.h"

void AiObjectContext::BuildSharedTriggerContexts(
    SharedNamedObjectContextList<Trigger>& triggerContexts)
{
    triggerContexts.Add(new TriggerContext());
    triggerContexts.Add(new ChatTriggerContext());
    triggerContexts.Add(new WorldPacketTriggerContext());
    triggerContexts.Add(new RaidAq20TriggerContext());
    triggerContexts.Add(new RaidMcTriggerContext());
    triggerContexts.Add(new RaidBwlTriggerContext());
    triggerContexts.Add(new RaidOnyxiaTriggerContext());

    // Other modules' contexts (PlayerbotsExtensions), after the built-in ones.
    PlayerbotsExtensions::BuildTriggerContexts(triggerContexts);
}
