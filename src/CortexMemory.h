/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 * Originally written as part of the wowproject patches to mod-playerbots.
 */

#ifndef MOD_CORTEX_CORTEXMEMORY_H
#define MOD_CORTEX_CORTEXMEMORY_H

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Define.h"

// What one bot knows about the world (docs/bot-ai-overhaul.md §4.2). Filled only from what the bot perceived or
// experienced; nothing here is read from spawn or loot tables. Times are unix seconds so records survive restarts.
// Owned by one bot and touched only on that bot's map thread (the world thread only loads it before first use).
class CortexMemory
{
public:
    static constexpr float CELL_SIZE = 50.0f;

    enum NpcService : uint32
    {
        SERVICE_QUESTGIVER = 0x001,
        SERVICE_VENDOR = 0x002,
        SERVICE_REPAIR = 0x004,
        SERVICE_TRAINER = 0x008,
        SERVICE_CLASS_TRAINER = 0x010,
        SERVICE_PROFESSION_TRAINER = 0x020,
        SERVICE_INNKEEPER = 0x040,
        SERVICE_FLIGHTMASTER = 0x080,
        SERVICE_AUCTIONEER = 0x100,
        SERVICE_BANKER = 0x200,
        SERVICE_SPIRITHEALER = 0x400,
        SERVICE_FOOD = 0x800,
        SERVICE_AMMO = 0x1000,
        SERVICE_REAGENT = 0x2000,
    };

    enum CellTag : uint8
    {
        CELL_TOWN = 0x01,  // two or more service NPCs seen in it
        CELL_GRAVEYARD = 0x02,
        CELL_DEATH = 0x04,
    };

    enum ObjectKind : uint8
    {
        OBJECT_QUEST = 1,  // usable or lootable for one of the bot's quests when seen (the quest sparkle)
        OBJECT_MAILBOX = 2,
        OBJECT_NODE = 3,  // a herb or ore node of a gathering skill the bot has, seen or tracked
    };

    struct Npc
    {
        uint32 entry = 0;
        uint16 map = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        uint32 lastSeen = 0;
        uint32 services = 0;
        uint8 trainerClass = 0;  // read from the title, e.g. <Warrior Trainer>
        uint8 marker = 0;        // QuestGiverStatus the bot saw over its head
        uint16 sightings = 0;
    };

    struct Mob
    {
        uint32 entry = 0;
        uint16 map = 0;
        uint64 cell = 0;
        uint32 lastSeen = 0;
        uint8 minLevel = 0;
        uint8 maxLevel = 0;
        uint8 maxCount = 0;  // most alive at once in this cell
        bool elite = false;
        uint16 sightings = 0;
        uint16 kills = 0;
    };

    struct Object
    {
        uint32 entry = 0;
        uint16 map = 0;
        uint64 cell = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        uint32 lastSeen = 0;
        uint8 kind = 0;
        uint32 goneUntil = 0;  // a node found gone: not there before a respawn could bring it back (unix seconds)
        uint8 misses = 0;      // times found gone since last seen up
    };

    struct Drop
    {
        int32 source = 0;  // creature entry, or -entry for a gameobject
        uint32 item = 0;
        uint16 looted = 0;
        uint32 lastSeen = 0;
    };

    struct Cell
    {
        uint32 firstSeen = 0;
        uint32 lastSeen = 0;
        uint16 visits = 0;
        uint8 tags = 0;
    };

    // A quest set aside (CortexQuests::Shelve): back when the bot levels or the time passes.
    struct Shelf
    {
        uint32 untilLevel = 0;
        uint32 untilTime = 0;  // unix seconds
        uint8 count = 0;
        uint8 rewardFails = 0;  // times at the turn-in with nobody there to take it (CortexQuests::Shelve "reward")
    };

    // A place the bot got stuck walking to (playerbots' unstick gave up, hearthed or moved it to a graveyard): not
    // a pick-up, grind or vendor spot until the time passes, longer each time it happens again.
    struct Blocked
    {
        uint16 map = 0;
        float x = 0.0f;
        float y = 0.0f;
        uint32 untilTime = 0;  // unix seconds
        uint8 count = 0;
    };

    struct Danger
    {
        uint16 map = 0;
        float x = 0.0f;
        float y = 0.0f;
        uint32 time = 0;
        uint32 killer = 0;  // creature entry, 0 for a player or unknown
        uint8 attackers = 0;
        int8 levelDiff = 0;
    };

    // 30 bits: map (10), cell x (10), cell y (10). Map coordinates span ±17066 yd, 683 cells per axis.
    static uint64 CellKey(uint32 map, float x, float y);
    // Key of an NPC, mob or object record: (entry << 30) | CellKey.
    static uint64 EntryKey(uint32 entry, uint64 cell) { return (static_cast<uint64>(entry) << 30) | cell; }

    void SeeNpc(Npc const& seen);
    void SeeMob(uint32 entry, uint32 map, float x, float y, uint8 level, uint8 count, bool elite, uint32 now);
    void SeeObject(Object const& seen);
    // A remembered node not up where the bot would see it: gone for a respawn time, forgotten when found gone again.
    void MissObject(uint64 key, uint32 now);
    void VisitCell(uint32 map, float x, float y, uint32 now);
    void TagCell(uint32 map, float x, float y, uint8 tag, uint32 now);
    void NoteKill(uint32 entry, uint32 map, float x, float y, uint32 now);
    void NoteLoot(int32 source, uint32 item, uint32 count, uint32 now);
    void NoteDeath(Danger const& death);
    // A quest the bot dropped: it won't take it again until it has outlevelled the trouble, longer each time.
    void GiveUp(uint32 questId, uint8 level);
    bool GaveUp(uint32 questId, uint8 level) const;
    bool HasGivenUp() const { return !_givenUp.empty(); }
    // Set-asides are kept here, not in the plan, so a restart doesn't send every bot back to them at once.
    Shelf& ShelfFor(uint32 questId) { return _shelved[questId]; }
    Shelf const* FindShelf(uint32 questId) const;
    void Unshelve(uint32 questId) { _shelved.erase(questId); }
    // A quest nobody would take in: dropped and never taken again (its ender never shows for a bot).
    void GiveUpForGood(uint32 questId);
    // Records a stuck place; returns how long it is blocked, in seconds, and the times it was blocked in `count`.
    uint32 BlockSpot(uint32 map, float x, float y, uint32 now, uint32& count);
    bool SpotBlocked(uint32 map, float x, float y, uint32 now) const;

    std::unordered_map<uint64, Npc> const& Npcs() const { return _npcs; }
    std::unordered_map<uint64, Mob> const& Mobs() const { return _mobs; }
    std::unordered_map<uint64, Object> const& Objects() const { return _objects; }
    std::unordered_map<uint64, Drop> const& Drops() const { return _drops; }
    uint16 Kills(uint32 entry) const;
    bool HasCellTag(uint32 map, float x, float y, uint8 tag) const;
    static void CellCentre(uint64 cell, float& x, float& y);
    // The bot went to a remembered "!" and found nothing to take there. The givers there are remembered as offering
    // nothing at this level: their "!" (a seasonal quest, a turn-in that wants items, a chain the bot isn't on) is
    // not marked again until the bot levels.
    void ClearMarkersNear(uint32 map, float x, float y, float range, uint8 level);
    bool NothingOffered(uint32 entry, uint8 level) const;

    size_t NpcCount() const { return _npcs.size(); }
    size_t MobCount() const { return _mobs.size(); }
    size_t ObjectCount() const { return _objects.size(); }
    size_t DropCount() const { return _drops.size(); }
    size_t CellCount() const { return _cells.size(); }
    size_t DangerCount() const { return _dangers.size(); }
    // Deaths since `since` (unix seconds) within `radius` of (map, x, y).
    uint32 DeathsNear(uint32 map, float x, float y, float radius, uint32 since) const
    {
        uint32 n = 0;
        for (Danger const& d : _dangers)
            if (d.time >= since && d.map == map && (d.x - x) * (d.x - x) + (d.y - y) * (d.y - y) <= radius * radius)
                ++n;
        return n;
    }
    // Places it died since `since` (unix seconds) to a killer at least `levelDiff` levels above it, on `map`.
    std::vector<Danger> Outclassed(uint32 map, uint32 since, int8 levelDiff) const
    {
        std::vector<Danger> out;
        for (Danger const& d : _dangers)
            if (d.time >= since && d.map == map && d.levelDiff >= levelDiff)
                out.push_back(d);
        return out;
    }
    // Deaths since `since` (unix seconds) to a killer at least `levelDiff` levels above the bot.
    uint32 DeathsOutclassed(uint32 since, int8 levelDiff) const
    {
        uint32 n = 0;
        for (Danger const& d : _dangers)
            if (d.time >= since && d.levelDiff >= levelDiff)
                ++n;
        return n;
    }

    std::string Serialize() const;
    bool Deserialize(std::string const& data);
    void Clear();

private:
    // Keys: (entry << 30) | CellKey for NPCs, mobs and objects; (source << 32) | item for drops.
    std::unordered_map<uint64, Npc> _npcs;
    std::unordered_map<uint64, Mob> _mobs;
    std::unordered_map<uint64, Object> _objects;
    std::unordered_map<uint64, Drop> _drops;
    std::unordered_map<uint64, Cell> _cells;
    std::unordered_map<uint32, uint16> _kills; // creature entry -> kills, for drop rates
    std::vector<Danger> _dangers;
    std::unordered_map<uint32, std::pair<uint8, uint8>> _givenUp;  // quest -> (untilLevel, times dropped)
    std::unordered_map<uint32, uint8> _nothingOffered;  // giver entry -> bot level it offered nothing at
    std::unordered_map<uint32, Shelf> _shelved;
    std::vector<Blocked> _blocked;
};

#endif
