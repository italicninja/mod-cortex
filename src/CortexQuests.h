/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 * Originally written as part of the wowproject patches to mod-playerbots.
 */

#ifndef MOD_CORTEX_CORTEXQUESTS_H
#define MOD_CORTEX_CORTEXQUESTS_H

#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "CortexTravel.h"
#include "Define.h"
#include "ObjectGuid.h"

class CortexMemory;
class Player;
class Quest;

// Cortex's quest plan (docs/bot-ai-overhaul.md §7b). Built only from what a player has: the quest log
// (objectives and counters), the quest map areas (quest_poi, as the client gets them) and the bot's own memory.

enum class CortexGoal : uint8
{
    NONE,
    HAND_IN,
    WORK,
    PICK_UP,
    GRIND,
};

enum class CortexTaskKind : uint8
{
    KILL,
    COLLECT,
    USE,
    EXPLORE,
    TURN_IN,
    USE_ITEM,  // use an item the quest gave (its "Use:" spell) on the objective, near a focus, or in the area
};

struct CortexArea
{
    std::vector<std::pair<float, float>> points;  // polygon, or one point
};

struct CortexTask
{
    uint32 questId = 0;
    int32 objective = 0;  // 0-3 creature/object, 4-9 item, -1 hand in
    CortexTaskKind kind = CortexTaskKind::KILL;
    int32 target = 0;  // creature entry, -object entry, or item id; USE_ITEM: creature, -object, or 0 (any)
    uint32 item = 0;   // USE_ITEM: the item and its spell
    uint32 spell = 0;
    bool buyable = false;  // COLLECT: a trade good or food and drink that some vendor sells
    bool underground = false;  // the quest text puts it in a mine, cave, den ...
    uint32 map = 0;
    float x = 0.0f;    // centre of the areas
    float y = 0.0f;
    uint32 mapArea = 0;  // the quest map the areas are drawn on (quest_poi.WorldMapAreaId): Undercity, Thunder Bluff ...
    bool remote = false;  // TURN_IN on another continent (CortexTravel::CrossMap)
    // TURN_IN: the height the quest ender stands at, as a player reads "in the Undercity" from the quest; the map
    // area alone can't tell the city from the ruins above it.
    bool hasZ = false;
    float z = 0.0f;
    std::vector<CortexArea> areas;
};

struct CortexSearchPoint
{
    float x = 0.0f;
    float y = 0.0f;
    bool searched = false;
    bool floorKnown = false;  // underground clusters: z is the floor below the surface, if there is one
    bool cave = false;
    float z = 0.0f;
};

struct CortexCluster
{
    uint32 map = 0;
    float x = 0.0f;
    float y = 0.0f;
    float radius = 0.0f;
    std::vector<uint32> tasks;  // indices into CortexPlan::tasks
    std::vector<CortexSearchPoint> points;
    bool underground = false;   // any of its tasks is; search points go to the floor below the surface
    uint32 mapArea = 0;         // of its first task
};

struct CortexAvoid
{
    uint32 map = 0;
    float x = 0.0f;
    float y = 0.0f;
    uint32 untilTime = 0;  // unix seconds
};

// The last quest item the bot used on a creature it picked by trial: judged once the spell has had time to work.
struct CortexItemUse
{
    uint32 questId = 0;
    uint32 entry = 0;
    uint32 progress = 0;
    uint32 judgeMs = 0;
};

struct CortexPlan
{
    CortexGoal goal = CortexGoal::NONE;
    std::vector<CortexTask> tasks;
    std::vector<CortexCluster> clusters;
    int32 cluster = -1;  // the cluster being worked
    int32 handIn = -1;   // the task being handed in

    bool dirty = true;
    uint32 nextThinkMs = 0;
    uint32 nextScanMs = 0;       // hunt, quest objects and progress
    uint32 nextGiverScanMs = 0;  // quest givers in reach; its own timer, or it would starve the hunt scan

    // Progress in the current cluster or hand-in, for set-aside decisions.
    uint32 progress = 0;
    uint32 progressMs = 0;
    uint32 arrivedMs = 0;
    uint32 searchResetMs = 0;
    uint8 clusterDeaths = 0;
    // Back from a death (CortexRegroupAction): when, and where it died; 0 once regrouped or after REGROUP_MS.
    uint32 revivedMs = 0;
    float deathX = 0.0f, deathY = 0.0f;

    ObjectGuid huntTarget;
    ObjectGuid interactTarget;
    ObjectGuid useTarget;
    ObjectGuid itemTarget;  // walking to the creature or object a quest item is for

    // Where the current outing goes; a re-plan to the same goal and place keeps the timers above.
    float outingX = 0.0f;
    float outingY = 0.0f;

    // Travel watchdog: the closest the bot has come to the travel target, and when.
    uint32 travelMap = 0;
    float travelX = 0.0f;
    float travelY = 0.0f;
    float travelBest = 0.0f;
    uint32 travelMs = 0;
    uint32 travelSeenMs = 0;
    uint32 travelAwayMs = 0;  // Cortex::AwayMs + ErrandMs at the last step
    // The way to the travel target is one to avoid (CortexTravel::DangerousWalk) and there's no other: set when
    // Travel gives up for that, so the caller sets the quest aside until the bot levels rather than drop it.
    bool travelDanger = false;
    bool travelSplit = false;  // the target is on another map or in another region: no straight walk to look at
    uint32 dangerCheckMs = 0;  // last look at the rest of the walk
    // The last place the bot walked through with no mobs well above it around: where it turns back to when it
    // finds itself among them with nowhere safe to go on to.
    uint32 safeMap = UINT32_MAX;
    float safeX = 0.0f;
    float safeY = 0.0f;
    bool retreating = false;
    uint32 retreatMs = 0;

    // Flights, boats, lifts and portals toward the travel target (CortexTravel). Trips planned for the current
    // target: a few at most, so a trip that ends where walking fails again doesn't loop.
    CortexTrip trip;
    uint8 tripsForTarget = 0;
    // Directed links (boat, lift, portal) that failed for this bot lately, and until when (getMSTime): left out of
    // every trip planned meanwhile, not just the one that failed, so a lift it can't board doesn't loop.
    std::vector<std::pair<uint16, uint32>> badLinks;
    // A flight master passed by whose flight point the bot doesn't know yet (Cortex.Travel.LearnFlights).
    ObjectGuid learnFlightMaster;
    uint32 learnSinceMs = 0;

    uint32 interactSinceMs = 0;
    std::unordered_map<ObjectGuid, uint32> ignoredGivers;  // quest givers it couldn't reach, until getMSTime
    std::vector<CortexAvoid> avoid;                         // pick-up and grind spots it couldn't reach

    // PICK_UP / GRIND destination
    uint32 destMap = 0;
    float destX = 0.0f;
    float destY = 0.0f;
    float destZ = 0.0f;
    bool hasDest = false;
    bool revisit = false;  // PICK_UP: back to givers last seen at a lower level, no "!" known (Cortex.Revisit)

    // Patience: a bot still killing in the area keeps at it past the no-progress timer, up to a cap.
    uint32 lastKillMs = 0;
    // Something to do was seen on this outing (a target, an object, a vendor errand): an area swept without any
    // is set aside at once instead of after the no-progress timer.
    bool outingActive = false;

    // Vendor errand for a buyable COLLECT item: the remembered vendor being visited.
    uint32 vendorEntry = 0;
    uint32 vendorItem = 0;
    uint32 vendorMap = 0;
    float vendorX = 0.0f;
    float vendorY = 0.0f;
    float vendorZ = 0.0f;
    std::unordered_set<uint64> vendorLacks;  // (vendor entry << 32) | item it was found not to sell
    std::unordered_map<uint32, uint8> vendorTries;  // item -> vendors tried for it
    bool swept = false;  // the current outing's area has been searched end to end once

    // Quest sparkles (B4d): when the walk to the object in sight started, and the remembered one being walked back
    // to (CortexMemory object key, entry, spot). Remembered sparkles found gone, out of reach or just used are
    // skipped until getMSTime (object key -> time).
    uint32 useSinceMs = 0;
    uint64 sparkleKey = 0;
    uint32 sparkleEntry = 0;
    uint32 sparkleMap = 0;
    float sparkleX = 0.0f;
    float sparkleY = 0.0f;
    float sparkleZ = 0.0f;
    uint32 sparkleSinceMs = 0;
    std::unordered_map<uint64, uint32> sparkleSkip;

    CortexItemUse itemUse;

    // What the step (or the needs action) did last and where it was headed, for the watchdog's "frozen" rows:
    // string literals, and the target (stepMap UINT32_MAX: none).
    char const* step = "";
    char const* stepDetail = "";
    uint32 stepMap = UINT32_MAX;
    float stepX = 0.0f;
    float stepY = 0.0f;
    float stepZ = 0.0f;
    void SetStepTarget(uint32 map, float x, float y, float z)
    {
        stepMap = map;
        stepX = x;
        stepY = y;
        stepZ = z;
    }
    std::unordered_map<ObjectGuid, uint32> usedOn;  // creatures and objects a quest item was used on, until getMSTime
    std::unordered_set<uint64> wrongTargets;       // (quest << 32) | creature entry the item did nothing on

    // Taming (hunters): the beast picked, when the walk to it started, whether Tame Beast was cast on it.
    ObjectGuid tameTarget;
    uint32 tameEntry = 0;
    uint32 tameSinceMs = 0;
    bool tameCast = false;
    uint32 nextTameScanMs = 0;
    uint8 tameFails = 0;                              // in a row; a few and it waits a while
    std::unordered_map<ObjectGuid, uint32> tameSkip;  // beasts that got away or broke the channel, until getMSTime
    // Pet watch (telemetry only): next look, since when the pet has been away out of combat, last report.
    uint32 nextPetCheckMs = 0;
    uint32 petAwaySinceMs = 0;
    uint32 petReportMs = 0;

    // Gathering trips (Cortex.Gather): the remembered node being walked to, when the walk started, when the last
    // trip ended, and remembered nodes found empty or out of reach (CortexMemory object key -> until getMSTime).
    uint32 gatherEntry = 0;
    uint32 gatherSkill = 0;
    uint32 gatherMap = 0;
    float gatherX = 0.0f;
    float gatherY = 0.0f;
    float gatherZ = 0.0f;
    uint64 gatherKey = 0;
    uint32 gatherSinceMs = 0;
    uint32 gatherEndMs = 0;
    bool gatherSeen = false;  // the node was up when the bot came in sight of the spot
    uint32 nextGatherScanMs = 0;
    std::unordered_map<uint64, uint32> gatherSkip;
};

namespace CortexQuests
{
    // Reads the log into tasks and clusters and picks the goal.
    void Build(Player* bot, CortexPlan& plan, CortexMemory const& memory);
    // Sum of the counters of the cluster's tasks (kills, items, objects), to notice progress.
    uint32 Progress(Player* bot, CortexPlan const& plan, CortexCluster const& cluster);
    bool ClusterHasCompletedTask(Player* bot, CortexPlan const& plan, CortexCluster const& cluster);
    bool InArea(CortexTask const& task, float x, float y, float margin);
    // Class quests (trainer chains such as Taming the Beast) come first and are never abandoned, only set aside.
    bool IsClassQuest(Quest const* quest);
    // Sets a quest aside. Reason "reward" (at the turn-in, nobody to take it) twice drops a non-class quest for good.
    void Shelve(Player* bot, CortexPlan& plan, uint32 questId, char const* reason);
    // Another spawn of the turn-in's ender on its map, away from the turn-in area and not a place the bot got stuck.
    bool OtherEnder(CortexTask const& task, CortexMemory const& memory, float& x, float& y, float& z);
    bool IsShelved(Player* bot, CortexPlan const& plan, uint32 questId);
    // A place the bot couldn't reach: skipped as a pick-up or grind spot for a while.
    void Avoid(CortexPlan& plan, uint32 map, float x, float y);
    bool IsAvoided(CortexPlan const& plan, uint32 map, float x, float y);
    // The place the current goal leads to, to tell a new outing from a re-plan of the same one.
    void OutingPlace(CortexPlan const& plan, float& x, float& y);
    char const* GoalName(CortexGoal goal);
    // A real client tells the server when it walks into an area trigger, and explore objectives complete only on
    // that. Cortex does the same for the explore objectives in its log (client data: AreaTrigger.dbc).
    void TouchAreaTriggers(Player* bot);
    // Whether this vendor sells the item for gold. A player reads it off the vendor's title ("Trade Supplies") or
    // from having browsed it; the vendor list stands in for both.
    bool VendorSells(uint32 vendorEntry, uint32 item);
    // Height of the walkable floor nearest below refZ + 30 at (x, y), or the top surface as a fallback.
    float GroundZ(Player* bot, float x, float y, float refZ);
}

#endif
