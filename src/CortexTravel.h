/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#ifndef MOD_CORTEX_CORTEXTRAVEL_H
#define MOD_CORTEX_CORTEXTRAVEL_H

#include <string>
#include <vector>

#include "Define.h"
#include "ObjectGuid.h"

class Player;

// Long-distance travel (plan.md B2b, docs/bot-ai-overhaul.md): flights between taxi nodes the bot knows, and the
// links walking can't cross: boats and zeppelins, city lifts, the Rut'theran and Wizard's Sanctum portals,
// Teleport: Moonglade. A trip is a list of legs; walking to the start of each leg and from the last one to the
// destination is left to the normal walk. Links are a hand-made table (CortexTravel.cpp) checked against the world DB.

enum class CortexLegKind : uint8
{
    FLIGHT,  // taxi from a flight master, through known nodes only
    BOAT,    // ship or zeppelin (a moving transport); may change map
    LIFT,    // elevator (a static transport): Thunder Bluff, Undercity, the Great Lift, Freewind Post
    PORTAL,  // area trigger that teleports (Rut'theran <-> Darnassus, Stormwind's Wizard's Sanctum)
    SPELL,   // a teleport spell the bot knows (Teleport: Moonglade)
    HEARTH,  // the hearthstone, to the inn the bot is bound at
};

enum class CortexTripPhase : uint8
{
    APPROACH,  // walking to the leg's start: the flight master, the pier, the lift landing, the portal
    WAIT,      // at the pier or landing until the transport is there
    BOARD,     // stepping onto the deck
    RIDE,      // aboard
    EXIT,      // stepping off at the far end
    ACTIVATE,  // talking to the flight master, casting the spell
    ARRIVE,    // in flight or being teleported
};

struct CortexLeg
{
    CortexLegKind kind = CortexLegKind::FLIGHT;
    uint16 link = 0;    // directed link id + 1 (0 for flights): failed links are left out of the next plan
    uint32 entry = 0;   // transport or lift entry, area trigger, spell; FLIGHT: the destination node
    uint32 map = 0;     // where the leg starts (the pier, landing, trigger or flight master's node)
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32 toMap = 0;   // where it ends (the far pier or landing, the teleport target, the last node)
    float toX = 0.0f;
    float toY = 0.0f;
    float toZ = 0.0f;
    float stopZ = 0.0f;    // LIFT: the lift's height at the boarding stop
    float toStopZ = 0.0f;  // LIFT: and at the exit stop
    float liftX = 0.0f;
    float liftY = 0.0f;
    uint32 price = 0;            // FLIGHT: copper
    std::vector<uint32> nodes;   // FLIGHT: taxi nodes, departure first
};

struct CortexTrip
{
    bool active = false;
    bool finished = false;     // the last leg is done: the caller takes a fresh look at the target
    bool failed = false;       // the last trip toward (map, x, y) gave up: the caller counts it as unreachable
    bool allowDirect = false;  // walking straight there was an option when it was planned
    bool noFlights = false;    // a flight leg failed: replans leave flights out
    bool noHearth = false;     // the hearthstone leg failed (cast interrupted): replans leave it out
    uint32 map = 0;            // the destination the trip is for
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32 mapArea = 0;
    std::vector<CortexLeg> legs;
    uint32 leg = 0;
    CortexTripPhase phase = CortexTripPhase::APPROACH;
    uint32 startMs = 0;
    uint32 legMs = 0;
    uint32 phaseMs = 0;
    uint32 seenMs = 0;      // last step
    uint32 seenAwayMs = 0;  // Cortex::AwayMs then: time away (fighting, dead, recovering) doesn't count
    float best = 0.0f;     // APPROACH: closest so far to the leg's start, and when
    uint32 bestMs = 0;
    float approachDist = 0.0f;  // APPROACH: distance to the leg's start when the walk began
    float movedX = 0.0f;        // APPROACH: where the bot last stood still from, and since when
    float movedY = 0.0f;
    uint32 movedMs = 0;
    bool onSpot = false;        // BOARD: the bot got onto the deck spot at least once
    ObjectGuid transport;  // BOAT, LIFT: the transport being waited for or ridden
    float lastTX = 0.0f;   // its position at the last step, to tell when it stands still
    float lastTY = 0.0f;
    float lastTZ = 0.0f;
    uint32 stillMs = 0;    // standing still since
    uint32 lostMs = 0;     // RIDE: not a passenger since (a map change takes a moment)
    float boardX = 0.0f;   // BOARD: the spot on the deck
    float boardY = 0.0f;
    float boardZ = 0.0f;
    uint8 boardTries = 0;
    uint8 replans = 0;
    std::vector<uint16> excluded;  // links that failed on this trip
    uint32 estimate = 0;           // seconds the plan expected
};

namespace CortexTravel
{
    void LoadConfig();
    // Cortex.Travel: transports, portals, lifts, spells. Flights and flight-point discovery have their own switch.
    bool Enabled();
    bool Flights();
    bool LearnFlights();
    // Cortex.Travel.Hearth: the hearthstone home is a way to get somewhere near the inn the bot is bound at.
    bool Hearth();
    // Cortex.Travel.CrossMap: finished quests whose turn-in is on another continent are handed in by boat or zeppelin.
    bool CrossMap();

    // The part of a map a bot can walk around in: Rut'theran Village, the top of Teldrassil, Thunder Bluff's mesas,
    // Freewind Post, Undercity under the ruins, the Wizard's Sanctum basement in Stormwind, and the rest of each
    // continent. `mapArea` is the quest map's WorldMapArea (quest_poi), which names Undercity, Thunder Bluff and
    // Darnassus the way the client's map does when z alone can't (a point in Undercity lies under the ruins and the
    // hills). 0 = unknown, matches any.
    uint8 Region(uint32 map, float x, float y, float z, uint32 mapArea);
    // The bot can walk to the place: same map and region (always true with travel off, on the same map).
    bool SameRegion(Player* bot, uint32 map, float x, float y, float z, uint32 mapArea);

    // Plans a trip from the bot to (map, x, y, z); z may be INVALID_HEIGHT for another map. With `allowDirect`
    // walking straight there is an option and the trip is kept only if it beats the walk clearly. Fills trip.legs
    // and trip.estimate; false when there is no trip worth taking.
    bool Plan(Player* bot, uint32 map, float x, float y, float z, uint32 mapArea, bool allowDirect, CortexTrip& trip);
    // The hearthstone is in the bags and ready.
    bool HearthReady(Player* bot);
    // Walking straight from (x1, y1) to (x2, y2) on the bot's map passes a place where it died lately to something
    // well above its level, or through cells where it has seen mobs well above it (B3 route exposure; its own
    // memory only).
    bool DangerousWalk(Player* bot, float x1, float y1, float x2, float y2);
    // Cortex.Travel.Recheck: the rest of a walk is looked at again on the way, grind spots and givers past a walk to
    // avoid aren't picked, and a search point a danger walk away sets its cluster aside.
    bool Recheck();
    // The bot stands among mobs well above it: several such cells within `radius`.
    bool OutOfDepth(Player* bot, float radius);
    // Whether some trip reaches (map, x, y) from where the bot is (remote hand-ins).
    bool Reachable(Player* bot, uint32 map, float x, float y, uint32 mapArea);
    // The way out of a place only a portal leaves (the Wizard's Sanctum basement): the nearest portal trigger in the
    // bot's region within `range`, and where it leads. False when there's none.
    bool PortalOut(Player* bot, float range, float& toX, float& toY, float& toZ);

    // A phase that has to run to its end once started: waiting at the pier, boarding, aboard, stepping off,
    // casting, being teleported.
    bool Busy(CortexTrip const& trip);
    char const* LegName(CortexLegKind kind);
    // "boat-portal" (telemetry)
    std::string Describe(std::vector<CortexLeg> const& legs);
}

#endif
