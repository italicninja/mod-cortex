/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgBaseAction.h"
#include <unordered_map>
#include <mutex>
#include "Bag.h"
#include "GameGraveyard.h"
#include "BotDanger.h"
#include "BotLevels.h"
#include "BotRoutes.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "Config.h"
#include "Creature.h"
#include "GameObject.h"
#include "GatherNodeMgr.h"
#include "GossipDef.h"
#include "GridTerrainData.h"
#include "IVMapMgr.h"
#include "MotionMaster.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "OutdoorPvPMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "PlayerbotsExtensions.h"
#include "Playerbots.h"
#include "Position.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "StatsWeightCalculator.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "G3D/Vector2.h"

namespace
{
constexpr uint32 ITEM_HEARTHSTONE = 6948;
constexpr uint32 SPELL_HEARTHSTONE = 8690;
// Walk unstick: a destination at least this far off (beyond what one partial path covers) that the bot gave up on
// while within TRAPPED_RADIUS of where its detour started counts as a dead end (a cave, a hollow) to hearth out of.
constexpr float TRAPPED_MIN_DISTANCE = 300.0f;
constexpr float TRAPPED_RADIUS = 100.0f;
// Travel node routes (Playerbots.NewRpgRoutes) are only worth it past what one mmap path covers.
constexpr float ROUTE_MIN_DISTANCE = 150.0f;
// Walk unstick: stuck within this many yards (2D) of the destination counts as there.
constexpr float NEAR_2D = 40.0f;
// Grind / camp selection skips 100 yd cells with this many recent deaths of similar-level bots (BotDanger::Heat).
constexpr uint32 HOT_CELL_DEATHS = 4;
// Stuck in place: this many give-ups within STUCK_PLACE_RADIUS yards in STUCK_PLACE_MS. The bot hearths, or with
// the hearthstone on cooldown is moved to the nearest graveyard, as the game's "Stuck" help option does.
constexpr uint32 ZONE_THUNDER_BLUFF = 1638;
constexpr uint32 ZONE_UNDERCITY = 1497;
constexpr uint32 STUCK_PLACE_GIVE_UPS = 3;
constexpr float STUCK_PLACE_RADIUS = 60.0f;
constexpr uint32 STUCK_PLACE_MS = 20 * MINUTE * IN_MILLISECONDS;

struct GiveUp
{
    uint32 map;
    float x, y, z;
    uint32 ms;
};
std::mutex giveUpMutex;
std::unordered_map<ObjectGuid::LowType, std::vector<GiveUp>> giveUps;

// Notes a give-up here; true when it is the STUCK_PLACE_GIVE_UPS-th nearby one (and forgets them).
bool StuckInPlace(Player* bot)
{
    std::lock_guard<std::mutex> lock(giveUpMutex);
    std::vector<GiveUp>& list = giveUps[bot->GetGUID().GetCounter()];
    list.erase(std::remove_if(list.begin(), list.end(),
                              [](GiveUp const& g) { return GetMSTimeDiffToNow(g.ms) >= STUCK_PLACE_MS; }),
               list.end());
    list.push_back({bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), getMSTime()});

    uint32 near = 0;
    for (GiveUp const& g : list)
    {
        if (g.map == bot->GetMapId() && bot->GetExactDist(g.x, g.y, g.z) < STUCK_PLACE_RADIUS)
            ++near;
    }
    if (near < STUCK_PLACE_GIVE_UPS)
        return false;

    list.clear();
    return true;
}

// Steep escapes (Playerbots.NewRpgSteepEscape). Bots path with NAV_GROUND_STEEP left out (core
// PathGenerator::CreateFilter keeps them off 50-60 degree mountainsides), but the extractor tags stairs and some
// ledges steep too: the spiral stair of Orgrimmar's flight tower is every other poly steep, so a bot a flight sets
// down on top has no path off it and one at the foot none up to the flight master (336 detours and 104 hearths on
// the tower in a day), and pockets on Teldrassil's rim hold bots for good. After a few MoveFarTo calls without
// progress the bot paths again with steep ground allowed at a cost and follows that path as given.
constexpr uint32 STEEP_ESCAPE_ATTEMPTS = 2;
constexpr float STEEP_ESCAPE_COST = 4.0f;
constexpr float STEEP_ESCAPE_PROGRESS = 5.0f;

bool SteepEscapeEnabled()
{
    static bool const enabled = sConfigMgr->GetOption<bool>("Playerbots.NewRpgSteepEscape", true);
    return enabled;
}

// The steep-allowed path toward (x, y, z), or empty when it doesn't end STEEP_ESCAPE_PROGRESS closer than the bot is.
Movement::PointsArray SteepEscapePath(Player* bot, float x, float y, float z)
{
    PathGenerator path(bot);
    path.SetExcludeFlags(NAV_MAGMA | NAV_SLIME);
    path.SetNavTerrainCost(NAV_GROUND_STEEP, STEEP_ESCAPE_COST);
    path.CalculatePath(x, y, z);
    uint32 const typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
    if (path.GetPathType() & ~typeOk)
        return {};
    Movement::PointsArray const& points = path.GetPath();
    if (points.size() < 2)
        return {};
    G3D::Vector3 const& end = points.back();
    float before = bot->GetExactDist(x, y, z);
    float after = (end - G3D::Vector3(x, y, z)).length();
    if (after + STEEP_ESCAPE_PROGRESS >= before)
        return {};
    return points;
}
}  // namespace

// Class quests (Taming the Beast, a warrior's or warlock's trainer chain) teach spells: a bot never abandons
// one it can still finish, only sets it aside. Failed ones are still dropped so they can be taken again.
bool KeepsQuest(Quest const* quest) { return quest && quest->GetRequiredClasses(); }

bool NewRpgBaseAction::MoveFarTo(WorldPosition dest)
{
    if (dest == WorldPosition())
        return false;

    // Don't start a ground move while the bot is on a taxi: the MovePoint issued
    // below would fight the active flight spline. Returning true consumes the tick
    // instead of returning false, because the callers of MoveFarTo fall through to
    // MoveRandomNear, which is also a ground move.
    if (bot->IsInFlight())
        return true;

    // A quest, grind spot or camp where the bot keeps dying (Playerbots.NewRpgDangerZones): pick something else
    {
        NewRpgStatus status = botAI->rpgInfo.GetStatus();
        if ((status == RPG_DO_QUEST || status == RPG_GO_GRIND || status == RPG_GO_CAMP) &&
            BotDanger::IsDangerous(bot, dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(),
                                   dest.GetPositionZ()))
        {
            uint32 questId = 0;
            if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data))
                questId = quest->questId;

            BotTelemetry::instance().Event(botAI, "danger", "avoid", static_cast<int32>(status),
                                           static_cast<int32>(questId));
            if (questId)
                ShelveQuest(questId, "shelve_danger");
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
    }

    // Another level of the map (Playerbots.NewRpgLevels): across Teldrassil's portals the bot walks to the portal and
    // steps in (as a client tells the server it walked into the trigger); up a lift it gives up on the place at once
    // instead of walking to the foot of the mesa and hearthing from there.
    if (BotLevels::Enabled())
    {
        float dx = dest.GetPositionX();
        float dy = dest.GetPositionY();
        float dz = dest.GetPositionZ();
        if (uint32 portal = BotLevels::Portal(bot, dest.GetMapId(), dx, dy, dz))
        {
            AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(portal);
            if (trigger && trigger->map == bot->GetMapId())
            {
                if (bot->IsInAreaTriggerRadius(trigger))
                {
                    if (bot->isMoving())
                        bot->StopMoving();
                    BotTelemetry::instance().Event(botAI, "levels", "portal", static_cast<int32>(portal),
                                                   static_cast<int32>(bot->GetExactDist2d(dx, dy)));
                    WorldPacket packet(CMSG_AREATRIGGER, 4);
                    packet << trigger->entry;
                    bot->GetSession()->HandleAreaTriggerOpcode(packet);
                    return true;
                }
                dest = WorldPosition(trigger->map, trigger->x, trigger->y, trigger->z);
            }
        }
        else if (BotLevels::LiftOnly(bot, dest.GetMapId(), dx, dy, dz))
        {
            NewRpgInfo& info = botAI->rpgInfo;
            uint32 questId = 0;
            if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data))
                questId = quest->questId;
            BotTelemetry::instance().Event(botAI, "levels", "lift_only",
                                           static_cast<int32>(bot->GetExactDist(dx, dy, dz)),
                                           static_cast<int32>(BotLevels::Of(dest.GetMapId(), dx, dy, dz)),
                                           static_cast<int32>(questId));
            // Remembered as unreachable like a walk-unstick give-up: trips (trainer, auction) end on it, and Cortex
            // plans its lift trip when it sees it.
            info.MarkUnreachable(dest);
            BotRoutes::Forget(bot);
            if (questId)
                ShelveQuest(questId, "shelve_lift");
            info.ChangeToIdle();
            return true;
        }
    }

    bool newDest = dest != botAI->rpgInfo.moveFarPos;
    if (newDest)
    {
        // clear stuck information if it's a new dest
        botAI->rpgInfo.SetMoveFarTo(dest);
    }

    // performance optimization
    if (!newDest && IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        return false;
    }

    // Let previously committed movement finish before recomputing.
    //
    // MoveTo internally caps its stored delay at maxWaitForMove
    // (default 5s), but a long path (200+ yd routed around a
    // mountain) takes 30+ seconds to walk. After 5s
    // IsWaitingForLastMove returns false and MoveFarTo re-enters.
    // Without this gate, DoMovePoint would call mm->Clear() and
    // reissue MovePoint from the new bot position — and from a new
    // position mmap's partial-path endpoint often differs, so the
    // bot gets clobbered mid-walk and ends up oscillating (e.g.
    // cave entrance -> inside cave -> cave entrance -> mountain
    // base -> cave entrance...) around an unreachable destination.
    //
    // If the bot is still actively walking toward its last
    // committed point on the same map, just let the current spline
    // finish. The stuck counter below continues to track real
    // progress toward dest and triggers teleport recovery if the
    // committed paths genuinely aren't closing the gap.
    //
    // A changed destination (e.g. the gather state switching to a
    // live node it passes, or a quest POI change) skips this gate:
    // letting the old spline finish would keep walking the bot
    // toward a target it no longer has.
    if (!newDest)
    {
        LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
        if (bot->isMoving() && lastMove.lastMoveToMapId == bot->GetMapId())
        {
            float remaining = bot->GetExactDist(lastMove.lastMoveToX, lastMove.lastMoveToY, lastMove.lastMoveToZ);
            if (remaining > 10.0f)
                return true;
        }
    }

    // stuck check
    float disToDest = bot->GetDistance(dest);
    if (sPlayerbotAIConfig.NewRpgWalkUnstick && botAI->rpgInfo.stuckPos != WorldPosition())
    {
        WorldPosition here(bot);
        botAI->rpgInfo.stuckRoam = std::max(botAI->rpgInfo.stuckRoam, here.distance(botAI->rpgInfo.stuckPos));
    }
    // Require a meaningful improvement (5yd) to reset the stuck counter.
    // The old 1yd threshold was small enough that bots oscillating back
    // and forth around an obstacle would keep "making progress" forever
    // and never trigger the teleport recovery below.
    if (disToDest + 5.0f < botAI->rpgInfo.nearestMoveFarDis)
    {
        botAI->rpgInfo.nearestMoveFarDis = disToDest;
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;
    }
    else if (++botAI->rpgInfo.stuckAttempts >= 5 && GetMSTimeDiffToNow(botAI->rpgInfo.stuckTs) >= stuckTime)
    {
        if (sPlayerbotAIConfig.NewRpgWalkUnstick)
            return Unstick(dest, disToDest);

        // No meaningful progress toward dest for `stuckTime`: fall
        // back to teleporting directly so the bot can get on with
        // its RPG objective instead of oscillating indefinitely.
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;
        AreaTableEntry const* entry = sAreaTableStore.LookupEntry(bot->GetZoneId());
        std::string zone_name = PlayerbotAI::GetLocalizedAreaName(entry);
        LOG_DEBUG(
            "playerbots",
            "[New RPG] Teleport {} from ({},{},{},{}) to ({},{},{},{}) as it stuck when moving far - Zone: {} ({})",
            bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId(),
            dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), dest.GetMapId(), bot->GetZoneId(),
            zone_name);
        BotTelemetry::instance().Event(botAI, "stuck_tp", {}, static_cast<int32>(disToDest));
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        return bot->TeleportTo(dest);
    }

    float dis = bot->GetExactDist(dest);

    // No progress for a few calls: maybe the bot stands where its own mesh has no way out (steep stairs), see
    // SteepEscapePath. The path is followed as given, as a spline; the bot's own pathing would stop at the stairs.
    bool escape = SteepEscapeEnabled() && botAI->rpgInfo.stuckAttempts >= STEEP_ESCAPE_ATTEMPTS;
    auto steepEscape = [&](float tx, float ty, float tz) -> bool
    {
        Movement::PointsArray points = SteepEscapePath(bot, tx, ty, tz);
        if (points.empty())
            return false;
        float length = 0.0f;
        for (size_t i = 1; i < points.size(); ++i)
            length += (points[i] - points[i - 1]).length();
        G3D::Vector3 const end = points.back();
        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        bot->GetMotionMaster()->MoveSplinePath(&points, FORCED_MOVEMENT_RUN);
        float delay = std::min(static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove),
                               static_cast<float>(IN_MILLISECONDS) * MoveDelay(length));
        AI_VALUE(LastMovement&, "last movement")
            .Set(bot->GetMapId(), end.x, end.y, end.z, bot->GetOrientation(), delay, MovementPriority::MOVEMENT_NORMAL);
        BotTelemetry::instance().Event(botAI, "unstick", "steep", static_cast<int32>(disToDest),
                                       static_cast<int32>(length), static_cast<int32>(points.size()));
        return true;
    };

    if (dis < pathFinderDis)
    {
        if (escape && steepEscape(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ()))
            return true;
        return MoveTo(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), false, false,
                      false, true);
    }

    const uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;

    // Primary strategy: ask mmap for a route to the TRUE destination.
    // If mmap can reach it directly (PATHFIND_NORMAL) or partially
    // (PATHFIND_INCOMPLETE — destinations beyond the smooth-path cap
    // of ~296 yards, or where local geometry blocks the final step),
    // walk to the furthest reachable waypoint mmap computed. This
    // lets bots follow the real route around obstacles (mountains,
    // cave walls, cliffs) instead of trying to cut straight through.
    // The spline system walks the whole returned path smoothly, so
    // subsequent ticks early-out via IsWaitingForLastMove and no
    // further PathGenerator calls fire until the bot arrives.
    {
        PathGenerator path(bot);
        path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        PathType type = path.GetPathType();
        bool canReach = !(type & (~typeOk));
        bool complete = (type & PATHFIND_NORMAL) && !(type & PATHFIND_INCOMPLETE);

        // mmap can't get all the way: follow the travel node route instead of walking into the dead end its
        // partial path ends at. Reaching each waypoint counts as progress, since a route may head away at first.
        BotRoutes::Point waypoint;
        bool advanced = false;
        if (!complete && dis >= ROUTE_MIN_DISTANCE &&
            BotRoutes::NextWaypoint(bot, dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), waypoint,
                                    advanced))
        {
            if (advanced)
            {
                botAI->rpgInfo.nearestMoveFarDis = disToDest;
                botAI->rpgInfo.stuckTs = getMSTime();
                botAI->rpgInfo.stuckAttempts = 0;
            }
            // The waypoint may be off the level the bot stands on (the foot of the flight tower).
            if (escape && steepEscape(waypoint.x, waypoint.y, waypoint.z))
                return true;
            return MoveTo(bot->GetMapId(), waypoint.x, waypoint.y, waypoint.z, false, false, false, true);
        }

        if (canReach)
        {
            G3D::Vector3 const& endPos = path.GetActualEndPosition();
            // Only commit if the mmap endpoint actually makes progress
            // toward the destination. For pathological INCOMPLETE
            // results (e.g. disconnected polys that still report
            // INCOMPLETE) the endpoint can land right under the bot;
            // fall through to cone sampling in that case.
            float endDistToDest = dest.GetExactDist(endPos.x, endPos.y, endPos.z);
            if (endDistToDest + 5.0f < disToDest)
            {
                return MoveTo(bot->GetMapId(), endPos.x, endPos.y, endPos.z, false, false, false, true);
            }
        }

        if (escape)
        {
            if (steepEscape(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ()))
                return true;
            // A far destination's tile may not be loaded (no path at all): toward a point that way instead.
            float angle = bot->GetAngle(&dest);
            if (steepEscape(bot->GetPositionX() + std::cos(angle) * pathFinderDis,
                            bot->GetPositionY() + std::sin(angle) * pathFinderDis, bot->GetPositionZ()))
                return true;
        }
    }

    // Fallback: mmap couldn't route to the destination. Sample the
    // forward cone for a reachable stepping stone so the bot keeps
    // moving and can try again from a new vantage point. Cap at 2
    // samples — we already spent one PathGenerator call above and at
    // 3000 bots every extra CalculatePath matters.
    float minDelta = M_PI;
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    const float baseAngle = bot->GetAngle(&dest);
    float rx, ry, rz;
    bool found = false;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        float delta = (rand_norm() - 0.5f) * static_cast<float>(M_PI);  // ±π/2, forward cone
        float sampleDis = (0.5f + rand_norm() * 0.5f) * pathFinderDis;
        float angle = baseAngle + delta;
        float dx = x + cos(angle) * sampleDis;
        float dy = y + sin(angle) * sampleDis;
        float dz = z + 0.5f;
        PathGenerator path(bot);
        path.CalculatePath(dx, dy, dz);
        PathType type = path.GetPathType();
        bool canReach = !(type & (~typeOk));

        if (canReach && fabs(delta) <= minDelta)
        {
            found = true;
            G3D::Vector3 const& endPos = path.GetActualEndPosition();
            rx = endPos.x;
            ry = endPos.y;
            rz = endPos.z;
            minDelta = fabs(delta);
        }
    }
    if (found)
    {
        return MoveTo(bot->GetMapId(), rx, ry, rz, false, false, false, true);
    }
    return false;
}

bool NewRpgBaseAction::MoveWorldObjectTo(ObjectGuid guid, float distance)
{
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        return false;
    }

    WorldObject* object = botAI->GetWorldObject(guid);
    if (!object)
        return false;
    float x = object->GetPositionX();
    float y = object->GetPositionY();
    float z = object->GetPositionZ();
    float mapId = object->GetMapId();
    float angle = 0.f;

    if (!object->ToUnit() || !object->ToUnit()->isMoving())
        angle = object->GetAngle(bot) + (M_PI * irand(-25, 25) / 100.0);  // Closest 45 degrees towards the target
    else
        angle = object->GetOrientation() +
                (M_PI * irand(-25, 25) / 100.0);  // 45 degrees infront of target (leading it's movement)

    float rnd = rand_norm();
    x += cos(angle) * distance * rnd;
    y += sin(angle) * distance * rnd;
    if (!object->GetMap()->CheckCollisionAndGetValidCoords(object, object->GetPositionX(), object->GetPositionY(),
                                                           object->GetPositionZ(), x, y, z))
    {
        x = object->GetPositionX();
        y = object->GetPositionY();
        z = object->GetPositionZ();
    }
    return MoveTo(mapId, x, y, z, false, false, false, true);
}

bool NewRpgBaseAction::MoveRandomNear(float moveStep, MovementPriority priority, WorldObject*)
{
    if (IsWaitingForLastMove(priority))
        return false;

    Map* map = bot->GetMap();
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    // Previously: attempts = 1. A single random sample often landed in
    // water / blocked geometry / unreachable poly, the function returned
    // false, and the caller had no fallback — bot stood still. Retry a
    // handful of times with a fresh distance each loop so a bad roll
    // doesn't lock the bot in place.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        float distance = (0.4f + rand_norm() * 0.6f) * moveStep;
        float angle = (float)rand_norm() * 2 * static_cast<float>(M_PI);
        float dx = x + distance * cos(angle);
        float dy = y + distance * sin(angle);
        float dz = z;

        PathGenerator path(bot);
        path.CalculatePath(dx, dy, dz);
        PathType type = path.GetPathType();
        uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
        bool canReach = !(type & (~typeOk));

        if (!canReach)
            continue;

        if (!map->CanReachPositionAndGetValidCoords(bot, dx, dy, dz))
            continue;

        if (map->IsInWater(bot->GetPhaseMask(), dx, dy, dz, bot->GetCollisionHeight()))
            continue;

        bool moved = MoveTo(bot->GetMapId(), dx, dy, dz, false, false, false, true, priority);
        if (moved)
            return true;
    }

    return false;
}

bool NewRpgBaseAction::ForceToWait(uint32 duration, MovementPriority priority)
{
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation(),
             duration, priority);
    return true;
}

/// @TODO: Fix redundant code
/// Quest related method refer to TalkToQuestGiverAction.h
bool NewRpgBaseAction::InteractWithNpcOrGameObjectForQuest(ObjectGuid guid)
{
    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);
    if (!object || !bot->CanInteractWithQuestGiver(object))
        return false;

    // Creature* creature = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_NONE);
    // if (creature)
    // {
    //     WorldPacket packet(CMSG_GOSSIP_HELLO);
    //     packet << guid;
    //     bot->GetSession()->HandleGossipHelloOpcode(packet);
    // }

    bot->PrepareQuestMenu(guid);
    QuestMenu const& menu = bot->PlayerTalkClass->GetQuestMenu();
    if (menu.Empty())
        return true;

    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        QuestMenuItem const& item = menu.GetItem(idx);
        Quest const* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;

        QuestStatus const& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) &&
            IsQuestWorthDoing(quest) && IsQuestCapableDoing(quest) &&
            PlayerbotsExtensions::AllowsQuest(bot, quest))
        {
            AcceptQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_accepted",
                    "Quest accepted %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestAccepted(botAI, bot, quest);
            botAI->rpgStatistic.questAccepted++;
            BotTelemetry::instance().Event(botAI, "quest", "accept", quest->GetQuestId());
            LOG_DEBUG("playerbots", "[New RPG] {} accept quest {}", bot->GetName(), quest->GetQuestId());
        }
        if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, 0, false))
        {
            TurnInQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_rewarded",
                    "Quest rewarded %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestTurnedIn(botAI, bot, quest);
            botAI->rpgStatistic.questRewarded++;
            BotTelemetry::instance().Event(botAI, "quest", "reward", quest->GetQuestId());
            LOG_DEBUG("playerbots", "[New RPG] {} turned in quest {}", bot->GetName(), quest->GetQuestId());
        }
    }
    return true;
}

bool NewRpgBaseAction::CanInteractWithQuestGiver(Object* questGiver)
{
    // This is a variant of Player::CanInteractWithQuestGiver
    // that removes the distance check and keeps all other checks
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT: // Player::GetNPCIfCanInteractWith
        {
            ObjectGuid guid = questGiver->GetGUID();

            // unit checks
            if (!guid)
                return false;

            if (!bot->IsInWorld() || bot->IsDuringRemoveFromWorld())
                return false;

            if (bot->IsInFlight())
                return false;

            // exist (we need look pets also for some interaction (quest/etc)
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
            if (!creature)
                return false;

            // Deathstate checks
            if (!bot->IsAlive() &&
                !(creature->GetCreatureTemplate()->type_flags & CREATURE_TYPE_FLAG_VISIBLE_TO_GHOSTS))
                return false;

            // alive or spirit healer
            if (!creature->IsAlive() &&
                !(creature->GetCreatureTemplate()->type_flags & CREATURE_TYPE_FLAG_INTERACT_WHILE_DEAD))
                return false;

            // appropriate npc type
            if (!creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
                return false;

            // not allow interaction under control, but allow with own pets
            if (creature->GetCharmerGUID())
                return false;

            // xinef: perform better check
            if (creature->GetReactionTo(bot) <= REP_UNFRIENDLY)
                return false;

            return true;
        }
        case TYPEID_GAMEOBJECT: // Player::GetGameObjectIfCanInteractWith
        {
            ObjectGuid guid = questGiver->GetGUID();

            if (GameObject* go = bot->GetMap()->GetGameObject(guid))
            {
                if (go->GetGoType() == GAMEOBJECT_TYPE_QUESTGIVER)
                {
                    // Players cannot interact with gameobjects that use the "Point" icon
                    if (go->GetGOInfo()->IconName == "Point")
                        return false;

                    return true;
                }
            }

            return false;
        }
        // unused for now
        // case TYPEID_PLAYER:
        //     return bot->IsAlive() && questGiver->ToPlayer()->IsAlive();
        // case TYPEID_ITEM:
        //     return bot->IsAlive();
        default:
            break;
    }
    return false;
}

bool NewRpgBaseAction::IsWithinInteractionDist(Object* questGiver)
{
    // This is a variant of Player::CanInteractWithQuestGiver
    // that only keep the distance check
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            // unit checks
            if (!guid)
                return false;

            // exist (we need look pets also for some interaction (quest/etc)
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
            if (!creature)
                return false;

            if (!creature->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
                return false;

            return true;
        }
        case TYPEID_GAMEOBJECT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            if (GameObject* go = bot->GetMap()->GetGameObject(guid))
            {
                if (go->IsWithinDistInMap(bot))
                {
                    return true;
                }
            }
            return false;
        }
        // case TYPEID_PLAYER:
        //     return bot->IsAlive() && questGiver->ToPlayer()->IsAlive();
        // case TYPEID_ITEM:
        //     return bot->IsAlive();
        default:
            break;
    }
    return false;
}

bool NewRpgBaseAction::AcceptQuest(Quest const* quest, ObjectGuid guid)
{
    WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST);
    uint32 unk1 = 0;
    p << guid << quest->GetQuestId() << unk1;
    p.rpos(0);
    bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(p);

    return true;
}

bool NewRpgBaseAction::TurnInQuest(Quest const* quest, ObjectGuid guid)
{
    uint32 questID = quest->GetQuestId();

    if (bot->GetQuestRewardStatus(questID))
    {
        return false;
    }

    if (!bot->CanRewardQuest(quest, false))
    {
        return false;
    }

    bot->PlayDistanceSound(621);

    WorldPacket p(CMSG_QUESTGIVER_CHOOSE_REWARD);
    p << guid << quest->GetQuestId();
    if (quest->GetRewChoiceItemsCount() <= 1)
    {
        p << 0;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }
    else
    {
        uint32 bestId = BestRewardIndex(quest);
        p << bestId;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }

    return true;
}

uint32 NewRpgBaseAction::BestRewardIndex(Quest const* quest)
{
    ItemIds returnIds;
    ItemUsage bestUsage = ITEM_USAGE_NONE;
    if (quest->GetRewChoiceItemsCount() <= 1)
        return 0;
    else
    {
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
                bestUsage = ITEM_USAGE_EQUIP;
            else if (usage == ITEM_USAGE_BAD_EQUIP && bestUsage != ITEM_USAGE_EQUIP)
                bestUsage = usage;
            else if (usage != ITEM_USAGE_NONE && bestUsage == ITEM_USAGE_NONE)
                bestUsage = usage;
        }
        StatsWeightCalculator calc(bot);
        uint32 best = 0;
        float bestScore = 0;
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == bestUsage || usage == ITEM_USAGE_REPLACE)
            {
                float score = calc.CalculateItem(quest->RewardChoiceItemId[i]);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }
        }
        return best;
    }
}

bool NewRpgBaseAction::IsQuestWorthDoing(Quest const* quest)
{
    bool isLowLevelQuest =
        bot->GetLevel() > (bot->GetQuestLevel(quest) + sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF));

    if (isLowLevelQuest)
        return false;

    if (quest->IsRepeatable())
        return false;

    if (quest->IsSeasonal())
        return false;

    return true;
}

bool NewRpgBaseAction::IsQuestCapableDoing(Quest const* quest)
{
    bool highLevelQuest = bot->GetLevel() + 3 < bot->GetQuestLevel(quest);
    if (highLevelQuest)
        return false;

    // Elite quest and dungeon quest etc
    if (quest->GetType() != 0)
        return false;

    // now we only capable of doing solo quests
    if (quest->GetSuggestedPlayers() >= 2)
        return false;

    return true;
}

bool NewRpgBaseAction::OrganizeQuestLog()
{
    int32 freeSlotNum = 0;

    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            freeSlotNum++;
    }

    // it's ok if we have two more free slots
    if (freeSlotNum >= 2)
        return false;

    int32 dropped = 0;
    // remove quests that not worth doing or not capable of doing
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        bool failed = bot->GetQuestStatus(questId) == QUEST_STATUS_FAILED;
        if ((!IsQuestWorthDoing(quest) || !IsQuestCapableDoing(quest) || failed) && (failed || !KeepsQuest(quest)))
        {
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->rpgStatistic.questDropped++;
            BotTelemetry::instance().Event(botAI, "quest", "drop", quest->GetQuestId());
            dropped++;
        }
    }

    // drop more than 8 quests at once to avoid repeated accept and drop
    if (dropped >= 8)
        return true;

    // remove festival quests and quests in different zone (class quests stay)
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        const int64_t botZoneId = this->bot->GetZoneId();
        if (KeepsQuest(quest))
            continue;

        if (quest->GetZoneOrSort() < 0 || (quest->GetZoneOrSort() > 0 && quest->GetZoneOrSort() != botZoneId))
        {
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->rpgStatistic.questDropped++;
            BotTelemetry::instance().Event(botAI, "quest", "drop", quest->GetQuestId());
            dropped++;
        }
    }

    if (dropped >= 8)
        return true;

    // clear quests log, but for class quests
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (KeepsQuest(quest))
            continue;
        LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
        WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
        packet << (uint8)i;
        WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
        removeQuest.Read();
        bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
        if (botAI->GetMaster())
            botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "new_rpg_quest_dropped",
                "Quest dropped %quest",
                {{"%quest", ChatHelper::FormatQuest(quest)}}));
        botAI->rpgStatistic.questDropped++;
        BotTelemetry::instance().Event(botAI, "quest", "drop", quest->GetQuestId());
    }

    return true;
}

bool NewRpgBaseAction::SearchQuestGiverAndAcceptOrReward()
{
    OrganizeQuestLog();
    if (ObjectGuid npcOrGo = ChooseNpcOrGameObjectToInteract(true, 80.0f))
    {
        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, npcOrGo);
        if (!object)
            return false;

        if (bot->CanInteractWithQuestGiver(object))
        {
            InteractWithNpcOrGameObjectForQuest(npcOrGo);
            ForceToWait(5000);
            return true;
        }
        return MoveWorldObjectTo(npcOrGo);
    }
    return false;
}

ObjectGuid NewRpgBaseAction::ChooseNpcOrGameObjectToInteract(bool questgiverOnly, float distanceLimit)
{
    GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
    GuidVector possibleGameObjects = AI_VALUE(GuidVector, "possible new rpg game objects");

    if (possibleTargets.empty() && possibleGameObjects.empty())
        return ObjectGuid();

    WorldObject* nearestObject = nullptr;
    for (ObjectGuid& guid : possibleTargets)
    {
        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);

        if (!object || !object->IsInWorld())
            continue;

        if (distanceLimit && bot->GetDistance(object) > distanceLimit)
            continue;

        if (CanInteractWithQuestGiver(object) && HasQuestToAcceptOrReward(object))
        {
            if (!nearestObject || bot->GetExactDist(nearestObject) > bot->GetExactDist(object))
                nearestObject = object;
            break;
        }
    }

    for (ObjectGuid& guid : possibleGameObjects)
    {
        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);

        if (!object || !object->IsInWorld())
            continue;

        if (distanceLimit && bot->GetDistance(object) > distanceLimit)
            continue;

        if (CanInteractWithQuestGiver(object) && HasQuestToAcceptOrReward(object))
        {
            if (!nearestObject || bot->GetExactDist(nearestObject) > bot->GetExactDist(object))
                nearestObject = object;
            break;
        }
    }

    if (nearestObject)
        return nearestObject->GetGUID();

    // No questgiver to accept or reward
    if (questgiverOnly)
        return ObjectGuid();

    if (possibleTargets.empty())
        return ObjectGuid();

    int idx = urand(0, possibleTargets.size() - 1);
    ObjectGuid guid = possibleTargets[idx];
    WorldObject* object = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
    if (!object)
        object = ObjectAccessor::GetGameObject(*bot, guid);

    if (object && object->IsInWorld())
    {
        return object->GetGUID();
    }
    return ObjectGuid();
}

bool NewRpgBaseAction::HasQuestToAcceptOrReward(WorldObject* object)
{
    ObjectGuid guid = object->GetGUID();
    bot->PrepareQuestMenu(guid);
    QuestMenu const& menu = bot->PlayerTalkClass->GetQuestMenu();
    if (menu.Empty())
        return false;

    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        QuestMenuItem const& item = menu.GetItem(idx);
        Quest const* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;
        QuestStatus const& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, 0, false))
        {
            return true;
        }
    }
    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        QuestMenuItem const& item = menu.GetItem(idx);
        Quest const* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;

        QuestStatus const& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) &&
            IsQuestWorthDoing(quest) && IsQuestCapableDoing(quest) &&
            PlayerbotsExtensions::AllowsQuest(bot, quest))
        {
            return true;
        }
    }
    return false;
}

static std::vector<float> GenerateRandomWeights(int n)
{
    std::vector<float> weights(n);
    float sum = 0.0;

    for (int i = 0; i < n; ++i)
    {
        weights[i] = rand_norm();
        sum += weights[i];
    }
    for (int i = 0; i < n; ++i)
    {
        weights[i] /= sum;
    }
    return weights;
}

bool NewRpgBaseAction::GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return false;

    QuestPOIVector const* poiVector = sObjectMgr->GetQuestPOIVector(questId);
    if (!poiVector)
    {
        return false;
    }

    QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);

    if (toComplete && q_status.Status == QUEST_STATUS_COMPLETE)
    {
        for (QuestPOI const& qPoi : *poiVector)
        {
            if (qPoi.MapId != bot->GetMapId())
                continue;

            // not the poi pos to reward quest
            if (qPoi.ObjectiveIndex != -1)
                continue;

            if (qPoi.points.size() == 0)
                continue;

            float dx = 0, dy = 0;
            std::vector<float> weights = GenerateRandomWeights(qPoi.points.size());
            for (size_t i = 0; i < qPoi.points.size(); i++)
            {
                QuestPOIPoint const& point = qPoi.points[i];
                dx += point.x * weights[i];
                dy += point.y * weights[i];
            }

            if (bot->GetDistance2d(dx, dy) >= 1500.0f)
                continue;

            float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

            if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
                continue;

            if (bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
                continue;

            poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex});
        }

        if (poiInfo.empty())
            return false;

        return true;
    }

    if (q_status.Status != QUEST_STATUS_INCOMPLETE)
        return false;

    // Get incomplete quest objective index
    std::vector<int32> incompleteObjectiveIdx;
    for (int i = 0; i < QUEST_OBJECTIVES_COUNT; i++)
    {
        int32 npcOrGo = quest->RequiredNpcOrGo[i];
        if (!npcOrGo)
            continue;

        if (q_status.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
            incompleteObjectiveIdx.push_back(i);
    }
    for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; i++)
    {
        uint32 itemId = quest->RequiredItemId[i];
        if (!itemId)
            continue;

        if (q_status.ItemCount[i] < quest->RequiredItemCount[i])
            incompleteObjectiveIdx.push_back(QUEST_OBJECTIVES_COUNT + i);
    }

    // Get POIs to go
    for (QuestPOI const& qPoi : *poiVector)
    {
        if (qPoi.MapId != bot->GetMapId())
            continue;

        bool inComplete = false;
        for (uint32 objective : incompleteObjectiveIdx)
        {
            if (qPoi.ObjectiveIndex == static_cast<int32>(objective))
            {
                inComplete = true;
                break;
            }
        }
        if (!inComplete)
            continue;
        if (qPoi.points.size() == 0)
            continue;
        float dx = 0, dy = 0;
        std::vector<float> weights = GenerateRandomWeights(qPoi.points.size());
        for (size_t i = 0; i < qPoi.points.size(); i++)
        {
            QuestPOIPoint const& point = qPoi.points[i];
            dx += point.x * weights[i];
            dy += point.y * weights[i];
        }

        if (bot->GetDistance2d(dx, dy) >= 1500.0f)
            continue;

        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            continue;

        if (bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
            continue;

        poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex});
    }

    if (poiInfo.size() == 0)
    {
        // LOG_DEBUG("playerbots", "[New rpg] {}: No available poi can be found for quest {}", bot->GetName(), questId);
        return false;
    }

    return true;
}

WorldPosition NewRpgBaseAction::SelectRandomGrindPos(Player* bot)
{
    std::vector<WorldLocation> const& locs = sTravelMgr.GetLocsPerLevelCache(bot->GetLevel());
    float hiRange = 500.0f;
    float loRange = 2500.0f;
    if (bot->GetLevel() < 5)
    {
        hiRange /= 3;
        loRange /= 3;
    }
    std::vector<WorldLocation> lo_prepared_locs, hi_prepared_locs;

    bool inCity = false;
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    bool skipUnreachable = botAI && sPlayerbotAIConfig.NewRpgWalkUnstick;
    uint32 unreachableMs = sPlayerbotAIConfig.NewRpgUnreachableMinutes * MINUTE * IN_MILLISECONDS;

    BotDanger::HeatMap const heat = BotDanger::Heat(bot->GetMapId(), bot->GetLevel());
    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        if (bot->GetExactDist(loc) > 2500.0f)
            continue;

        if (!inCity && !sPlayerbotAIConfig.NewRpgCrossZone &&
            bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                     loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        if (skipUnreachable && botAI->rpgInfo.IsUnreachable(WorldPosition(loc), unreachableMs))
            continue;

        if (BotDanger::IsDangerous(bot, loc.GetMapId(), loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ()))
            continue;

        // Where similar-level bots keep dying (4+ in the last hour in its 100 yd cell)
        auto hot = heat.find(BotDanger::Cell(loc.GetPositionX(), loc.GetPositionY()));
        if (hot != heat.end() && hot->second >= HOT_CELL_DEATHS)
            continue;

        if (bot->GetExactDist(loc) < hiRange)
        {
            hi_prepared_locs.push_back(loc);
        }

        if (bot->GetExactDist(loc) < loRange)
        {
            lo_prepared_locs.push_back(loc);
        }
    }
    WorldPosition dest{};
    if (urand(1, 100) <= 50 && !hi_prepared_locs.empty())
    {
        uint32 idx = urand(0, hi_prepared_locs.size() - 1);
        dest = hi_prepared_locs[idx];
    }
    else if (!lo_prepared_locs.empty())
    {
        uint32 idx = urand(0, lo_prepared_locs.size() - 1);
        dest = lo_prepared_locs[idx];
    }
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random grind pos Map:{} X:{} Y:{} Z:{} ({}+{} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              hi_prepared_locs.size(), lo_prepared_locs.size() - hi_prepared_locs.size(), locs.size());
    return dest;
}

WorldPosition NewRpgBaseAction::SelectRandomCampPos(Player* bot)
{
    const std::vector<WorldLocation> locs = sTravelMgr.GetTravelHubs(bot);

    bool inCity = false;

    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    bool skipUnreachable = botAI && sPlayerbotAIConfig.NewRpgWalkUnstick;
    uint32 unreachableMs = sPlayerbotAIConfig.NewRpgUnreachableMinutes * MINUTE * IN_MILLISECONDS;

    std::vector<WorldLocation> prepared_locs;
    BotDanger::HeatMap const heat = BotDanger::Heat(bot->GetMapId(), bot->GetLevel());
    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        float range = bot->GetLevel() <= 5 ? 500.0f : 2500.0f;
        if (bot->GetExactDist(loc) > range)
            continue;

        if (bot->GetExactDist(loc) < 50.0f)
            continue;

        if (!inCity && !sPlayerbotAIConfig.NewRpgCrossZone &&
            bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                     loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        if (skipUnreachable && botAI->rpgInfo.IsUnreachable(WorldPosition(loc), unreachableMs))
            continue;

        if (BotDanger::IsDangerous(bot, loc.GetMapId(), loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ()))
            continue;

        // Where similar-level bots keep dying (4+ in the last hour in its 100 yd cell)
        auto hot = heat.find(BotDanger::Cell(loc.GetPositionX(), loc.GetPositionY()));
        if (hot != heat.end() && hot->second >= HOT_CELL_DEATHS)
            continue;

        prepared_locs.push_back(loc);
    }
    WorldPosition dest{};
    if (!prepared_locs.empty())
    {
        uint32 idx = urand(0, prepared_locs.size() - 1);
        dest = prepared_locs[idx];
    }
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random inn keeper pos Map:{} X:{} Y:{} Z:{} ({} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              prepared_locs.size(), locs.size());
    return dest;
}

bool NewRpgBaseAction::SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path)
{
    TravelMgr::FlightMasterInfo const* info = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!info)
        return false;

    std::vector<std::vector<uint32>> availablePaths = sTravelMgr.GetOptimalFlightDestinations(bot);
    if (availablePaths.empty())
        return false;

    flightMasterEntry = info->templateEntry;
    flightMasterPos = info->pos;
    path = availablePaths[urand(0, availablePaths.size() - 1)];
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random flight taxi node from:{} (node {}) to:{} ({} available)",
              bot->GetName(), flightMasterEntry, path[0], path[path.size() - 1], availablePaths.size());
    return true;
}

bool NewRpgBaseAction::RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus)
{
    std::vector<NewRpgStatus> availableStatus;
    uint32 probSum = 0;
    for (NewRpgStatus status : candidateStatus)
    {
        if (sPlayerbotAIConfig.RpgStatusProbWeight[status] == 0)
            continue;

        if (CheckRpgStatusAvailable(status))
        {
            availableStatus.push_back(status);
            probSum += sPlayerbotAIConfig.RpgStatusProbWeight[status];
        }
    }
    // Safety check. Default to "rest" if all RPG weights = 0
    if (availableStatus.empty() || probSum == 0)
    {
        botAI->rpgInfo.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }
    uint32 rand = urand(1, probSum);
    uint32 accumulate = 0;
    NewRpgStatus chosenStatus = RPG_STATUS_END;
    for (NewRpgStatus status : availableStatus)
    {
        accumulate += sPlayerbotAIConfig.RpgStatusProbWeight[status];
        if (accumulate >= rand)
        {
            chosenStatus = status;
            break;
        }
    }

    switch (chosenStatus)
    {
        case RPG_WANDER_RANDOM:
        {
            botAI->rpgInfo.ChangeToWanderRandom();
            return true;
        }
        case RPG_WANDER_NPC:
        {
            botAI->rpgInfo.ChangeToWanderNpc();
            return true;
        }
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToGoGrind(pos);
                return true;
            }
            return false;
        }
        case RPG_GO_CAMP:
        {
            WorldPosition pos = SelectRandomCampPos(bot);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToGoCamp(pos);
                return true;
            }
            return false;
        }
        case RPG_DO_QUEST:
        {
            std::vector<uint32> availableQuests;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (!IsQuestSelectable(questId))
                    continue;

                std::vector<POIInfo> poiInfo;
                if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
                {
                    availableQuests.push_back(questId);
                }
            }
            if (availableQuests.size())
            {
                uint32 questId = availableQuests[urand(0, availableQuests.size() - 1)];
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (quest)
                {
                    botAI->rpgInfo.ChangeToDoQuest(questId, quest);
                    return true;
                }
            }
            return false;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            if (SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
            {
                botAI->rpgInfo.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, path);
                return true;
            }
            return false;
        }
        case RPG_DO_GATHER:
        {
            botAI->rpgInfo.ChangeToDoGather();
            return true;
        }
        case RPG_IDLE:
        {
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        case RPG_REST:
        {
            botAI->rpgInfo.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }
        case RPG_OUTDOOR_PVP:
        {
            botAI->rpgInfo.ChangeToOutdoorPvp();
            return true;
        }
        default:
        {
            botAI->rpgInfo.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }
    }
    return false;
}

bool NewRpgBaseAction::CheckRpgStatusAvailable(NewRpgStatus status)
{
    switch (status)
    {
        case RPG_IDLE:
        case RPG_REST:
            return true;
        case RPG_WANDER_RANDOM:
        {
            Unit* target = AI_VALUE(Unit*, "grind target");
            return target != nullptr;
        }
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            return pos != WorldPosition();
        }
        case RPG_GO_CAMP:
        {
            WorldPosition pos = SelectRandomCampPos(bot);
            return pos != WorldPosition();
        }
        case RPG_WANDER_NPC:
        {
            GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
            return possibleTargets.size() >= 3;
        }
        case RPG_DO_QUEST:
        {
            std::vector<uint32> availableQuests;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (!IsQuestSelectable(questId))
                    continue;

                std::vector<POIInfo> poiInfo;
                if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
                {
                    return true;
                }
            }
            return false;
        }
        case RPG_DO_GATHER:
        {
            if (!botAI->HasSkill(SKILL_HERBALISM) && !botAI->HasSkill(SKILL_MINING))
                return false;

            // No point farming with (nearly) full bags - the loot pipeline
            // can't store the harvest (see StoreLootAction).
            if (AI_VALUE(uint8, "bag space") > 80)
                return false;

            return sGatherNodeMgr.HasUsableNodes(bot);
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            return SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path);
        }
        case RPG_OUTDOOR_PVP:
        {
            if (!bot->IsPvP())
                return false;
            uint32 zoneId = bot->GetZoneId();
            if (zoneId == AREA_NAGRAND)
                return false;

            OutdoorPvP* outdoorPvP = sOutdoorPvPMgr->GetOutdoorPvPToZoneId(zoneId);
            return outdoorPvP != nullptr;
        }
        default:
            return false;
    }
    return false;
}

bool NewRpgBaseAction::IsQuestSelectable(uint32 questId)
{
    if (!questId)
        return false;

    if (!sPlayerbotAIConfig.NewRpgQuestCare)
        return botAI->lowPriorityQuest.find(questId) == botAI->lowPriorityQuest.end();

    uint32 revisitMs = sPlayerbotAIConfig.NewRpgQuestRevisitMinutes * MINUTE * IN_MILLISECONDS;
    if (botAI->rpgInfo.IsQuestShelved(questId, bot->GetLevel(), sPlayerbotAIConfig.NewRpgQuestRevisitLevels,
                                      revisitMs))
        return false;

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return false;

    int32 maxLevel = static_cast<int32>(bot->GetLevel() + sPlayerbotAIConfig.NewRpgQuestMaxLevelAbove);
    return bot->GetQuestLevel(quest) <= maxLevel;
}

void NewRpgBaseAction::ShelveQuest(uint32 questId, char const* reason)
{
    if (sPlayerbotAIConfig.NewRpgQuestCare)
        botAI->rpgInfo.ShelveQuest(questId, bot->GetLevel());
    else
        botAI->lowPriorityQuest.insert(questId);

    botAI->rpgStatistic.questAbandoned++;
    BotTelemetry::instance().Event(botAI, "quest", reason, questId);
    LOG_DEBUG("playerbots", "[New RPG] {} set aside quest {} ({})", bot->GetName(), questId, reason);
}

uint32 NewRpgBaseAction::QuestObjectiveProgress(uint32 questId, int32 objectiveIdx)
{
    auto const& statusMap = bot->getQuestStatusMap();
    auto itr = statusMap.find(questId);
    if (itr == statusMap.end() || objectiveIdx < 0)
        return 0;

    if (objectiveIdx < QUEST_OBJECTIVES_COUNT)
        return itr->second.CreatureOrGOCount[objectiveIdx];

    if (objectiveIdx < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        return itr->second.ItemCount[objectiveIdx - QUEST_OBJECTIVES_COUNT];

    return 0;
}

void NewRpgBaseAction::LeaveQuestToGrind()
{
    WorldPosition pos = SelectRandomGrindPos(bot);
    if (pos != WorldPosition())
        botAI->rpgInfo.ChangeToGoGrind(pos);
    else
        botAI->rpgInfo.ChangeToIdle();
}

bool NewRpgBaseAction::Unstick(WorldPosition const& dest, float disToDest)
{
    NewRpgInfo& info = botAI->rpgInfo;
    info.stuckTs = getMSTime();
    info.stuckAttempts = 0;

    // Unable to move: never got more than a few yards from where the last stuck check left it,
    // across the whole window (a bot pacing near an unreachable spot does roam, and just gives up).
    bool couldNotMove = info.stuckPos != WorldPosition() && info.stuckRoam < stuckNoMoveDistance;
    // Trapped: partial paths toward a far destination end at the same dead end (the reachable spot nearest to it,
    // e.g. the back of a cave that points that way), so after the detour the bot walked straight back to it.
    WorldPosition here(bot);
    bool trapped = info.unstickStage == 1 && info.stuckPos != WorldPosition() &&
                   disToDest >= TRAPPED_MIN_DISTANCE && here.distance(info.stuckPos) < TRAPPED_RADIUS;
    info.stuckPos = here;
    info.stuckRoam = 0.0f;

    // Already above or below the destination (a quest POI on the ground over a cave, a camp in a tree town, a
    // spot on another level): call it reached. Its height came from the top surface, which may not be walkable
    // from here; 152 of 368 detours had the bot 50+ yd off the destination's height.
    {
        float dx = bot->GetPositionX() - dest.GetPositionX();
        float dy = bot->GetPositionY() - dest.GetPositionY();
        if (dx * dx + dy * dy < NEAR_2D * NEAR_2D)
        {
            WorldPosition reached(bot);
            bool moved = false;
            if (auto* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data))
                quest->pos = reached, moved = true;
            else if (auto* grind = std::get_if<NewRpgInfo::GoGrind>(&info.data))
                grind->pos = reached, moved = true;
            else if (auto* camp = std::get_if<NewRpgInfo::GoCamp>(&info.data))
                camp->pos = reached, moved = true;

            if (moved)
            {
                info.unstickStage = 0;
                BotTelemetry::instance().Event(botAI, "unstick", "near",
                                               static_cast<int32>(dest.GetPositionZ() - bot->GetPositionZ()));
                return true;
            }
        }
    }

    // Really stuck here (not just above or below the destination): count it for routes (BotDanger::StuckHeat)
    BotDanger::NoteStuck(bot);

    // First time: step aside and let the next ticks retry from a new spot.
    if (info.unstickStage == 0)
    {
        info.unstickStage = 1;
        BotTelemetry::instance().Event(botAI, "unstick", "detour", static_cast<int32>(disToDest));
        BotRoutes::Reroute(bot);
        return MoveRandomNear(30.0f);
    }

    // Second time: give up on this destination.
    info.unstickStage = 0;
    info.MarkUnreachable(dest);
    BotRoutes::Forget(bot);

    uint32 questId = 0;
    if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data))
        questId = quest->questId;

    // Thunder Bluff and Undercity are left by elevator, which bots can't ride: a give-up there means stuck for good
    bool elevatorCity = bot->GetZoneId() == ZONE_THUNDER_BLUFF || bot->GetZoneId() == ZONE_UNDERCITY;
    bool stuckInPlace = StuckInPlace(bot) || elevatorCity;
    if ((couldNotMove || trapped || stuckInPlace) && bot->HasItemCount(ITEM_HEARTHSTONE, 1) &&
        !bot->HasSpellCooldown(SPELL_HEARTHSTONE) && botAI->DoSpecificAction("hearthstone", Event(), true))
    {
        BotTelemetry::instance().Event(botAI, "unstick", "hearth", static_cast<int32>(disToDest));
        // REST keeps the bot in place for the 10 s cast; IDLE would roll a status that walks off
        info.ChangeToRest();
        return true;
    }

    // Third give-up in the same spot and no hearthstone to use: the "Stuck" help option's graveyard move
    if (stuckInPlace)
    {
        if (GraveyardStruct const* grave = sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId()))
        {
            BotTelemetry::instance().Event(botAI, "unstick", "graveyard", static_cast<int32>(disToDest),
                                           static_cast<int32>(grave->ID));
            if (questId)
                ShelveQuest(questId, "shelve_unreach");
            info.ChangeToIdle();
            bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
            return bot->TeleportTo(grave->Map, grave->x, grave->y, grave->z, bot->GetOrientation());
        }
    }

    BotTelemetry::instance().Event(botAI, "unstick", "give_up", static_cast<int32>(disToDest));
    if (questId)
        ShelveQuest(questId, "shelve_unreach");

    info.ChangeToIdle();
    return true;
}

bool NewRpgBaseAction::QuestLogNeedsItem(uint32 itemId)
{
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(bot->GetQuestSlotQuestId(slot));
        if (!quest)
            continue;

        if (quest->GetSrcItemId() == itemId)
            return true;

        for (uint32 required : quest->RequiredItemId)
        {
            if (required == itemId)
                return true;
        }

        for (uint32 source : quest->ItemDrop)
        {
            if (source == itemId)
                return true;
        }
    }
    return false;
}

uint32 NewRpgBaseAction::TidyQuestLog()
{
    uint32 changed = 0;
    // Signed math: a level 2 bot minus a hide diff of 4 must stay negative, not wrap around
    int32 hideDiff = static_cast<int32>(sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF));
    int32 greyBelow = static_cast<int32>(bot->GetLevel()) - hideDiff;

    // Grey quests the bot hasn't finished: a player would abandon these too. Finished ones wait for turn-in.
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest || bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE || KeepsQuest(quest))
            continue;

        if (bot->GetQuestLevel(quest) >= greyBelow)
            continue;

        WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
        packet << slot;
        WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
        removeQuest.Read();
        bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
        botAI->rpgStatistic.questDropped++;
        BotTelemetry::instance().Event(botAI, "quest", "drop_grey", questId);
        ++changed;
    }

    // Quest items left over from abandoned quests (the core only takes back a quest's source item)
    auto tidyItem = [&](Item* item, uint8 bag, uint8 slot)
    {
        if (!item)
            return;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->StartQuest || (proto->Class != ITEM_CLASS_QUEST && proto->Bonding != BIND_QUEST_ITEM))
            return;

        if (QuestLogNeedsItem(proto->ItemId))
            return;

        BotTelemetry::instance().Event(botAI, "item", "destroy_quest", static_cast<int32>(proto->ItemId),
                                       static_cast<int32>(item->GetCount()));
        bot->DestroyItem(bag, slot, true);
        ++changed;
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        tidyItem(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), INVENTORY_SLOT_BAG_0, slot);

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = bot->GetBagByPos(bagSlot);
        if (!bag)
            continue;

        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            tidyItem(bag->GetItemByPos(slot), bagSlot, static_cast<uint8>(slot));
    }

    return changed;
}
