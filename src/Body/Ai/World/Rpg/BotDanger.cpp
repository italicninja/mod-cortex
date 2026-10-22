/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotDanger.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Config.h"
#include "Player.h"
#include "Timer.h"

namespace BotDanger
{
namespace
{
constexpr uint32 DEATHS = 2;
constexpr float RADIUS = 150.0f;
constexpr uint32 WINDOW_MS = 30 * MINUTE * IN_MILLISECONDS;
constexpr size_t KEEP = 8;

struct Death
{
    uint32 map;
    float x, y, z;
    uint32 ms;
};

constexpr uint32 HEAT_WINDOW_MS = HOUR * IN_MILLISECONDS;
constexpr size_t HEAT_KEEP = 6000;
constexpr float CELL = 100.0f;

struct GlobalDeath
{
    uint32 map;
    uint64 cell;
    uint8 level;
    uint32 ms;
};
std::deque<GlobalDeath> recent;  // all bots, oldest first (under deathMutex)

constexpr uint32 STUCK_WINDOW_MS = 2 * HOUR * IN_MILLISECONDS;
struct Stuck
{
    uint32 map;
    uint64 cell;
    uint32 ms;
};
std::deque<Stuck> stuck;  // all bots, oldest first (under deathMutex)

std::once_flag configOnce;
bool enabled = false;
std::mutex deathMutex;
std::unordered_map<ObjectGuid::LowType, std::vector<Death>> deaths;

void ReadConfig() { enabled = sConfigMgr->GetOption<bool>("Playerbots.NewRpgDangerZones", false); }
}  // namespace

bool Enabled()
{
    std::call_once(configOnce, ReadConfig);
    return enabled;
}

void NoteDeath(Player* bot)
{
    if (!Enabled())
        return;

    std::lock_guard<std::mutex> lock(deathMutex);
    std::vector<Death>& list = deaths[bot->GetGUID().GetCounter()];
    if (list.size() >= KEEP)
        list.erase(list.begin());
    list.push_back({bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), getMSTime()});

    if (recent.size() >= HEAT_KEEP)
        recent.pop_front();
    recent.push_back({bot->GetMapId(), Cell(bot->GetPositionX(), bot->GetPositionY()), bot->GetLevel(), getMSTime()});
}

void NoteStuck(Player* bot)
{
    if (!Enabled())
        return;

    std::lock_guard<std::mutex> lock(deathMutex);
    if (stuck.size() >= HEAT_KEEP)
        stuck.pop_front();
    stuck.push_back({bot->GetMapId(), Cell(bot->GetPositionX(), bot->GetPositionY()), getMSTime()});
}

HeatMap StuckHeat(uint32 mapId)
{
    HeatMap heat;
    if (!Enabled())
        return heat;

    std::lock_guard<std::mutex> lock(deathMutex);
    while (!stuck.empty() && GetMSTimeDiffToNow(stuck.front().ms) >= STUCK_WINDOW_MS)
        stuck.pop_front();

    for (Stuck const& s : stuck)
    {
        if (s.map == mapId)
            ++heat[s.cell];
    }
    return heat;
}

std::vector<Saved> Snapshot()
{
    std::vector<Saved> saved;
    std::lock_guard<std::mutex> lock(deathMutex);
    for (GlobalDeath const& d : recent)
        saved.push_back({'D', d.map, d.cell, d.level, GetMSTimeDiffToNow(d.ms)});
    for (Stuck const& st : stuck)
        saved.push_back({'S', st.map, st.cell, 0, GetMSTimeDiffToNow(st.ms)});
    return saved;
}

void Restore(std::vector<Saved> const& saved)
{
    std::lock_guard<std::mutex> lock(deathMutex);
    uint32 now = getMSTime();
    // oldest first, as the deques expect
    std::vector<Saved> sorted = saved;
    std::sort(sorted.begin(), sorted.end(), [](Saved const& a, Saved const& b) { return a.ageMs > b.ageMs; });
    for (Saved const& s : sorted)
    {
        uint32 ms = now - s.ageMs;  // wraps like getMSTime does; GetMSTimeDiffToNow handles it
        if (s.kind == 'D' && s.ageMs < HEAT_WINDOW_MS && recent.size() < HEAT_KEEP)
            recent.push_back({s.map, s.cell, s.level, ms});
        else if (s.kind == 'S' && s.ageMs < STUCK_WINDOW_MS && stuck.size() < HEAT_KEEP)
            stuck.push_back({s.map, s.cell, ms});
    }
}

uint64 Cell(float x, float y)
{
    int32 cx = static_cast<int32>(std::floor(x / CELL));
    int32 cy = static_cast<int32>(std::floor(y / CELL));
    return (static_cast<uint64>(static_cast<uint32>(cx)) << 32) | static_cast<uint32>(cy);
}

HeatMap Heat(uint32 mapId, uint8 level)
{
    HeatMap heat;
    if (!Enabled())
        return heat;

    std::lock_guard<std::mutex> lock(deathMutex);
    while (!recent.empty() && GetMSTimeDiffToNow(recent.front().ms) >= HEAT_WINDOW_MS)
        recent.pop_front();

    for (GlobalDeath const& death : recent)
    {
        if (death.map == mapId && death.level + 2 >= level && death.level <= level + 2)
            ++heat[death.cell];
    }
    return heat;
}

bool IsDangerous(Player* bot, uint32 mapId, float x, float y, float z)
{
    if (!Enabled())
        return false;

    std::lock_guard<std::mutex> lock(deathMutex);
    auto it = deaths.find(bot->GetGUID().GetCounter());
    if (it == deaths.end())
        return false;

    uint32 near = 0;
    for (Death const& death : it->second)
    {
        if (death.map != mapId || GetMSTimeDiffToNow(death.ms) >= WINDOW_MS)
            continue;

        float dx = death.x - x, dy = death.y - y, dz = death.z - z;
        if (dx * dx + dy * dy + dz * dz < RADIUS * RADIUS)
            ++near;
    }
    return near >= DEATHS;
}
}  // namespace BotDanger
