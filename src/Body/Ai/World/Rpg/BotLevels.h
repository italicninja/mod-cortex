/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTLEVELS_H
#define PLAYERBOTS_BOTLEVELS_H

#include "Define.h"

class Player;

// Parts of a continent walking doesn't join (Playerbots.NewRpgLevels): Thunder Bluff's mesas and Undercity are
// reached by lift, the top of Teldrassil and Rut'theran Village at its foot by the portals. New rpg walked to the
// spot under the place (the foot of Hunter's Rise under the hunter trainer, the edge of the tree over Rut'theran) and
// hearthed out after a detour. Now a place across a portal is walked to by the portal, and a place up a lift is given
// up on at once (new rpg doesn't ride lifts; Cortex does, and plans the trip when it sees the give-up).
namespace BotLevels
{
enum Level : uint8
{
    LEVEL_OPEN = 0,       // the rest of the map, or a height that isn't known
    LEVEL_THUNDER_BLUFF,  // the mesas
    LEVEL_UNDERCITY,      // under the ruins
    LEVEL_TELDRASSIL,     // the top of the tree, Darnassus included
    LEVEL_RUTTHERAN,      // the foot of the tree
};

bool Enabled();
uint8 Of(uint32 map, float x, float y, float z);
// The place is on Thunder Bluff's mesas or in Undercity, and the bot (not riding anything) isn't.
bool LiftOnly(Player* bot, uint32 map, float x, float y, float z);
// The area trigger to walk into first for a place on the other level of Teldrassil, or 0.
uint32 Portal(Player* bot, uint32 map, float x, float y, float z);
}  // namespace BotLevels

#endif
