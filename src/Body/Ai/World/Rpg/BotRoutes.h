/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTROUTES_H
#define PLAYERBOTS_BOTROUTES_H

#include "Define.h"

class Player;

// Long walks along the travel node graph (Playerbots.NewRpgRoutes).
//
// mmap answers a far destination with a partial path to the explored poly nearest to it, which is a dead end
// whenever the real way starts by heading off (out of a bowl, a cave, around a ridge). The playerbots_travelnode
// tables hold a walk graph between towns, camps and POIs with precomputed point paths; new rpg's MoveFarTo follows
// a route over it when mmap alone can't reach the destination. Loaded on a background thread at startup; until
// then, and with the setting off, nothing changes.
namespace BotRoutes
{
struct Point
{
    float x, y, z;
};

bool Enabled();
// Once, on the world thread after the world has loaded: read the setting and start loading the graph.
void StartLoading();

// Next point to walk to on the way to (x, y, z) on the bot's map. Builds (or rebuilds) the bot's route when the
// destination changes. False when there's no route: disabled, not loaded, no nodes nearby, or the route is walked
// to its end. `advanced` is set when the bot reached a waypoint since the last call (progress for the stuck check).
bool NextWaypoint(Player* bot, float x, float y, float z, Point& out, bool& advanced);

// The route didn't work out from here (walk unstick): build a new one next time, avoiding the start node it used.
void Reroute(Player* bot);
void Forget(Player* bot);

// What bots learned (death and stuck heat, edge strikes) survives restarts in Playerbots.NewRpgLearnFile: restored
// once the graph is loaded, saved every 10 minutes (UpdateLearned, world thread) and on shutdown (SaveLearned).
void UpdateLearned(uint32 diff);
void SaveLearned();
}  // namespace BotRoutes

#endif
