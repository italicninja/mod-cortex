/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include "CortexTravel.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Config.h"
#include "Cortex.h"
#include "CortexMemory.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"

namespace
{
    bool sEnabled = true;
    bool sFlights = true;
    bool sLearnFlights = true;
    bool sCrossMap = true;
    bool sHearth = true;
    bool sHighZones = true;
    bool sRecheck = true;

    enum Region : uint8
    {
        REGION_ANY = 0,  // unknown: matches any region on the map
        REGION_KALIMDOR,
        REGION_RUTTHERAN,
        REGION_TELDRASSIL,  // the top of the tree, Darnassus included
        REGION_THUNDER_BLUFF,
        REGION_FREEWIND,
        REGION_EASTERN_KINGDOMS,
        REGION_UNDERCITY,
        REGION_WIZARD_SANCTUM,  // the Stormwind mage tower's basement: the four mage trainers, in by portal only
        REGION_OTHER,  // any other map: one region per map
    };

    // quest_poi.WorldMapAreaId of the client's city maps (Teldrassil, 41, holds both Rut'theran and the top)
    constexpr uint32 MAP_AREA_THUNDER_BLUFF = 362;
    constexpr uint32 MAP_AREA_DARNASSUS = 381;
    constexpr uint32 MAP_AREA_UNDERCITY = 382;

    constexpr float WALK_SPEED = 7.0f;       // yd/s, on foot
    constexpr float WALK_DETOUR = 1.25f;     // a walk is longer than the straight line
    constexpr float FLIGHT_SPEED = 30.0f;    // yd/s
    constexpr float FLIGHT_DETOUR = 1.3f;
    constexpr float FLIGHT_HOP_COST = 5.0f;  // seconds per node on the way
    constexpr float FLIGHT_BOARD_COST = 25.0f;  // finding the flight master and talking to it
    constexpr float BETTER_THAN_WALK = 0.8f;    // a trip that could be walked must save a fifth of the time
    constexpr float SPELL_REACH = 700.0f;       // a teleport is for places this close to where it lands
    // The hearthstone: 10 s to cast and a loading screen, and it isn't there again for 30 minutes, so it has to
    // save a few minutes of walking to be worth it.
    constexpr float HEARTH_COST = 120.0f;
    // Route exposure (B3): a walk past a place where the bot died lately to something DANGER_LEVELS or more above it
    // costs DANGER_PENALTY more. Level 17-19 bots walking to hand-ins through Searing Gorge, Alterac and Felwood died
    // there again and again (23 times in 19 min for one) once revives no longer moved them away.
    constexpr float DANGER_RADIUS = 200.0f;
    constexpr int8 DANGER_LEVELS = 5;
    constexpr uint32 DANGER_WINDOW = 2 * HOUR;
    constexpr float DANGER_PENALTY = 900.0f;
    constexpr float DANGER_START_SKIP = 250.0f;  // where it just died is where it walks away from, not into
    // And a walk through a zone well above the bot: HIGH_MOB_CELLS or more cells within HIGH_MOB_RADIUS of the line
    // where it has seen mobs HIGH_MOB_LEVELS or more above it. Deaths only marked a place after the bot died there,
    // and the straight line misses the way the path winds through the mountains: one level 19 walked from Thelsamar
    // to a hand-in in Redridge by Searing Gorge again and again, 124 deaths in 4 h.
    constexpr int32 HIGH_MOB_LEVELS = 6;
    constexpr float HIGH_MOB_RADIUS = 120.0f;
    constexpr uint32 HIGH_MOB_CELLS = 3;
    constexpr uint32 HIGH_MOB_KINDS = 2;
    constexpr float HIGH_MOB_START_SKIP = 60.0f;  // what it is already next to it deals with by fighting or leaving
    constexpr uint32 ITEM_HEARTHSTONE = 6948;
    constexpr uint32 SPELL_HEARTHSTONE = 8690;
    constexpr float INF = std::numeric_limits<float>::max();

    constexpr uint8 TEAM_BIT_ALLIANCE = 1;
    constexpr uint8 TEAM_BIT_HORDE = 2;
    constexpr uint8 TEAM_BIT_BOTH = TEAM_BIT_ALLIANCE | TEAM_BIT_HORDE;

    struct LinkEnd
    {
        uint32 map;
        float x;
        float y;
        float z;
        float stopZ;  // LIFT: the lift's z when it stands at this end
    };

    // One way across something walking can't. Ends are where a player stands: the pier next to the berth, the
    // landing in front of the lift, the trigger. Coordinates from the world DB (gameobject spawns of the lifts,
    // areatrigger / areatrigger_teleport, spell_target_position, creatures and objects standing on the piers) and
    // playerbots_travelnode (the transport stops and the walkable path ends next to them), 2026-10-18.
    struct Link
    {
        char const* name;
        CortexLegKind kind;
        uint32 entry;  // transport or lift gameobject entry, area trigger, spell
        uint8 teams;
        LinkEnd a;     // SPELL: unused (cast anywhere)
        LinkEnd b;
        bool oneWay;   // a -> b only
        float liftX;
        float liftY;
        float cost;    // seconds, waiting included
    };

    Link const LINKS[] = {
        // Kalimdor: Teldrassil
        {"Moonspray (Auberdine - Rut'theran)", CortexLegKind::BOAT, 176244, TEAM_BIT_ALLIANCE,
         {1, 6580.0f, 768.5f, 6.0f, 0.0f}, {1, 8560.0f, 1014.0f, 5.6f, 0.0f}, false, 0.0f, 0.0f, 210.0f},
        {"Rut'theran portal to Darnassus", CortexLegKind::PORTAL, 542, TEAM_BIT_BOTH,
         {1, 8799.41f, 969.79f, 30.24f, 0.0f}, {1, 9945.13f, 2616.89f, 1316.46f, 0.0f}, true, 0.0f, 0.0f, 10.0f},
        {"Darnassus portal to Rut'theran", CortexLegKind::PORTAL, 527, TEAM_BIT_BOTH,
         {1, 9947.48f, 2630.04f, 1318.6f, 0.0f}, {1, 8786.36f, 967.45f, 30.2f, 0.0f}, true, 0.0f, 0.0f, 10.0f},
        // Kalimdor: Thunder Bluff lifts (bottom, top)
        {"Thunder Bluff lift 4170", CortexLegKind::LIFT, 4170, TEAM_BIT_BOTH,
         {1, -1278.1f, 180.6f, 69.4f, 68.8f}, {1, -1284.1f, 180.8f, 130.9f, 130.1f}, false, -1286.24f, 189.72f, 40.0f},
        {"Thunder Bluff lift 4171", CortexLegKind::LIFT, 4171, TEAM_BIT_BOTH,
         {1, -1310.6f, 172.0f, 60.6f, 68.6f}, {1, -1306.2f, 177.1f, 130.9f, 130.1f}, false, -1308.38f, 185.29f, 40.0f},
        {"Thunder Bluff lift 47296", CortexLegKind::LIFT, 47296, TEAM_BIT_BOTH,
         {1, -1018.3f, -21.3f, 70.2f, 69.0f}, {1, -1039.0f, -23.8f, 141.6f, 140.5f}, false, -1028.04f, -28.36f, 40.0f},
        {"Thunder Bluff lift 47297", CortexLegKind::LIFT, 47297, TEAM_BIT_BOTH,
         {1, -1034.9f, -61.0f, 70.2f, 69.3f}, {1, -1046.4f, -42.9f, 141.4f, 140.5f}, false, -1037.27f, -49.24f, 40.0f},
        // Kalimdor: the Great Lift (Thousand Needles below, the Barrens above) and Freewind Post
        {"Great Lift 11898", CortexLegKind::LIFT, 11898, TEAM_BIT_BOTH,
         {1, -4670.0f, -1819.5f, -48.8f, -43.9f}, {1, -4652.7f, -1831.2f, 86.4f, 85.4f}, false, -4665.43f, -1827.67f,
         50.0f},
        {"Great Lift 11899", CortexLegKind::LIFT, 11899, TEAM_BIT_BOTH,
         {1, -4678.8f, -1850.2f, -48.8f, -44.1f}, {1, -4657.0f, -1851.9f, 86.4f, 85.4f}, false, -4670.77f, -1849.61f,
         50.0f},
        {"Freewind Post lift 11898", CortexLegKind::LIFT, 11898, TEAM_BIT_BOTH,
         {1, -5402.0f, -2519.9f, -41.3f, -40.3f}, {1, -5409.0f, -2493.1f, 89.9f, 89.0f}, false, -5399.19f, -2504.62f,
         50.0f},
        {"Freewind Post lift 11899", CortexLegKind::LIFT, 11899, TEAM_BIT_BOTH,
         {1, -5379.1f, -2504.2f, -47.6f, -40.5f}, {1, -5394.2f, -2478.7f, 89.9f, 89.0f}, false, -5382.5f, -2489.42f,
         50.0f},
        // Eastern Kingdoms: Undercity lifts (city below, ruins above)
        {"Undercity lift 20649", CortexLegKind::LIFT, 20649, TEAM_BIT_BOTH,
         {0, 1594.5f, 306.0f, -40.8f, -40.8f}, {0, 1596.2f, 278.7f, 56.0f, 55.7f}, false, 1596.15f, 291.8f, 40.0f},
        {"Undercity lift 20652", CortexLegKind::LIFT, 20652, TEAM_BIT_BOTH,
         {0, 1595.2f, 175.0f, -40.5f, -40.8f}, {0, 1595.6f, 201.7f, 56.0f, 55.7f}, false, 1595.26f, 188.64f, 40.0f},
        {"Undercity lift 20655", CortexLegKind::LIFT, 20655, TEAM_BIT_BOTH,
         {0, 1530.4f, 238.9f, -40.8f, -40.8f}, {0, 1557.3f, 240.4f, 56.0f, 55.7f}, false, 1544.24f, 240.77f, 40.0f},
        // Eastern Kingdoms: the Wizard's Sanctum portals in Stormwind (the room atop the tower, which the Mage Quarter
        // walks up to, and the basement under it, which has no stairs). Ends: the trigger's centre on its floor.
        {"Wizard's Sanctum portal down", CortexLegKind::PORTAL, 704, TEAM_BIT_BOTH,
         {0, -9020.09f, 880.5f, 148.62f, 0.0f}, {0, -9018.56f, 887.66f, 29.63f, 0.0f}, true, 0.0f, 0.0f, 10.0f},
        {"Wizard's Sanctum portal up", CortexLegKind::PORTAL, 702, TEAM_BIT_BOTH,
         {0, -9023.26f, 892.4f, 29.62f, 0.0f}, {0, -9015.76f, 874.94f, 148.62f, 0.0f}, true, 0.0f, 0.0f, 10.0f},
        // Zeppelins and ships
        {"Thundercaller (Orgrimmar - Undercity)", CortexLegKind::BOAT, 164871, TEAM_BIT_HORDE,
         {1, 1323.8f, -4654.6f, 54.3f, 0.0f}, {0, 2063.8f, 279.3f, 97.8f, 0.0f}, false, 0.0f, 0.0f, 420.0f},
        {"Purple Princess (Undercity - Grom'gol)", CortexLegKind::BOAT, 176495, TEAM_BIT_HORDE,
         {0, 2061.9f, 247.5f, 100.6f, 0.0f}, {0, -12418.1f, 210.9f, 32.7f, 0.0f}, false, 0.0f, 0.0f, 420.0f},
        {"Mighty Wind (Orgrimmar - Warsong Hold)", CortexLegKind::BOAT, 186238, TEAM_BIT_HORDE,
         {1, 1175.0f, -4151.2f, 52.4f, 0.0f}, {571, 2830.5f, 6182.8f, 122.9f, 0.0f}, false, 0.0f, 0.0f, 420.0f},
        {"Bravery (Stormwind - Auberdine)", CortexLegKind::BOAT, 176310, TEAM_BIT_ALLIANCE,
         {0, -8643.6f, 1334.7f, 6.1f, 0.0f}, {1, 6427.0f, 818.0f, 5.6f, 0.0f}, false, 0.0f, 0.0f, 420.0f},
        {"Lady Mehley (Menethil - Theramore)", CortexLegKind::BOAT, 176231, TEAM_BIT_ALLIANCE,
         {0, -3898.2f, -602.1f, 5.9f, 0.0f}, {1, -4010.1f, -4719.6f, 5.7f, 0.0f}, false, 0.0f, 0.0f, 360.0f},
        {"Maiden's Fancy (Ratchet - Booty Bay)", CortexLegKind::BOAT, 20808, TEAM_BIT_BOTH,
         {1, -1001.3f, -3825.0f, 6.0f, 0.0f}, {0, -14280.1f, 569.2f, 7.1f, 0.0f}, false, 0.0f, 0.0f, 360.0f},
        // Added 2026-10-18. Berths are the stops of the transport's TaxiPathNode path (delay 60 s, the ferry's 30 s);
        // the pier ends are where players stand next to them: the zeppelin master or dockmaster on that platform or
        // pier, or a walkable point 8-14 yd from the berth (playerbots_travelnode_path, bot telemetry) as for the
        // links above.
        {"Iron Eagle (Orgrimmar - Grom'gol)", CortexLegKind::BOAT, 175080, TEAM_BIT_HORDE,
         {1, 1354.6f, -4636.0f, 54.3f, 0.0f}, {0, -12441.3f, 215.0f, 31.3f, 0.0f}, false, 0.0f, 0.0f, 420.0f},
        {"Zephyr (Orgrimmar - Thunder Bluff)", CortexLegKind::BOAT, 190549, TEAM_BIT_HORDE,
         {1, 1149.0f, -4152.0f, 51.7f, 0.0f}, {1, -1026.5f, 358.5f, 133.0f, 0.0f}, false, 0.0f, 0.0f, 300.0f},
        {"Cloudkisser (Undercity - Vengeance Landing)", CortexLegKind::BOAT, 181689, TEAM_BIT_HORDE,
         {0, 2058.0f, 357.8f, 82.6f, 0.0f}, {571, 1975.0f, -6095.2f, 67.2f, 0.0f}, false, 0.0f, 0.0f, 420.0f},
        {"Northspear (Menethil - Valgarde)", CortexLegKind::BOAT, 181688, TEAM_BIT_ALLIANCE,
         {0, -3726.6f, -576.8f, 4.6f, 0.0f}, {571, 586.0f, -5105.3f, 5.4f, 0.0f}, false, 0.0f, 0.0f, 420.0f},
        {"Stormwind's Pride (Stormwind - Valiance Keep)", CortexLegKind::BOAT, 190536, TEAM_BIT_ALLIANCE,
         {0, -8288.8f, 1410.4f, 5.0f, 0.0f}, {571, 2235.7f, 5129.3f, 5.4f, 0.0f}, false, 0.0f, 0.0f, 420.0f},
        {"Elune's Blessing (Auberdine - Azuremyst)", CortexLegKind::BOAT, 181646, TEAM_BIT_ALLIANCE,
         {1, 6547.0f, 928.0f, 6.0f, 0.0f}, {530, -4270.1f, -11333.1f, 5.6f, 0.0f}, false, 0.0f, 0.0f, 420.0f},
        {"Feathermoon Ferry (Forgotten Coast - Feathermoon)", CortexLegKind::BOAT, 177233, TEAM_BIT_ALLIANCE,
         {1, -4352.0f, 2430.7f, 6.1f, 0.0f}, {1, -4212.1f, 3292.7f, 6.2f, 0.0f}, false, 0.0f, 0.0f, 240.0f},
        // Druids
        {"Teleport: Moonglade", CortexLegKind::SPELL, 18960, TEAM_BIT_BOTH,
         {0, 0.0f, 0.0f, 0.0f, 0.0f}, {1, 7991.88f, -2679.84f, 512.1f, 0.0f}, true, 0.0f, 0.0f, 20.0f},
    };
    constexpr size_t LINK_COUNT = sizeof(LINKS) / sizeof(LINKS[0]);

    struct Directed
    {
        Link const* link;
        uint16 id;  // index * 2 + reverse
        LinkEnd from;
        LinkEnd to;
    };

    std::vector<Directed> const& DirectedLinks()
    {
        static std::vector<Directed> const links = []()
        {
            std::vector<Directed> out;
            for (size_t i = 0; i < LINK_COUNT; ++i)
            {
                Link const& link = LINKS[i];
                out.push_back({&link, static_cast<uint16>(i * 2), link.a, link.b});
                if (!link.oneWay)
                    out.push_back({&link, static_cast<uint16>(i * 2 + 1), link.b, link.a});
            }
            return out;
        }();
        return links;
    }

    bool InBox(float x, float y, float minX, float maxX, float minY, float maxY)
    {
        return x >= minX && x <= maxX && y >= minY && y <= maxY;
    }

    bool Compatible(uint8 a, uint8 b) { return a == REGION_ANY || b == REGION_ANY || a == b; }

    float Dist2d(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

    // Distance from (px, py) to the segment (ax, ay)-(bx, by).
    float SegmentDist(float px, float py, float ax, float ay, float bx, float by)
    {
        float dx = bx - ax, dy = by - ay;
        float len2 = dx * dx + dy * dy;
        float t = len2 > 0.0f ? std::clamp(((px - ax) * dx + (py - ay) * dy) / len2, 0.0f, 1.0f) : 0.0f;
        return Dist2d(px, py, ax + t * dx, ay + t * dy);
    }

    struct Hazards
    {
        std::vector<CortexMemory::Danger> deaths;
        struct HighMob
        {
            float x, y;
            uint32 entry;
        };
        std::vector<HighMob> highMobs;  // one per cell and kind of mob
    };

    Hazards DangersFor(Player* bot, uint32 map)
    {
        Hazards hazards;
        CortexMemory* memory = Cortex::Memory(bot);
        if (!memory)
            return hazards;
        uint32 now = static_cast<uint32>(GameTime::GetGameTime().count());
        hazards.deaths = memory->Outclassed(map, now - DANGER_WINDOW, DANGER_LEVELS);
        if (!sHighZones)
            return hazards;
        // Only what was hostile to it (a red name): a neutral goblin bruiser in Ratchet or a Cenarion druid in
        // Moonglade is far above a level 18 too, and at first marked the Barrens, Moonglade and towns as high zones.
        uint8 minLevel =
            static_cast<uint8>(std::min<int32>(0xFF, static_cast<int32>(bot->GetLevel()) + HIGH_MOB_LEVELS));
        FactionTemplateEntry const* own = bot->GetFactionTemplateEntry();
        for (auto const& [key, mob] : memory->Mobs())
        {
            if (mob.map != map || mob.minLevel < minLevel)
                continue;
            CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(mob.entry);
            FactionTemplateEntry const* faction = proto ? sFactionTemplateStore.LookupEntry(proto->faction) : nullptr;
            if (!own || !faction || !faction->IsHostileTo(*own))
                continue;
            float x = 0.0f;
            float y = 0.0f;
            CortexMemory::CellCentre(mob.cell, x, y);
            hazards.highMobs.push_back({x, y, mob.entry});
        }
        return hazards;
    }

    // A zone well above the bot: HIGH_MOB_CELLS cells and HIGH_MOB_KINDS kinds of mob among them. One high mob on
    // its rounds (Stitches on Duskwood's road, a rare) marks every cell it walks through, not a zone.
    bool HighZone(std::vector<Hazards::HighMob const*> const& near)
    {
        std::vector<std::pair<float, float>> cells;
        std::vector<uint32> kinds;
        for (Hazards::HighMob const* m : near)
        {
            if (std::find(cells.begin(), cells.end(), std::make_pair(m->x, m->y)) == cells.end())
                cells.emplace_back(m->x, m->y);
            if (std::find(kinds.begin(), kinds.end(), m->entry) == kinds.end())
                kinds.push_back(m->entry);
        }
        return cells.size() >= HIGH_MOB_CELLS && kinds.size() >= HIGH_MOB_KINDS;
    }

    bool OnLine(Hazards const& hazards, float x1, float y1, float x2, float y2)
    {
        for (CortexMemory::Danger const& d : hazards.deaths)
            if (Dist2d(d.x, d.y, x1, y1) > DANGER_START_SKIP && SegmentDist(d.x, d.y, x1, y1, x2, y2) <= DANGER_RADIUS)
                return true;
        std::vector<Hazards::HighMob const*> near;
        for (Hazards::HighMob const& m : hazards.highMobs)
            if (Dist2d(m.x, m.y, x1, y1) > HIGH_MOB_START_SKIP && SegmentDist(m.x, m.y, x1, y1, x2, y2) <= HIGH_MOB_RADIUS)
                near.push_back(&m);
        return HighZone(near);
    }

    // Within `radius` of (x, y) the bot has seen mobs well above it.
    bool HighZoneNear(Hazards const& hazards, float x, float y, float radius)
    {
        std::vector<Hazards::HighMob const*> near;
        for (Hazards::HighMob const& m : hazards.highMobs)
            if (Dist2d(m.x, m.y, x, y) <= radius)
                near.push_back(&m);
        return HighZone(near);
    }

    float Dist3d(float ax, float ay, float az, float bx, float by, float bz)
    {
        return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by) + (az - bz) * (az - bz));
    }

    enum class EdgeKind : uint8
    {
        NONE,
        WALK,
        LINK,
        TAXI,
        SPELL,
        HEARTH,
    };

    struct Vertex
    {
        uint32 map = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        uint8 region = REGION_ANY;
        int32 linkStart = -1;  // index into DirectedLinks(): this is where the link starts; its end is the next vertex
        int32 spellEnd = -1;   // index into DirectedLinks(): where the spell lands
        bool home = false;     // where the hearthstone takes the bot
        uint32 taxi = 0;       // taxi node
    };

    // Cheapest way from the bot to the target over walking (same map and region), links and known flights.
    // Returns the cost (INF when there's none) and fills `legs`.
    float Search(Player* bot, uint32 map, float x, float y, float z, uint32 mapArea, bool allowDirect, bool flights,
                 bool hearth, std::vector<uint16> const& excluded, std::vector<CortexLeg>& legs)
    {
        legs.clear();
        std::vector<Directed> const& links = DirectedLinks();
        Hazards const dangers = DangersFor(bot, bot->GetMapId());
        uint8 teamBit = bot->GetTeamId() == TEAM_ALLIANCE ? TEAM_BIT_ALLIANCE : TEAM_BIT_HORDE;

        std::vector<Vertex> vertices;
        Vertex start;
        start.map = bot->GetMapId();
        start.x = bot->GetPositionX();
        start.y = bot->GetPositionY();
        start.z = bot->GetPositionZ();
        start.region = CortexTravel::Region(start.map, start.x, start.y, start.z, 0);
        vertices.push_back(start);
        Vertex target;
        target.map = map;
        target.x = x;
        target.y = y;
        target.z = z;
        target.region = CortexTravel::Region(map, x, y, z, mapArea);
        vertices.push_back(target);

        for (size_t i = 0; i < links.size(); ++i)
        {
            Directed const& d = links[i];
            if (!(d.link->teams & teamBit) || std::find(excluded.begin(), excluded.end(), d.id) != excluded.end())
                continue;
            if (d.link->kind == CortexLegKind::PORTAL && !sObjectMgr->GetAreaTriggerTeleport(d.link->entry))
                continue;
            Vertex end;
            end.map = d.to.map;
            end.x = d.to.x;
            end.y = d.to.y;
            end.z = d.to.z;
            end.region = CortexTravel::Region(end.map, end.x, end.y, end.z, 0);
            if (d.link->kind == CortexLegKind::SPELL)
            {
                // Teleport: Moonglade is for Moonglade, not a shortcut to the rest of the continent.
                if (map != d.to.map || Dist2d(x, y, d.to.x, d.to.y) > SPELL_REACH || !bot->HasSpell(d.link->entry) ||
                    bot->HasSpellCooldown(d.link->entry))
                    continue;
                end.spellEnd = static_cast<int32>(i);
                vertices.push_back(end);
                continue;
            }
            Vertex from;
            from.map = d.from.map;
            from.x = d.from.x;
            from.y = d.from.y;
            from.z = d.from.z;
            from.region = CortexTravel::Region(from.map, from.x, from.y, from.z, 0);
            from.linkStart = static_cast<int32>(i);
            vertices.push_back(from);
            vertices.push_back(end);
        }

        // Home: the inn it is bound at, which a player always knows (the hearthstone says so).
        if (hearth && CortexTravel::HearthReady(bot))
        {
            Vertex home;
            home.map = bot->m_homebindMapId;
            home.x = bot->m_homebindX;
            home.y = bot->m_homebindY;
            home.z = bot->m_homebindZ;
            home.region = CortexTravel::Region(home.map, home.x, home.y, home.z, 0);
            home.home = true;
            vertices.push_back(home);
        }

        if (flights)
        {
            uint32 mountIndex = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 0;
            for (uint32 id = 1; id < sTaxiNodesStore.GetNumRows(); ++id)
            {
                TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(id);
                if (!node || (node->map_id != start.map && node->map_id != map) || !node->MountCreatureID[mountIndex])
                    continue;
                uint8 field = static_cast<uint8>((id - 1) / 32);
                uint32 submask = 1u << ((id - 1) % 32);
                if (field >= TaxiMaskSize || !(sTaxiNodesMask[field] & submask) || !bot->m_taxi.IsTaximaskNodeKnown(id))
                    continue;
                Vertex v;
                v.map = node->map_id;
                v.x = node->x;
                v.y = node->y;
                v.z = node->z;
                v.region = CortexTravel::Region(v.map, v.x, v.y, v.z, 0);
                v.taxi = id;
                vertices.push_back(v);
            }
        }

        size_t n = vertices.size();
        std::vector<float> cost(n, INF);
        std::vector<int32> prev(n, -1);
        std::vector<EdgeKind> via(n, EdgeKind::NONE);
        std::vector<uint32> price(n, 0);
        std::vector<bool> done(n, false);
        cost[0] = 0.0f;

        for (size_t round = 0; round < n; ++round)
        {
            int32 u = -1;
            for (size_t i = 0; i < n; ++i)
                if (!done[i] && cost[i] < INF && (u < 0 || cost[i] < cost[u]))
                    u = static_cast<int32>(i);
            if (u < 0 || u == 1)
                break;
            done[u] = true;
            Vertex const& from = vertices[u];

            for (size_t v = 1; v < n; ++v)
            {
                if (done[v])
                    continue;
                Vertex const& to = vertices[v];
                float best = INF;
                EdgeKind kind = EdgeKind::NONE;
                uint32 fare = 0;

                if (from.linkStart >= 0 && static_cast<int32>(v) == u + 1)
                {
                    best = links[from.linkStart].link->cost;
                    kind = EdgeKind::LINK;
                }
                else if (u == 0 && to.spellEnd >= 0)
                {
                    best = links[to.spellEnd].link->cost;
                    kind = EdgeKind::SPELL;
                }
                else if (u == 0 && to.home)
                {
                    best = HEARTH_COST;
                    kind = EdgeKind::HEARTH;
                }
                if (from.taxi && to.taxi && from.map == to.map)
                {
                    auto source = sTaxiPathSetBySource.find(from.taxi);
                    if (source != sTaxiPathSetBySource.end())
                    {
                        auto path = source->second.find(to.taxi);
                        if (path != source->second.end() && path->second)
                        {
                            float c = Dist3d(from.x, from.y, from.z, to.x, to.y, to.z) * FLIGHT_DETOUR / FLIGHT_SPEED +
                                      FLIGHT_HOP_COST;
                            if (c < best)
                            {
                                best = c;
                                kind = EdgeKind::TAXI;
                                fare = path->second->price;
                            }
                        }
                    }
                }
                // Walking: same map and region. Not into a link's far end (that is reached by the link) and not into
                // where a spell lands.
                bool linkEnd = v >= 2 && vertices[v - 1].linkStart >= 0;
                if (from.map == to.map && Compatible(from.region, to.region) && !linkEnd && to.spellEnd < 0 && !to.home &&
                    !(u == 0 && v == 1 && !allowDirect))
                {
                    float c = Dist2d(from.x, from.y, to.x, to.y) * WALK_DETOUR / WALK_SPEED +
                              (to.taxi ? FLIGHT_BOARD_COST : 0.0f);
                    if (from.map == bot->GetMapId() && OnLine(dangers, from.x, from.y, to.x, to.y))
                        c += DANGER_PENALTY;
                    if (c < best)
                    {
                        best = c;
                        kind = EdgeKind::WALK;
                        fare = 0;
                    }
                }
                if (kind == EdgeKind::NONE || cost[u] + best >= cost[v])
                    continue;
                cost[v] = cost[u] + best;
                prev[v] = u;
                via[v] = kind;
                price[v] = price[u] + fare;
            }
        }

        if (cost[1] >= INF)
            return INF;

        std::vector<int32> path;
        for (int32 v = 1; v >= 0; v = prev[v])
            path.push_back(v);
        std::reverse(path.begin(), path.end());

        for (size_t i = 1; i < path.size(); ++i)
        {
            Vertex const& from = vertices[path[i - 1]];
            Vertex const& to = vertices[path[i]];
            switch (via[path[i]])
            {
                case EdgeKind::LINK:
                case EdgeKind::SPELL:
                {
                    Directed const& d = links[via[path[i]] == EdgeKind::LINK ? from.linkStart : to.spellEnd];
                    CortexLeg leg;
                    leg.kind = d.link->kind;
                    leg.link = static_cast<uint16>(d.id + 1);
                    leg.entry = d.link->entry;
                    leg.map = d.link->kind == CortexLegKind::SPELL ? from.map : d.from.map;
                    leg.x = d.link->kind == CortexLegKind::SPELL ? from.x : d.from.x;
                    leg.y = d.link->kind == CortexLegKind::SPELL ? from.y : d.from.y;
                    leg.z = d.link->kind == CortexLegKind::SPELL ? from.z : d.from.z;
                    leg.toMap = d.to.map;
                    leg.toX = d.to.x;
                    leg.toY = d.to.y;
                    leg.toZ = d.to.z;
                    leg.stopZ = d.from.stopZ;
                    leg.toStopZ = d.to.stopZ;
                    leg.liftX = d.link->liftX;
                    leg.liftY = d.link->liftY;
                    legs.push_back(std::move(leg));
                    break;
                }
                case EdgeKind::HEARTH:
                {
                    CortexLeg leg;
                    leg.kind = CortexLegKind::HEARTH;
                    leg.entry = SPELL_HEARTHSTONE;
                    leg.map = from.map;
                    leg.x = from.x;
                    leg.y = from.y;
                    leg.z = from.z;
                    leg.toMap = to.map;
                    leg.toX = to.x;
                    leg.toY = to.y;
                    leg.toZ = to.z;
                    legs.push_back(std::move(leg));
                    break;
                }
                case EdgeKind::TAXI:
                {
                    uint32 fare = price[path[i]] - price[path[i - 1]];
                    if (!legs.empty() && legs.back().kind == CortexLegKind::FLIGHT && legs.back().nodes.back() == from.taxi)
                    {
                        CortexLeg& leg = legs.back();
                        leg.nodes.push_back(to.taxi);
                        leg.entry = to.taxi;
                        leg.toX = to.x;
                        leg.toY = to.y;
                        leg.toZ = to.z;
                        leg.price += fare;
                        break;
                    }
                    CortexLeg leg;
                    leg.kind = CortexLegKind::FLIGHT;
                    leg.entry = to.taxi;
                    leg.map = from.map;
                    leg.x = from.x;
                    leg.y = from.y;
                    leg.z = from.z;
                    leg.toMap = to.map;
                    leg.toX = to.x;
                    leg.toY = to.y;
                    leg.toZ = to.z;
                    leg.price = fare;
                    leg.nodes = {from.taxi, to.taxi};
                    legs.push_back(std::move(leg));
                    break;
                }
                default:
                    break;
            }
        }
        return cost[1];
    }
}

void CortexTravel::LoadConfig()
{
    sEnabled = sConfigMgr->GetOption<bool>("Cortex.Travel", true);
    sFlights = sConfigMgr->GetOption<bool>("Cortex.Travel.Flights", true);
    sLearnFlights = sConfigMgr->GetOption<bool>("Cortex.Travel.LearnFlights", true);
    sCrossMap = sConfigMgr->GetOption<bool>("Cortex.Travel.CrossMap", true);
    sHearth = sConfigMgr->GetOption<bool>("Cortex.Travel.Hearth", true);
    sHighZones = sConfigMgr->GetOption<bool>("Cortex.Travel.HighZones", true);
    sRecheck = sConfigMgr->GetOption<bool>("Cortex.Travel.Recheck", true);
    LOG_INFO("server.loading", "Cortex: Travel {}, Flights {}, LearnFlights {}, CrossMap {}, Hearth {}, HighZones {}, Recheck {}, {} links",
             sEnabled, sFlights, sLearnFlights, sCrossMap, sHearth, sHighZones, sRecheck, LINK_COUNT);
}

bool CortexTravel::Enabled() { return sEnabled; }

bool CortexTravel::Flights() { return sEnabled && sFlights; }

bool CortexTravel::LearnFlights() { return sLearnFlights; }

bool CortexTravel::CrossMap() { return sEnabled && sCrossMap; }

bool CortexTravel::Hearth() { return sEnabled && sHearth; }

bool CortexTravel::Recheck() { return sEnabled && sRecheck; }

bool CortexTravel::DangerousWalk(Player* bot, float x1, float y1, float x2, float y2)
{
    return OnLine(DangersFor(bot, bot->GetMapId()), x1, y1, x2, y2);
}

bool CortexTravel::OutOfDepth(Player* bot, float radius)
{
    return HighZoneNear(DangersFor(bot, bot->GetMapId()), bot->GetPositionX(), bot->GetPositionY(), radius);
}

bool CortexTravel::HearthReady(Player* bot)
{
    return bot->HasItemCount(ITEM_HEARTHSTONE, 1) && !bot->HasSpellCooldown(SPELL_HEARTHSTONE);
}

uint8 CortexTravel::Region(uint32 map, float x, float y, float z, uint32 mapArea)
{
    bool zKnown = z > INVALID_HEIGHT + 1.0f;
    if (map == 1)
    {
        if (mapArea == MAP_AREA_DARNASSUS)
            return REGION_TELDRASSIL;
        if (mapArea == MAP_AREA_THUNDER_BLUFF)
            return REGION_THUNDER_BLUFF;
        // Teldrassil: nothing stands between z 200 and 1100 there, Rut'theran Village is at the foot of the tree.
        // Without a height, its quest points (x <= 8710) and the top's (x >= 9072) still fall apart.
        if (InBox(x, y, 7900.0f, 11500.0f, -500.0f, 3300.0f))
        {
            if (zKnown)
                return z < 400.0f ? REGION_RUTTHERAN : REGION_TELDRASSIL;
            return x < 8950.0f ? REGION_RUTTHERAN : REGION_TELDRASSIL;
        }
        // Thunder Bluff's mesas: the city is above z 95 (the Pools of Vision), the lifts start at 60-70.
        if (InBox(x, y, -1550.0f, -850.0f, -400.0f, 400.0f))
            return zKnown ? (z >= 85.0f ? REGION_THUNDER_BLUFF : REGION_KALIMDOR) : REGION_ANY;
        // Freewind Post: the mesa top is at z 89, the canyon floor at -40 to -50.
        if (InBox(x, y, -5550.0f, -5250.0f, -2650.0f, -2350.0f))
            return zKnown ? (z >= 30.0f ? REGION_FREEWIND : REGION_KALIMDOR) : REGION_ANY;
        return REGION_KALIMDOR;
    }
    if (map == 0)
    {
        if (mapArea == MAP_AREA_UNDERCITY)
            return REGION_UNDERCITY;
        // Undercity: spawns there are at z 0 and below, the ruins and Tirisfal around at 30 and above.
        if (InBox(x, y, 1350.0f, 1900.0f, -50.0f, 550.0f))
            return zKnown ? (z < 10.0f ? REGION_UNDERCITY : REGION_EASTERN_KINGDOMS) : REGION_ANY;
        // The Wizard's Sanctum basement is at z 30 under the Mage Quarter (z 105) and the tower top (z 148).
        if (InBox(x, y, -9040.0f, -8970.0f, 830.0f, 905.0f))
            return zKnown ? (z < 60.0f ? REGION_WIZARD_SANCTUM : REGION_EASTERN_KINGDOMS) : REGION_ANY;
        return REGION_EASTERN_KINGDOMS;
    }
    return REGION_OTHER;
}

bool CortexTravel::SameRegion(Player* bot, uint32 map, float x, float y, float z, uint32 mapArea)
{
    if (bot->GetMapId() != map)
        return false;
    if (!sEnabled)
        return true;
    return Compatible(Region(map, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), 0),
                      Region(map, x, y, z, mapArea));
}

bool CortexTravel::Plan(Player* bot, uint32 map, float x, float y, float z, uint32 mapArea, bool allowDirect,
                        CortexTrip& trip)
{
    if (!sEnabled || !bot)
        return false;

    std::vector<CortexLeg> legs;
    bool flights = sFlights && !trip.noFlights;
    bool hearth = sHearth && !trip.noHearth;
    float best = Search(bot, map, x, y, z, mapArea, allowDirect, flights, hearth, trip.excluded, legs);
    if (best < INF && flights)
    {
        uint32 fares = 0;
        for (CortexLeg const& leg : legs)
            fares += leg.price;
        // Can't pay for it: the same trip without flights.
        if (fares > bot->GetMoney())
            best = Search(bot, map, x, y, z, mapArea, allowDirect, false, hearth, trip.excluded, legs);
    }
    if (best >= INF || legs.empty())
        return false;

    if (allowDirect && bot->GetMapId() == map)
    {
        float walk = Dist2d(bot->GetPositionX(), bot->GetPositionY(), x, y) * WALK_DETOUR / WALK_SPEED;
        if (DangerousWalk(bot, bot->GetPositionX(), bot->GetPositionY(), x, y))
            walk += DANGER_PENALTY;
        if (best > walk * BETTER_THAN_WALK)
            return false;
    }

    trip.legs = std::move(legs);
    trip.estimate = static_cast<uint32>(best);
    return true;
}

bool CortexTravel::Reachable(Player* bot, uint32 map, float x, float y, uint32 mapArea)
{
    CortexTrip probe;
    return Plan(bot, map, x, y, INVALID_HEIGHT, mapArea, true, probe);
}

bool CortexTravel::PortalOut(Player* bot, float range, float& toX, float& toY, float& toZ)
{
    // Rut'theran has its boat and its ground beside the portal: an island there is left the usual way.
    uint8 region = Region(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), 0);
    if (region != REGION_WIZARD_SANCTUM)
        return false;
    float best = range;
    bool found = false;
    for (Directed const& d : DirectedLinks())
    {
        if (d.link->kind != CortexLegKind::PORTAL || d.from.map != bot->GetMapId() || d.to.map != bot->GetMapId() ||
            Region(d.from.map, d.from.x, d.from.y, d.from.z, 0) != region ||
            Region(d.to.map, d.to.x, d.to.y, d.to.z, 0) == region || !sObjectMgr->GetAreaTriggerTeleport(d.link->entry))
            continue;
        float dist = bot->GetExactDist(d.from.x, d.from.y, d.from.z);
        if (dist > best)
            continue;
        best = dist;
        toX = d.to.x;
        toY = d.to.y;
        toZ = d.to.z;
        found = true;
    }
    return found;
}

bool CortexTravel::Busy(CortexTrip const& trip)
{
    return trip.active && trip.phase != CortexTripPhase::APPROACH;
}

char const* CortexTravel::LegName(CortexLegKind kind)
{
    switch (kind)
    {
        case CortexLegKind::FLIGHT:
            return "flight";
        case CortexLegKind::BOAT:
            return "boat";
        case CortexLegKind::LIFT:
            return "lift";
        case CortexLegKind::PORTAL:
            return "portal";
        case CortexLegKind::SPELL:
            return "spell";
        case CortexLegKind::HEARTH:
            return "hearth";
    }
    return "?";
}

std::string CortexTravel::Describe(std::vector<CortexLeg> const& legs)
{
    std::string out;
    for (CortexLeg const& leg : legs)
    {
        if (!out.empty())
            out += '-';  // telemetry info keeps letters, digits and ' _:.-' only
        out += LegName(leg.kind);
    }
    return out;
}
