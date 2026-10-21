/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 * Originally written as part of the wowproject patches to mod-playerbots.
 */

#include "CortexActions.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_set>

#include "BotLevels.h"
#include "BotTelemetry.h"
#include "CortexMemory.h"
#include "CortexQuests.h"
#include "CortexTravel.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Formulas.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "GossipDef.h"
#include "LootMgr.h"
#include "LootObjectStack.h"
#include "ObjectAccessor.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Cortex.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    constexpr uint32 THINK_BACKSTOP_MS = 45 * IN_MILLISECONDS;
    constexpr uint32 GRIND_THINK_MS = 60 * IN_MILLISECONDS;
    constexpr uint32 SCAN_MS = 1000;
    constexpr float QUEST_GIVER_REACH = 40.0f;
    constexpr float HUNT_RANGE = 45.0f;
    constexpr float PACK_RANGE = 10.0f;
    constexpr uint32 PACK_LIMIT = 1;
    constexpr uint8 LEVELS_ABOVE = 2;
    // Quest sparkles (B4d): in sight out to QUEST_OBJECT_RANGE, without a line-of-sight test this close.
    constexpr float QUEST_OBJECT_RANGE = 60.0f;
    constexpr float QUEST_OBJECT_NO_LOS = 30.0f;
    constexpr uint32 USE_REACH_MS = 45 * IN_MILLISECONDS;              // walking to one in sight
    constexpr float SPARKLE_LOOK = 15.0f;    // this close to a remembered spot the bot sees whether it's up
    constexpr float SPARKLE_SPOT = 5.0f;     // an object of that entry this close to the spot is the one
    constexpr uint32 SPARKLE_TRIP_MS = 5 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 SPARKLE_USED_SKIP_MS = 5 * MINUTE * IN_MILLISECONDS;   // just opened: despawned for a while
    constexpr uint32 SPARKLE_GONE_SKIP_MS = 5 * MINUTE * IN_MILLISECONDS;   // taken by someone: back after a respawn
    constexpr uint32 SPARKLE_SLOW_SKIP_MS = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 SPARKLE_STUCK_SKIP_MS = 30 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 SPARKLE_FORGET_SECONDS = 3 * DAY;  // not seen since: likely a pooled spawn that moved
    constexpr float AREA_MARGIN = 30.0f;
    constexpr float CLUSTER_ARRIVE_MARGIN = 40.0f;
    constexpr float SEARCHED_RANGE = 35.0f;
    constexpr uint32 SEARCH_RESET_MS = 90 * IN_MILLISECONDS;
    constexpr float CAVE_BELOW_SURFACE = 4.0f;    // a floor at least this far under the surface is a cave floor
    constexpr float CAVE_SEARCH_DEPTH = 150.0f;
    constexpr float CAVE_SEARCHED_HEIGHT = 10.0f;
    constexpr uint32 NO_PROGRESS_MS = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 KILLING_MS = 3 * MINUTE * IN_MILLISECONDS;        // a kill this recent counts as working
    constexpr uint32 MAX_PATIENCE_MS = 25 * MINUTE * IN_MILLISECONDS;  // without progress, even while killing
    constexpr float VENDOR_RANGE = 1500.0f;
    constexpr float VENDOR_ARRIVE = 20.0f;
    constexpr uint8 VENDOR_TRIES = 2;
    constexpr uint8 DEATHS_TO_SHELVE = 2;
    constexpr uint16 KILLS_WITHOUT_DROP = 10;
    constexpr float HAND_IN_ARRIVE = 30.0f;
    // Up or down this far from the ender is another floor: the Stormwind mage trainers stand in the Wizard's
    // Sanctum basement, 76 yd under the street, and a bot on the street never sees their "?".
    constexpr float HAND_IN_ARRIVE_Z = 20.0f;
    constexpr uint32 HAND_IN_WAIT_MS = 2 * MINUTE * IN_MILLISECONDS;
    constexpr float PICK_UP_ARRIVE = 20.0f;
    constexpr uint32 PICK_UP_WAIT_MS = 20 * IN_MILLISECONDS;
    constexpr float GRIND_ARRIVE = 40.0f;
    constexpr uint32 LOOK_ATTEMPTS = 8;
    constexpr float LOOK_WINDING = 2.0f;  // a look-around path at most this many times the straight distance
    constexpr uint32 GRIND_WAIT_MS = 60 * IN_MILLISECONDS;
    constexpr float ERRAND_ARRIVE = 10.0f;
    constexpr float SAME_OUTING = 50.0f;
    constexpr uint32 GIVER_REACH_MS = 30 * IN_MILLISECONDS;
    constexpr uint32 GIVER_IGNORE_MS = 10 * MINUTE * IN_MILLISECONDS;
    constexpr float TRAVEL_SAME_TARGET = 30.0f;
    constexpr float TRAVEL_PROGRESS = 10.0f;
    constexpr uint32 TRAVEL_STUCK_MS = 3 * MINUTE * IN_MILLISECONDS;
    constexpr float ITEM_TARGET_RANGE = 50.0f;
    constexpr uint32 ITEM_REUSE_MS = 2 * MINUTE * IN_MILLISECONDS;   // same creature or object again
    constexpr uint32 ITEM_SELF_REUSE_MS = 30 * IN_MILLISECONDS;      // self-cast or focus use in the area
    constexpr uint32 ITEM_JUDGE_MS = 25 * IN_MILLISECONDS;           // a channel (a taming rod) runs ~20 s
    constexpr float FLIGHT_MIN_DISTANCE = 800.0f;  // farther than this, a known flight may beat walking
    // The rest of a walk is looked at again this often for mobs well above the bot ahead (CortexTravel::DangerousWalk),
    // when the target is further than DANGER_CHECK_MIN; a place SAFE_RADIUS clear of them is where it turns back to.
    constexpr uint32 DANGER_CHECK_MS = 15 * IN_MILLISECONDS;
    constexpr float DANGER_CHECK_MIN = 150.0f;
    constexpr float SAFE_RADIUS = 150.0f;
    constexpr float RETREAT_ARRIVE = 20.0f;
    constexpr uint32 RETREAT_MS = 5 * MINUTE * IN_MILLISECONDS;
    constexpr uint8 TRIPS_PER_TARGET = 3;          // trips planned toward one target, so they can't loop
    constexpr uint32 LEARN_FLIGHT_MS = 30 * IN_MILLISECONDS;

    float Dist2d(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

    // Memory key of a live object (CortexMemory keys objects by entry and the cell they stand in).
    uint64 SparkleKey(GameObject const* object)
    {
        uint64 cell = CortexMemory::CellKey(object->GetMapId(), object->GetPositionX(), object->GetPositionY());
        return CortexMemory::EntryKey(object->GetEntry(), cell);
    }

    // Only kinds the client makes sparkle or lets the player activate for a quest. A quest giver object (a wanted
    // poster, a Candy Bucket) passes ActivateToQuest too, but using it does nothing for an objective.
    bool IsQuestObjectKind(GameObject const* object)
    {
        switch (object->GetGoType())
        {
            case GAMEOBJECT_TYPE_CHEST:
            case GAMEOBJECT_TYPE_GOOBER:
            case GAMEOBJECT_TYPE_SPELL_FOCUS:
            case GAMEOBJECT_TYPE_GENERIC:
                return true;
            default:
                return false;
        }
    }

    // Whether an object of this kind would sparkle for the bot now, as the client decides it: one of its quests
    // still needs the object used, or still needs an item its loot holds (GameObject::ActivateToQuest, minus the
    // live object).
    bool WouldSparkle(Player* bot, uint32 entry)
    {
        if (bot->HasQuestForGO(static_cast<int32>(entry)))
            return true;
        GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(entry);
        if (!info)
            return false;
        switch (info->type)
        {
            case GAMEOBJECT_TYPE_CHEST:
                return (info->chest.questId && bot->GetQuestStatus(info->chest.questId) == QUEST_STATUS_INCOMPLETE) ||
                       LootTemplates_Gameobject.HaveQuestLootForPlayer(info->GetLootId(), bot);
            case GAMEOBJECT_TYPE_GOOBER:
                return info->goober.questId > 0 &&
                       bot->GetQuestStatus(static_cast<uint32>(info->goober.questId)) == QUEST_STATUS_INCOMPLETE;
            default:
                return false;
        }
    }

    uint32 ManaPct(Player* bot)
    {
        if (bot->getPowerType() != POWER_MANA || !bot->GetMaxPower(POWER_MANA))
            return 100;
        return bot->GetPower(POWER_MANA) * 100 / bot->GetMaxPower(POWER_MANA);
    }
}

bool CortexThinkAction::isUseful()
{
    CortexPlan* plan = Cortex::Plan(bot);
    if (!plan || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight())
        return false;
    return plan->dirty || static_cast<int32>(getMSTime() - plan->nextThinkMs) >= 0;
}

bool CortexThinkAction::Execute(Event /*event*/)
{
    CortexPlan* plan = Cortex::Plan(bot);
    CortexMemory* memory = Cortex::Memory(bot);
    if (!plan || !memory)
        return false;

    CortexGoal oldGoal = plan->goal;
    float oldX = plan->outingX;
    float oldY = plan->outingY;

    CortexQuests::Build(bot, *plan, *memory);
    CortexQuests::OutingPlace(*plan, plan->outingX, plan->outingY);

    uint32 now = getMSTime();
    plan->dirty = false;
    plan->nextThinkMs = now + (plan->goal == CortexGoal::GRIND ? GRIND_THINK_MS : THINK_BACKSTOP_MS);

    // A new outing starts its own timers and death count; a re-plan of the same one keeps them, or the
    // backstop re-plan would restart every wait before it runs out.
    bool sameOuting = plan->goal == oldGoal && Dist2d(oldX, oldY, plan->outingX, plan->outingY) < SAME_OUTING;
    if (!sameOuting)
    {
        plan->arrivedMs = 0;
        plan->progressMs = now;
        plan->searchResetMs = 0;
        plan->clusterDeaths = 0;
        plan->outingActive = false;
        plan->swept = false;
        plan->vendorEntry = 0;
        plan->sparkleKey = 0;
        plan->useTarget.Clear();
        plan->progress = plan->cluster >= 0 ? CortexQuests::Progress(bot, *plan, plan->clusters[plan->cluster]) : 0;
    }

    uint32 quests = 0;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (bot->GetQuestSlotQuestId(slot))
            ++quests;
    std::string info = std::string(CortexQuests::GoalName(plan->goal)) + " quests " + std::to_string(quests);
    if (plan->goal == CortexGoal::PICK_UP && plan->revisit)
        info += " revisit";
    BotTelemetry::instance().Event(botAI, "plan", info, static_cast<int32>(plan->tasks.size()),
                                   static_cast<int32>(plan->clusters.size()), plan->cluster);
    return true;
}

bool CortexStepAction::isUseful()
{
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight())
        return false;
    CortexPlan* plan = Cortex::Plan(bot);
    return plan && (plan->goal != CortexGoal::NONE || botAI->rpgInfo.GetStatus() == RPG_GO_CAMP);
}

bool CortexStepAction::Execute(Event /*event*/)
{
    CortexPlan* plan = Cortex::Plan(bot);
    CortexMemory* memory = Cortex::Memory(bot);
    if (!plan || !memory)
        return false;

    // Upkeep (0006) picks a town and sets GO_CAMP; walking there was new-rpg's job, so Cortex does it.
    plan->stepDetail = "";
    plan->stepMap = UINT32_MAX;
    if (botAI->rpgInfo.GetStatus() == RPG_GO_CAMP)
    {
        plan->step = "errand";
        return Errand();
    }

    if (TalkToQuestGivers(*plan))
    {
        plan->step = "givers";
        return true;
    }
    if (LearnFlightPoint(*plan))
    {
        plan->step = "flightpt";
        return true;
    }

    if (plan->retreating)
    {
        plan->step = "retreat";
        return Retreat(*plan);
    }

    // The goal's own branches name themselves more closely where it matters (grind, travel).
    switch (plan->goal)
    {
        case CortexGoal::HAND_IN:
            plan->step = "hand_in";
            return HandIn(*plan, *memory);
        case CortexGoal::WORK:
            plan->step = "work";
            return Work(*plan, *memory);
        case CortexGoal::PICK_UP:
            plan->step = "pick_up";
            return PickUp(*plan, *memory);
        case CortexGoal::GRIND:
            plan->step = "grind";
            return Grind(*plan, *memory);
        default:
            return false;
    }
}

// Among mobs well above it with no safe way on (Travel gave up for that): back to the last place it walked through
// without them, as a player turns round at the sight of skulls. A trip there may be the hearthstone.
bool CortexStepAction::Retreat(CortexPlan& plan)
{
    uint32 now = getMSTime();
    bool arrived = plan.safeMap == bot->GetMapId() && bot->GetExactDist2d(plan.safeX, plan.safeY) <= RETREAT_ARRIVE;
    bool timeout = getMSTimeDiff(plan.retreatMs, now) >= RETREAT_MS;
    if (arrived || timeout || plan.safeMap != bot->GetMapId())
    {
        BotTelemetry::instance().Event(botAI, "retreat", arrived ? "done" : (timeout ? "timeout" : "left"),
                                       static_cast<int32>(getMSTimeDiff(plan.retreatMs, now) / IN_MILLISECONDS),
                                       static_cast<int32>(bot->GetExactDist2d(plan.safeX, plan.safeY)));
        plan.retreating = false;
        plan.dirty = true;
        return false;
    }
    bool stuck = false;
    bool moved = MoveToGround(plan, plan.safeMap, plan.safeX, plan.safeY, stuck);
    if (stuck)
    {
        BotTelemetry::instance().Event(botAI, "retreat", "stuck",
                                       static_cast<int32>(getMSTimeDiff(plan.retreatMs, now) / IN_MILLISECONDS),
                                       static_cast<int32>(bot->GetExactDist2d(plan.safeX, plan.safeY)));
        plan.retreating = false;
        plan.dirty = true;
    }
    return moved;
}

bool CortexStepAction::Errand()
{
    auto* camp = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data);
    if (!camp)
        return false;
    if (bot->GetExactDist(camp->pos.GetPositionX(), camp->pos.GetPositionY(), camp->pos.GetPositionZ()) <=
        ERRAND_ARRIVE)
    {
        botAI->rpgInfo.ChangeToIdle();
        return false;
    }
    return MoveFarTo(camp->pos);
}

// Any quest giver in reach with a quest to take or hand in: the bot sees its "!" or "?" and walks over.
bool CortexStepAction::TalkToQuestGivers(CortexPlan& plan)
{
    uint32 now = getMSTime();
    if (!plan.interactTarget && static_cast<int32>(now - plan.nextGiverScanMs) >= 0)
    {
        plan.nextGiverScanMs = now + SCAN_MS;
        plan.interactTarget = ChooseQuestGiver(plan);
        plan.interactSinceMs = now;
    }
    if (!plan.interactTarget)
        return false;

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, plan.interactTarget);
    if (!object || !object->IsInWorld() || !HasQuestToAcceptOrReward(object))
    {
        plan.interactTarget.Clear();
        return false;
    }
    if (!IsWithinInteractionDist(object))
    {
        // Up a ledge or behind a wall: leave it be for a while, as a player gives up on an NPC they can't reach.
        if (getMSTimeDiff(plan.interactSinceMs, now) >= GIVER_REACH_MS)
        {
            plan.ignoredGivers[plan.interactTarget] = now + GIVER_IGNORE_MS;
            plan.interactTarget.Clear();
            return false;
        }
        return MoveWorldObjectTo(plan.interactTarget);
    }

    InteractWithNpcOrGameObjectForQuest(plan.interactTarget);
    plan.interactTarget.Clear();
    plan.dirty = true;
    return true;
}

// The nearest quest giver or object in reach showing "!" or "?" for the bot, other than ones it couldn't reach.
ObjectGuid CortexStepAction::ChooseQuestGiver(CortexPlan& plan)
{
    uint32 now = getMSTime();
    std::erase_if(plan.ignoredGivers, [now](auto const& entry) { return static_cast<int32>(now - entry.second) >= 0; });

    WorldObject* best = nullptr;
    float bestDist = QUEST_GIVER_REACH;
    for (char const* value : {"possible new rpg targets", "possible new rpg game objects"})
        for (ObjectGuid const& guid : AI_VALUE(GuidVector, value))
        {
            if (plan.ignoredGivers.count(guid))
                continue;
            WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);
            if (!object || !object->IsInWorld())
                continue;
            float d = bot->GetDistance(object);
            if (d >= bestDist || !CanInteractWithQuestGiver(object) || !HasQuestToAcceptOrReward(object))
                continue;
            best = object;
            bestDist = d;
        }
    return best ? best->GetGUID() : ObjectGuid();
}

bool CortexStepAction::HandIn(CortexPlan& plan, CortexMemory& memory)
{
    if (plan.handIn < 0 || plan.handIn >= static_cast<int32>(plan.tasks.size()))
    {
        plan.dirty = true;
        return false;
    }
    CortexTask const& task = plan.tasks[plan.handIn];
    if (bot->GetQuestStatus(task.questId) != QUEST_STATUS_COMPLETE)
    {
        plan.dirty = true;
        return false;
    }

    // There means on the same level too: a turn-in in Undercity isn't reached from the ruins above it. The height
    // is the quest ender's where known, else the ground (another map's isn't known from here).
    float z = task.hasZ ? task.z
                        : (task.map == bot->GetMapId() ? CortexQuests::GroundZ(bot, task.x, task.y, bot->GetPositionZ())
                                                       : INVALID_HEIGHT);
    bool there = bot->GetMapId() == task.map && bot->GetExactDist2d(task.x, task.y) <= HAND_IN_ARRIVE &&
                 (!task.hasZ || std::fabs(bot->GetPositionZ() - task.z) <= HAND_IN_ARRIVE_Z) &&
                 CortexTravel::SameRegion(bot, task.map, task.x, task.y, z, task.mapArea);
    if (!there)
    {
        plan.arrivedMs = 0;
        bool stuck = false;
        bool moved = Travel(plan, task.map, task.x, task.y, z, stuck, task.mapArea);
        if (stuck)
        {
            // Travel blocked the spot if the bot got stuck there; the next plan goes to another place the ender
            // stands, if it has one, and the quest is set aside only when it hasn't.
            float ox = 0.0f;
            float oy = 0.0f;
            float oz = 0.0f;
            uint32 now = static_cast<uint32>(GameTime::GetGameTime().count());
            if (plan.travelDanger)
                CortexQuests::Shelve(bot, plan, task.questId, "danger");
            else if (memory.SpotBlocked(task.map, task.x, task.y, now) &&
                     CortexQuests::OtherEnder(task, memory, ox, oy, oz))
                plan.dirty = true;
            else
                CortexQuests::Shelve(bot, plan, task.questId, "unreach");
        }
        return moved;
    }

    uint32 now = getMSTime();
    if (!plan.arrivedMs)
        plan.arrivedMs = now;
    // At the turn-in area and no "?" in reach: some enders can't take it from a bot (e.g. a barrel).
    if (getMSTimeDiff(plan.arrivedMs, now) >= HAND_IN_WAIT_MS)
    {
        CortexQuests::Shelve(bot, plan, task.questId, "reward");
        return false;
    }
    return LookAround(20.0f);
}

bool CortexStepAction::Work(CortexPlan& plan, CortexMemory& memory)
{
    if (plan.cluster < 0 || plan.cluster >= static_cast<int32>(plan.clusters.size()))
    {
        plan.dirty = true;
        return false;
    }
    CortexCluster& cluster = plan.clusters[plan.cluster];
    uint32 now = getMSTime();

    if (static_cast<int32>(now - plan.nextScanMs) >= 0)
    {
        if (CortexQuests::ClusterHasCompletedTask(bot, plan, cluster))
        {
            plan.dirty = true;
            return false;
        }
        uint32 progress = CortexQuests::Progress(bot, plan, cluster);
        if (progress != plan.progress)
        {
            plan.progress = progress;
            plan.progressMs = now;
        }
    }

    // Two deaths on one outing, or 10 minutes in the area without progress: set those quests aside. A bot still
    // killing there (a low drop rate) keeps at it up to 25 minutes.
    uint32 sinceProgress = getMSTimeDiff(plan.progressMs, now);
    bool killing = plan.lastKillMs && getMSTimeDiff(plan.lastKillMs, now) < KILLING_MS;
    bool stalled = plan.arrivedMs && sinceProgress >= NO_PROGRESS_MS && (!killing || sinceProgress >= MAX_PATIENCE_MS);
    if (plan.clusterDeaths >= DEATHS_TO_SHELVE || stalled)
    {
        ShelveCluster(plan, cluster, stalled ? "noprog" : "deaths");
        plan.clusterDeaths = 0;
        return false;
    }

    if (VendorErrand(plan, memory, cluster))
        return true;

    if (bot->GetExactDist2d(cluster.x, cluster.y) > cluster.radius + CLUSTER_ARRIVE_MARGIN ||
        (CortexTravel::Enabled() &&
         !CortexTravel::SameRegion(bot, cluster.map, cluster.x, cluster.y,
                                   CortexQuests::GroundZ(bot, cluster.x, cluster.y, bot->GetPositionZ()),
                                   cluster.mapArea)))
    {
        plan.arrivedMs = 0;
        bool stuck = false;
        bool moved = MoveToGround(plan, cluster.map, cluster.x, cluster.y, stuck, cluster.mapArea);
        if (stuck)
            ShelveCluster(plan, cluster, plan.travelDanger ? "danger" : "unreach");
        return moved;
    }
    if (!plan.arrivedMs)
    {
        plan.arrivedMs = now;
        plan.progressMs = now;
    }

    if (static_cast<int32>(now - plan.nextScanMs) >= 0)
    {
        plan.nextScanMs = now + SCAN_MS;

        // What counts: the creatures the log names, the sources the bot has looted the item from, and for
        // items with no known source yet, hostile creatures inside the item's map area.
        std::unordered_set<uint32> wanted;
        std::unordered_set<uint32> useTargets;  // creatures a quest item is for: never attacked for the quest
        std::vector<CortexTask const*> unknownSources;
        for (uint32 index : cluster.tasks)
        {
            CortexTask const& task = plan.tasks[index];
            if (task.kind == CortexTaskKind::USE_ITEM && task.target > 0)
                useTargets.insert(static_cast<uint32>(task.target));
            else if (task.kind == CortexTaskKind::KILL)
                wanted.insert(static_cast<uint32>(task.target));
            else if (task.kind == CortexTaskKind::COLLECT)
            {
                // Looted from a chest or sack before: the item comes from those, not from the creatures around them
                // (Filthy Paws' kobolds stand between the crates).
                bool known = false;
                for (auto const& [key, drop] : memory.Drops())
                    if (drop.item == static_cast<uint32>(task.target))
                    {
                        if (drop.source > 0)
                            wanted.insert(static_cast<uint32>(drop.source));
                        known = true;
                    }
                if (!known)
                    unknownSources.push_back(&task);
            }
        }

        auto isWanted = [&](Creature* creature)
        {
            if (useTargets.count(creature->GetEntry()))
                return false;
            if (wanted.count(creature->GetEntry()))
                return true;
            for (CortexTask const* task : unknownSources)
            {
                if (memory.Kills(creature->GetEntry()) >= KILLS_WITHOUT_DROP)
                    continue;
                if (CortexQuests::InArea(*task, creature->GetPositionX(), creature->GetPositionY(), AREA_MARGIN))
                    return true;
            }
            return false;
        };

        if (UseQuestItems(plan, cluster))
            return true;
        if (TalkForQuest(plan, cluster))
            return true;
        Unit* target = FindHuntTarget(isWanted);
        if (sawWanted)
            plan.outingActive = true;
        if (target)
            return Hunt(plan, target);
        if (UseQuestObject(plan, cluster, memory, true))
            return true;
    }
    else if (plan.useTarget || plan.sparkleKey)
        return UseQuestObject(plan, cluster, memory, false);
    else if (plan.itemTarget)
        return UseQuestItems(plan, cluster);

    return Search(plan, cluster);
}

bool CortexStepAction::Search(CortexPlan& plan, CortexCluster& cluster)
{
    float bx = bot->GetPositionX();
    float by = bot->GetPositionY();
    float bz = bot->GetPositionZ();
    CortexSearchPoint* next = nullptr;
    float nextDist = 0.0f;
    for (CortexSearchPoint& point : cluster.points)
    {
        float d = Dist2d(bx, by, point.x, point.y);
        // A cave point counts as searched only from down there, not from the hill above it.
        if (d <= SEARCHED_RANGE && (!point.cave || std::fabs(bz - point.z) <= CAVE_SEARCHED_HEIGHT))
            point.searched = true;
        if (!point.searched && (!next || d < nextDist))
        {
            next = &point;
            nextDist = d;
        }
    }

    uint32 now = getMSTime();
    if (!next)
    {
        plan.swept = true;
        // Searched it all without seeing anything to do: nothing here for these quests now (a target that never
        // spawns, a chest underground), so set them aside instead of waiting out the no-progress timer.
        if (!plan.searchResetMs && !plan.outingActive)
        {
            ShelveCluster(plan, cluster, "empty");
            return false;
        }
        // Searched it all: wait for respawns, then search again.
        if (!plan.searchResetMs)
            plan.searchResetMs = now + SEARCH_RESET_MS;
        if (static_cast<int32>(now - plan.searchResetMs) >= 0)
        {
            plan.searchResetMs = 0;
            for (CortexSearchPoint& point : cluster.points)
                point.searched = false;
        }
        return false;
    }
    // Underground: the floor below the surface at this point, if the map has one.
    if (cluster.underground && !next->floorKnown)
    {
        next->floorKnown = true;
        float surface = bot->GetMap()->GetHeight(bot->GetPhaseMask(), next->x, next->y, MAX_HEIGHT, true, MAX_HEIGHT);
        float floor = bot->GetMap()->GetHeight(bot->GetPhaseMask(), next->x, next->y, surface - CAVE_BELOW_SURFACE,
                                               true, CAVE_SEARCH_DEPTH);
        if (surface > INVALID_HEIGHT && floor > INVALID_HEIGHT && floor < surface - CAVE_BELOW_SURFACE)
        {
            next->cave = true;
            next->z = floor;
        }
    }

    bool stuck = false;
    bool moved = next->cave ? Travel(plan, cluster.map, next->x, next->y, next->z, stuck, cluster.mapArea)
                            : MoveToGround(plan, cluster.map, next->x, next->y, stuck, cluster.mapArea);
    if (stuck && plan.travelDanger && CortexTravel::Recheck())
        ShelveCluster(plan, cluster, "danger");
    else if (stuck)
        next->searched = true;
    return moved;
}

// A sparkling object for one of the bot's quests (the client highlights these): walk over and use it. Past what is
// in sight, a sparkle remembered in the quest area is walked back to (B4d); one found gone is tried again later.
bool CortexStepAction::UseQuestObject(CortexPlan& plan, CortexCluster const& cluster, CortexMemory const& memory,
                                      bool scan)
{
    uint32 now = getMSTime();
    GameObject* object = plan.useTarget ? ObjectAccessor::GetGameObject(*bot, plan.useTarget) : nullptr;
    if (!object || !object->isSpawned() || !IsQuestObjectKind(object) || !object->ActivateToQuest(bot))
    {
        plan.useTarget.Clear();
        object = scan ? FindQuestObject(plan) : nullptr;
        if (!object)
            return (scan || plan.sparkleKey) && ReturnToSparkle(plan, cluster, memory);
        plan.useTarget = object->GetGUID();
        plan.useSinceMs = now;
        plan.outingActive = true;
    }

    uint64 key = SparkleKey(object);
    if (!bot->IsWithinDistInMap(object, INTERACTION_DISTANCE))
    {
        // Up a floor with no stairs found, across a ravine: leave it for a while.
        if (getMSTimeDiff(plan.useSinceMs, now) >= USE_REACH_MS)
        {
            plan.sparkleSkip[key] = now + SPARKLE_STUCK_SKIP_MS;
            plan.usedOn[plan.useTarget] = now + SPARKLE_STUCK_SKIP_MS;
            plan.useTarget.Clear();
            if (plan.sparkleKey == key)
                EndSparkle(plan, "unreach", SPARKLE_STUCK_SKIP_MS);
            else
                BotTelemetry::instance().Event(botAI, "sparkle", "unreach", static_cast<int32>(object->GetEntry()));
            return false;
        }
        return MoveWorldObjectTo(plan.useTarget);
    }

    // Used: it despawns or stays spent for a while, so it isn't the next one to walk back to.
    BotTelemetry::instance().Event(botAI, "sparkle", "use", static_cast<int32>(object->GetEntry()),
                                   plan.sparkleKey == key ? 1 : 0, static_cast<int32>(object->GetGoType()));
    if (plan.sparkleKey == key)
        EndSparkle(plan, "found", SPARKLE_USED_SKIP_MS);
    else
        plan.sparkleSkip[key] = now + SPARKLE_USED_SKIP_MS;
    plan.usedOn[object->GetGUID()] = now + SPARKLE_USED_SKIP_MS;

    if (object->GetGoType() == GAMEOBJECT_TYPE_CHEST)
    {
        context->GetValue<LootObject>("loot target")->Set(LootObject(bot, object->GetGUID()));
        botAI->DoSpecificAction("open loot", Event(), true);
    }
    else
    {
        WorldPacket packet(CMSG_GAMEOBJ_USE, 8);
        packet << object->GetGUID();
        bot->GetSession()->HandleGameObjectUseOpcode(packet);
    }
    plan.useTarget.Clear();
    return true;
}

// The nearest sparkle for one of the bot's quests it can see from here: in line of sight, or close enough to make
// out through a window or a doorway (the spellbooks upstairs in a farmhouse), as perception counts it.
GameObject* CortexStepAction::FindQuestObject(CortexPlan const& plan)
{
    std::list<GameObject*> objects;
    Acore::GameObjectInRangeCheck check(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                        QUEST_OBJECT_RANGE);
    Acore::GameObjectListSearcher<Acore::GameObjectInRangeCheck> searcher(bot, objects, check);
    Cell::VisitObjects(bot, searcher, QUEST_OBJECT_RANGE);
    GameObject* best = nullptr;
    float bestDist = QUEST_OBJECT_RANGE;
    for (GameObject* candidate : objects)
    {
        if (!candidate->isSpawned() || plan.usedOn.count(candidate->GetGUID()) || !IsQuestObjectKind(candidate) ||
            !candidate->ActivateToQuest(bot))
            continue;
        float d = bot->GetExactDist(candidate);
        if (d >= bestDist || (d > QUEST_OBJECT_NO_LOS && !bot->IsWithinLOSInMap(candidate)))
            continue;
        best = candidate;
        bestDist = d;
    }
    return best;
}

// Walks back to the nearest remembered sparkle for the cluster's quests. Close to the spot, the object is either up
// (used from there) or gone: taken by someone, or not respawned yet, and the bot tries the next one.
bool CortexStepAction::ReturnToSparkle(CortexPlan& plan, CortexCluster const& cluster, CortexMemory const& memory)
{
    if (!Cortex::Sparkles())
        return false;
    uint32 now = getMSTime();
    if (!plan.sparkleKey && !PickSparkle(plan, cluster, memory))
        return false;

    if (bot->GetMapId() != plan.sparkleMap || !WouldSparkle(bot, plan.sparkleEntry))
    {
        EndSparkle(plan, "moot", 0);
        return false;
    }
    if (getMSTimeDiff(plan.sparkleSinceMs, now) >= SPARKLE_TRIP_MS)
    {
        EndSparkle(plan, "slow", SPARKLE_SLOW_SKIP_MS);
        return false;
    }

    if (bot->GetExactDist(plan.sparkleX, plan.sparkleY, plan.sparkleZ) <= SPARKLE_LOOK)
    {
        std::list<GameObject*> objects;
        Acore::GameObjectInRangeCheck check(plan.sparkleX, plan.sparkleY, plan.sparkleZ, SPARKLE_SPOT,
                                            plan.sparkleEntry);
        Acore::GameObjectListSearcher<Acore::GameObjectInRangeCheck> searcher(bot, objects, check);
        Cell::VisitObjects(bot, searcher, SPARKLE_LOOK + SPARKLE_SPOT);
        for (GameObject* go : objects)
            if (go->isSpawned() && IsQuestObjectKind(go) && go->ActivateToQuest(bot) && !plan.usedOn.count(go->GetGUID()))
            {
                plan.useTarget = go->GetGUID();
                plan.useSinceMs = now;
                return UseQuestObject(plan, cluster, memory, false);
            }
        EndSparkle(plan, "gone", SPARKLE_GONE_SKIP_MS);
        return false;
    }

    bool stuck = false;
    bool moved = Travel(plan, plan.sparkleMap, plan.sparkleX, plan.sparkleY, plan.sparkleZ, stuck, cluster.mapArea);
    if (stuck)
        EndSparkle(plan, "unreach", SPARKLE_STUCK_SKIP_MS);
    return moved;
}

// The nearest remembered sparkle near the cluster that would still sparkle for the bot (its quests still want what
// it gives), other than ones found gone or out of reach lately.
bool CortexStepAction::PickSparkle(CortexPlan& plan, CortexCluster const& cluster, CortexMemory const& memory)
{
    uint32 now = getMSTime();
    std::erase_if(plan.sparkleSkip, [now](auto const& entry) { return static_cast<int32>(now - entry.second) >= 0; });

    uint32 seenAfter = static_cast<uint32>(GameTime::GetGameTime().count()) - SPARKLE_FORGET_SECONDS;
    float reach = cluster.radius + AREA_MARGIN;
    float bestDist = FLT_MAX;
    CortexMemory::Object const* best = nullptr;
    uint64 bestKey = 0;
    std::unordered_map<uint32, bool> sparkles;  // entry -> would sparkle, asked once per entry
    for (auto const& [key, object] : memory.Objects())
    {
        if (object.kind != CortexMemory::OBJECT_QUEST || object.map != cluster.map || object.lastSeen < seenAfter ||
            plan.sparkleSkip.count(key) || Dist2d(object.x, object.y, cluster.x, cluster.y) > reach)
            continue;
        float d = bot->GetExactDist(object.x, object.y, object.z);
        if (d >= bestDist)
            continue;
        auto [it, inserted] = sparkles.try_emplace(object.entry, false);
        if (inserted)
            it->second = WouldSparkle(bot, object.entry);
        if (!it->second)
            continue;
        best = &object;
        bestKey = key;
        bestDist = d;
    }
    if (!best)
        return false;

    plan.sparkleKey = bestKey;
    plan.sparkleEntry = best->entry;
    plan.sparkleMap = best->map;
    plan.sparkleX = best->x;
    plan.sparkleY = best->y;
    plan.sparkleZ = best->z;
    plan.sparkleSinceMs = now;
    plan.outingActive = true;
    BotTelemetry::instance().Event(botAI, "sparkle", "trip", static_cast<int32>(best->entry),
                                   static_cast<int32>(bestDist), static_cast<int32>(plan.sparkleSkip.size()));
    return true;
}

void CortexStepAction::EndSparkle(CortexPlan& plan, char const* outcome, uint32 skipMs)
{
    uint32 now = getMSTime();
    if (!plan.sparkleKey)
        return;
    BotTelemetry::instance().Event(botAI, "sparkle", outcome, static_cast<int32>(plan.sparkleEntry),
                                   static_cast<int32>(getMSTimeDiff(plan.sparkleSinceMs, now) / IN_MILLISECONDS),
                                   static_cast<int32>(bot->GetExactDist(plan.sparkleX, plan.sparkleY, plan.sparkleZ)));
    if (skipMs)
        plan.sparkleSkip[plan.sparkleKey] = now + skipMs;
    plan.sparkleKey = 0;
    plan.sparkleEntry = 0;
}

// Quest items with a "Use:" spell (CortexTaskKind::USE_ITEM): on the creature or object the log names, on a creature
// in the area found by trial when the log names none, next to the spell's focus ("near a forge"), or in the area.
bool CortexStepAction::UseQuestItems(CortexPlan& plan, CortexCluster& cluster)
{
    uint32 now = getMSTime();
    plan.itemTarget.Clear();
    std::erase_if(plan.usedOn, [now](auto const& entry) { return static_cast<int32>(now - entry.second) >= 0; });

    // A trial use that changed nothing in the log: that kind of creature is the wrong one for this quest.
    if (plan.itemUse.judgeMs && static_cast<int32>(now - plan.itemUse.judgeMs) >= 0)
    {
        if (CortexQuests::Progress(bot, plan, cluster) == plan.itemUse.progress &&
            bot->GetQuestStatus(plan.itemUse.questId) == QUEST_STATUS_INCOMPLETE)
            plan.wrongTargets.insert((static_cast<uint64>(plan.itemUse.questId) << 32) | plan.itemUse.entry);
        plan.itemUse = {};
    }
    if (plan.itemUse.judgeMs || bot->IsNonMeleeSpellCast(false))
        return false;

    for (uint32 index : cluster.tasks)
    {
        CortexTask const& task = plan.tasks[index];
        if (task.kind != CortexTaskKind::USE_ITEM)
            continue;
        Item* item = bot->GetItemByEntry(task.item);
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(task.spell);
        if (!item || !spell || bot->HasSpellCooldown(task.spell))
            continue;
        float range = std::max({spell->GetMaxRange(true), spell->GetMaxRange(false), 2.0f});

        WorldObject* target = nullptr;
        if (task.target < 0)
            target = FindNearbyObject([&](GameObject* go)
                                      { return go->GetEntry() == static_cast<uint32>(-task.target); });
        else if (spell->NeedsExplicitUnitTarget())
            target = FindItemTarget(plan, task);
        else if (spell->RequiresSpellFocus)
        {
            // Used on the bot itself, standing next to the focus.
            GameObject* focus = FindNearbyObject(
                [&](GameObject* go)
                {
                    return go->GetGoType() == GAMEOBJECT_TYPE_SPELL_FOCUS &&
                           go->GetGOInfo()->spellFocus.focusId == spell->RequiresSpellFocus;
                });
            if (!focus || plan.usedOn.count(bot->GetGUID()))
                continue;
            float reach = std::max(2.0f, static_cast<float>(focus->GetGOInfo()->spellFocus.dist) - 1.0f);
            if (bot->GetExactDist(focus) > reach)
            {
                plan.itemTarget = focus->GetGUID();
                return MoveWorldObjectTo(focus->GetGUID(), std::min(reach, INTERACTION_DISTANCE));
            }
            UseItemOn(item, task.spell, nullptr);
            plan.outingActive = true;
            plan.usedOn[bot->GetGUID()] = now + ITEM_SELF_REUSE_MS;
            return true;
        }
        else
        {
            if (plan.usedOn.count(bot->GetGUID()) ||
                !CortexQuests::InArea(task, bot->GetPositionX(), bot->GetPositionY(), AREA_MARGIN))
                continue;
            UseItemOn(item, task.spell, nullptr);
            plan.outingActive = true;
            plan.usedOn[bot->GetGUID()] = now + ITEM_SELF_REUSE_MS;
            return true;
        }

        if (!target)
            continue;
        if (bot->GetExactDist(target) > range - 0.5f || !bot->IsWithinLOSInMap(target))
        {
            plan.itemTarget = target->GetGUID();
            return MoveWorldObjectTo(target->GetGUID(), std::min(range - 0.5f, INTERACTION_DISTANCE));
        }
        UseItemOn(item, task.spell, target);
        plan.outingActive = true;
        plan.usedOn[target->GetGUID()] = now + ITEM_REUSE_MS;
        if (task.target == 0)
            plan.itemUse = {task.questId, target->GetEntry(), CortexQuests::Progress(bot, plan, cluster),
                            now + ITEM_JUDGE_MS};
        return true;
    }
    return false;
}

// The nearest live creature in sight the item could be for: the one the log names, or, when it names none, a
// creature in the task's area that isn't a kind the item already did nothing on.
Creature* CortexStepAction::FindItemTarget(CortexPlan& plan, CortexTask const& task)
{
    std::list<Unit*> units;
    Acore::AnyUnitInObjectRangeCheck check(bot, ITEM_TARGET_RANGE);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
    Cell::VisitObjects(bot, searcher, ITEM_TARGET_RANGE);

    Creature* best = nullptr;
    float bestDist = ITEM_TARGET_RANGE;
    for (Unit* unit : units)
    {
        Creature* creature = unit->ToCreature();
        if (!creature || !creature->IsAlive() || creature->IsPet() || creature->IsTotem() || creature->IsGuardian() ||
            plan.usedOn.count(creature->GetGUID()))
            continue;
        if (task.target > 0)
        {
            if (creature->GetEntry() != static_cast<uint32>(task.target))
                continue;
        }
        else if (creature->IsCritter() || !bot->IsValidAttackTarget(creature) ||
                 creature->GetLevel() > bot->GetLevel() || (creature->IsInCombat() && creature->GetVictim() != bot) ||
                 plan.wrongTargets.count((static_cast<uint64>(task.questId) << 32) | creature->GetEntry()) ||
                 !CortexQuests::InArea(task, creature->GetPositionX(), creature->GetPositionY(), AREA_MARGIN))
            continue;
        float d = bot->GetExactDist(creature);
        if (d >= bestDist || !bot->IsWithinLOSInMap(creature))
            continue;
        best = creature;
        bestDist = d;
    }
    return best;
}

GameObject* CortexStepAction::FindNearbyObject(std::function<bool(GameObject*)> const& match)
{
    std::list<GameObject*> objects;
    Acore::GameObjectInRangeCheck check(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                        ITEM_TARGET_RANGE);
    Acore::GameObjectListSearcher<Acore::GameObjectInRangeCheck> searcher(bot, objects, check);
    Cell::VisitObjects(bot, searcher, ITEM_TARGET_RANGE);
    GameObject* best = nullptr;
    float bestDist = ITEM_TARGET_RANGE;
    for (GameObject* go : objects)
    {
        if (!go->isSpawned() || !match(go))
            continue;
        float d = bot->GetExactDist(go);
        if (d < bestDist)
        {
            best = go;
            bestDist = d;
        }
    }
    return best;
}

// The same packet the client sends for a right-click on the item, with the target the spell asks for.
void CortexStepAction::UseItemOn(Item* item, uint32 spellId, WorldObject* target)
{
    if (bot->isMoving())
        bot->StopMoving();
    if (target)
        bot->SetFacingToObject(target);

    WorldPacket packet(CMSG_USE_ITEM);
    packet << item->GetBagSlot() << item->GetSlot() << uint8(1) << spellId << item->GetGUID() << uint32(0)
           << uint8(0);
    if (!target)
        packet << uint32(TARGET_FLAG_NONE);
    else if (target->IsGameObject())
        packet << uint32(TARGET_FLAG_GAMEOBJECT) << target->GetGUID().WriteAsPacked();
    else
        packet << uint32(TARGET_FLAG_UNIT) << target->GetGUID().WriteAsPacked();
    bot->GetSession()->HandleUseItemOpcode(packet);
    BotTelemetry::instance().Event(botAI, "item_use", target ? (target->IsGameObject() ? "object" : "unit") : "self",
                                   static_cast<int32>(item->GetEntry()), target ? static_cast<int32>(target->GetEntry())
                                                                                 : 0);
}

void CortexStepAction::ShelveCluster(CortexPlan& plan, CortexCluster const& cluster, char const* reason)
{
    std::unordered_set<uint32> quests;
    for (uint32 index : cluster.tasks)
        quests.insert(plan.tasks[index].questId);
    for (uint32 questId : quests)
        CortexQuests::Shelve(bot, plan, questId, reason);
}

// A creature the log names that can't be fought but will talk ("Find Mankrik's wife", "inspect the body"): walk
// up, open its gossip and take the first option, as a player reading it would.
bool CortexStepAction::TalkForQuest(CortexPlan& plan, CortexCluster const& cluster)
{
    std::unordered_set<uint32> entries;
    for (uint32 index : cluster.tasks)
        if (plan.tasks[index].kind == CortexTaskKind::KILL)
            entries.insert(static_cast<uint32>(plan.tasks[index].target));
    if (entries.empty())
        return false;

    std::list<Unit*> units;
    Acore::AnyUnitInObjectRangeCheck check(bot, ITEM_TARGET_RANGE);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
    Cell::VisitObjects(bot, searcher, ITEM_TARGET_RANGE);
    Creature* best = nullptr;
    float bestDist = ITEM_TARGET_RANGE;
    for (Unit* unit : units)
    {
        Creature* creature = unit->ToCreature();
        if (!creature || !entries.count(creature->GetEntry()) || !creature->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP) ||
            bot->IsValidAttackTarget(creature) || plan.usedOn.count(creature->GetGUID()))
            continue;
        float d = bot->GetExactDist(creature);
        if (d < bestDist)
        {
            best = creature;
            bestDist = d;
        }
    }
    if (!best)
        return false;

    plan.outingActive = true;
    if (!bot->IsWithinDistInMap(best, INTERACTION_DISTANCE - 1.0f))
    {
        plan.itemTarget = best->GetGUID();
        return MoveWorldObjectTo(best->GetGUID(), INTERACTION_DISTANCE - 1.0f);
    }
    if (bot->isMoving())
        bot->StopMoving();
    bot->SetFacingToObject(best);

    WorldPacket hello(CMSG_GOSSIP_HELLO, 8);
    hello << best->GetGUID();
    bot->GetSession()->HandleGossipHelloOpcode(hello);
    GossipMenu const& menu = bot->PlayerTalkClass->GetGossipMenu();
    if (!menu.GetMenuItems().empty())
    {
        WorldPacket select(CMSG_GOSSIP_SELECT_OPTION, 8 + 4 + 4);
        select << best->GetGUID() << menu.GetMenuId() << menu.GetMenuItems().begin()->first;
        bot->GetSession()->HandleGossipSelectOptionOpcode(select);
    }
    bot->PlayerTalkClass->SendCloseGossip();
    plan.usedOn[best->GetGUID()] = getMSTime() + ITEM_REUSE_MS;
    BotTelemetry::instance().Event(botAI, "quest", "talk", static_cast<int32>(best->GetEntry()));
    return true;
}

// Thread, vials, drinks the log asks for and the area didn't give: once the area has been searched and the bot has
// never looted the item anywhere, it tries the nearest vendors it remembers that sell it (two at most per item) and
// buys what's missing. A vendor without it is remembered as such for the item.
bool CortexStepAction::VendorErrand(CortexPlan& plan, CortexMemory& memory, CortexCluster const& cluster)
{
    auto looted = [&](uint32 item)
    {
        for (auto const& [key, drop] : memory.Drops())
            if (drop.item == item)
                return true;
        return false;
    };
    auto missing = [&](uint32 item) -> uint32
    {
        for (uint32 index : cluster.tasks)
        {
            CortexTask const& task = plan.tasks[index];
            if (task.kind != CortexTaskKind::COLLECT || !task.buyable || static_cast<uint32>(task.target) != item)
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(task.questId);
            if (!quest)
                continue;
            uint32 need = quest->RequiredItemCount[task.objective - QUEST_OBJECTIVES_COUNT];
            uint32 have = bot->GetItemCount(item);
            return need > have ? need - have : 0;
        }
        return 0;
    };
    auto giveUp = [&]()
    {
        plan.vendorLacks.insert((static_cast<uint64>(plan.vendorEntry) << 32) | plan.vendorItem);
        plan.vendorEntry = 0;
    };

    if (!plan.vendorEntry)
    {
        for (uint32 index : cluster.tasks)
        {
            CortexTask const& task = plan.tasks[index];
            uint32 item = static_cast<uint32>(task.target);
            if (task.kind != CortexTaskKind::COLLECT || !task.buyable || !plan.swept || !missing(item) ||
                plan.vendorTries[item] >= VENDOR_TRIES || looted(item))
                continue;
            float best = VENDOR_RANGE;
            for (auto const& [key, npc] : memory.Npcs())
            {
                if (!(npc.services & CortexMemory::SERVICE_VENDOR) || npc.map != bot->GetMapId() ||
                    !CortexQuests::VendorSells(npc.entry, item) ||
                    plan.vendorLacks.count((static_cast<uint64>(npc.entry) << 32) | item) ||
                    CortexQuests::IsAvoided(plan, npc.map, npc.x, npc.y) ||
                    memory.SpotBlocked(npc.map, npc.x, npc.y, static_cast<uint32>(GameTime::GetGameTime().count())))
                    continue;
                float d = bot->GetExactDist2d(npc.x, npc.y);
                if (d >= best)
                    continue;
                best = d;
                plan.vendorEntry = npc.entry;
                plan.vendorItem = item;
                plan.vendorMap = npc.map;
                plan.vendorX = npc.x;
                plan.vendorY = npc.y;
                plan.vendorZ = npc.z;
            }
            if (plan.vendorEntry)
                break;
        }
        if (!plan.vendorEntry)
            return false;
        ++plan.vendorTries[plan.vendorItem];
        BotTelemetry::instance().Event(botAI, "quest", "vendor_errand", static_cast<int32>(plan.vendorItem),
                                       static_cast<int32>(plan.vendorEntry),
                                       static_cast<int32>(bot->GetExactDist2d(plan.vendorX, plan.vendorY)));
    }

    uint32 need = missing(plan.vendorItem);
    if (!need)
    {
        plan.vendorEntry = 0;
        return false;
    }
    plan.outingActive = true;
    plan.arrivedMs = 0;

    Creature* vendor = nullptr;
    std::list<Unit*> units;
    Acore::AnyUnitInObjectRangeCheck check(bot, ITEM_TARGET_RANGE);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
    Cell::VisitObjects(bot, searcher, ITEM_TARGET_RANGE);
    for (Unit* unit : units)
        if (Creature* creature = unit->ToCreature())
            if (creature->GetEntry() == plan.vendorEntry && creature->IsAlive() && creature->IsVendor())
                vendor = creature;

    if (!vendor)
    {
        if (bot->GetExactDist2d(plan.vendorX, plan.vendorY) <= VENDOR_ARRIVE &&
            CortexTravel::SameRegion(bot, plan.vendorMap, plan.vendorX, plan.vendorY, plan.vendorZ, 0))
        {
            giveUp();
            return false;
        }
        bool stuck = false;
        bool moved = Travel(plan, plan.vendorMap, plan.vendorX, plan.vendorY, plan.vendorZ, stuck);
        if (stuck)
        {
            CortexQuests::Avoid(plan, plan.vendorMap, plan.vendorX, plan.vendorY);
            giveUp();
        }
        return moved;
    }
    if (!bot->IsWithinDistInMap(vendor, INTERACTION_DISTANCE - 1.0f))
        return MoveWorldObjectTo(vendor->GetGUID(), INTERACTION_DISTANCE - 1.0f);

    VendorItemData const* items = vendor->GetVendorItems();
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(plan.vendorItem);
    bool bought = false;
    for (uint32 slot = 0; items && proto && slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* entry = items->GetItem(slot);
        if (!entry || entry->item != plan.vendorItem)
            continue;
        // The count is in vendor stacks (BuyCount items each), and the return value only means something for
        // limited stock, so the bags tell whether it worked.
        uint32 stack = std::max<uint32>(1, proto->BuyCount);
        uint32 count = std::min<uint32>((need + stack - 1) / stack, 255);
        uint32 had = bot->GetItemCount(plan.vendorItem);
        bot->BuyItemFromVendorSlot(vendor->GetGUID(), slot, plan.vendorItem, static_cast<uint8>(count), NULL_BAG,
                                   NULL_SLOT);
        bought = bot->GetItemCount(plan.vendorItem) > had;
        BotTelemetry::instance().Event(botAI, "quest", bought ? "vendor_buy" : "vendor_cant",
                                       static_cast<int32>(plan.vendorItem), static_cast<int32>(count));
        break;
    }
    if (!bought)
        giveUp();
    else
        plan.vendorEntry = 0;
    plan.dirty = true;
    return bought;
}

bool CortexStepAction::PickUp(CortexPlan& plan, CortexMemory& memory)
{
    if (!plan.hasDest)
    {
        plan.dirty = true;
        return false;
    }
    if (bot->GetExactDist2d(plan.destX, plan.destY) > PICK_UP_ARRIVE ||
        !CortexTravel::SameRegion(bot, plan.destMap, plan.destX, plan.destY, plan.destZ, 0))
    {
        plan.arrivedMs = 0;
        bool stuck = false;
        bool moved = Travel(plan, plan.destMap, plan.destX, plan.destY, plan.destZ, stuck);
        if (stuck)
        {
            CortexQuests::Avoid(plan, plan.destMap, plan.destX, plan.destY);
            plan.dirty = true;
        }
        return moved;
    }

    uint32 now = getMSTime();
    if (!plan.arrivedMs)
        plan.arrivedMs = now;
    // Nothing to take here any more (TalkToQuestGivers would have found it): forget the "!", and the place for a
    // while, since the giver may still show a "!" for a quest the bot won't take.
    if (getMSTimeDiff(plan.arrivedMs, now) >= PICK_UP_WAIT_MS)
    {
        BotTelemetry::instance().Event(botAI, "pick_up", plan.revisit ? "revisit empty" : "marker empty");
        memory.ClearMarkersNear(plan.destMap, plan.destX, plan.destY, PICK_UP_ARRIVE,
                                static_cast<uint8>(bot->GetLevel()));
        CortexQuests::Avoid(plan, plan.destMap, plan.destX, plan.destY);
        plan.arrivedMs = now;
        plan.dirty = true;
    }
    return false;
}

bool CortexStepAction::Grind(CortexPlan& plan, CortexMemory& /*memory*/)
{
    uint32 now = getMSTime();
    if (static_cast<int32>(now - plan.nextScanMs) >= 0)
    {
        plan.nextScanMs = now + SCAN_MS;
        uint8 gray = Acore::XP::GetGrayLevel(static_cast<uint8>(bot->GetLevel()));
        auto givesXp = [gray](Creature* creature) { return creature->GetLevel() > gray; };
        if (Unit* target = FindHuntTarget(givesXp))
        {
            plan.step = "g:hunt";
            return Hunt(plan, target);
        }
    }

    if (plan.hasDest && (bot->GetExactDist2d(plan.destX, plan.destY) > GRIND_ARRIVE ||
                         !CortexTravel::SameRegion(bot, plan.destMap, plan.destX, plan.destY, plan.destZ, 0)))
    {
        plan.arrivedMs = 0;
        bool stuck = false;
        bool moved = Travel(plan, plan.destMap, plan.destX, plan.destY, plan.destZ, stuck);
        if (stuck)
        {
            CortexQuests::Avoid(plan, plan.destMap, plan.destX, plan.destY);
            plan.dirty = true;
        }
        return moved;
    }

    if (!plan.arrivedMs)
        plan.arrivedMs = now;
    if (getMSTimeDiff(plan.arrivedMs, now) >= GRIND_WAIT_MS)
    {
        plan.arrivedMs = now;
        plan.dirty = true;
        plan.step = plan.hasDest ? "g:waited" : "g:waited_nodest";
        return false;
    }
    // Nothing remembered to grind on, or nothing here now: look around, as a player would.
    bool moved = LookAround(60.0f);
    plan.step = moved ? (plan.hasDest ? "g:look" : "g:look_nodest") : (plan.hasDest ? "g:look_fail" : "g:look_fail_nodest");
    return moved;
}

bool CortexStepAction::LookAround(float range)
{
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        return false;
    Map* map = bot->GetMap();
    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const z = bot->GetPositionZ();
    for (uint32 attempt = 0; attempt < LOOK_ATTEMPTS; ++attempt)
    {
        float distance = (0.4f + rand_norm() * 0.6f) * range;
        float angle = rand_norm() * 2.0f * static_cast<float>(M_PI);
        float dx = x + distance * std::cos(angle);
        float dy = y + distance * std::sin(angle);
        float dz = CortexQuests::GroundZ(bot, dx, dy, z);
        if (dz <= INVALID_HEIGHT)
            continue;
        PathGenerator path(bot);
        path.CalculatePath(dx, dy, dz);
        // A whole path, not one that winds far round to get there. A swimmer has none (not using paths): straight
        // on, out of the water.
        PathType type = path.GetPathType();
        bool swimming = bot->IsInWater() && (type & PATHFIND_NOT_USING_PATH);
        if (!swimming && (type != PATHFIND_NORMAL || path.getPathLength() > distance * LOOK_WINDING + 10.0f))
            continue;
        G3D::Vector3 const& end = path.GetActualEndPosition();
        if (map->IsInWater(bot->GetPhaseMask(), end.x, end.y, end.z, bot->GetCollisionHeight()))
            continue;
        if (MoveTo(bot->GetMapId(), end.x, end.y, end.z, false, false, false, true))
            return true;
    }
    return false;
}

bool CortexStepAction::CanPull()
{
    // Resurrection Sickness after a spirit healer: a player doesn't pull at a quarter of its strength, it waits it
    // out (or travels, hands in, shops) first.
    constexpr uint32 RESURRECTION_SICKNESS = 15007;
    if (bot->IsInCombat() || botAI->rpgInfo.recoverSinceMs || bot->HasAura(RESURRECTION_SICKNESS))
        return false;
    return bot->GetHealthPct() >= sPlayerbotAIConfig.NewRpgRecoverHealthPct &&
           ManaPct(bot) >= sPlayerbotAIConfig.NewRpgRecoverManaPct;
}

// The nearest visible creature that `wanted` accepts and a player would pull: attackable, not someone else's
// fight or tag, at most two levels up (no elites unless three below), and not standing in a pack.
Unit* CortexStepAction::FindHuntTarget(std::function<bool(Creature*)> const& wanted)
{
    // Too hurt to pull counts as having seen something: the area isn't judged empty while the bot recovers.
    sawWanted = !CanPull();
    if (sawWanted)
        return nullptr;

    std::list<Unit*> units;
    Acore::AnyUnitInObjectRangeCheck check(bot, HUNT_RANGE);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
    Cell::VisitObjects(bot, searcher, HUNT_RANGE);

    uint32 level = bot->GetLevel();
    Creature* best = nullptr;
    float bestDist = HUNT_RANGE;
    for (Unit* unit : units)
    {
        Creature* creature = unit->ToCreature();
        if (!creature || creature->IsPet() || creature->IsTotem() || creature->IsGuardian() || creature->IsCritter())
            continue;
        if (!wanted(creature))
            continue;
        // A corpse counts too: someone else got there first, so the area isn't empty, just contested.
        sawWanted = true;
        if (!creature->IsAlive())
            continue;
        float d = bot->GetExactDist(creature);
        if (d >= bestDist || !bot->IsValidAttackTarget(creature))
            continue;
        if (creature->IsInCombat() && creature->GetVictim() != bot)
            continue;
        if (creature->hasLootRecipient() && !creature->isTappedBy(bot))
            continue;
        if (creature->GetLevel() > level + LEVELS_ABOVE || (creature->isElite() && creature->GetLevel() + 3 > level))
            continue;

        uint32 pack = 0;
        for (Unit* other : units)
            if (other != creature && other->IsAlive() && !other->IsInCombat() && other->IsHostileTo(bot) &&
                other->GetExactDist(creature) <= PACK_RANGE)
                ++pack;
        if (pack > PACK_LIMIT || !bot->IsWithinLOSInMap(creature))
            continue;

        best = creature;
        bestDist = d;
    }
    return best;
}

bool CortexStepAction::Hunt(CortexPlan& plan, Unit* target)
{
    plan.huntTarget = target->GetGUID();
    return botAI->DoSpecificAction("cortex attack", Event(), true);
}

bool CortexStepAction::MoveToGround(CortexPlan& plan, uint32 map, float x, float y, bool& stuck, uint32 mapArea)
{
    // Another map's ground isn't known from here; the trip there doesn't need it.
    float z = map == bot->GetMapId() ? CortexQuests::GroundZ(bot, x, y, bot->GetPositionZ()) : INVALID_HEIGHT;
    return Travel(plan, map, x, y, z, stuck, mapArea);
}

// Walks toward a place and sets `stuck` once three minutes of trying haven't brought the bot 10 yd closer. Time
// spent fighting, dead or on an errand doesn't count. A place walking can't reach (another map, or another region:
// an island, a city up a lift) or that a known flight reaches much sooner gets a trip (CortexTravel) first, and so
// does a walk that got stuck; only when there is no trip, or the trip fails, is it `stuck`.
bool CortexStepAction::Travel(CortexPlan& plan, uint32 map, float x, float y, float z, bool& stuck, uint32 mapArea)
{
    uint32 now = getMSTime();
    stuck = false;
    plan.travelDanger = false;
    plan.SetStepTarget(map, x, y, z);
    CortexTrip& trip = plan.trip;
    bool tripTarget = trip.map == map && Dist2d(trip.x, trip.y, x, y) <= TRAVEL_SAME_TARGET;

    auto giveUp = [&]()
    {
        plan.step = "t:give_up";
        trip.failed = false;
        stuck = true;
        plan.travelMap = UINT32_MAX;
        return false;
    };
    auto runTrip = [&]()
    {
        bool failed = false;
        bool moved = TripStep(plan, failed);
        plan.travelSeenMs = now;
        plan.travelAwayMs = Cortex::AwayMs(bot) + Cortex::ErrandMs(bot);
        plan.travelMs = now;
        if (failed)
            return giveUp();
        return moved;
    };
    // The way there is one to avoid and no trip goes round it: the caller sets the target aside until the bot
    // levels. Standing among mobs well above it, it first turns back to where it last walked free of them.
    auto dangerGiveUp = [&](char const* what, float d)
    {
        BotTelemetry::instance().Event(botAI, "travel_fail", std::string(what) + " " + CortexQuests::GoalName(plan.goal),
                                       static_cast<int32>(map), CortexTravel::OutOfDepth(bot, SAFE_RADIUS) ? 1 : 0,
                                       static_cast<int32>(d));
        if (plan.safeMap == bot->GetMapId() && bot->GetExactDist2d(plan.safeX, plan.safeY) > RETREAT_ARRIVE &&
            CortexTravel::OutOfDepth(bot, SAFE_RADIUS))
        {
            plan.retreating = true;
            plan.retreatMs = now;
            BotTelemetry::instance().Event(botAI, "retreat", "start", 0,
                                           static_cast<int32>(bot->GetExactDist2d(plan.safeX, plan.safeY)));
        }
        plan.travelDanger = true;
        return giveUp();
    };

    // A trip somewhere else is dropped, unless the bot is at the pier or aboard (the ride action finishes that).
    if (trip.active && !tripTarget)
    {
        if (CortexTravel::Busy(trip))
            return false;
        trip.active = false;
    }
    // The ride action gave up on the trip to here.
    if (!trip.active && trip.failed && tripTarget)
        return giveUp();
    if (trip.active)
    {
        plan.step = "t:trip";
        bool moved = runTrip();
        if (stuck || trip.active)
            return moved;
    }
    // The last leg is done (here or by the ride action): the bot walks from here, after a fresh look at the target
    // (off a zeppelin, a lift may still be needed).
    if (trip.finished)
    {
        trip.finished = false;
        if (tripTarget)
            plan.travelMap = UINT32_MAX;
    }

    bool otherMap = map != bot->GetMapId();
    // With a height, progress counts in 3D: a turn-in on the floor below is reached by walking down, not across.
    float d = otherMap ? 0.0f : (z > INVALID_HEIGHT ? bot->GetExactDist(x, y, z) : bot->GetExactDist2d(x, y));
    uint32 awayMs = Cortex::AwayMs(bot) + Cortex::ErrandMs(bot);
    if (plan.travelMap != map || Dist2d(plan.travelX, plan.travelY, x, y) > TRAVEL_SAME_TARGET)
    {
        if (!tripTarget)
            plan.tripsForTarget = 0;
        plan.travelMap = map;
        plan.travelX = x;
        plan.travelY = y;
        plan.travelBest = d;
        plan.travelMs = now;

        if (CortexTravel::Enabled() && plan.tripsForTarget < TRIPS_PER_TARGET)
        {
            bool split = otherMap || !CortexTravel::SameRegion(bot, map, x, y, z, mapArea);
            plan.travelSplit = split;
            bool far = !otherMap && (CortexTravel::Flights() || CortexTravel::Hearth()) && d >= FLIGHT_MIN_DISTANCE;
            // The way there passes where it died lately to something well above it, or through a zone well above
            // it (B3): another way, or not now.
            bool danger = !split && !plan.retreating &&
                          CortexTravel::DangerousWalk(bot, bot->GetPositionX(), bot->GetPositionY(), x, y);
            plan.dangerCheckMs = now;
            if ((split || far || danger) &&
                StartTrip(plan, map, x, y, z, mapArea, !split && !danger, split ? "split" : (danger ? "danger" : "far")))
                return runTrip();
            if (danger)
                return dangerGiveUp("danger", d);
            if (split)
            {
                BotTelemetry::instance().Event(botAI, "travel_fail", "noplan", static_cast<int32>(map),
                                               static_cast<int32>(CortexTravel::Region(map, x, y, z, mapArea)),
                                               static_cast<int32>(d));
                return giveUp();
            }
        }
        else if (otherMap)
            return giveUp();
    }
    // Time away (fighting, dead, recovering, on an errand) doesn't count; the gap between steps doesn't tell it, a
    // bot's own moves set its next AI check seconds out.
    else
        plan.travelMs += awayMs - plan.travelAwayMs;
    plan.travelAwayMs = awayMs;
    plan.travelSeenMs = now;

    if (otherMap)
        return giveUp();

    // On the way: what it has seen since (mobs well above it ahead) can make the rest of the walk one to avoid.
    // Where it walks clear of them is where it turns back to if it must.
    if (CortexTravel::Enabled() && !plan.retreating && getMSTimeDiff(plan.dangerCheckMs, now) >= DANGER_CHECK_MS)
    {
        plan.dangerCheckMs = now;
        if (!CortexTravel::OutOfDepth(bot, SAFE_RADIUS))
        {
            plan.safeMap = bot->GetMapId();
            plan.safeX = bot->GetPositionX();
            plan.safeY = bot->GetPositionY();
        }
        if (CortexTravel::Recheck() && !plan.travelSplit && d > DANGER_CHECK_MIN &&
            CortexTravel::DangerousWalk(bot, bot->GetPositionX(), bot->GetPositionY(), x, y))
        {
            if (plan.tripsForTarget < TRIPS_PER_TARGET && StartTrip(plan, map, x, y, z, mapArea, false, "ahead"))
                return runTrip();
            return dangerGiveUp("ahead", d);
        }
    }

    if (d + TRAVEL_PROGRESS < plan.travelBest)
    {
        plan.travelBest = d;
        plan.travelMs = now;
    }
    stuck = getMSTimeDiff(plan.travelMs, now) >= TRAVEL_STUCK_MS;
    if (stuck)
    {
        plan.step = "t:stuck";
        // Walking got nowhere: a way round it (a flight, a lift, a boat) before the place counts as unreachable.
        if (CortexTravel::Enabled() && plan.tripsForTarget < TRIPS_PER_TARGET &&
            StartTrip(plan, map, x, y, z, mapArea, false, "stuck"))
        {
            stuck = false;
            plan.travelBest = d;
            return runTrip();
        }
        plan.travelMap = UINT32_MAX;
        return false;
    }

    // Playerbots' unstick inside MoveFarTo gives up on a place after a detour didn't help (it remembers the place
    // as unreachable), and a bot that couldn't move at all hearths or is moved to a graveyard. Cortex's own
    // watchdog never saw it, so the bot walked straight back, hearthed again, and so on. Now the place counts as
    // stuck: a way round it first, as above, unless the bot already left; then the spot is blocked in memory.
    // Up a lift from here (Thunder Bluff's mesas, Undercity): MoveFarTo gives up on such a place at once (bots
    // 0064) without marking anything new, so the bot asked again every tick. A lift trip, or the spot is stuck.
    if (BotLevels::LiftOnly(bot, map, x, y, z))
    {
        if (CortexTravel::Enabled() && plan.tripsForTarget < TRIPS_PER_TARGET &&
            StartTrip(plan, map, x, y, z, mapArea, false, "levels"))
        {
            plan.travelBest = d;
            plan.travelMs = now;
            return runTrip();
        }
        if (CortexMemory* memory = Cortex::Memory(bot))
        {
            uint32 count = 0;
            uint32 seconds = memory->BlockSpot(map, x, y, static_cast<uint32>(GameTime::GetGameTime().count()), count);
            BotTelemetry::instance().Event(botAI, "blocked", std::string(CortexQuests::GoalName(plan.goal)) + " lift",
                                           static_cast<int32>(seconds / MINUTE), static_cast<int32>(count),
                                           static_cast<int32>(d));
        }
        stuck = true;
        plan.travelMap = UINT32_MAX;
        return false;
    }

    NewRpgInfo& info = botAI->rpgInfo;
    uint32 lastGiveUp = info.unreachable.empty() ? 0 : info.unreachable.back().sinceMs;
    bool resting = info.GetStatus() == RPG_REST;
    bool moved = MoveFarTo(WorldPosition(map, x, y, z));
    plan.step = moved ? "t:walk" : "t:walk_fail";
    if (info.unreachable.empty() || info.unreachable.back().sinceMs == lastGiveUp)
        return moved;
    WorldPosition const& gaveUp = info.unreachable.back().pos;
    if (gaveUp.GetMapId() != map || Dist2d(gaveUp.GetPositionX(), gaveUp.GetPositionY(), x, y) > 1.0f)
        return moved;

    bool graveyard = bot->IsBeingTeleported();
    bool hearth = !graveyard && !resting && info.GetStatus() == RPG_REST;
    char const* how = graveyard ? "graveyard" : (hearth ? "hearth" : "give_up");
    if (!graveyard && !hearth && CortexTravel::Enabled() && plan.tripsForTarget < TRIPS_PER_TARGET &&
        StartTrip(plan, map, x, y, z, mapArea, false, "gave_up"))
    {
        plan.travelBest = d;
        plan.travelMs = now;
        return true;
    }
    if (CortexMemory* memory = Cortex::Memory(bot))
    {
        uint32 count = 0;
        uint32 seconds = memory->BlockSpot(map, x, y, static_cast<uint32>(GameTime::GetGameTime().count()), count);
        BotTelemetry::instance().Event(botAI, "blocked", std::string(CortexQuests::GoalName(plan.goal)) + " " + how,
                                       static_cast<int32>(seconds / MINUTE), static_cast<int32>(count),
                                       static_cast<int32>(d));
    }
    stuck = true;
    plan.travelMap = UINT32_MAX;
    return false;
}

// A flight master the bot passed whose flight point it doesn't know (Cortex perception picks it): a player walks
// over and talks to it, which learns the node.
bool CortexStepAction::LearnFlightPoint(CortexPlan& plan)
{
    if (!plan.learnFlightMaster)
        return false;
    uint32 now = getMSTime();
    Creature* master = ObjectAccessor::GetCreature(*bot, plan.learnFlightMaster);
    uint32 node = master ? sObjectMgr->GetNearestTaxiNode(master->GetPositionX(), master->GetPositionY(),
                                                          master->GetPositionZ(), master->GetMapId(),
                                                          bot->GetTeamId(true))
                         : 0;
    bool late = getMSTimeDiff(plan.learnSinceMs, now) >= LEARN_FLIGHT_MS;
    if (!master || !master->IsAlive() || !node || bot->m_taxi.IsTaximaskNodeKnown(node) || late ||
        CortexTravel::Busy(plan.trip))
    {
        if (late)
            plan.ignoredGivers[plan.learnFlightMaster] = now + GIVER_IGNORE_MS;
        plan.learnFlightMaster.Clear();
        return false;
    }
    if (!bot->IsWithinDistInMap(master, INTERACTION_DISTANCE - 1.0f))
        return MoveWorldObjectTo(master->GetGUID(), INTERACTION_DISTANCE - 1.0f);

    if (bot->isMoving())
        bot->StopMoving();
    bot->SetFacingToObject(master);
    bot->GetSession()->SendLearnNewTaxiNode(master);
    plan.learnFlightMaster.Clear();
    if (bot->m_taxi.IsTaximaskNodeKnown(node))
    {
        TaxiNodesEntry const* entry = sTaxiNodesStore.LookupEntry(node);
        BotTelemetry::instance().Event(botAI, "fp_learned", entry && entry->name[0] ? entry->name[0] : "",
                                       static_cast<int32>(node), static_cast<int32>(master->GetEntry()));
    }
    return true;
}

bool CortexAttackAction::isUseful() { return !bot->IsInCombat() && Cortex::Plan(bot); }

bool CortexAttackAction::Execute(Event /*event*/)
{
    CortexPlan* plan = Cortex::Plan(bot);
    if (!plan || !plan->huntTarget)
        return false;
    Unit* target = ObjectAccessor::GetUnit(*bot, plan->huntTarget);
    plan->huntTarget.Clear();
    if (!target || !target->IsAlive())
        return false;
    return Attack(target);
}
