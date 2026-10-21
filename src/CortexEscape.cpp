/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include "CortexEscape.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

#include "BotTelemetry.h"
#include "CortexTravel.h"
#include "DetourNavMesh.h"
#include "DetourCommon.h"
#include "DetourNavMeshQuery.h"
#include "Map.h"
#include "MapDefines.h"
#include "MotionMaster.h"
#include "Player.h"
#include "PlayerbotAI.h"

namespace
{
    // An island has fewer polys than this; the mainland near any road has thousands.
    constexpr size_t ISLAND_POLYS = 200;
    // Mainland searched this far round the bot, near first.
    constexpr float SEARCH_NEAR = 15.0f;
    constexpr float SEARCH_FAR = 45.0f;
    // Cities hold a lot of polys in that box (1500 round Darnassus' bank), and Stormwind has a field of 149 unlinked
    // slivers with the mainland 23 yd off.
    constexpr int MAX_QUERY_POLYS = 2048;
    // Candidate mainland polys whose own reach is tested, nearest first; unlinked slivers aren't candidates at all.
    constexpr int MAX_CANDIDATES = 16;
    // A walk off is straight (no path: there is none) and only downhill or level, in sight and short.
    constexpr float WALK_MAX = 25.0f;
    constexpr float WALK_RISE = 2.0f;

    // Accepts what `base` accepts, but only polys linked to another (core 0010: unlinked slivers on the floor).
    class LinkedFilter : public dtQueryFilter
    {
    public:
        explicit LinkedFilter(dtQueryFilter const& base) : _base(base) {}
        bool passFilter(const dtPolyRef ref, const dtMeshTile* tile, const dtPoly* poly) const override
        {
            return poly->firstLink != DT_NULL_LINK && _base.passFilter(ref, tile, poly);
        }

    private:
        dtQueryFilter const& _base;
    };

    // Polys reachable from `start` over the mesh's links, any flags, up to `limit`.
    void Reach(dtNavMesh const* mesh, dtPolyRef start, size_t limit, std::unordered_set<dtPolyRef>& seen)
    {
        std::vector<dtPolyRef> open{start};
        seen.insert(start);
        while (!open.empty() && seen.size() < limit)
        {
            dtPolyRef ref = open.back();
            open.pop_back();
            dtMeshTile const* tile = nullptr;
            dtPoly const* poly = nullptr;
            if (dtStatusFailed(mesh->getTileAndPolyByRef(ref, &tile, &poly)))
                continue;
            for (unsigned int l = poly->firstLink; l != DT_NULL_LINK; l = tile->links[l].next)
            {
                dtPolyRef next = tile->links[l].ref;
                if (next && seen.insert(next).second)
                    open.push_back(next);
            }
        }
    }
}

bool CortexEscape::Try(PlayerbotAI* botAI, Player* bot, bool hasTarget, float tx, float ty, float tz, bool mayWalk)
{
    if (bot->GetTransport() || bot->IsInFlight() || bot->IsFlying())
        return false;

    Map* map = bot->GetMap();
    MMapData& data = map->GetMapCollisionData().GetMMapData();
    dtNavMesh const* mesh = data.GetNavMesh();
    dtNavMeshQuery const* query = data.GetNavMeshQuery();
    if (!mesh || !query)
        return false;

    dtQueryFilter filter;
    filter.setIncludeFlags(0xFFFF);
    filter.setExcludeFlags(NAV_MAGMA | NAV_SLIME);

    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();
    float const bz = bot->GetPositionZ();
    // Detour is y-up: (wowY, wowZ, wowX).
    float pos[3] = {by, bz, bx};
    float ext[3] = {3.0f, 5.0f, 3.0f};
    dtPolyRef start = 0;
    float nearest[3];
    query->findNearestPoly(pos, ext, &filter, &start, nearest);
    // An unlinked sliver lying on the floor isn't where the bot stands, for its paths (core 0010) or here.
    if (start)
    {
        dtMeshTile const* tile = nullptr;
        dtPoly const* poly = nullptr;
        if (dtStatusSucceed(mesh->getTileAndPolyByRef(start, &tile, &poly)) && poly->firstLink == DT_NULL_LINK)
        {
            LinkedFilter linked(filter);
            dtPolyRef linkedRef = 0;
            float linkedNearest[3];
            query->findNearestPoly(pos, ext, &linked, &linkedRef, linkedNearest);
            if (linkedRef && dtVdist(linkedNearest, pos) <= dtVdist(nearest, pos) + 2.0f)
                start = linkedRef;
        }
    }

    std::unordered_set<dtPolyRef> island;
    if (start)
    {
        Reach(mesh, start, ISLAND_POLYS, island);
        if (island.size() >= ISLAND_POLYS)
            return false;
        // Its business is on the island (a speck of a ledge it was sent to): leave it there.
        if (hasTarget)
        {
            float target[3] = {ty, tz, tx};
            dtPolyRef targetRef = 0;
            float onTarget[3];
            query->findNearestPoly(target, ext, &filter, &targetRef, onTarget);
            if (targetRef && island.count(targetRef))
                return false;
        }
    }

    // A place only a portal leaves (the Wizard's Sanctum basement, whose nearest "mainland" is a pocket in the
    // tower's wall): through the portal, as a player would.
    float px, py, pz;
    if (start && CortexTravel::PortalOut(bot, SEARCH_FAR * 2.0f, px, py, pz))
    {
        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        BotTelemetry::instance().Event(botAI, "escape", "portal", static_cast<int32>(island.size()));
        bot->NearTeleportTo(px, py, pz, bot->GetOrientation());
        return true;
    }

    // The nearest mainland poly: not on the island, and reaching ISLAND_POLYS itself (another speck won't do).
    // A rise counts double, since only a drop can be walked.
    struct Candidate
    {
        float cost;
        dtPolyRef ref;
        float x, y, z;
    };
    std::unordered_set<dtPolyRef> rejected;
    Candidate best{0.0f, 0, 0.0f, 0.0f, 0.0f};
    for (float range : {SEARCH_NEAR, SEARCH_FAR})
    {
        float box[3] = {range, 30.0f, range};
        std::vector<dtPolyRef> polys(MAX_QUERY_POLYS);
        int count = 0;
        query->queryPolygons(pos, box, &filter, polys.data(), &count, MAX_QUERY_POLYS);
        std::vector<Candidate> candidates;
        for (int i = 0; i < count; ++i)
        {
            if (island.count(polys[i]) || rejected.count(polys[i]))
                continue;
            dtMeshTile const* tile = nullptr;
            dtPoly const* poly = nullptr;
            if (dtStatusFailed(mesh->getTileAndPolyByRef(polys[i], &tile, &poly)) || poly->firstLink == DT_NULL_LINK)
                continue;
            float c[3];
            if (dtStatusFailed(query->closestPointOnPoly(polys[i], pos, c, nullptr)))
                continue;
            float dx = c[2] - bx;
            float dy = c[0] - by;
            float dz = c[1] - bz;
            float cost = std::sqrt(dx * dx + dy * dy) + std::fabs(dz) + std::max(0.0f, dz);
            candidates.push_back({cost, polys[i], c[2], c[0], c[1]});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](Candidate const& a, Candidate const& b) { return a.cost < b.cost; });
        int tested = 0;
        for (Candidate const& candidate : candidates)
        {
            if (tested++ >= MAX_CANDIDATES)
                break;
            std::unordered_set<dtPolyRef> reach;
            Reach(mesh, candidate.ref, ISLAND_POLYS, reach);
            if (reach.size() >= ISLAND_POLYS)
            {
                best = candidate;
                break;
            }
            // Another island: none of its polys is worth a look again.
            rejected.insert(reach.begin(), reach.end());
        }
        if (best.ref)
            break;
    }

    int32 islandSize = static_cast<int32>(island.size());
    if (!best.ref)
    {
        // No mesh under it and none near (bots found standing at 0,0,0 for days): back to its inn, as a hearth would.
        if (!start && bot->GetExactDist(bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ) > SEARCH_FAR)
        {
            BotTelemetry::instance().Event(botAI, "escape", "home", islandSize);
            bot->TeleportTo(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ,
                            bot->GetOrientation());
            return true;
        }
        BotTelemetry::instance().Event(botAI, "escape", "none", islandSize);
        return false;
    }

    float dist = bot->GetExactDist(best.x, best.y, best.z);
    bool walk = mayWalk && dist <= WALK_MAX && best.z <= bz + WALK_RISE &&
                map->isInLineOfSight(bx, by, bz + 2.0f, best.x, best.y, best.z + 2.0f, bot->GetPhaseMask(),
                                     LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);
    if (walk)
    {
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MovePoint(0, best.x, best.y, best.z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false);
    }
    else
        bot->NearTeleportTo(best.x, best.y, best.z + 0.5f, bot->GetOrientation());

    BotTelemetry::instance().Event(botAI, "escape", walk ? "walk" : "set_down", islandSize, static_cast<int32>(dist),
                                   static_cast<int32>(best.z - bz));
    return true;
}
