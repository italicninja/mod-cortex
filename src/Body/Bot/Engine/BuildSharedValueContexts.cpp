/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AiObjectContext.h"
#include "PlayerbotsExtensions.h"
#include "ValueContext.h"

void AiObjectContext::BuildSharedValueContexts(
    SharedNamedObjectContextList<UntypedValue>& valueContexts)
{
    valueContexts.Add(new ValueContext());

    // Other modules' contexts (PlayerbotsExtensions), after the built-in ones.
    PlayerbotsExtensions::BuildValueContexts(valueContexts);
}
