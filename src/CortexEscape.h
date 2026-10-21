/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#ifndef _CORTEX_ESCAPE_H
#define _CORTEX_ESCAPE_H

class Player;
class PlayerbotAI;

// Navmesh islands (Cortex.Escape). The extractor leaves specks of mesh with no link to the rest: a 2x1 yd poly in the
// Wailing Caverns ravine (35 bots stood on it for hours, every path from it ended where it began), one poly at the
// foot of the Undercity zeppelin tower, a pocket at Deathknell. A bot gets onto one by dropping off a ledge or being
// knocked there, and walking can never take it off: every detour, unstick and random step is a path that ends where
// it starts. A bot that has stood still a while and whose poly reaches fewer than ISLAND_POLYS others (or that has no
// mesh under it at all) steps to the nearest poly of the mainland: walks straight there when it is below or level and
// in sight (a player hops down), else is set down there.
namespace CortexEscape
{
    // The bot's target is on its island (hasTarget false: no target): staying is fine then. `mayWalk` false: set it
    // down even where a walk would do (a walk from here was tried and the bot is still there).
    // Returns true when the bot was moved off an island.
    bool Try(PlayerbotAI* botAI, Player* bot, bool hasTarget, float tx, float ty, float tz, bool mayWalk);
}

#endif
