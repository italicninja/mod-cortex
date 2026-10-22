/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotRoutes.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BotDanger.h"
#include "BotTelemetry.h"
#include "Config.h"
#include "Log.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "Timer.h"

namespace BotRoutes
{
namespace
{
// Start nodes: the nearest ones within this range, the first reachable by mmap wins (at most START_TESTS tested).
constexpr float START_RANGE = 450.0f;
constexpr size_t START_CANDIDATES = 4;
constexpr size_t START_TESTS = 2;
// Goal nodes: within this range of the destination; the last leg is weighted so near ones win.
constexpr float GOAL_RANGE = 500.0f;
constexpr size_t GOAL_CANDIDATES = 6;
constexpr float LAST_LEG_WEIGHT = 1.5f;
// Following: a point counts as reached within REACHED yards (MoveFarTo replans within 10 yd of its target);
// the bot walks to the furthest point within HOP yards, at most MAX_HOP_POINTS ahead.
constexpr float REACHED = 12.0f;
constexpr float HOP = 45.0f;
constexpr size_t MAX_HOP_POINTS = 40;
// Off the route by more than this: pick it up again at the nearest point within RESYNC_PICKUP, else rebuild.
constexpr float RESYNC = 60.0f;
constexpr float RESYNC_PICKUP = 40.0f;
constexpr uint32 REBUILD_MIN_MS = 20 * IN_MILLISECONDS;
// Swimming is slow and drowns low-level bots: weight swim yards up.
constexpr float SWIM_WEIGHT = 2.0f;
// Bridges between pieces of the graph that the precomputed links never joined (the Camp Narache mesa and the
// Rolling Plains): closest node pair within BRIDGE_MAX yards and BRIDGE_MAX_DZ of height, at BRIDGE_COST_FACTOR so
// routes only use them when needed. A bridge is just its two nodes: past the first one the bot heads for the other
// and mmap (players search 8192 nodes, core 0008) finds the way, which is rarely the straight line (a ramp
// around a cliff).
constexpr float BRIDGE_MAX = 300.0f;
constexpr float BRIDGE_MAX_DZ = 60.0f;
constexpr float BRIDGE_COST_FACTOR = 2.0f;

struct Edge
{
    uint32 to;
    float cost;
    uint32 first;  // into MapGraph::points
    uint32 count;
    bool reverse;  // walk the stored points backwards (the link only exists the other way)
    uint32 cellFirst = 0;
    uint32 cellCount = 0;
};

struct Node
{
    uint32 id;
    float x, y, z;
    std::vector<Edge> edges;
};

struct MapGraph
{
    std::vector<Node> nodes;
    std::vector<Point> points;
    std::vector<uint64> edgeCells;  // BotDanger cells each edge's points pass through (Edge::cellFirst / cellCount)
};

struct BotRoute
{
    uint32 map = 0;
    Point dest{0.0f, 0.0f, 0.0f};
    bool hasDest = false;
    bool done = false;
    std::vector<Point> path;
    size_t idx = 0;
    uint32 builtMs = 0;
    uint32 startNode = 0;
    std::vector<uint32> badStarts;  // node ids that led nowhere for this destination
    std::vector<uint64> edges;      // per path point: the graph edge it belongs to (EdgeKey), 0 for the start node
};

// Edges bots keep getting stuck on (a hand-made link or bridge over a cliff, a road join that isn't walkable):
// EDGE_STRIKES walk-unstick detours on an edge within EDGE_STRIKE_MS and route searches skip it, for every bot, for a
// fixed time from that third strike. An edge that strikes out again within EDGE_RECALL_MS of its skip ending is
// skipped longer (EDGE_SKIP_MS: 1 h, 3 h, 12 h); one that stays clean that long starts over at 1 h.
constexpr uint32 EDGE_STRIKES = 3;
constexpr uint32 EDGE_STRIKE_MS = HOUR * IN_MILLISECONDS;
constexpr uint32 EDGE_SKIP_LEVELS = 3;
constexpr uint32 EDGE_SKIP_MS[EDGE_SKIP_LEVELS] = {HOUR * IN_MILLISECONDS, 3 * HOUR * IN_MILLISECONDS,
                                                   12 * HOUR * IN_MILLISECONDS};
constexpr uint32 EDGE_RECALL_MS = 6 * HOUR * IN_MILLISECONDS;

struct EdgeState
{
    std::vector<uint32> strikes;  // getMSTime of each strike in the last EDGE_STRIKE_MS
    uint32 skipStartMs = 0;       // when the last skip began
    uint32 skipMs = 0;            // how long it lasts; 0: never skipped
    uint32 level = 0;             // index into EDGE_SKIP_MS of the last skip
};

std::mutex edgeMutex;
std::unordered_map<uint32, std::unordered_map<uint64, EdgeState>> edgeStrikes;  // map -> edge -> state

uint64 EdgeKey(uint32 a, uint32 b)
{
    return (static_cast<uint64>(std::min(a, b)) << 32) | std::max(a, b);
}

// The helpers below need edgeMutex held.
bool Skipping(EdgeState const& edge)
{
    return edge.skipMs && GetMSTimeDiffToNow(edge.skipStartMs) < edge.skipMs;
}

// Within EDGE_RECALL_MS of the last skip ending (or still skipping): striking out now escalates.
bool Recalled(EdgeState const& edge)
{
    return edge.skipMs && GetMSTimeDiffToNow(edge.skipStartMs) < edge.skipMs + EDGE_RECALL_MS;
}

void PruneStrikes(EdgeState& edge)
{
    edge.strikes.erase(std::remove_if(edge.strikes.begin(), edge.strikes.end(),
                                      [](uint32 ms) { return GetMSTimeDiffToNow(ms) >= EDGE_STRIKE_MS; }),
                       edge.strikes.end());
}

// Third strike: skip the edge from now, longer if it struck out shortly after its last skip. Returns the skip length.
uint32 StartSkip(EdgeState& edge)
{
    edge.level = Recalled(edge) ? std::min(edge.level + 1, EDGE_SKIP_LEVELS - 1) : 0;
    edge.skipStartMs = getMSTime();
    edge.skipMs = EDGE_SKIP_MS[edge.level];
    edge.strikes.clear();
    return edge.skipMs;
}

std::unordered_set<uint64> BadEdges(uint32 map)
{
    std::unordered_set<uint64> bad;
    std::lock_guard<std::mutex> lock(edgeMutex);
    auto it = edgeStrikes.find(map);
    if (it == edgeStrikes.end())
        return bad;
    for (auto edgeIt = it->second.begin(); edgeIt != it->second.end();)
    {
        EdgeState& edge = edgeIt->second;
        PruneStrikes(edge);
        if (Skipping(edge))
            bad.insert(edgeIt->first);
        else if (edge.strikes.empty() && !Recalled(edge))
        {
            edgeIt = it->second.erase(edgeIt);  // nothing left to remember
            continue;
        }
        ++edgeIt;
    }
    return bad;
}

bool enabled = false;
std::string learnFile;
std::atomic<bool> started{false};
std::atomic<bool> loaded{false};
std::unordered_map<uint32, MapGraph> graphs;  // read-only once `loaded`

std::mutex routeMutex;
std::unordered_map<ObjectGuid::LowType, BotRoute> routes;

float Dist(float ax, float ay, float az, float bx, float by, float bz)
{
    float dx = ax - bx, dy = ay - by, dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float Dist(Point const& a, Point const& b) { return Dist(a.x, a.y, a.z, b.x, b.y, b.z); }

// Hand-made links (Playerbots.NewRpgRouteLinks) for ways down that neither the precomputed links nor a closest-pair
// bridge get right: "map:x,y,z;x,y,z;...|map:..." Each chain is attached to the nearest node (within LINK_ATTACH yards)
// at both ends and walked point by point, both ways, at its real length.
constexpr float LINK_ATTACH = 300.0f;

uint32 AddLinks(std::unordered_map<uint32, MapGraph>& graphs, std::string const& spec)
{
    uint32 added = 0;
    std::stringstream chains(spec);
    std::string chain;
    while (std::getline(chains, chain, '|'))
    {
        size_t colon = chain.find(':');
        if (colon == std::string::npos)
            continue;

        uint32 map = static_cast<uint32>(std::stoul(chain.substr(0, colon)));
        auto graphIt = graphs.find(map);
        if (graphIt == graphs.end())
            continue;
        MapGraph& graph = graphIt->second;

        std::vector<Point> points;
        std::stringstream list(chain.substr(colon + 1));
        std::string item;
        while (std::getline(list, item, ';'))
        {
            Point p{};
            if (std::sscanf(item.c_str(), "%f,%f,%f", &p.x, &p.y, &p.z) == 3)
                points.push_back(p);
        }
        if (points.size() < 2)
            continue;

        auto nearest = [&graph](Point const& p)
        {
            uint32 best = UINT32_MAX;
            float bestDist = LINK_ATTACH;
            for (uint32 i = 0; i < graph.nodes.size(); ++i)
            {
                Node const& node = graph.nodes[i];
                float d = Dist(node.x, node.y, node.z, p.x, p.y, p.z);
                if (d < bestDist)
                {
                    bestDist = d;
                    best = i;
                }
            }
            return best;
        };

        uint32 from = nearest(points.front()), to = nearest(points.back());
        if (from == UINT32_MAX || to == UINT32_MAX || from == to)
        {
            LOG_ERROR("playerbots", "Playerbots routes: link on map {} has no travel node near one end", map);
            continue;
        }

        Node const& na = graph.nodes[from];
        Node const& nb = graph.nodes[to];
        uint32 first = static_cast<uint32>(graph.points.size());
        graph.points.push_back({na.x, na.y, na.z});
        float length = Dist(na.x, na.y, na.z, points.front().x, points.front().y, points.front().z);
        for (size_t i = 0; i < points.size(); ++i)
        {
            graph.points.push_back(points[i]);
            if (i)
                length += Dist(points[i - 1], points[i]);
        }
        graph.points.push_back({nb.x, nb.y, nb.z});
        length += Dist(points.back().x, points.back().y, points.back().z, nb.x, nb.y, nb.z);

        uint32 count = static_cast<uint32>(points.size() + 2);
        graph.nodes[from].edges.push_back({to, length, first, count, false});
        graph.nodes[to].edges.push_back({from, length, first, count, true});
        ++added;
        LOG_INFO("playerbots", "Playerbots routes: link on map {} from node {} to node {}, {} points, {:.0f} yd", map,
                 na.id, nb.id, count, length);
    }
    return added;
}

// Roads (Playerbots.NewRpgRoadsFile, made by tools/roads/roads.py from the client's road textures): polylines
// "<map> x y z x y z ...". Their ends and junctions become nodes; road edges cost ROAD_FACTOR of their length so
// routes take roads where they can; each road node is joined to its ROAD_ATTACH_NODES nearest travel nodes within
// ROAD_ATTACH yards, and road ends to another road's node within ROAD_GAP yards (texture gaps at bridges, gates).
constexpr float ROAD_FACTOR = 0.85f;
constexpr float ROAD_ATTACH = 120.0f;
constexpr size_t ROAD_ATTACH_NODES = 2;
constexpr float ROAD_ATTACH_FACTOR = 1.3f;
constexpr float ROAD_GAP = 60.0f;
constexpr uint32 ROAD_NODE_ID = 0x40000000;  // ids of road nodes (travel node ids are small)

uint32 AddRoads(std::unordered_map<uint32, MapGraph>& graphs, std::string const& file)
{
    if (file.empty())
        return 0;

    std::ifstream in(file);
    if (!in)
    {
        LOG_ERROR("playerbots", "Playerbots routes: can't read roads file {}", file);
        return 0;
    }

    struct Key
    {
        uint32 map;
        int64 x, y;
        bool operator==(Key const& o) const { return map == o.map && x == o.x && y == o.y; }
    };
    struct KeyHash
    {
        size_t operator()(Key const& k) const { return (k.x * 73856093) ^ (k.y * 19349663) ^ k.map; }
    };
    std::unordered_map<Key, uint32, KeyHash> roadNodes;  // endpoint (rounded to 0.1 yd) -> node index
    std::unordered_map<uint32, uint32> firstRoadNode;    // map -> index of its first road node
    std::unordered_map<uint32, std::vector<uint8>> ends; // map -> per road node: polylines ending there
    uint32 nextId = ROAD_NODE_ID;
    uint32 lines = 0;

    auto nodeAt = [&](uint32 map, MapGraph& graph, Point const& p)
    {
        Key key{map, static_cast<int64>(std::lround(p.x * 10)), static_cast<int64>(std::lround(p.y * 10))};
        auto it = roadNodes.find(key);
        if (it != roadNodes.end())
            return it->second;
        uint32 index = static_cast<uint32>(graph.nodes.size());
        if (!firstRoadNode.count(map))
            firstRoadNode[map] = index;
        graph.nodes.push_back({nextId++, p.x, p.y, p.z, {}});
        roadNodes[key] = index;
        return index;
    };

    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream fields(line);
        uint32 map;
        if (!(fields >> map))
            continue;
        auto graphIt = graphs.find(map);
        if (graphIt == graphs.end())
            continue;
        MapGraph& graph = graphIt->second;

        std::vector<Point> points;
        Point p{};
        while (fields >> p.x >> p.y >> p.z)
            points.push_back(p);
        if (points.size() < 2)
            continue;

        uint32 a = nodeAt(map, graph, points.front());
        uint32 b = nodeAt(map, graph, points.back());
        if (a == b)
            continue;

        float length = 0.0f;
        for (size_t i = 1; i < points.size(); ++i)
            length += Dist(points[i - 1], points[i]);

        uint32 first = static_cast<uint32>(graph.points.size());
        graph.points.insert(graph.points.end(), points.begin(), points.end());
        uint32 count = static_cast<uint32>(points.size());
        graph.nodes[a].edges.push_back({b, length * ROAD_FACTOR, first, count, false});
        graph.nodes[b].edges.push_back({a, length * ROAD_FACTOR, first, count, true});
        ++lines;
    }

    // Join road nodes to travel nodes, and road ends across small gaps
    uint32 joins = 0;
    for (auto const& [map, firstRoad] : firstRoadNode)
    {
        MapGraph& graph = graphs[map];
        size_t roadEnd = graph.nodes.size();
        auto join = [&](uint32 from, uint32 to, float factor)
        {
            Node const& na = graph.nodes[from];
            Node const& nb = graph.nodes[to];
            float d = Dist(na.x, na.y, na.z, nb.x, nb.y, nb.z);
            uint32 first = static_cast<uint32>(graph.points.size());
            graph.points.push_back({na.x, na.y, na.z});
            graph.points.push_back({nb.x, nb.y, nb.z});
            graph.nodes[from].edges.push_back({to, d * factor, first, 2, false});
            graph.nodes[to].edges.push_back({from, d * factor, first, 2, true});
            ++joins;
        };

        // road degree before any joins (road ends have 1), and a grid of road nodes for the gap search
        std::vector<uint32> degree(roadEnd - firstRoad);
        std::unordered_map<uint64, std::vector<uint32>> grid;
        auto gridKey = [](float x, float y)
        {
            return (static_cast<uint64>(static_cast<uint32>(static_cast<int32>(std::floor(x / ROAD_GAP)))) << 32) |
                   static_cast<uint32>(static_cast<int32>(std::floor(y / ROAD_GAP)));
        };
        for (uint32 r = firstRoad; r < roadEnd; ++r)
        {
            degree[r - firstRoad] = static_cast<uint32>(graph.nodes[r].edges.size());
            grid[gridKey(graph.nodes[r].x, graph.nodes[r].y)].push_back(r);
        }

        for (uint32 r = firstRoad; r < roadEnd; ++r)
        {
            Node const road = graph.nodes[r];
            std::vector<std::pair<float, uint32>> near;
            for (uint32 t = 0; t < firstRoad; ++t)
            {
                Node const& node = graph.nodes[t];
                float d = Dist(road.x, road.y, road.z, node.x, node.y, node.z);
                if (d < ROAD_ATTACH)
                    near.push_back({d, t});
            }
            std::sort(near.begin(), near.end());
            for (size_t k = 0; k < std::min(ROAD_ATTACH_NODES, near.size()); ++k)
                join(r, near[k].second, ROAD_ATTACH_FACTOR);

            if (degree[r - firstRoad] != 1)
                continue;

            // a road end reaches over to the nearest node of another road
            uint32 own = graph.nodes[r].edges.front().to;
            float best = ROAD_GAP;
            uint32 other = UINT32_MAX;
            int32 gx = static_cast<int32>(std::floor(road.x / ROAD_GAP));
            int32 gy = static_cast<int32>(std::floor(road.y / ROAD_GAP));
            for (int32 dx = -1; dx <= 1; ++dx)
            {
                for (int32 dy = -1; dy <= 1; ++dy)
                {
                    auto cell = grid.find((static_cast<uint64>(static_cast<uint32>(gx + dx)) << 32) |
                                          static_cast<uint32>(gy + dy));
                    if (cell == grid.end())
                        continue;
                    for (uint32 o : cell->second)
                    {
                        if (o == r || o == own)
                            continue;
                        Node const& node = graph.nodes[o];
                        float d = Dist(road.x, road.y, road.z, node.x, node.y, node.z);
                        if (d < best)
                        {
                            best = d;
                            other = o;
                        }
                    }
                }
            }
            if (other != UINT32_MAX)
                join(r, other, 1.0f);
        }
    }

    LOG_INFO("playerbots", "Playerbots routes: {} roads, {} road nodes, {} joins from {}", lines, roadNodes.size(),
             joins, file);
    return lines;
}

// Join each piece of the walk graph to a bigger one through its closest node pair, when close enough.
uint32 Bridge(uint32 map, MapGraph& graph)
{
    size_t n = graph.nodes.size();
    std::vector<uint32> comp(n, UINT32_MAX);
    std::vector<uint32> sizes;
    for (uint32 i = 0; i < n; ++i)
    {
        if (comp[i] != UINT32_MAX)
            continue;
        uint32 id = static_cast<uint32>(sizes.size());
        sizes.push_back(0);
        std::vector<uint32> stack{i};
        comp[i] = id;
        while (!stack.empty())
        {
            uint32 u = stack.back();
            stack.pop_back();
            ++sizes[id];
            for (Edge const& edge : graph.nodes[u].edges)
            {
                if (comp[edge.to] == UINT32_MAX)
                {
                    comp[edge.to] = id;
                    stack.push_back(edge.to);
                }
            }
        }
    }

    uint32 added = 0;
    for (uint32 c = 0; c < sizes.size(); ++c)
    {
        if (sizes[c] < 2)
            continue;  // lone nodes (flight masters, instance portals) have nothing to walk from

        float best = BRIDGE_MAX;
        uint32 from = UINT32_MAX, to = UINT32_MAX;
        for (uint32 a = 0; a < n; ++a)
        {
            if (comp[a] != c)
                continue;
            Node const& na = graph.nodes[a];
            for (uint32 b = 0; b < n; ++b)
            {
                // to a bigger piece (ties: lower id), so each pair is bridged once
                if (comp[b] == c || sizes[comp[b]] < sizes[c] || (sizes[comp[b]] == sizes[c] && comp[b] < c))
                    continue;
                Node const& nb = graph.nodes[b];
                if (std::fabs(na.z - nb.z) > BRIDGE_MAX_DZ)
                    continue;
                float d = Dist(na.x, na.y, na.z, nb.x, nb.y, nb.z);
                if (d < best)
                {
                    best = d;
                    from = a;
                    to = b;
                }
            }
        }

        if (from == UINT32_MAX)
            continue;

        Node const& na = graph.nodes[from];
        Node const& nb = graph.nodes[to];
        uint32 first = static_cast<uint32>(graph.points.size());
        graph.points.push_back({na.x, na.y, na.z});
        graph.points.push_back({nb.x, nb.y, nb.z});
        uint32 count = 2;
        float cost = best * BRIDGE_COST_FACTOR;
        graph.nodes[from].edges.push_back({to, cost, first, count, false});
        graph.nodes[to].edges.push_back({from, cost, first, count, true});
        ++added;
        LOG_INFO("playerbots", "Playerbots routes: bridge on map {} from node {} ({:.0f} {:.0f} {:.0f}) to node {} "
                 "({:.0f} {:.0f} {:.0f}), {:.0f} yd", map, na.id, na.x, na.y, na.z, nb.id, nb.x, nb.y, nb.z, best);
    }
    return added;
}

// Learned state file: "D map cell level ageMs" (death heat), "S map cell 0 ageMs" (stuck heat),
// "E map nodeIdA nodeIdB ageMs" (edge strike, by travel / road node id so it survives index changes),
// "K map nodeIdA nodeIdB ageMs skipMs level" (an edge's last skip, ageMs since it began). Readers skip kinds they
// don't know, so a file from either version loads in the other.
void RestoreLearned()
{
    if (learnFile.empty())
        return;
    std::ifstream in(learnFile);
    if (!in)
        return;

    std::vector<BotDanger::Saved> heat;
    uint32 edges = 0;
    uint32 skips = 0;
    std::unordered_map<uint32, std::unordered_map<uint32, uint32>> indexById;  // map -> node id -> index
    // Node ids -> edge key on the loaded graph; false if the map or a node is gone
    auto edgeKey = [&](uint32 map, uint32 a, uint32 b, uint64& key)
    {
        auto graphIt = graphs.find(map);
        if (graphIt == graphs.end())
            return false;
        auto& ids = indexById[map];
        if (ids.empty())
            for (uint32 i = 0; i < graphIt->second.nodes.size(); ++i)
                ids[graphIt->second.nodes[i].id] = i;
        auto ia = ids.find(a), ib = ids.find(b);
        if (ia == ids.end() || ib == ids.end())
            return false;
        key = EdgeKey(ia->second, ib->second);
        return true;
    };
    std::string kind;
    while (in >> kind)
    {
        if (kind == "D" || kind == "S")
        {
            BotDanger::Saved saved{kind[0], 0, 0, 0, 0};
            uint32 level = 0;
            if (in >> saved.map >> saved.cell >> level >> saved.ageMs)
            {
                saved.level = static_cast<uint8>(level);
                heat.push_back(saved);
            }
        }
        else if (kind == "E")
        {
            uint32 map, a, b, age;
            uint64 key;
            if (!(in >> map >> a >> b >> age) || age >= EDGE_STRIKE_MS || !edgeKey(map, a, b, key))
                continue;
            std::lock_guard<std::mutex> lock(edgeMutex);
            edgeStrikes[map][key].strikes.push_back(getMSTime() - age);
            ++edges;
        }
        else if (kind == "K")
        {
            uint32 map, a, b, age, skipMs, level;
            uint64 key;
            if (!(in >> map >> a >> b >> age >> skipMs >> level) || !skipMs || age >= skipMs + EDGE_RECALL_MS ||
                !edgeKey(map, a, b, key))
                continue;
            std::lock_guard<std::mutex> lock(edgeMutex);
            EdgeState& edge = edgeStrikes[map][key];
            edge.skipStartMs = getMSTime() - age;
            edge.skipMs = skipMs;
            edge.level = std::min(level, EDGE_SKIP_LEVELS - 1);
            ++skips;
        }
        else
            in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }

    // A file from before skips were kept: an edge with 3 strikes inside the hour was being skipped; keep it so,
    // from its latest strike.
    {
        std::lock_guard<std::mutex> lock(edgeMutex);
        for (auto& [map, mapEdges] : edgeStrikes)
            for (auto& [key, edge] : mapEdges)
                if (edge.strikes.size() >= EDGE_STRIKES && !Skipping(edge))
                {
                    uint32 latest = *std::min_element(edge.strikes.begin(), edge.strikes.end(),
                                                      [](uint32 x, uint32 y)
                                                      { return GetMSTimeDiffToNow(x) < GetMSTimeDiffToNow(y); });
                    StartSkip(edge);
                    edge.skipStartMs = latest;
                    ++skips;
                }
    }

    BotDanger::Restore(heat);
    LOG_INFO("playerbots", "Playerbots routes: restored {} heat entries, {} edge strikes and {} edge skips from {}",
             heat.size(), edges, skips, learnFile);
}

void Load()
{
    uint32 startMs = getMSTime();

    struct NodeRef
    {
        uint32 map;
        uint32 index;
    };
    std::unordered_map<uint32, NodeRef> byId;
    std::unordered_map<uint32, MapGraph> loading;

    if (QueryResult result = PlayerbotsDatabase.Query("SELECT id, map_id, x, y, z FROM playerbots_travelnode"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 id = fields[0].Get<uint32>();
            uint32 map = fields[1].Get<uint32>();
            MapGraph& graph = loading[map];
            byId[id] = {map, static_cast<uint32>(graph.nodes.size())};
            graph.nodes.push_back({id, fields[2].Get<float>(), fields[3].Get<float>(), fields[4].Get<float>(), {}});
        } while (result->NextRow());
    }

    struct Link
    {
        float cost;
        uint32 first = 0;
        uint32 count = 0;
    };
    auto key = [](uint32 from, uint32 to) { return (static_cast<uint64>(from) << 32) | to; };
    std::unordered_map<uint64, Link> links;

    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT node_id, to_node_id, distance, swim_distance, extra_cost FROM playerbots_travelnode_link "
            "WHERE type = 1 AND calculated = 1"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 from = fields[0].Get<uint32>();
            uint32 to = fields[1].Get<uint32>();
            auto a = byId.find(from), b = byId.find(to);
            if (a == byId.end() || b == byId.end() || a->second.map != b->second.map)
                continue;

            float cost = fields[2].Get<float>() + fields[3].Get<float>() * (SWIM_WEIGHT - 1.0f) + fields[4].Get<float>();
            links[key(from, to)] = {cost};
        } while (result->NextRow());
    }

    // Points arrive ordered by link, then nr (the primary key), so each link's points are contiguous.
    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT node_id, to_node_id, x, y, z FROM playerbots_travelnode_path ORDER BY node_id, to_node_id, nr"))
    {
        uint64 current = 0;
        Link* link = nullptr;
        MapGraph* graph = nullptr;
        do
        {
            Field* fields = result->Fetch();
            uint32 from = fields[0].Get<uint32>();
            uint64 k = key(from, fields[1].Get<uint32>());
            if (k != current)
            {
                current = k;
                auto it = links.find(k);
                link = it == links.end() ? nullptr : &it->second;
                graph = link ? &loading[byId[from].map] : nullptr;
                if (link)
                    link->first = static_cast<uint32>(graph->points.size());
            }
            if (!link)
                continue;

            graph->points.push_back({fields[2].Get<float>(), fields[3].Get<float>(), fields[4].Get<float>()});
            ++link->count;
        } while (result->NextRow());
    }

    uint32 edges = 0;
    for (auto const& [k, link] : links)
    {
        if (link.count < 2)
            continue;

        uint32 from = static_cast<uint32>(k >> 32), to = static_cast<uint32>(k & 0xFFFFFFFF);
        NodeRef a = byId[from], b = byId[to];
        MapGraph& graph = loading[a.map];
        graph.nodes[a.index].edges.push_back({b.index, link.cost, link.first, link.count, false});
        ++edges;

        auto back = links.find(key(to, from));
        if (back == links.end() || back->second.count < 2)
        {
            graph.nodes[b.index].edges.push_back({a.index, link.cost, link.first, link.count, true});
            ++edges;
        }
    }

    // Roads and hand-made links first, so the pieces they join don't also get a closest-pair bridge
    AddRoads(loading, sConfigMgr->GetOption<std::string>("Playerbots.NewRpgRoadsFile", ""));
    uint32 bridges = AddLinks(loading, sConfigMgr->GetOption<std::string>("Playerbots.NewRpgRouteLinks", ""));
    for (auto& [map, graph] : loading)
        bridges += Bridge(map, graph);

    // Cells each edge passes through, for death heat (BotDanger)
    for (auto& [map, graph] : loading)
    {
        for (Node& node : graph.nodes)
        {
            for (Edge& edge : node.edges)
            {
                edge.cellFirst = static_cast<uint32>(graph.edgeCells.size());
                uint64 last = UINT64_MAX;
                for (uint32 p = 0; p < edge.count; ++p)
                {
                    Point const& point = graph.points[edge.first + p];
                    uint64 cell = BotDanger::Cell(point.x, point.y);
                    if (cell != last &&
                        std::find(graph.edgeCells.begin() + edge.cellFirst, graph.edgeCells.end(), cell) ==
                            graph.edgeCells.end())
                        graph.edgeCells.push_back(cell);
                    last = cell;
                }
                edge.cellCount = static_cast<uint32>(graph.edgeCells.size()) - edge.cellFirst;
            }
        }
    }

    size_t points = 0;
    for (auto const& [map, graph] : loading)
        points += graph.points.size();

    graphs = std::move(loading);
    RestoreLearned();
    loaded.store(true, std::memory_order_release);
    LOG_INFO("playerbots", ">> Playerbots routes: {} travel nodes, {} walk edges, {} bridges, {} path points in {} ms",
             byId.size(), edges, bridges, points, GetMSTimeDiffToNow(startMs));
}

BotRoute& State(Player* bot)
{
    std::lock_guard<std::mutex> lock(routeMutex);
    return routes[bot->GetGUID().GetCounter()];
}

// A* from `start` to the destination over the walk edges; the destination hangs off the goal nodes.
// Extra cost per recent death (similar-level bots) in a cell an edge passes through, capped per cell: routes bend
// around camps where bots keep dying on the way through (58% of deaths came 300+ yd short of the destination).
constexpr float HEAT_COST = 120.0f;
constexpr uint32 HEAT_CAP = 6;
// Same for cells where bots of any level keep getting stuck (walk unstick detours and give-ups, last 2 hours).
constexpr float STUCK_COST = 80.0f;
constexpr uint32 STUCK_CAP = 8;

std::vector<uint32> FindNodes(MapGraph const& graph, uint32 start, std::vector<uint32> const& goals, Point const& dest,
                              BotDanger::HeatMap const& heat, BotDanger::HeatMap const& stuckHeat,
                              std::unordered_set<uint64> const& bad)
{
    auto heatCost = [&](Edge const& edge)
    {
        if (heat.empty() && stuckHeat.empty())
            return 0.0f;
        uint32 deaths = 0;
        uint32 stucks = 0;
        for (uint32 c = 0; c < edge.cellCount; ++c)
        {
            uint64 cell = graph.edgeCells[edge.cellFirst + c];
            auto it = heat.find(cell);
            if (it != heat.end())
                deaths += std::min(it->second, HEAT_CAP);
            auto st = stuckHeat.find(cell);
            if (st != stuckHeat.end())
                stucks += std::min(st->second, STUCK_CAP);
        }
        return deaths * HEAT_COST + stucks * STUCK_COST;
    };

    size_t n = graph.nodes.size();
    std::vector<float> g(n, FLT_MAX);
    std::vector<uint32> parent(n, UINT32_MAX);
    std::vector<float> finish(n, FLT_MAX);  // last leg cost for goal nodes
    for (uint32 goal : goals)
    {
        Node const& node = graph.nodes[goal];
        finish[goal] = Dist(node.x, node.y, node.z, dest.x, dest.y, dest.z) * LAST_LEG_WEIGHT;
    }

    auto h = [&](uint32 i)
    {
        Node const& node = graph.nodes[i];
        return Dist(node.x, node.y, node.z, dest.x, dest.y, dest.z);
    };

    using Entry = std::pair<float, uint32>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    g[start] = 0.0f;
    open.push({h(start), start});

    float best = FLT_MAX;
    uint32 bestGoal = UINT32_MAX;
    while (!open.empty())
    {
        auto [f, i] = open.top();
        open.pop();
        if (f >= best)
            break;
        if (f > g[i] + h(i) + 0.01f)
            continue;  // stale entry

        if (finish[i] != FLT_MAX && g[i] + finish[i] < best)
        {
            best = g[i] + finish[i];
            bestGoal = i;
        }

        for (Edge const& edge : graph.nodes[i].edges)
        {
            if (!bad.empty() && bad.count(EdgeKey(i, edge.to)))
                continue;
            float cost = g[i] + edge.cost + heatCost(edge);
            if (cost < g[edge.to])
            {
                g[edge.to] = cost;
                parent[edge.to] = i;
                open.push({cost + h(edge.to), edge.to});
            }
        }
    }

    std::vector<uint32> chain;
    for (uint32 i = bestGoal; i != UINT32_MAX; i = parent[i])
        chain.push_back(i);
    std::reverse(chain.begin(), chain.end());
    return chain;
}

bool Build(Player* bot, BotRoute& route, MapGraph const& graph)
{
    route.path.clear();
    route.edges.clear();
    route.idx = 0;
    route.done = true;  // until a route is found
    route.builtMs = getMSTime();

    float bx = bot->GetPositionX(), by = bot->GetPositionY(), bz = bot->GetPositionZ();
    Point const& dest = route.dest;

    std::vector<std::pair<float, uint32>> near, goalNear;
    for (uint32 i = 0; i < graph.nodes.size(); ++i)
    {
        Node const& node = graph.nodes[i];
        if (node.edges.empty())
            continue;

        float toBot = Dist(node.x, node.y, node.z, bx, by, bz);
        if (toBot < START_RANGE &&
            std::find(route.badStarts.begin(), route.badStarts.end(), node.id) == route.badStarts.end())
            near.push_back({toBot, i});

        float toDest = Dist(node.x, node.y, node.z, dest.x, dest.y, dest.z);
        if (toDest < GOAL_RANGE)
            goalNear.push_back({toDest, i});
    }

    if (near.empty() || goalNear.empty())
        return false;

    std::sort(near.begin(), near.end());
    std::sort(goalNear.begin(), goalNear.end());
    if (near.size() > START_CANDIDATES)
        near.resize(START_CANDIDATES);
    if (goalNear.size() > GOAL_CANDIDATES)
        goalNear.resize(GOAL_CANDIDATES);

    // Start at the nearest node mmap can actually reach (out of the cave, not through its back wall)
    uint32 start = near.front().second;
    for (size_t t = 0; t < std::min(START_TESTS, near.size()); ++t)
    {
        Node const& node = graph.nodes[near[t].second];
        PathGenerator path(bot);
        path.CalculatePath(node.x, node.y, node.z);
        G3D::Vector3 const& end = path.GetActualEndPosition();
        if (path.GetPathType() == PATHFIND_NORMAL || Dist(end.x, end.y, end.z, node.x, node.y, node.z) < REACHED)
        {
            start = near[t].second;
            break;
        }
    }

    std::vector<uint32> goals;
    for (auto const& [d, i] : goalNear)
        goals.push_back(i);

    std::vector<uint32> chain = FindNodes(graph, start, goals, dest, BotDanger::Heat(bot->GetMapId(), bot->GetLevel()),
                                          BotDanger::StuckHeat(bot->GetMapId()), BadEdges(bot->GetMapId()));
    if (chain.empty())
        return false;

    Node const& first = graph.nodes[chain.front()];
    route.startNode = first.id;
    route.path.push_back({first.x, first.y, first.z});
    route.edges.push_back(0);
    for (size_t c = 0; c + 1 < chain.size(); ++c)
    {
        Node const& from = graph.nodes[chain[c]];
        for (Edge const& edge : from.edges)
        {
            if (edge.to != chain[c + 1])
                continue;

            for (uint32 p = 0; p < edge.count; ++p)
            {
                uint32 offset = edge.reverse ? edge.first + edge.count - 1 - p : edge.first + p;
                route.path.push_back(graph.points[offset]);
                route.edges.push_back(EdgeKey(chain[c], chain[c + 1]));
            }
            break;
        }
    }

    route.done = false;
    return true;
}

// Nearest point of the route at or after `from` within `range` of the bot, or SIZE_MAX.
size_t PickUp(BotRoute const& route, Point const& at, size_t from, float range)
{
    size_t bestIdx = SIZE_MAX;
    float bestDist = range;
    for (size_t i = from; i < route.path.size(); ++i)
    {
        float d = Dist(route.path[i], at);
        if (d < bestDist)
        {
            bestDist = d;
            bestIdx = i;
        }
    }
    return bestIdx;
}
}  // namespace

bool Enabled() { return enabled && loaded.load(std::memory_order_acquire); }

void StartLoading()
{
    if (started.exchange(true))
        return;

    enabled = sConfigMgr->GetOption<bool>("Playerbots.NewRpgRoutes", false);
    learnFile = sConfigMgr->GetOption<std::string>("Playerbots.NewRpgLearnFile", "");
    if (!enabled)
        return;

    std::thread(Load).detach();
}

bool NextWaypoint(Player* bot, float x, float y, float z, Point& out, bool& advanced)
{
    advanced = false;
    if (!Enabled())
        return false;

    auto graphIt = graphs.find(bot->GetMapId());
    if (graphIt == graphs.end())
        return false;
    MapGraph const& graph = graphIt->second;

    BotRoute& route = State(bot);
    Point dest{x, y, z};
    Point here{bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);

    if (!route.hasDest || route.map != bot->GetMapId() || Dist(route.dest, dest) > 10.0f)
    {
        route.map = bot->GetMapId();
        route.dest = dest;
        route.hasDest = true;
        route.badStarts.clear();
        bool built = Build(bot, route, graph);
        if (botAI)
            BotTelemetry::instance().Event(botAI, "route", built ? "build" : "none",
                                           static_cast<int32>(route.path.size()),
                                           static_cast<int32>(Dist(here, dest)));
    }

    if (route.done || route.path.empty())
        return false;

    // Off the route (fought, fled, detoured): pick it up again nearby, or rebuild from here.
    if (Dist(here, route.path[route.idx]) > RESYNC)
    {
        size_t pick = PickUp(route, here, route.idx, RESYNC_PICKUP);
        if (pick != SIZE_MAX)
            route.idx = pick;
        else if (GetMSTimeDiffToNow(route.builtMs) >= REBUILD_MIN_MS && !Build(bot, route, graph))
            return false;
    }

    while (route.idx < route.path.size() && Dist(here, route.path[route.idx]) < REACHED)
    {
        ++route.idx;
        advanced = true;
    }

    if (route.idx >= route.path.size())
    {
        route.done = true;
        return false;
    }

    size_t target = route.idx;
    while (target + 1 < route.path.size() && target + 1 - route.idx <= MAX_HOP_POINTS &&
           Dist(here, route.path[target + 1]) <= HOP)
        ++target;

    out = route.path[target];
    return true;
}

void Reroute(Player* bot)
{
    BotRoute& route = State(bot);
    if (!route.hasDest)
        return;

    if (route.startNode)
        route.badStarts.push_back(route.startNode);

    // A strike for the edge the bot was on (none while still heading for the start node)
    if (!route.done && route.idx < route.edges.size() && route.edges[route.idx] && route.map == bot->GetMapId())
    {
        uint64 key = route.edges[route.idx];
        uint32 skipMs = 0;
        {
            std::lock_guard<std::mutex> lock(edgeMutex);
            EdgeState& edge = edgeStrikes[route.map][key];
            // Bots whose route predates the skip still walk the edge; their strikes would only restart the count
            if (!Skipping(edge))
            {
                PruneStrikes(edge);
                edge.strikes.push_back(getMSTime());
                if (edge.strikes.size() >= EDGE_STRIKES)
                    skipMs = StartSkip(edge);
            }
        }
        if (skipMs)
        {
            uint32 hours = skipMs / (HOUR * IN_MILLISECONDS);
            auto graphIt = graphs.find(route.map);
            if (graphIt != graphs.end())
            {
                Node const& a = graphIt->second.nodes[key >> 32];
                Node const& b = graphIt->second.nodes[key & 0xFFFFFFFF];
                LOG_INFO("playerbots",
                         "Playerbots routes: edge on map {} from node {} ({:.0f} {:.0f} {:.0f}) to node {} "
                         "({:.0f} {:.0f} {:.0f}) skipped for {} h: {} bots stuck on it",
                         route.map, a.id, a.x, a.y, a.z, b.id, b.x, b.y, b.z, hours, EDGE_STRIKES);
            }
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                BotTelemetry::instance().Event(botAI, "route", "bad_edge", static_cast<int32>(key >> 32),
                                               static_cast<int32>(key & 0xFFFFFFFF), static_cast<int32>(hours));
        }
    }

    auto graphIt = graphs.find(bot->GetMapId());
    if (!Enabled() || graphIt == graphs.end() || route.map != bot->GetMapId())
        return;

    Build(bot, route, graphIt->second);
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        BotTelemetry::instance().Event(botAI, "route", route.done ? "reroute_none" : "reroute",
                                       static_cast<int32>(route.path.size()), 0);
}

void SaveLearned()
{
    if (learnFile.empty() || !loaded.load(std::memory_order_acquire))
        return;

    std::string tmp = learnFile + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
        {
            LOG_ERROR("playerbots", "Playerbots routes: can't write {}", tmp);
            return;
        }
        for (BotDanger::Saved const& saved : BotDanger::Snapshot())
            out << saved.kind << ' ' << saved.map << ' ' << saved.cell << ' ' << uint32(saved.level) << ' '
                << saved.ageMs << '\n';

        std::lock_guard<std::mutex> lock(edgeMutex);
        for (auto const& [map, mapEdges] : edgeStrikes)
        {
            auto graphIt = graphs.find(map);
            if (graphIt == graphs.end())
                continue;
            for (auto const& [key, edge] : mapEdges)
            {
                uint32 a = graphIt->second.nodes[key >> 32].id;
                uint32 b = graphIt->second.nodes[key & 0xFFFFFFFF].id;
                for (uint32 ms : edge.strikes)
                {
                    uint32 age = GetMSTimeDiffToNow(ms);
                    if (age < EDGE_STRIKE_MS)
                        out << "E " << map << ' ' << a << ' ' << b << ' ' << age << '\n';
                }
                if (Recalled(edge))
                    out << "K " << map << ' ' << a << ' ' << b << ' ' << GetMSTimeDiffToNow(edge.skipStartMs) << ' '
                        << edge.skipMs << ' ' << edge.level << '\n';
            }
        }
    }
    std::rename(tmp.c_str(), learnFile.c_str());
}

void UpdateLearned(uint32 diff)
{
    static uint32 sinceSave = 0;
    sinceSave += diff;
    if (sinceSave < 10 * MINUTE * IN_MILLISECONDS)
        return;
    sinceSave = 0;
    SaveLearned();
}

void Forget(Player* bot)
{
    std::lock_guard<std::mutex> lock(routeMutex);
    routes.erase(bot->GetGUID().GetCounter());
}
}  // namespace BotRoutes
