/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 * Originally written as part of the wowproject patches to mod-playerbots.
 */

#include "CortexMemory.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "GameTime.h"

namespace
{
    constexpr float MAP_HALF_SIZE = 17066.666f;
    constexpr uint32 CELL_AXIS_MASK = 0x3FF;
    constexpr uint32 MAP_MASK = 0x3FF;

    constexpr size_t MAX_NPCS = 300;
    constexpr size_t MAX_MOBS = 600;
    constexpr size_t MAX_OBJECTS = 400;  // quest objects, mailboxes, gathering nodes
    // Node respawns are 2 to 15 minutes, and most nodes are in spawn pools that bring them back at another spawn
    // point: a node found gone is left for a respawn time, and forgotten when it's gone the next time too.
    constexpr uint32 NODE_GONE_SECONDS = 15 * 60;
    constexpr uint8 NODE_MAX_MISSES = 2;
    constexpr size_t MAX_DROPS = 300;
    constexpr size_t MAX_CELLS = 2000;
    constexpr size_t MAX_KILL_ENTRIES = 400;
    constexpr size_t MAX_DANGERS = 64;
    constexpr size_t MAX_GIVEN_UP = 100;
    constexpr uint8 GIVE_UP_LEVELS = 3;  // per time dropped
    constexpr uint32 SHELF_KEEP_SECONDS = 24 * 3600;  // past its time, so the next set-aside still backs off longer
    constexpr uint8 GIVEN_UP_FOR_GOOD = 0xFF;          // untilLevel no bot reaches
    // Stuck places: within this range of a recorded one is the same place. Blocked 30 min, then 2 h, then 6 h; the
    // record is kept a day past its time so the next block there still escalates.
    constexpr float BLOCK_RADIUS = 50.0f;
    constexpr uint32 BLOCK_SECONDS[] = {30 * 60, 2 * 3600, 6 * 3600};
    constexpr uint32 BLOCK_KEEP_SECONDS = 24 * 3600;
    constexpr size_t MAX_BLOCKED = 64;

    // A cell counts as a new visit after this long away.
    constexpr uint32 CELL_REVISIT_SECONDS = 600;
    // Same entry within this range of a remembered NPC: the same NPC, moved.
    constexpr float NPC_SAME_RANGE = 60.0f;

    constexpr char const* FORMAT_VERSION = "v1";

    // Drops the oldest tenth of a full table, so eviction runs once per many inserts.
    template <typename Map>
    void Evict(Map& records, size_t cap)
    {
        if (records.size() <= cap)
            return;

        std::vector<uint32> ages;
        ages.reserve(records.size());
        for (auto const& [key, record] : records)
            ages.push_back(record.lastSeen);

        size_t drop = std::max<size_t>(1, cap / 10);
        std::nth_element(ages.begin(), ages.begin() + drop - 1, ages.end());
        uint32 cutoff = ages[drop - 1];
        for (auto it = records.begin(); it != records.end() && drop;)
        {
            if (it->second.lastSeen <= cutoff)
            {
                it = records.erase(it);
                --drop;
            }
            else
                ++it;
        }
    }

    template <typename T>
    bool Read(std::istringstream& in, T& value)
    {
        return static_cast<bool>(in >> value);
    }

    // Narrow integers read through a wider type, so uint8 fields don't parse as characters.
    template <typename Narrow>
    bool ReadNarrow(std::istringstream& in, Narrow& value)
    {
        int64 wide = 0;
        if (!(in >> wide))
            return false;
        value = static_cast<Narrow>(wide);
        return true;
    }
}

uint64 CortexMemory::CellKey(uint32 map, float x, float y)
{
    uint32 cx = static_cast<uint32>(std::max(0.0f, (x + MAP_HALF_SIZE) / CELL_SIZE)) & CELL_AXIS_MASK;
    uint32 cy = static_cast<uint32>(std::max(0.0f, (y + MAP_HALF_SIZE) / CELL_SIZE)) & CELL_AXIS_MASK;
    return (static_cast<uint64>(map & MAP_MASK) << 20) | (cx << 10) | cy;
}

void CortexMemory::CellCentre(uint64 cell, float& x, float& y)
{
    x = static_cast<float>((cell >> 10) & CELL_AXIS_MASK) * CELL_SIZE + CELL_SIZE / 2 - MAP_HALF_SIZE;
    y = static_cast<float>(cell & CELL_AXIS_MASK) * CELL_SIZE + CELL_SIZE / 2 - MAP_HALF_SIZE;
}

uint16 CortexMemory::Kills(uint32 entry) const
{
    auto it = _kills.find(entry);
    return it == _kills.end() ? 0 : it->second;
}

bool CortexMemory::HasCellTag(uint32 map, float x, float y, uint8 tag) const
{
    auto it = _cells.find(CellKey(map, x, y));
    return it != _cells.end() && (it->second.tags & tag);
}

void CortexMemory::ClearMarkersNear(uint32 map, float x, float y, float range, uint8 level)
{
    for (auto& [key, npc] : _npcs)
        if (npc.map == map && std::hypot(npc.x - x, npc.y - y) <= range)
        {
            if (npc.marker)
                _nothingOffered[npc.entry] = level;
            npc.marker = 0;
            npc.seenLevel = std::max(npc.seenLevel, level);
        }
}

bool CortexMemory::NothingOffered(uint32 entry, uint8 level) const
{
    auto it = _nothingOffered.find(entry);
    return it != _nothingOffered.end() && it->second == level;
}

void CortexMemory::GiveUp(uint32 questId, uint8 level)
{
    auto& [untilLevel, times] = _givenUp[questId];
    times = static_cast<uint8>(std::min<uint32>(times + 1, 0xFF));
    untilLevel = static_cast<uint8>(std::min<uint32>(level + GIVE_UP_LEVELS * times, 0xFF));
    if (_givenUp.size() > MAX_GIVEN_UP)
        std::erase_if(_givenUp, [level](auto const& entry) { return entry.second.first <= level; });
}

void CortexMemory::GiveUpForGood(uint32 questId)
{
    auto& [untilLevel, times] = _givenUp[questId];
    times = static_cast<uint8>(std::min<uint32>(times + 1, 0xFF));
    untilLevel = GIVEN_UP_FOR_GOOD;
}

uint32 CortexMemory::BlockSpot(uint32 map, float x, float y, uint32 now, uint32& count)
{
    std::erase_if(_blocked, [now](Blocked const& b) { return b.untilTime + BLOCK_KEEP_SECONDS <= now; });

    Blocked* spot = nullptr;
    for (Blocked& b : _blocked)
        if (b.map == map && std::hypot(b.x - x, b.y - y) <= BLOCK_RADIUS)
        {
            spot = &b;
            break;
        }
    if (!spot)
    {
        if (_blocked.size() >= MAX_BLOCKED)
            _blocked.erase(std::min_element(_blocked.begin(), _blocked.end(), [](Blocked const& a, Blocked const& b)
                                            { return a.untilTime < b.untilTime; }));
        _blocked.push_back({static_cast<uint16>(map), x, y, 0, 0});
        spot = &_blocked.back();
    }
    spot->count = static_cast<uint8>(std::min<uint32>(spot->count + 1, 0xFF));
    constexpr size_t steps = sizeof(BLOCK_SECONDS) / sizeof(BLOCK_SECONDS[0]);
    uint32 seconds = BLOCK_SECONDS[std::min<size_t>(spot->count - 1, steps - 1)];
    spot->untilTime = std::max(spot->untilTime, now + seconds);
    count = spot->count;
    return seconds;
}

bool CortexMemory::SpotBlocked(uint32 map, float x, float y, uint32 now) const
{
    for (Blocked const& b : _blocked)
        if (b.untilTime > now && b.map == map && std::hypot(b.x - x, b.y - y) <= BLOCK_RADIUS)
            return true;
    return false;
}

bool CortexMemory::GaveUp(uint32 questId, uint8 level) const
{
    auto it = _givenUp.find(questId);
    return it != _givenUp.end() && level < it->second.first;
}

CortexMemory::Shelf const* CortexMemory::FindShelf(uint32 questId) const
{
    auto it = _shelved.find(questId);
    return it == _shelved.end() ? nullptr : &it->second;
}

void CortexMemory::SeeNpc(Npc const& seen)
{
    uint64 key = EntryKey(seen.entry, CellKey(seen.map, seen.x, seen.y));
    // An NPC that walks into the next cell is the same NPC: move its record instead of adding a second one.
    auto moveFromNeighbourCell = [&]()
    {
        for (int32 dx = -1; dx <= 1; ++dx)
            for (int32 dy = -1; dy <= 1; ++dy)
            {
                uint64 near = EntryKey(seen.entry, CellKey(seen.map, seen.x + dx * CELL_SIZE, seen.y + dy * CELL_SIZE));
                auto old = _npcs.find(near);
                if (near == key || old == _npcs.end())
                    continue;
                float ox = old->second.x - seen.x;
                float oy = old->second.y - seen.y;
                if (ox * ox + oy * oy > NPC_SAME_RANGE * NPC_SAME_RANGE)
                    continue;
                Npc moved = old->second;
                _npcs.erase(old);
                _npcs.emplace(key, moved);
                return;
            }
    };
    if (!_npcs.count(key))
        moveFromNeighbourCell();

    auto [it, inserted] = _npcs.try_emplace(key, seen);
    Npc& npc = it->second;
    if (!inserted)
    {
        uint16 sightings = npc.sightings;
        npc = seen;
        npc.sightings = sightings;
    }
    npc.sightings = static_cast<uint16>(std::min<uint32>(npc.sightings + 1, 0xFFFF));
    Evict(_npcs, MAX_NPCS);
}

void CortexMemory::SeeMob(uint32 entry, uint32 map, float x, float y, uint8 level, uint8 count, bool elite, uint32 now)
{
    uint64 cell = CellKey(map, x, y);
    Mob& mob = _mobs[EntryKey(entry, cell)];
    if (!mob.entry)
    {
        mob.entry = entry;
        mob.map = static_cast<uint16>(map);
        mob.cell = cell;
        mob.minLevel = level;
        mob.maxLevel = level;
    }
    mob.minLevel = std::min(mob.minLevel, level);
    mob.maxLevel = std::max(mob.maxLevel, level);
    mob.maxCount = std::max(mob.maxCount, count);
    mob.elite = mob.elite || elite;
    mob.lastSeen = now;
    mob.sightings = static_cast<uint16>(std::min<uint32>(mob.sightings + 1, 0xFFFF));
    Evict(_mobs, MAX_MOBS);
}

void CortexMemory::SeeObject(Object const& seen)
{
    uint64 cell = CellKey(seen.map, seen.x, seen.y);
    Object& object = _objects[EntryKey(seen.entry, cell)];
    object = seen;
    object.cell = cell;
    Evict(_objects, MAX_OBJECTS);
}

void CortexMemory::MissObject(uint64 key, uint32 now)
{
    auto it = _objects.find(key);
    if (it == _objects.end() || it->second.goneUntil > now)
        return;
    if (++it->second.misses >= NODE_MAX_MISSES)
        _objects.erase(it);
    else
        it->second.goneUntil = now + NODE_GONE_SECONDS;
}

void CortexMemory::VisitCell(uint32 map, float x, float y, uint32 now)
{
    Cell& cell = _cells[CellKey(map, x, y)];
    if (!cell.firstSeen)
        cell.firstSeen = now;
    if (!cell.visits || now - cell.lastSeen >= CELL_REVISIT_SECONDS)
        cell.visits = static_cast<uint16>(std::min<uint32>(cell.visits + 1, 0xFFFF));
    cell.lastSeen = now;
    Evict(_cells, MAX_CELLS);
}

void CortexMemory::TagCell(uint32 map, float x, float y, uint8 tag, uint32 now)
{
    Cell& cell = _cells[CellKey(map, x, y)];
    if (!cell.firstSeen)
        cell.firstSeen = now;
    cell.lastSeen = std::max(cell.lastSeen, now);
    cell.tags |= tag;
    Evict(_cells, MAX_CELLS);
}

void CortexMemory::NoteKill(uint32 entry, uint32 map, float x, float y, uint32 now)
{
    uint16& kills = _kills[entry];
    kills = static_cast<uint16>(std::min<uint32>(kills + 1, 0xFFFF));
    if (_kills.size() > MAX_KILL_ENTRIES)
    {
        // Rarely killed entries go first; they carry the least drop-rate evidence.
        auto fewest = std::min_element(_kills.begin(), _kills.end(),
                                       [](auto const& a, auto const& b) { return a.second < b.second; });
        if (fewest->first != entry)
            _kills.erase(fewest);
    }

    auto it = _mobs.find(EntryKey(entry, CellKey(map, x, y)));
    if (it != _mobs.end())
    {
        it->second.kills = static_cast<uint16>(std::min<uint32>(it->second.kills + 1, 0xFFFF));
        it->second.lastSeen = now;
    }
}

void CortexMemory::NoteLoot(int32 source, uint32 item, uint32 count, uint32 now)
{
    uint64 key = (static_cast<uint64>(static_cast<uint32>(source)) << 32) | item;
    Drop& drop = _drops[key];
    drop.source = source;
    drop.item = item;
    drop.looted = static_cast<uint16>(std::min<uint32>(drop.looted + std::max<uint32>(count, 1), 0xFFFF));
    drop.lastSeen = now;
    Evict(_drops, MAX_DROPS);
}

void CortexMemory::NoteDeath(Danger const& death)
{
    _dangers.push_back(death);
    if (_dangers.size() > MAX_DANGERS)
        _dangers.erase(_dangers.begin());
    TagCell(death.map, death.x, death.y, CELL_DEATH, death.time);
}

void CortexMemory::Clear()
{
    _npcs.clear();
    _mobs.clear();
    _objects.clear();
    _drops.clear();
    _cells.clear();
    _kills.clear();
    _dangers.clear();
    _givenUp.clear();
    _shelved.clear();
    _blocked.clear();
}

// One record per line: a type letter, then space-separated fields. Only digits, signs, dots, letters and
// whitespace, so the text is safe inside a quoted SQL literal.
std::string CortexMemory::Serialize() const
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << FORMAT_VERSION << '\n';
    for (auto const& [key, n] : _npcs)
        out << "N " << n.entry << ' ' << n.map << ' ' << n.x << ' ' << n.y << ' ' << n.z << ' ' << n.lastSeen << ' '
            << n.services << ' ' << uint32(n.trainerClass) << ' ' << uint32(n.marker) << ' ' << n.sightings << ' ' << uint32(n.seenLevel) << '\n';
    for (auto const& [key, m] : _mobs)
        out << "M " << m.entry << ' ' << m.map << ' ' << m.cell << ' ' << m.lastSeen << ' ' << uint32(m.minLevel)
            << ' ' << uint32(m.maxLevel) << ' ' << uint32(m.maxCount) << ' ' << uint32(m.elite) << ' ' << m.sightings
            << ' ' << m.kills << '\n';
    for (auto const& [key, o] : _objects)
        out << "O " << o.entry << ' ' << o.map << ' ' << o.x << ' ' << o.y << ' ' << o.z << ' ' << o.lastSeen << ' '
            << uint32(o.kind) << ' ' << o.goneUntil << ' ' << uint32(o.misses) << '\n';
    for (auto const& [key, d] : _drops)
        out << "D " << d.source << ' ' << d.item << ' ' << d.looted << ' ' << d.lastSeen << '\n';
    for (auto const& [key, c] : _cells)
        out << "C " << key << ' ' << c.firstSeen << ' ' << c.lastSeen << ' ' << c.visits << ' ' << uint32(c.tags)
            << '\n';
    for (auto const& [entry, kills] : _kills)
        out << "K " << entry << ' ' << kills << '\n';
    for (Danger const& d : _dangers)
        out << "X " << d.map << ' ' << d.x << ' ' << d.y << ' ' << d.time << ' ' << d.killer << ' '
            << uint32(d.attackers) << ' ' << int32(d.levelDiff) << '\n';
    for (auto const& [questId, given] : _givenUp)
        out << "Q " << questId << ' ' << uint32(given.first) << ' ' << uint32(given.second) << '\n';
    uint32 now = static_cast<uint32>(GameTime::GetGameTime().count());
    for (auto const& [questId, shelf] : _shelved)
        if (shelf.untilTime + SHELF_KEEP_SECONDS > now)
            out << "S " << questId << ' ' << shelf.untilLevel << ' ' << shelf.untilTime << ' ' << uint32(shelf.count)
                << ' ' << uint32(shelf.rewardFails) << '\n';
    for (Blocked const& b : _blocked)
        if (b.untilTime + BLOCK_KEEP_SECONDS > now)
            out << "B " << b.map << ' ' << b.x << ' ' << b.y << ' ' << b.untilTime << ' ' << uint32(b.count) << '\n';
    return out.str();
}

bool CortexMemory::Deserialize(std::string const& data)
{
    Clear();
    std::istringstream lines(data);
    std::string line;
    if (!std::getline(lines, line) || line != FORMAT_VERSION)
        return false;

    while (std::getline(lines, line))
    {
        if (line.size() < 2)
            continue;

        std::istringstream in(line.substr(2));
        switch (line[0])
        {
            case 'N':
            {
                Npc n;
                if (Read(in, n.entry) && ReadNarrow(in, n.map) && Read(in, n.x) && Read(in, n.y) && Read(in, n.z) &&
                    Read(in, n.lastSeen) && Read(in, n.services) && ReadNarrow(in, n.trainerClass) &&
                    ReadNarrow(in, n.marker) && ReadNarrow(in, n.sightings))
                {
                    // Added later: absent from older records.
                    ReadNarrow(in, n.seenLevel);
                    _npcs[EntryKey(n.entry, CellKey(n.map, n.x, n.y))] = n;
                }
                break;
            }
            case 'M':
            {
                Mob m;
                if (Read(in, m.entry) && ReadNarrow(in, m.map) && Read(in, m.cell) && Read(in, m.lastSeen) &&
                    ReadNarrow(in, m.minLevel) && ReadNarrow(in, m.maxLevel) && ReadNarrow(in, m.maxCount) &&
                    ReadNarrow(in, m.elite) && ReadNarrow(in, m.sightings) && ReadNarrow(in, m.kills))
                    _mobs[EntryKey(m.entry, m.cell)] = m;
                break;
            }
            case 'O':
            {
                Object o;
                if (Read(in, o.entry) && ReadNarrow(in, o.map) && Read(in, o.x) && Read(in, o.y) && Read(in, o.z) &&
                    Read(in, o.lastSeen) && ReadNarrow(in, o.kind))
                {
                    // Added later: absent from older records.
                    if (Read(in, o.goneUntil))
                        ReadNarrow(in, o.misses);
                    o.cell = CellKey(o.map, o.x, o.y);
                    _objects[EntryKey(o.entry, o.cell)] = o;
                }
                break;
            }
            case 'D':
            {
                Drop d;
                if (Read(in, d.source) && Read(in, d.item) && ReadNarrow(in, d.looted) && Read(in, d.lastSeen))
                    _drops[(static_cast<uint64>(static_cast<uint32>(d.source)) << 32) | d.item] = d;
                break;
            }
            case 'C':
            {
                uint64 key = 0;
                Cell c;
                if (Read(in, key) && Read(in, c.firstSeen) && Read(in, c.lastSeen) && ReadNarrow(in, c.visits) &&
                    ReadNarrow(in, c.tags))
                    _cells[key] = c;
                break;
            }
            case 'K':
            {
                uint32 entry = 0;
                uint16 kills = 0;
                if (Read(in, entry) && ReadNarrow(in, kills))
                    _kills[entry] = kills;
                break;
            }
            case 'X':
            {
                Danger d;
                if (ReadNarrow(in, d.map) && Read(in, d.x) && Read(in, d.y) && Read(in, d.time) && Read(in, d.killer) &&
                    ReadNarrow(in, d.attackers) && ReadNarrow(in, d.levelDiff) && _dangers.size() < MAX_DANGERS)
                    _dangers.push_back(d);
                break;
            }
            case 'Q':
            {
                uint32 questId = 0;
                std::pair<uint8, uint8> given;
                if (Read(in, questId) && ReadNarrow(in, given.first) && ReadNarrow(in, given.second))
                    _givenUp[questId] = given;
                break;
            }
            case 'S':
            {
                uint32 questId = 0;
                Shelf shelf;
                if (Read(in, questId) && Read(in, shelf.untilLevel) && Read(in, shelf.untilTime) &&
                    ReadNarrow(in, shelf.count))
                {
                    ReadNarrow(in, shelf.rewardFails);  // added later: absent from older records
                    _shelved[questId] = shelf;
                }
                break;
            }
            case 'B':
            {
                Blocked b;
                if (ReadNarrow(in, b.map) && Read(in, b.x) && Read(in, b.y) && Read(in, b.untilTime) &&
                    ReadNarrow(in, b.count) && _blocked.size() < MAX_BLOCKED)
                    _blocked.push_back(b);
                break;
            }
            default:
                break;
        }
    }
    return true;
}
