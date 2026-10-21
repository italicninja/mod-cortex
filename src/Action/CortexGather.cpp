/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include <limits>
#include <list>
#include <string>

#include "BotTelemetry.h"
#include "CellImpl.h"
#include "Cortex.h"
#include "CortexActions.h"
#include "CortexMemory.h"
#include "CortexQuests.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "NewRpgGatherAction.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"

namespace
{
    constexpr uint32 SCAN_MS = 15 * IN_MILLISECONDS;
    constexpr uint32 IDLE_GAP_MS = MINUTE * IN_MILLISECONDS;       // between trips with no quest to work
    constexpr uint32 BUSY_GAP_MS = 10 * MINUTE * IN_MILLISECONDS;  // between trips while questing
    constexpr float BUSY_TRIP_RANGE = 150.0f;
    constexpr float MIN_TRIP = 40.0f;          // closer than this the bot sees whether the node is up: no trip
    constexpr float SIGHT = 60.0f;             // on the way, whether the node is up is checked from this close
    constexpr float ARRIVE = 8.0f;
    constexpr float NODE_SPOT_RANGE = 12.0f;   // a node of that entry this close to the remembered spot is the one
    constexpr uint32 TRIP_MS = 3 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 EMPTY_SKIP_MS = 20 * MINUTE * IN_MILLISECONDS;  // gathered by someone: back after a respawn
    constexpr uint32 FOUND_SKIP_MS = 5 * MINUTE * IN_MILLISECONDS;   // up, and left to the node detour
    constexpr uint32 STUCK_SKIP_MS = 2 * HOUR * IN_MILLISECONDS;
    // Not seen since: the node has likely been gathered and respawned at another point of its spawn pool.
    constexpr uint32 FORGET_SECONDS = 12 * HOUR;
    // A node seen an hour ago counts as this much farther than one seen just now: older memories are more often gone.
    constexpr float AGE_YARDS_PER_HOUR = 75.0f;
    constexpr uint8 MAX_BAG_USE = 80;

    bool Due(uint32 now, uint32 at) { return static_cast<int32>(now - at) >= 0; }

    bool Idle(CortexPlan const& plan) { return plan.goal == CortexGoal::GRIND || plan.goal == CortexGoal::NONE; }
}

bool CortexGatherAction::isUseful()
{
    if (!Cortex::Gathering())
        return false;
    CortexPlan* plan = Cortex::Plan(bot);
    CortexMemory* memory = Cortex::Memory(bot);
    if (!plan || !memory || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() ||
        bot->GetMap()->Instanceable())
        return false;
    if (plan->gatherEntry)
        return true;

    uint32 now = getMSTime();
    if (!Due(now, plan->nextGatherScanMs))
        return false;
    plan->nextGatherScanMs = now + SCAN_MS;
    if (plan->gatherEndMs && getMSTimeDiff(plan->gatherEndMs, now) < (Idle(*plan) ? IDLE_GAP_MS : BUSY_GAP_MS))
        return false;
    if (botAI->rpgInfo.recoverSinceMs || AI_VALUE(uint8, "bag space") > MAX_BAG_USE)
        return false;
    return Pick(*plan, *memory);
}

bool CortexGatherAction::Execute(Event /*event*/)
{
    CortexPlan* plan = Cortex::Plan(bot);
    if (!plan || !plan->gatherEntry)
        return false;

    uint32 now = getMSTime();
    if (bot->GetMapId() != plan->gatherMap)
    {
        End(*plan, nullptr, 0);
        return false;
    }
    if (getMSTimeDiff(plan->gatherSinceMs, now) >= TRIP_MS)
    {
        End(*plan, "trip_timeout", STUCK_SKIP_MS);
        return false;
    }

    // Perception found the node gone since the bot set off (gathered on the way by the node detour, by someone else,
    // or its tracking shows nothing there).
    uint32 unixNow = static_cast<uint32>(GameTime::GetGameTime().count());
    CortexMemory* memory = Cortex::Memory(bot);
    if (memory)
    {
        auto it = memory->Objects().find(plan->gatherKey);
        if (it == memory->Objects().end() || it->second.goneUntil > unixNow)
        {
            End(*plan, "trip_gone", EMPTY_SKIP_MS);
            return false;
        }
    }

    float d = bot->GetExactDist2d(plan->gatherX, plan->gatherY);
    if (d <= SIGHT)
    {
        bool up = NodeThere(*plan);
        // Up when the bot came in sight and gone now: gathered on the way (its loot counts as gather_done) or by
        // someone else.
        if (plan->gatherSeen && !up)
        {
            End(*plan, nullptr, EMPTY_SKIP_MS);
            return false;
        }
        plan->gatherSeen = up;
        if (d <= ARRIVE)
        {
            // Up: the node detour or the loot strategy takes it from here.
            if (up)
                End(*plan, nullptr, FOUND_SKIP_MS);
            else
            {
                if (memory)
                    memory->MissObject(plan->gatherKey, unixNow);
                End(*plan, "trip_empty", EMPTY_SKIP_MS);
            }
            return false;
        }
    }
    return MoveFarTo(WorldPosition(plan->gatherMap, plan->gatherX, plan->gatherY, plan->gatherZ));
}

// The nearest remembered node the bot can gather, past what it can see from here and within the trip range (the
// whole range with no quest to work, a short one while questing), recently seen ones first. Nodes found gone are
// left until a respawn could have brought them back.
bool CortexGatherAction::Pick(CortexPlan& plan, CortexMemory const& memory)
{
    uint32 now = getMSTime();
    std::erase_if(plan.gatherSkip, [now](auto const& entry) { return Due(now, entry.second); });

    uint32 unixNow = static_cast<uint32>(GameTime::GetGameTime().count());
    uint32 seenAfter = unixNow - FORGET_SECONDS;
    float range = Idle(plan) ? Cortex::GatherTripRange() : BUSY_TRIP_RANGE;
    float bestScore = std::numeric_limits<float>::max();
    float bestDist = 0.0f;
    CortexMemory::Object const* best = nullptr;
    uint64 bestKey = 0;
    uint32 bestSkill = 0;
    for (auto const& [key, object] : memory.Objects())
    {
        if (object.kind != CortexMemory::OBJECT_NODE || object.map != bot->GetMapId() || object.lastSeen < seenAfter ||
            object.goneUntil > unixNow || plan.gatherSkip.count(key))
            continue;
        float d = bot->GetExactDist2d(object.x, object.y);
        if (d < MIN_TRIP || d >= range)
            continue;
        uint32 age = unixNow > object.lastSeen ? unixNow - object.lastSeen : 0;
        float score = d + AGE_YARDS_PER_HOUR * static_cast<float>(age) / HOUR;
        if (score >= bestScore || CortexQuests::IsAvoided(plan, object.map, object.x, object.y))
            continue;
        uint32 required = 0;
        uint32 skill = NewRpgGatherAction::NodeSkill(sObjectMgr->GetGameObjectTemplate(object.entry), &required);
        if (!NewRpgGatherAction::CanGather(bot, skill, required))
            continue;
        best = &object;
        bestKey = key;
        bestSkill = skill;
        bestDist = d;
        bestScore = score;
    }
    if (!best)
        return false;

    plan.gatherEntry = best->entry;
    plan.gatherSkill = bestSkill;
    plan.gatherMap = best->map;
    plan.gatherX = best->x;
    plan.gatherY = best->y;
    plan.gatherZ = best->z;
    plan.gatherKey = bestKey;
    plan.gatherSinceMs = now;
    plan.gatherSeen = false;
    // b: minutes since the node was last seen up (the skill follows from the entry).
    uint32 age = unixNow > best->lastSeen ? unixNow - best->lastSeen : 0;
    BotTelemetry::instance().Event(botAI, "gather", "gather_trip", static_cast<int32>(best->entry),
                                   static_cast<int32>(age / MINUTE), static_cast<int32>(bestDist));
    return true;
}

// A node of the remembered entry up at the remembered spot.
bool CortexGatherAction::NodeThere(CortexPlan const& plan)
{
    std::list<GameObject*> objects;
    Acore::GameObjectInRangeCheck check(plan.gatherX, plan.gatherY, plan.gatherZ, NODE_SPOT_RANGE, plan.gatherEntry);
    Acore::GameObjectListSearcher<Acore::GameObjectInRangeCheck> searcher(bot, objects, check);
    Cell::VisitObjects(bot, searcher, bot->GetExactDist2d(plan.gatherX, plan.gatherY) + NODE_SPOT_RANGE);
    for (GameObject* go : objects)
        if (go->isSpawned() && go->GetGoState() == GO_STATE_READY && go->getLootState() == GO_READY)
            return true;
    return false;
}

void CortexGatherAction::End(CortexPlan& plan, char const* failReason, uint32 skipMs)
{
    uint32 now = getMSTime();
    if (failReason)
        BotTelemetry::instance().Event(botAI, "gather", std::string("gather_fail_") + failReason,
                                       static_cast<int32>(plan.gatherEntry), static_cast<int32>(plan.gatherSkill),
                                       static_cast<int32>(getMSTimeDiff(plan.gatherSinceMs, now) / IN_MILLISECONDS));
    if (skipMs)
        plan.gatherSkip[plan.gatherKey] = now + skipMs;
    plan.gatherEntry = 0;
    plan.gatherSkill = 0;
    plan.gatherKey = 0;
    plan.gatherSeen = false;
    plan.gatherEndMs = now;
}
