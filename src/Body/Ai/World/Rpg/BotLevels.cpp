/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotLevels.h"

#include "Config.h"
#include "Map.h"
#include "Player.h"

namespace BotLevels
{
namespace
{
constexpr uint32 MAP_EASTERN_KINGDOMS = 0;
constexpr uint32 MAP_KALIMDOR = 1;
// The Rut'theran portal into Darnassus and the Darnassus portal down to Rut'theran (areatrigger_teleport)
constexpr uint32 TRIGGER_TO_DARNASSUS = 542;
constexpr uint32 TRIGGER_TO_RUTTHERAN = 527;

bool InBox(float x, float y, float minX, float maxX, float minY, float maxY)
{
    return x >= minX && x <= maxX && y >= minY && y <= maxY;
}
}  // namespace

bool Enabled()
{
    static bool const enabled = sConfigMgr->GetOption<bool>("Cortex.Bots.NewRpgLevels", true);
    return enabled;
}

// The same boxes and heights as Cortex's travel regions (mod-cortex CortexTravel::Region).
uint8 Of(uint32 map, float x, float y, float z)
{
    if (z <= INVALID_HEIGHT + 1.0f)
        return LEVEL_OPEN;
    if (map == MAP_KALIMDOR)
    {
        // Teldrassil: nothing stands between z 200 and 1100, Rut'theran Village is at the foot of the tree.
        if (InBox(x, y, 7900.0f, 11500.0f, -500.0f, 3300.0f))
            return z < 400.0f ? LEVEL_RUTTHERAN : LEVEL_TELDRASSIL;
        // Thunder Bluff: the city is above z 95 (the Pools of Vision), the lifts start at 60-70.
        if (InBox(x, y, -1550.0f, -850.0f, -400.0f, 400.0f) && z >= 85.0f)
            return LEVEL_THUNDER_BLUFF;
        return LEVEL_OPEN;
    }
    // Undercity: spawns there are at z 0 and below, the ruins and Tirisfal around at 30 and above.
    if (map == MAP_EASTERN_KINGDOMS && InBox(x, y, 1350.0f, 1900.0f, -50.0f, 550.0f) && z < 10.0f)
        return LEVEL_UNDERCITY;
    return LEVEL_OPEN;
}

bool LiftOnly(Player* bot, uint32 map, float x, float y, float z)
{
    if (!Enabled() || bot->GetMapId() != map || bot->GetTransport())
        return false;
    uint8 there = Of(map, x, y, z);
    if (there != LEVEL_THUNDER_BLUFF && there != LEVEL_UNDERCITY)
        return false;
    return Of(map, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()) != there;
}

uint32 Portal(Player* bot, uint32 map, float x, float y, float z)
{
    if (!Enabled() || bot->GetMapId() != map || bot->GetTransport())
        return 0;
    uint8 there = Of(map, x, y, z);
    uint8 here = Of(map, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    if (here == LEVEL_TELDRASSIL && there == LEVEL_RUTTHERAN)
        return TRIGGER_TO_RUTTHERAN;
    if (here == LEVEL_RUTTHERAN && there == LEVEL_TELDRASSIL)
        return TRIGGER_TO_DARNASSUS;
    return 0;
}
}  // namespace BotLevels
