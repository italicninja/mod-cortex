/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAIDSTRATEGYCONTEXT_H
#define PLAYERBOTS_RAIDSTRATEGYCONTEXT_H

#include "Aq20Strategy.h"
#include "BWLStrategy.h"
#include "MCStrategy.h"
#include "OnyStrategy.h"
#include "Strategy.h"

class RaidStrategyContext : public NamedObjectContext<Strategy>
{
public:
    RaidStrategyContext() : NamedObjectContext<Strategy>(false, true)
    {
        creators["aq20"] = &RaidStrategyContext::aq20;
        creators["moltencore"] = &RaidStrategyContext::moltencore;
        creators["bwl"] = &RaidStrategyContext::bwl;
        creators["onyxia"] = &RaidStrategyContext::onyxia;
    }

private:
    static Strategy* aq20(PlayerbotAI* botAI) { return new RaidAq20Strategy(botAI); }
    static Strategy* moltencore(PlayerbotAI* botAI) { return new RaidMcStrategy(botAI); }
    static Strategy* bwl(PlayerbotAI* botAI) { return new RaidBwlStrategy(botAI); }
    static Strategy* onyxia(PlayerbotAI* botAI) { return new RaidOnyxiaStrategy(botAI); }
};

#endif
