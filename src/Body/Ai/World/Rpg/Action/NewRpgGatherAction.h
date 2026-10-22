/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGGATHERACTION_H
#define PLAYERBOTS_NEWRPGGATHERACTION_H

#include <ctime>
#include <unordered_map>

#include "NewRpgBaseAction.h"

class GameObject;
class Player;
struct GameObjectTemplate;

// Node detours (Cortex.Bots.Professions.Gather, docs/professions.md): a random bot with herbalism or mining that sees a
// node it can gather (skill high enough for the lock, a pick for ore) within GatherRange yards, reachable and with no
// hostile next to it, walks over, gathers and loots it, as a player leaves the road for a herb. At most one detour per
// GatherCooldown seconds (a node right next to the bot is no detour), and never to a node another bot is heading for.
// The loot strategy's own pickup (nodes within LootDistance) still runs; this only brings the bot within its reach.
// With GatherTracking it also keeps Find Herbs or Find Minerals up: nodes of the tracked kind count as seen out to the
// visibility range without line of sight (the minimap shows them); other nodes need line of sight and GatherRange.
class NewRpgGatherAction : public NewRpgBaseAction
{
public:
    NewRpgGatherAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg gather") {}
    bool isUseful() override;
    bool Execute(Event event) override;

    // The gathering skill a node's lock asks for (herbalism or mining), and in `required` the skill value it needs;
    // 0 for any other object.
    static uint32 NodeSkill(GameObjectTemplate const* info, uint32* required = nullptr);
    // Whether the bot can gather a node of this skill and value: knows the skill, high enough, a pick for ore.
    static bool CanGather(Player* bot, uint32 skill, uint32 required);
    // The gathering skill whose nodes the bot's resource tracking shows (Find Herbs, Find Minerals), or 0.
    static uint32 TrackedSkill(Player* bot);
    // How far nodes of the tracked kind are known: the map's visibility range, at most 100 yards.
    static float TrackRange(Player* bot);

private:
    bool LootBusy();
    uint32 TrackingToCast(time_t now);
    bool CastTracking(time_t now);
    GameObject* FindNode(time_t now, bool& viaTracking);
    bool HostileNear(GameObject* go);
    bool Reachable(GameObject* go, float range);
    void End(char const* failReason);

    ObjectGuid target;
    uint32 entry = 0;
    uint32 skill = 0;
    time_t startedAt = 0;
    time_t progressAt = 0;
    time_t lastStep = 0;
    float bestDist = 0.0f;
    uint8 attempts = 0;
    bool opened = false;      // "open loot" was cast on it
    bool handedOver = false;  // it came within the loot strategy's reach, which may have gathered it
    time_t nextScan = 0;
    time_t nextDetour = 0;
    std::unordered_map<ObjectGuid, time_t> skipped;  // nodes it couldn't reach or open, until when
    uint32 trackSpell = 0;     // tracking spell to cast in Execute
    uint32 trackSkill = 0;     // the skill a bot with both chose to track ...
    time_t trackChosenAt = 0;  // ... and when (re-decided at most every 10 minutes)
    time_t nextTrackTry = 0;   // after a failed cast
};

#endif
