/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTDANGER_H
#define PLAYERBOTS_BOTDANGER_H

#include <unordered_map>
#include <vector>

#include "Define.h"

class Player;

// Places a bot keeps dying at (Playerbots.NewRpgDangerZones). Bots came back from a corpse run at ~64% health,
// walked ~240 yd back to the same quest, grind spot or camp and died again within two minutes: 44% of all deaths.
// After DEATHS deaths within RADIUS yards in WINDOW minutes the area is off limits for that bot for the rest of
// the window: new rpg drops destinations in it and grind / camp selection skips it.
namespace BotDanger
{
bool Enabled();
void NoteDeath(Player* bot);
bool IsDangerous(Player* bot, uint32 mapId, float x, float y, float z);

// Deaths of all bots over the last HEAT_WINDOW, per 100 yd cell of one map, counting only bots within 2 levels of
// `level` (what kills a level 12 bot may not kill a level 16 one). Routes weight their edges by it and grind / camp
// selection skips hot cells. Empty when danger zones are off.
using HeatMap = std::unordered_map<uint64, uint32>;
HeatMap Heat(uint32 mapId, uint8 level);
// Walk-unstick detours and give-ups of all bots over the last STUCK_WINDOW, per 100 yd cell: pockets bots keep
// getting stuck in on the way through (they head 150-1000 yd past them). Routes weight their edges by it.
void NoteStuck(Player* bot);
HeatMap StuckHeat(uint32 mapId);

// What the heat maps hold, for saving across restarts (BotRoutes::SaveLearned): kind 'D' death, 'S' stuck.
struct Saved
{
    char kind;
    uint32 map;
    uint64 cell;
    uint8 level;
    uint32 ageMs;
};
std::vector<Saved> Snapshot();
void Restore(std::vector<Saved> const& saved);
uint64 Cell(float x, float y);
}  // namespace BotDanger

#endif
