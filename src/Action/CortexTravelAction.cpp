/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <list>

#include "BotLevels.h"
#include "BotTelemetry.h"
#include "CellImpl.h"
#include "Cortex.h"
#include "CortexActions.h"
#include "CortexQuests.h"
#include "CortexTravel.h"
#include "Creature.h"
#include "GameObject.h"
#include "GameObjectModel.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInitArgs.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"
#include "Transport.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    constexpr uint32 TRIP_GAP_MS = 10 * IN_MILLISECONDS;  // a longer pause between steps restarts the still/lost counts
    constexpr float LEG_ARRIVE = 5.0f;                    // at the pier or lift landing
    constexpr float LEG_ARRIVE_Z = 10.0f;
    constexpr float LEG_DRIFT = 15.0f;  // waiting, pushed this far from the pier: walk back
    constexpr float LEG_PROGRESS = 5.0f;
    constexpr uint32 LEG_STUCK_MS = 3 * MINUTE * IN_MILLISECONDS;  // at least; a long walk gets its straight time
    constexpr float WALK_SPEED = 7.0f;
    constexpr uint32 FROZEN_MS = 90 * IN_MILLISECONDS;  // the walk to a leg didn't move the bot at all this long
    constexpr float FROZEN_MOVE = 3.0f;
    constexpr float DOCK_RANGE = 45.0f;  // a ship or zeppelin this close to the pier, standing still, is in
    constexpr float LIFT_STOP = 1.0f;    // a lift this close to a stop's height is at it
    // Lifts rest 5 s at each stop (TransportAnimation.dbc) and spend the last ~2 s closing the last 5 yd: the bot
    // starts across the 10-16 yd from its landing as the lift comes in, not once it stands.
    constexpr float LIFT_ARRIVING = 6.0f;
    constexpr float LIFT_ON = 2.5f;      // this close to the lift's middle the bot is on it
    constexpr float LIFT_MATCH = 20.0f;  // a lift this close to the table's position is that one
    // With the lift's timetable known (TransportAnimation), the bot sets off this long before the lift reaches the
    // stop at most, and only when the lift stays long enough for the walk with this to spare.
    constexpr uint32 LIFT_LEAD_MS = 1500;
    constexpr uint32 LIFT_SLACK_MS = 2000;
    constexpr uint32 LIFT_STEP_MS = 100;  // the timetable is looked at in steps this long
    constexpr float ATTACH_DIST = 2.5f;   // this close to the deck spot the bot is made the transport's passenger
    constexpr uint32 LINK_BACKOFF_MS = 20 * MINUTE * IN_MILLISECONDS;  // a failed link stays out of this bot's trips
    constexpr uint32 STILL_MS = 1500;
    constexpr float STILL_MOVE = 0.3f;
    constexpr uint32 BOAT_WAIT_MS = 12 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 LIFT_WAIT_MS = 3 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 BOARD_MS = 20 * IN_MILLISECONDS;
    constexpr uint8 BOARD_TRIES = 4;
    constexpr uint32 LOST_MS = 10 * IN_MILLISECONDS;  // riding but not a passenger this long: fell off
    constexpr uint32 EXIT_MS = 30 * IN_MILLISECONDS;
    constexpr float EXIT_ARRIVE = 3.0f;
    constexpr float DECK_SEARCH_HEIGHT = 8.0f;     // the deck is looked for this far above and below the pier
    constexpr float FLIGHT_MASTER_SEARCH = 60.0f;  // the flight master stands this close to its node
    constexpr float FLIGHT_MASTER_LEVEL_Z = 5.0f;  // higher or lower than the bot by this: on another level
    constexpr uint32 FLIGHT_START_MS = 8 * IN_MILLISECONDS;
    constexpr float FLIGHT_ARRIVE = 150.0f;
    constexpr uint32 TELEPORT_MS = 15 * IN_MILLISECONDS;
    constexpr float TELEPORT_ARRIVE = 60.0f;
    constexpr uint8 TRIP_REPLANS = 2;

    // Height of the transport's deck under (x, y), looked for from `top` down `depth` yards; false when the ray
    // misses it. Map::GetTransportForPos tests passengers the same way.
    bool DeckHeight(GameObject const* transport, uint32 phaseMask, float x, float y, float top, float depth, float& z)
    {
        if (!transport->m_model)
            return false;
        G3D::Ray ray(G3D::Vector3(x, y, top), G3D::Vector3(0.0f, 0.0f, -1.0f));
        float dist = depth;
        if (!transport->m_model->intersectRay(ray, dist, false, phaseMask, VMAP::ModelIgnoreFlags::Nothing))
            return false;
        z = top - dist;
        return true;
    }

    // A lift's height `ahead` ms from now. Lifts go round on a fixed timetable of height offsets
    // (StaticTransport::RelocateToProgress); false for one held by a pause time or without a timetable.
    bool LiftZ(StaticTransport* lift, uint32 ahead, float& z)
    {
        TransportAnimation const* anim = lift->GetGOValue()->Transport.AnimationInfo;
        if (!anim || anim->Path.size() < 2 || !anim->TotalTime || lift->GetPauseTime())
            return false;
        uint32 t = (lift->GetPathProgress() + ahead) % anim->TotalTime;
        auto next = anim->Path.upper_bound(t);
        if (next == anim->Path.end() || next == anim->Path.begin())
            return false;
        auto curr = std::prev(next);
        float span = static_cast<float>(next->first - curr->first);
        float perc = span > 0.0f ? static_cast<float>(t - curr->first) / span : 0.0f;
        z = lift->GetStationaryZ() + curr->second->Z + perc * (next->second->Z - curr->second->Z);
        return true;
    }

    // When the lift is next at the stop (within `within` of `stopZ`): in `until` ms (0: it is there now), staying
    // `stay` ms from then. False when its timetable isn't known or never reaches that height.
    bool LiftWindow(StaticTransport* lift, float stopZ, float within, uint32& until, uint32& stay)
    {
        TransportAnimation const* anim = lift->GetGOValue()->Transport.AnimationInfo;
        if (!anim || !anim->TotalTime)
            return false;
        bool found = false;
        until = 0;
        stay = 0;
        for (uint32 t = 0; t <= 2 * anim->TotalTime; t += LIFT_STEP_MS)
        {
            float z = 0.0f;
            if (!LiftZ(lift, t, z))
                return false;
            bool at = std::fabs(z - stopZ) <= within;
            if (!found && at)
            {
                found = true;
                until = t;
            }
            else if (found && !at)
            {
                stay = t - until;
                return true;
            }
            else if (!found && t >= anim->TotalTime)
                return false;
        }
        return false;
    }

    CortexTripPhase FirstPhase(CortexLeg const& leg)
    {
        return leg.kind == CortexLegKind::SPELL || leg.kind == CortexLegKind::HEARTH ? CortexTripPhase::ACTIVATE
                                                                                     : CortexTripPhase::APPROACH;
    }
}

bool CortexTravelAction::StartTrip(CortexPlan& plan, uint32 map, float x, float y, float z, uint32 mapArea,
                                   bool allowDirect, char const* why)
{
    uint32 now = getMSTime();
    tripPlan = &plan;
    CortexTrip trip;
    trip.allowDirect = allowDirect;
    // Links that failed for this bot lately stay out until their backoff is over.
    plan.badLinks.erase(std::remove_if(plan.badLinks.begin(), plan.badLinks.end(),
                                       [now](std::pair<uint16, uint32> const& bad)
                                       { return static_cast<int32>(bad.second - now) <= 0; }),
                        plan.badLinks.end());
    for (std::pair<uint16, uint32> const& bad : plan.badLinks)
        trip.excluded.push_back(bad.first);
    if (!CortexTravel::Plan(bot, map, x, y, z, mapArea, allowDirect, trip))
        return false;

    trip.active = true;
    trip.map = map;
    trip.x = x;
    trip.y = y;
    trip.z = z;
    trip.mapArea = mapArea;
    trip.startMs = now;
    trip.legMs = now;
    trip.seenMs = now;
    trip.seenAwayMs = Cortex::AwayMs(bot);
    plan.trip = std::move(trip);
    SetPhase(plan.trip, FirstPhase(plan.trip.legs.front()));
    ++plan.tripsForTarget;

    float d = map == bot->GetMapId() ? bot->GetExactDist2d(x, y) : 0.0f;
    BotTelemetry::instance().Event(botAI, "travel_plan",
                                   std::string(why) + " " + CortexTravel::Describe(plan.trip.legs),
                                   static_cast<int32>(plan.trip.legs.size()), static_cast<int32>(plan.trip.estimate),
                                   static_cast<int32>(d));
    return true;
}

bool CortexTravelAction::TripStep(CortexPlan& plan, bool& failed)
{
    failed = false;
    tripFailed = false;
    tripPlan = &plan;
    CortexTrip& trip = plan.trip;
    if (!trip.active)
        return false;

    // Time away (fighting, dead, recovering, Cortex::AwayMs) doesn't count against the phase timers; the gap between
    // steps doesn't tell it, a bot's own moves set its next AI check seconds out.
    uint32 now = getMSTime();
    uint32 awayMs = Cortex::AwayMs(bot);
    uint32 away = awayMs - trip.seenAwayMs;
    trip.seenAwayMs = awayMs;
    if (away)
    {
        trip.phaseMs += away;
        trip.bestMs += away;
        if (trip.movedMs)
            trip.movedMs += away;  // not "frozen" for the time spent fighting or dead
    }
    if (getMSTimeDiff(trip.seenMs, now) > TRIP_GAP_MS)
    {
        trip.stillMs = 0;
        trip.lostMs = 0;
    }
    trip.seenMs = now;
    if (trip.leg >= trip.legs.size())
    {
        trip.active = false;
        return false;
    }

    // A copy: Fail may plan a new trip in its place.
    CortexLeg const leg = trip.legs[trip.leg];
    bool acted = false;
    switch (trip.phase)
    {
        case CortexTripPhase::APPROACH:
            acted = Approach(trip, leg);
            break;
        case CortexTripPhase::WAIT:
            acted = Wait(trip, leg);
            break;
        case CortexTripPhase::BOARD:
            acted = Board(trip, leg);
            break;
        case CortexTripPhase::RIDE:
            acted = Ride(trip, leg);
            break;
        case CortexTripPhase::EXIT:
            acted = Exit(trip, leg);
            break;
        case CortexTripPhase::ACTIVATE:
            acted = Activate(trip, leg);
            break;
        case CortexTripPhase::ARRIVE:
            acted = Arrive(trip, leg);
            break;
    }
    failed = tripFailed;
    return acted;
}

void CortexTravelAction::SetPhase(CortexTrip& trip, CortexTripPhase phase)
{
    uint32 now = getMSTime();
    trip.phase = phase;
    trip.phaseMs = now;
    trip.lostMs = 0;
    if (phase == CortexTripPhase::APPROACH)
    {
        trip.best = std::numeric_limits<float>::max();
        trip.bestMs = now;
        trip.movedMs = 0;
    }
}

void CortexTravelAction::LegDone(CortexTrip& trip)
{
    uint32 now = getMSTime();
    CortexLeg const& leg = trip.legs[trip.leg];
    // Event names are at most 12 characters (playerbots_telemetry.event); a longer one fails the whole batch.
    BotTelemetry::instance().Event(botAI, "travel_leg", CortexTravel::LegName(leg.kind),
                                   static_cast<int32>(leg.entry),
                                   static_cast<int32>(getMSTimeDiff(trip.legMs, now) / IN_MILLISECONDS),
                                   static_cast<int32>(trip.leg));
    ++trip.leg;
    trip.legMs = now;
    trip.boardTries = 0;
    if (trip.leg >= trip.legs.size())
    {
        trip.active = false;
        trip.finished = true;
        return;
    }
    SetPhase(trip, FirstPhase(trip.legs[trip.leg]));
}

// Gives up the leg: plans again from where the bot is without the link (or flights) that failed, at most twice,
// then gives up the trip.
void CortexTravelAction::Fail(CortexTrip& trip, char const* reason, bool replan)
{
    uint32 now = getMSTime();
    CortexLeg const leg = trip.legs[trip.leg];
    BotTelemetry::instance().Event(botAI, "travel_fail", std::string(reason) + " " + CortexTravel::LegName(leg.kind),
                                   static_cast<int32>(leg.entry), static_cast<int32>(trip.leg),
                                   static_cast<int32>(getMSTimeDiff(trip.legMs, now) / IN_MILLISECONDS));
    // "map": the bot was moved elsewhere (death, hearth), not the link's fault.
    bool linkFault = leg.link && std::strcmp(reason, "map") != 0;
    if (linkFault)
        trip.excluded.push_back(static_cast<uint16>(leg.link - 1));
    // And for a while not in this bot's next trips either ("frozen" is the walk's fault, not the link's).
    if (linkFault && replan && tripPlan)
    {
        uint16 id = static_cast<uint16>(leg.link - 1);
        uint32 until = now + LINK_BACKOFF_MS;
        auto bad = std::find_if(tripPlan->badLinks.begin(), tripPlan->badLinks.end(),
                                [id](std::pair<uint16, uint32> const& b) { return b.first == id; });
        if (bad != tripPlan->badLinks.end())
            bad->second = until;
        else
            tripPlan->badLinks.emplace_back(id, until);
    }
    if (leg.kind == CortexLegKind::FLIGHT)
        trip.noFlights = true;
    if (leg.kind == CortexLegKind::HEARTH)
        trip.noHearth = true;

    if (replan && !bot->GetTransport() && trip.replans < TRIP_REPLANS)
    {
        CortexTrip next;
        next.allowDirect = trip.allowDirect;
        next.noFlights = trip.noFlights;
        next.noHearth = trip.noHearth;
        next.excluded = trip.excluded;
        if (CortexTravel::Plan(bot, trip.map, trip.x, trip.y, trip.z, trip.mapArea, trip.allowDirect, next))
        {
            next.active = true;
            next.map = trip.map;
            next.x = trip.x;
            next.y = trip.y;
            next.z = trip.z;
            next.mapArea = trip.mapArea;
            next.replans = static_cast<uint8>(trip.replans + 1);
            next.startMs = trip.startMs;
            next.legMs = now;
            next.seenMs = now;
            next.seenAwayMs = Cortex::AwayMs(bot);
            trip = std::move(next);
            SetPhase(trip, FirstPhase(trip.legs.front()));
            BotTelemetry::instance().Event(botAI, "travel_plan", "replan " + CortexTravel::Describe(trip.legs),
                                           static_cast<int32>(trip.legs.size()), static_cast<int32>(trip.estimate),
                                           static_cast<int32>(trip.replans));
            return;
        }
    }
    trip.active = false;
    trip.failed = true;
    tripFailed = true;
}

// A move without pathfinding: onto a deck or lift and off it, where the navmesh has no ground. Not MovePoint: its
// generator pathfinds even with generatePath off (PointMovementGenerator hands MoveTo `true`), and with no navmesh on
// the deck the path stopped at the pier's edge ("board" failures). A path of three points is moved as given
// (EscortMovementGenerator), in deck coordinates while the bot is a passenger.
void CortexTravelAction::StepTo(float x, float y, float z)
{
    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);
    float fromX = bot->GetPositionX();
    float fromY = bot->GetPositionY();
    float fromZ = bot->GetPositionZ();
    if (bot->GetExactDist(x, y, z) < 0.3f)
        return;
    Movement::PointsArray path;
    path.emplace_back(fromX, fromY, fromZ);
    path.emplace_back((fromX + x) * 0.5f, (fromY + y) * 0.5f, (fromZ + z) * 0.5f);
    path.emplace_back(x, y, z);
    bot->GetMotionMaster()->MoveSplinePath(&path, FORCED_MOVEMENT_RUN);
}

// Makes the bot the transport's passenger when the map's own test (a ray down onto the transport's model, what
// playerbots' once-a-second check uses) puts it on the deck: rather than at that check, which may come after a lift's
// short rest, and in agreement with it, so the check doesn't take it off again.
bool CortexTravelAction::Attach(GameObject* go)
{
    Transport* transport = go ? go->ToTransport() : nullptr;
    if (!transport)
        return false;
    if (bot->GetTransport() == transport)
        return true;
    if (bot->GetMap()->GetTransportForPos(bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(),
                                          bot->GetPositionZ(), bot) != transport)
        return false;
    // Stopped in world coordinates before the deck's take over.
    if (!bot->movespline->Finalized())
        bot->StopMoving();
    if (Transport* old = bot->GetTransport())
        Detach(old);
    transport->AddPassenger(bot, true);
    return bot->GetTransport() == transport;
}

void CortexTravelAction::Detach(Transport* transport)
{
    // A walk started aboard is in deck coordinates: taken off the deck mid-walk, the next spline update reads them as
    // world coordinates and puts the bot next to the map's origin (the "frozen" legs at 0, 10, 0). Stop it first.
    if (!bot->movespline->Finalized())
        bot->StopMoving();
    transport->RemovePassenger(bot, true);
}

// The ship or zeppelin on this map (wherever it is on its route), or the lift of the leg.
GameObject* CortexTravelAction::FindTransport(CortexTrip& /*trip*/, CortexLeg const& leg)
{
    for (Transport* transport : bot->GetMap()->GetAllTransports())
    {
        if (!transport || !transport->IsInWorld() || transport->GetEntry() != leg.entry)
            continue;
        if (leg.kind == CortexLegKind::LIFT &&
            (!transport->IsStaticTransport() || transport->GetExactDist2d(leg.liftX, leg.liftY) > LIFT_MATCH))
            continue;
        return transport;
    }
    return nullptr;
}

// Whether the transport has stood still for a moment (in at a pier, or a lift at a stop).
bool CortexTravelAction::Still(CortexTrip& trip, GameObject* transport)
{
    uint32 now = getMSTime();
    float dx = transport->GetPositionX() - trip.lastTX;
    float dy = transport->GetPositionY() - trip.lastTY;
    float dz = transport->GetPositionZ() - trip.lastTZ;
    trip.lastTX = transport->GetPositionX();
    trip.lastTY = transport->GetPositionY();
    trip.lastTZ = transport->GetPositionZ();
    if (!trip.stillMs || dx * dx + dy * dy + dz * dz > STILL_MOVE * STILL_MOVE)
    {
        trip.stillMs = now;
        return false;
    }
    return getMSTimeDiff(trip.stillMs, now) >= STILL_MS;
}

// A flight master in reach, not hostile, whose flight point is `node`.
Creature* CortexTravelAction::FindFlightMaster(uint32 node)
{
    std::list<Unit*> units;
    Acore::AnyUnitInObjectRangeCheck check(bot, FLIGHT_MASTER_SEARCH);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
    Cell::VisitObjects(bot, searcher, FLIGHT_MASTER_SEARCH);

    Creature* best = nullptr;
    float bestDist = FLIGHT_MASTER_SEARCH;
    for (Unit* unit : units)
    {
        Creature* creature = unit->ToCreature();
        if (!creature || !creature->IsAlive() || !creature->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER) ||
            bot->IsHostileTo(creature))
            continue;
        if (sObjectMgr->GetNearestTaxiNode(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(),
                                           creature->GetMapId(), bot->GetTeamId(true)) != node)
            continue;
        float d = bot->GetExactDist(creature);
        if (d < bestDist)
        {
            best = creature;
            bestDist = d;
        }
    }
    return best;
}

// Walking to the leg's start; at a portal, through it.
bool CortexTravelAction::Approach(CortexTrip& trip, CortexLeg const& leg)
{
    if (bot->GetMapId() != leg.map)
    {
        Fail(trip, "map");
        return false;
    }
    uint32 now = getMSTime();
    float d = bot->GetExactDist2d(leg.x, leg.y);

    if (leg.kind == CortexLegKind::PORTAL)
    {
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(leg.entry);
        if (!trigger)
        {
            Fail(trip, "trigger");
            return false;
        }
        // A client tells the server when it walks into a trigger; the bot does the same.
        if (bot->IsInAreaTriggerRadius(trigger))
        {
            WorldPacket packet(CMSG_AREATRIGGER, 4);
            packet << trigger->entry;
            bot->GetSession()->HandleAreaTriggerOpcode(packet);
            SetPhase(trip, CortexTripPhase::ARRIVE);
            return true;
        }
    }
    else if (leg.kind == CortexLegKind::FLIGHT && d <= FLIGHT_MASTER_SEARCH && !leg.nodes.empty())
    {
        if (Creature* master = FindFlightMaster(leg.nodes.front()))
        {
            if (bot->IsWithinDistInMap(master, INTERACTION_DISTANCE - 1.0f))
            {
                SetPhase(trip, CortexTripPhase::ACTIVATE);
                return Activate(trip, leg);
            }
            if (getMSTimeDiff(trip.bestMs, now) >= LEG_STUCK_MS)
            {
                Fail(trip, "approach");
                return false;
            }
            // On another level (Orgrimmar's master is on top of the tower, the bot at its foot): the walk there,
            // whose stuck handling paths over steep stairs (playerbots 0064); the walk to an object doesn't.
            if (std::fabs(master->GetPositionZ() - bot->GetPositionZ()) > FLIGHT_MASTER_LEVEL_Z)
                return MoveFarTo(WorldPosition(master->GetMapId(), master->GetPositionX(), master->GetPositionY(),
                                               master->GetPositionZ()));
            return MoveWorldObjectTo(master->GetGUID(), INTERACTION_DISTANCE - 1.0f);
        }
        if (d <= LEG_ARRIVE)
        {
            Fail(trip, "master");
            return false;
        }
    }
    else if ((leg.kind == CortexLegKind::BOAT || leg.kind == CortexLegKind::LIFT) && d <= LEG_ARRIVE &&
             std::fabs(bot->GetPositionZ() - leg.z) <= LEG_ARRIVE_Z)
    {
        if (bot->isMoving())
            bot->StopMoving();
        SetPhase(trip, CortexTripPhase::WAIT);
        return true;
    }

    // A walk that doesn't move the bot at all (a pocket mmap can't route out of) is the walk's problem, not the
    // link's: another link won't help.
    if (!trip.movedMs || bot->GetExactDist2d(trip.movedX, trip.movedY) > FROZEN_MOVE)
    {
        trip.movedX = bot->GetPositionX();
        trip.movedY = bot->GetPositionY();
        trip.movedMs = now;
    }
    else if (getMSTimeDiff(trip.movedMs, now) >= FROZEN_MS)
    {
        Fail(trip, "frozen", false);
        return false;
    }
    // Getting closer: a long walk often heads away first (out of a valley, round a ridge), so it gets as long as
    // the straight walk would take before it counts as stuck.
    if (trip.best == std::numeric_limits<float>::max())
        trip.approachDist = d;
    if (d + LEG_PROGRESS < trip.best)
    {
        trip.best = d;
        trip.bestMs = now;
    }
    uint32 window = std::max(LEG_STUCK_MS, static_cast<uint32>(trip.approachDist / WALK_SPEED * IN_MILLISECONDS));
    if (getMSTimeDiff(trip.bestMs, now) >= window)
    {
        Fail(trip, "approach");
        return false;
    }
    // A leg start up a lift from here (a flight master on Thunder Bluff's mesas): MoveFarTo gives up on it at once
    // (bots 0064), so the walk would be asked for again every tick. The plan needs the lift first.
    if (BotLevels::LiftOnly(bot, leg.map, leg.x, leg.y, leg.z))
    {
        Fail(trip, "levels");
        return false;
    }
    return MoveFarTo(WorldPosition(leg.map, leg.x, leg.y, leg.z));
}

// At the pier or landing until the ship is in or the lift is at this stop; then picks the spot on the deck.
bool CortexTravelAction::Wait(CortexTrip& trip, CortexLeg const& leg)
{
    uint32 now = getMSTime();
    if (bot->GetExactDist2d(leg.x, leg.y) > LEG_DRIFT)
    {
        // Pushed off (a fight, a missed boarding): back to the pier once any step back has ended.
        if (bot->movespline->Finalized())
            SetPhase(trip, CortexTripPhase::APPROACH);
        return true;
    }
    if (getMSTimeDiff(trip.phaseMs, now) >= (leg.kind == CortexLegKind::BOAT ? BOAT_WAIT_MS : LIFT_WAIT_MS))
    {
        Fail(trip, "wait");
        return false;
    }
    GameObject* transport = FindTransport(trip, leg);
    if (!transport)
    {
        // A ship may be on its other map for now; a lift is always there.
        if (leg.kind == CortexLegKind::LIFT)
        {
            Fail(trip, "nolift");
            return false;
        }
        return true;
    }

    if (leg.kind == CortexLegKind::LIFT)
    {
        uint32 until = 0;
        uint32 stay = 0;
        StaticTransport* lift = transport->ToStaticTransport();
        if (lift && LiftWindow(lift, leg.stopZ, LIFT_STOP, until, stay))
        {
            // From the timetable: set off as the lift comes in, and only if it rests long enough for the walk to its
            // middle; otherwise the next one. (Thunder Bluff's lifts rest ~6 s per 30 s round, Undercity's ~5 s.)
            float speed = std::max(bot->GetSpeed(MOVE_RUN), 2.0f);
            uint32 walkMs = static_cast<uint32>(
                bot->GetExactDist2d(transport->GetPositionX(), transport->GetPositionY()) / speed * IN_MILLISECONDS);
            if (until > LIFT_LEAD_MS || until + stay < walkMs + LIFT_SLACK_MS)
                return true;
        }
        else
        {
            // At the stop, or coming in to it (closer than at the last step).
            float dz = std::fabs(transport->GetPositionZ() - leg.stopZ);
            float before = std::fabs(trip.lastTZ - leg.stopZ);
            trip.lastTX = transport->GetPositionX();
            trip.lastTY = transport->GetPositionY();
            trip.lastTZ = transport->GetPositionZ();
            if (dz > LIFT_STOP && (dz > LIFT_ARRIVING || dz >= before))
                return true;
        }
    }
    else if (!Still(trip, transport) || transport->GetExactDist2d(leg.x, leg.y) > DOCK_RANGE)
        return true;

    // The spot on the deck. A lift: its middle. A ship or zeppelin: part way from the pier to its middle (clear of
    // the mast), where a ray down from a little above the pier meets the deck.
    float top = std::max(leg.z, leg.stopZ) + DECK_SEARCH_HEIGHT;
    float depth = std::fabs(leg.z - leg.stopZ) + 2.0f * DECK_SEARCH_HEIGHT;
    bool found = false;
    if (leg.kind == CortexLegKind::LIFT)
    {
        // From just above the lift's own height, so a cage roof isn't taken for the floor; where the floor will be
        // once the lift stands at the stop.
        trip.boardX = transport->GetPositionX();
        trip.boardY = transport->GetPositionY();
        trip.boardZ = leg.stopZ;
        float deck = 0.0f;
        if (DeckHeight(transport, bot->GetPhaseMask(), trip.boardX, trip.boardY, transport->GetPositionZ() + 3.0f,
                       std::fabs(leg.z - leg.stopZ) + 10.0f, deck))
            trip.boardZ = leg.stopZ + (deck - transport->GetPositionZ());
        found = true;
    }
    else
    {
        for (float f : {0.5f, 0.65f, 0.35f, 0.8f, 0.9f, 1.0f})
        {
            float x = leg.x + (transport->GetPositionX() - leg.x) * f;
            float y = leg.y + (transport->GetPositionY() - leg.y) * f;
            float deck = 0.0f;
            if (!DeckHeight(transport, bot->GetPhaseMask(), x, y, top, depth, deck) ||
                std::fabs(deck - leg.z) > DECK_SEARCH_HEIGHT)
                continue;
            trip.boardX = x;
            trip.boardY = y;
            trip.boardZ = deck;
            found = true;
            break;
        }
    }
    if (!found)
        return true;

    trip.boardZ += 0.5f;
    trip.onSpot = false;
    SetPhase(trip, CortexTripPhase::BOARD);
    return Board(trip, leg);
}

// Stepping onto the deck. Playerbots makes a bot standing on a transport its passenger (PlayerbotAI::UpdateAI checks
// once a second), which is what the bot waits for here.
bool CortexTravelAction::Board(CortexTrip& trip, CortexLeg const& leg)
{
    uint32 now = getMSTime();
    GameObject* transport = FindTransport(trip, leg);
    if (transport && bot->GetTransport() == transport->ToTransport())
    {
        if (bot->isMoving())
            bot->StopMoving();
        SetPhase(trip, CortexTripPhase::RIDE);
        return true;
    }

    bool here = false;
    bool onSpot = bot->GetExactDist(trip.boardX, trip.boardY, trip.boardZ) <= ATTACH_DIST;
    if (transport && leg.kind == CortexLegKind::BOAT)
    {
        here = transport->GetExactDist2d(leg.x, leg.y) <= DOCK_RANGE && Still(trip, transport);
        // On the deck spot with the ship in: its passenger now.
        if (here && onSpot && Attach(transport))
        {
            SetPhase(trip, CortexTripPhase::RIDE);
            return true;
        }
    }
    else if (transport)
    {
        float dz = std::fabs(transport->GetPositionZ() - leg.stopZ);
        here = dz <= LIFT_ARRIVING;
        // On the lift's middle with the lift at the stop: its passenger now, rather than at playerbots' next
        // once-a-second check, which may come after the rest. Not before the lift is at the stop: the bot would ride
        // along standing in the air above the floor.
        if (dz <= 1.5f * LIFT_STOP && (onSpot || bot->GetExactDist2d(transport->GetPositionX(),
                                                                     transport->GetPositionY()) <= LIFT_ON))
        {
            if (Attach(transport))
            {
                SetPhase(trip, CortexTripPhase::RIDE);
                return true;
            }
            // The ray missed the floor (a gap in the model): on its middle all the same, as before.
            Transport* lift = transport->ToTransport();
            if (lift && bot->GetExactDist2d(transport->GetPositionX(), transport->GetPositionY()) <= LIFT_ON &&
                std::fabs(bot->GetPositionZ() - trip.boardZ) <= 3.0f)
            {
                if (!bot->movespline->Finalized())
                    bot->StopMoving();
                lift->AddPassenger(bot, true);
                if (bot->GetTransport() == lift)
                {
                    SetPhase(trip, CortexTripPhase::RIDE);
                    return true;
                }
            }
        }
    }
    if (bot->GetExactDist(trip.boardX, trip.boardY, trip.boardZ) <= 2.0f)
        trip.onSpot = true;
    if (!here || getMSTimeDiff(trip.phaseMs, now) >= BOARD_MS)
    {
        // Missed it: back to the pier to wait for the next one. "attach": the bot stood on the deck but never became
        // its passenger; "board": it didn't get there in time.
        StepTo(leg.x, leg.y, leg.z);
        if (++trip.boardTries >= BOARD_TRIES)
        {
            Fail(trip, trip.onSpot ? "attach" : "board");
            return false;
        }
        SetPhase(trip, CortexTripPhase::WAIT);
        return true;
    }
    if (bot->movespline->Finalized() && bot->GetExactDist(trip.boardX, trip.boardY, trip.boardZ) > 1.0f)
        StepTo(trip.boardX, trip.boardY, trip.boardZ);
    return true;
}

// Aboard until the ship is in at the far pier or the lift at the far stop.
bool CortexTravelAction::Ride(CortexTrip& trip, CortexLeg const& leg)
{
    uint32 now = getMSTime();
    Transport* transport = bot->GetTransport();
    if (!transport)
    {
        if (!trip.lostMs)
            trip.lostMs = now;
        bool atExit = bot->GetMapId() == leg.toMap && bot->GetExactDist2d(leg.toX, leg.toY) <= DOCK_RANGE &&
                      (leg.kind == CortexLegKind::BOAT || std::fabs(bot->GetPositionZ() - leg.toZ) <= LEG_ARRIVE_Z);
        if (atExit)
        {
            SetPhase(trip, CortexTripPhase::EXIT);
            return Exit(trip, leg);
        }
        // A map change takes a moment.
        if (getMSTimeDiff(trip.lostMs, now) < LOST_MS)
            return true;
        bool atStart = bot->GetMapId() == leg.map && bot->GetExactDist2d(leg.x, leg.y) <= DOCK_RANGE &&
                       (leg.kind == CortexLegKind::BOAT || std::fabs(bot->GetPositionZ() - leg.z) <= LEG_ARRIVE_Z);
        if (atStart && ++trip.boardTries < BOARD_TRIES)
        {
            // Never got going: wait for the next one.
            StepTo(leg.x, leg.y, leg.z);
            SetPhase(trip, CortexTripPhase::WAIT);
            return true;
        }
        Fail(trip, "ride");
        return false;
    }
    trip.lostMs = 0;

    bool still = Still(trip, transport);
    bool there = leg.kind == CortexLegKind::BOAT
                     ? bot->GetMapId() == leg.toMap && transport->GetExactDist2d(leg.toX, leg.toY) <= DOCK_RANGE
                     : std::fabs(transport->GetPositionZ() - leg.toStopZ) <= LIFT_STOP;
    // A lift rests only 5 s: off it as soon as it is at the stop.
    if (there && (still || leg.kind == CortexLegKind::LIFT))
    {
        SetPhase(trip, CortexTripPhase::EXIT);
        return Exit(trip, leg);
    }
    // Nothing to do but stand there.
    return true;
}

// Stepping off onto the far pier or landing.
bool CortexTravelAction::Exit(CortexTrip& trip, CortexLeg const& leg)
{
    uint32 now = getMSTime();
    Transport* transport = bot->GetTransport();
    if (bot->GetMapId() == leg.toMap && bot->GetExactDist2d(leg.toX, leg.toY) <= EXIT_ARRIVE)
    {
        // Off the deck: no longer its passenger, before playerbots' once-a-second check notices.
        if (transport)
            Detach(transport);
        LegDone(trip);
        return true;
    }
    if (transport && leg.kind == CortexLegKind::LIFT &&
        bot->GetExactDist2d(transport->GetPositionX(), transport->GetPositionY()) > LIFT_ON + 1.5f)
    {
        // Off the platform: no longer carried by it, even if the lift sets off before playerbots' check notices.
        Detach(transport);
        transport = nullptr;
    }
    if (transport)
    {
        // It left again before the bot got off: ride on, it comes back.
        bool gone = leg.kind == CortexLegKind::BOAT
                        ? transport->GetExactDist2d(leg.toX, leg.toY) > DOCK_RANGE
                        : std::fabs(transport->GetPositionZ() - leg.toStopZ) > 2.0f * LIFT_STOP;
        if (gone || getMSTimeDiff(trip.phaseMs, now) >= EXIT_MS)
        {
            if (bot->isMoving())
                bot->StopMoving();
            SetPhase(trip, CortexTripPhase::RIDE);
            return true;
        }
    }
    else if (getMSTimeDiff(trip.phaseMs, now) >= EXIT_MS)
    {
        Fail(trip, "exit");
        return false;
    }
    if (bot->movespline->Finalized())
        StepTo(leg.toX, leg.toY, leg.toZ);
    return true;
}

// Talking to the flight master, or casting the teleport.
bool CortexTravelAction::Activate(CortexTrip& trip, CortexLeg const& leg)
{
    if (leg.kind == CortexLegKind::HEARTH)
    {
        if (bot->IsNonMeleeSpellCast(false))
            return true;
        if (!CortexTravel::HearthReady(bot))
        {
            Fail(trip, "hearth");
            return false;
        }
        if (bot->isMoving())
            bot->StopMoving();
        if (bot->IsMounted())
            bot->Dismount();
        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        botAI->RemoveShapeshift();
        if (!botAI->DoSpecificAction("hearthstone", Event(), true))
        {
            Fail(trip, "cast");
            return false;
        }
        SetPhase(trip, CortexTripPhase::ARRIVE);
        return true;
    }

    if (leg.kind == CortexLegKind::SPELL)
    {
        if (bot->IsNonMeleeSpellCast(false))
            return true;
        if (!bot->HasSpell(leg.entry) || bot->HasSpellCooldown(leg.entry))
        {
            Fail(trip, "spell");
            return false;
        }
        if (bot->isMoving())
            bot->StopMoving();
        if (bot->IsMounted())
            bot->Dismount();
        if (bot->CastSpell(bot, leg.entry, false) != SPELL_CAST_OK)
        {
            Fail(trip, "cast");
            return false;
        }
        SetPhase(trip, CortexTripPhase::ARRIVE);
        return true;
    }

    // FLIGHT: the flight master of the departure node, the fare, then what a click on the destination does.
    Creature* master = leg.nodes.empty() ? nullptr : FindFlightMaster(leg.nodes.front());
    if (!master || !bot->IsWithinDistInMap(master, INTERACTION_DISTANCE))
    {
        SetPhase(trip, CortexTripPhase::APPROACH);
        return false;
    }
    if (bot->GetMoney() < leg.price)
    {
        Fail(trip, "money");
        return false;
    }
    if (bot->isMoving())
        bot->StopMoving();
    botAI->RemoveShapeshift();
    if (bot->IsMounted())
        bot->Dismount();
    bot->SetFacingToObject(master);
    bot->GetSession()->SendLearnNewTaxiNode(master);
    if (!bot->ActivateTaxiPathTo(leg.nodes, master, 0))
    {
        Fail(trip, "taxi");
        return false;
    }
    SetPhase(trip, CortexTripPhase::ARRIVE);
    return true;
}

// In flight or being teleported, until the bot is at the other end.
bool CortexTravelAction::Arrive(CortexTrip& trip, CortexLeg const& leg)
{
    uint32 since = getMSTimeDiff(trip.phaseMs, getMSTime());
    bool there = bot->GetMapId() == leg.toMap &&
                 bot->GetExactDist2d(leg.toX, leg.toY) <=
                     (leg.kind == CortexLegKind::FLIGHT ? FLIGHT_ARRIVE : TELEPORT_ARRIVE);
    if (leg.kind == CortexLegKind::FLIGHT)
    {
        if (bot->IsInFlight())
            return true;
        if (there)
        {
            LegDone(trip);
            return true;
        }
        if (since < FLIGHT_START_MS)
            return true;
        Fail(trip, "flight");
        return false;
    }
    if (there)
    {
        LegDone(trip);
        return true;
    }
    if (bot->IsNonMeleeSpellCast(false) || bot->IsBeingTeleported() || since < TELEPORT_MS)
        return true;
    Fail(trip, leg.kind == CortexLegKind::PORTAL ? "portal" : (leg.kind == CortexLegKind::HEARTH ? "hearth" : "spell"));
    return false;
}

bool CortexRideAction::isUseful()
{
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight())
        return false;
    CortexPlan* plan = Cortex::Plan(bot);
    return plan && CortexTravel::Busy(plan->trip);
}

bool CortexRideAction::Execute(Event /*event*/)
{
    CortexPlan* plan = Cortex::Plan(bot);
    if (!plan)
        return false;
    bool failed = false;
    return TripStep(*plan, failed);
}
