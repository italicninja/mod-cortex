/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ReviveFromCorpseAction.h"
#include "Corpse.h"
#include "Event.h"
#include "FleeManager.h"
#include "GameGraveyard.h"
#include "MapMgr.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ServerFacade.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "PlayerbotAIConfig.h"
#include "CortexBody.h"
#include "BotTelemetry.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <mutex>
#include <unordered_map>

namespace
{
// Random bots on their own (Cortex.Bots.NewRpgRecover) don't come back to life with a hostile within SAFE_REVIVE_RANGE:
// 42% of Loch Modan deaths came within 2 min of a revive (one bot revived at 49% health with 7 hostiles within
// 25 yd and died 2 s later). They look for a clear spot within reach of the corpse; after SAFE_REVIVE_WAIT seconds
// dead they take the spirit healer instead.
constexpr float SAFE_REVIVE_RANGE = 30.0f;
constexpr int64 SAFE_REVIVE_WAIT = 3 * MINUTE;

bool SafeReviveApplies(Player* bot)
{
    return sPlayerbotAIConfig.NewRpgRecover && !bot->GetGroup() && sRandomPlayerbotMgr.IsRandomBot(bot);
}

// Ghosts that chose the spirit healer for their current death (by corpse ghost time). Without this "find corpse"
// walked them back to the corpse after every step toward the healer (a ghost swung 300 yd for 7+ min).
std::mutex healerMutex;
std::unordered_map<ObjectGuid::LowType, time_t> toHealer;

void ChooseHealer(Player* bot, Corpse* corpse)
{
    std::lock_guard<std::mutex> lock(healerMutex);
    toHealer[bot->GetGUID().GetCounter()] = corpse->GetGhostTime();
}

bool GoingToHealer(Player* bot, Corpse* corpse)
{
    std::lock_guard<std::mutex> lock(healerMutex);
    auto it = toHealer.find(bot->GetGUID().GetCounter());
    if (it == toHealer.end())
        return false;
    if (it->second == corpse->GetGhostTime())
        return true;
    toHealer.erase(it);  // an earlier death
    return false;
}

// Corpse runners (Cortex::RunsToCorpse) look farther: a mob well above them pulls from up to 45 yd. A level 24 revived
// at a Western Plaguelands graveyard with nothing within 25 yd was killed by level 55s 5 s later, 37 times in 21 min.
// For them a hostile counts within its aggro range of the bot plus AGGRO_MARGIN (and never less than
// SAFE_REVIVE_RANGE).
constexpr float AGGRO_SCAN = 60.0f;
constexpr float AGGRO_MARGIN = 8.0f;

bool HostileNear(Player* bot)
{
    bool const corpseRun = Cortex::RunsToCorpse(bot);
    float const scan = corpseRun ? AGGRO_SCAN : SAFE_REVIVE_RANGE;
    std::list<Unit*> units;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, scan);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, units, check);
    Cell::VisitObjects(bot, searcher, scan);
    for (Unit* unit : units)
    {
        if (unit->IsPlayer() || !unit->IsHostileTo(bot))
            continue;
        if (!corpseRun)
            return true;
        Creature* creature = unit->ToCreature();
        float reach = creature ? std::max(SAFE_REVIVE_RANGE, creature->GetAggroRange(bot) + AGGRO_MARGIN)
                               : SAFE_REVIVE_RANGE;
        if (bot->GetExactDist2d(unit) <= reach)
            return true;
    }
    return false;
}

// Corpse runners (Cortex::RunsToCorpse) also wait out enemy players. On a PvP realm both factions' bots revive at
// shared graveyards (Ashenvale 2291,-1731; Stonetalon 900,430) and kill each other there: 60% of player kills of
// corpse runners on 10-20 came within 2 min of the victim's revive. A ghost doesn't come back while an enemy player
// is within CAMP_RANGE, at its corpse or at the healer (the random bot manager's 30 min rescue aside).
constexpr float CAMP_RANGE = 45.0f;

bool EnemyPlayerNear(Player* bot)
{
    std::list<Unit*> units;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, CAMP_RANGE);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, units, check);
    Cell::VisitObjects(bot, searcher, CAMP_RANGE);
    for (Unit* unit : units)
    {
        if (unit->IsPlayer() && unit->IsHostileTo(bot))
            return true;
    }
    return false;
}

std::mutex campMutex;
std::unordered_map<ObjectGuid::LowType, time_t> campNoted;

// True when a corpse runner's corpse or healer is camped; the first time per death it writes "corpse_run" "camped".
bool Camped(PlayerbotAI* botAI, Player* bot, Corpse* corpse)
{
    if (!Cortex::RunsToCorpse(bot) || !EnemyPlayerNear(bot))
        return false;

    {
        std::lock_guard<std::mutex> lock(campMutex);
        time_t& noted = campNoted[bot->GetGUID().GetCounter()];
        if (noted == corpse->GetGhostTime())
            return true;
        noted = corpse->GetGhostTime();
    }
    BotTelemetry::instance().Event(botAI, "corpse_run", "camped",
                                   static_cast<int32>(time(nullptr) - corpse->GetGhostTime()),
                                   static_cast<int32>(bot->GetExactDist2d(corpse)));
    return true;
}

// A corpse runner that died again where it died minutes ago takes the healer: back at the corpse it came to life
// among the same mobs at half health. Without the old "died 5 times" teleport, 49% of corpse runners' revives on
// 10-20 ended in another death within 2 min (one bot 31 times in 20 min at a Blackrock camp in Redridge).
constexpr float REPEAT_RADIUS = 60.0f;
constexpr time_t REPEAT_WINDOW = 10 * MINUTE;
constexpr uint32 REPEAT_DEATHS = 2;

struct PastDeath
{
    uint32 map;
    float x, y;
    time_t ghostTime;
};
std::mutex repeatMutex;
std::unordered_map<ObjectGuid::LowType, std::vector<PastDeath>> pastDeaths;

// Telemetry "corpse_run" healer_wait once a corpse runner has waited HEALER_WAIT at the healer for hostiles to move
// off (from its first wait for this death).
constexpr time_t HEALER_WAIT = 5 * MINUTE;
constexpr float SPIRIT_HEALER_SEARCH = 40.0f;
constexpr float AT_GRAVEYARD = 8.0f;
std::mutex healerWaitMutex;
std::unordered_map<ObjectGuid::LowType, std::pair<time_t, time_t>> healerWait;  // ghost time, waiting since

// True the first time this death's wait at the healer passes HEALER_WAIT.
bool HealerWaitPassed(Player* bot, Corpse* corpse)
{
    std::lock_guard<std::mutex> lock(healerWaitMutex);
    auto& wait = healerWait[bot->GetGUID().GetCounter()];
    time_t now = time(nullptr);
    if (wait.first != corpse->GetGhostTime())
        wait = {corpse->GetGhostTime(), now};
    if (!wait.second || now - wait.second < HEALER_WAIT)
        return false;
    wait.second = 0;
    return true;
}

// True when the corpse runner's current death is its REPEAT_DEATHS-th near here within REPEAT_WINDOW; the first time
// per death it writes "corpse_run" "healer".
bool RepeatDeath(PlayerbotAI* botAI, Player* bot, Corpse* corpse)
{
    if (!Cortex::RunsToCorpse(bot))
        return false;

    uint32 near = 0;
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(repeatMutex);
        std::vector<PastDeath>& list = pastDeaths[bot->GetGUID().GetCounter()];
        time_t now = time(nullptr);
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [now](PastDeath const& d) { return now - d.ghostTime >= REPEAT_WINDOW; }),
                   list.end());
        if (list.empty() || list.back().ghostTime != corpse->GetGhostTime())
        {
            list.push_back({corpse->GetMapId(), corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetGhostTime()});
            first = true;
        }
        for (PastDeath const& d : list)
        {
            if (d.map == corpse->GetMapId() &&
                std::hypot(d.x - corpse->GetPositionX(), d.y - corpse->GetPositionY()) < REPEAT_RADIUS)
                ++near;
        }
    }
    if (near < REPEAT_DEATHS)
        return false;
    if (first)
        BotTelemetry::instance().Event(botAI, "corpse_run", "healer", static_cast<int32>(near),
                                       static_cast<int32>(bot->GetExactDist2d(corpse)));
    return true;
}
}  // namespace

// Top of the default start-zone brackets (Cortex.Bots.ZoneBracket.*).
constexpr uint32 START_ZONE_GRAVEYARD_MAX_LEVEL = 12;

bool ReviveFromCorpseAction::Execute(Event event)
{
    // BG ghosts are revived by spirit healer waves; teleporting to a world graveyard removes them from the BG.
    if (bot->InBattleground())
        return false;

    Player* groupLeader = botAI->GetGroupLeader();
    Corpse* corpse = bot->GetCorpse();

    // follow group Leader when group Leader revives
    WorldPacket& p = event.getPacket();
    if (!p.empty() && p.GetOpcode() == CMSG_RECLAIM_CORPSE && groupLeader && !corpse && bot->IsAlive())
    {
        if (ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
                                              sPlayerbotAIConfig.FarDistance))
        {
            if (!botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
            {
                botAI->TellMasterNoFacing("Welcome back!");
                botAI->ChangeStrategy("+follow,-stay", BOT_STATE_NON_COMBAT);
                return true;
            }
        }
    }

    if (!corpse)
        return false;

    // if (corpse->GetGhostTime() + bot->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP) >
    // time(nullptr))
    //     return false;

    if (groupLeader)
    {
        if (!GET_PLAYERBOT_AI(groupLeader) && groupLeader->isDead() && groupLeader->GetCorpse() &&
            ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
                                              sPlayerbotAIConfig.FarDistance))
            return false;
    }

    // Corpse runners (Cortex::RunsToCorpse) don't take the healer for having died 5 times since their last repair
    if (!botAI->HasGameClientMaster() && !Cortex::RunsToCorpse(bot))
    {
        uint32 dCount = AI_VALUE(uint32, "death count");

        if (dCount >= 5)
            return botAI->DoSpecificAction("spirit healer");
    }

    if (RepeatDeath(botAI, bot, corpse))
    {
        ChooseHealer(bot, corpse);
        return botAI->DoSpecificAction("spirit healer", Event(), true);
    }

    if (Camped(botAI, bot, corpse))
        return false;  // "find corpse" holds the ghost at the corpse

    if (SafeReviveApplies(bot) && (GoingToHealer(bot, corpse) || HostileNear(bot)))
    {
        if (!GoingToHealer(bot, corpse) && time(nullptr) - corpse->GetGhostTime() < SAFE_REVIVE_WAIT)
            return false;  // "find corpse" keeps looking for a clear spot
        ChooseHealer(bot, corpse);
        return botAI->DoSpecificAction("spirit healer", Event(), true);
    }

    LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> revives at body", bot->GetGUID().ToString().c_str(),
              bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str());

    bot->GetMotionMaster()->Clear();
    bot->StopMoving();

    WorldPacket packet(CMSG_RECLAIM_CORPSE);
    packet << bot->GetGUID();
    bot->GetSession()->HandleReclaimCorpseOpcode(packet);

    return true;
}

bool FindCorpseAction::Execute(Event /*event*/)
{
    if (bot->InBattleground())
        return false;

    Player* groupLeader = botAI->GetGroupLeader();
    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
        return false;

    // if (groupLeader)
    // {
    //     if (!GET_PLAYERBOT_AI(groupLeader) &&
    //         ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
    //         sPlayerbotAIConfig.FarDistance)) return false;
    // }

    // Corpse runners walk back however often they died: "died 5 times" is no reason to revive and move them
    bool const corpseRun = Cortex::RunsToCorpse(bot);
    uint32 dCount = corpseRun ? 0 : AI_VALUE(uint32, "death count");

    if (!botAI->HasGameClientMaster())
    {
        if (dCount >= 5)
        {
            // LOG_INFO("playerbots", "Bot {} {}:{} <{}>: died too many times, was revived and teleported",
            //     bot->GetGUID().ToString().c_str(), bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(),
            //     bot->GetName().c_str());
            context->GetValue<uint32>("death count")->Set(0);
            // sRandomPlayerbotMgr.RandomTeleportForLevel(bot);
            sRandomPlayerbotMgr.Revive(bot);
            return true;
        }
    }

    if (RepeatDeath(botAI, bot, corpse))
        ChooseHealer(bot, corpse);

    if ((SafeReviveApplies(bot) || corpseRun) && GoingToHealer(bot, corpse))
        return botAI->DoSpecificAction("spirit healer", Event(), true);

    WorldPosition botPos(bot);
    WorldPosition corpsePos(corpse);
    WorldPosition moveToPos = corpsePos;
    WorldPosition leaderPos(groupLeader);

    float reclaimDist = CORPSE_RECLAIM_RADIUS - 5.0f;
    float corpseDist = botPos.distance(corpsePos);
    int64 deadTime = time(nullptr) - corpse->GetGhostTime();

    bool moveToLeader = groupLeader && groupLeader != bot && leaderPos.fDist(corpsePos) < reclaimDist;

    // Should we ressurect? If so, return false.
    if (corpseDist < reclaimDist)
    {
        // A camped corpse runner waits here as a ghost, neither reviving nor switching to the healer
        if (Camped(botAI, bot, corpse))
        {
            botAI->SetNextCheckDelay(2 * IN_MILLISECONDS);
            return true;
        }

        if (moveToLeader)  // We are near group leader.
        {
            if (botPos.fDist(leaderPos) < sPlayerbotAIConfig.SpellDistance)
                return false;
        }
        else if (deadTime > 8 * MINUTE)  // We have walked too long already.
            return false;
        else
        {
            GuidVector units = AI_VALUE(GuidVector, "possible targets no los");

            bool hostileNear = SafeReviveApplies(bot) && HostileNear(bot);
            if (botPos.getUnitsAggro(units, bot) == 0 && !hostileNear)  // There are no mobs near.
                return false;

            // Still no clear spot after a while: the spirit healer rather than a second death
            if (hostileNear && deadTime >= SAFE_REVIVE_WAIT)
            {
                ChooseHealer(bot, corpse);
                return botAI->DoSpecificAction("spirit healer", Event(), true);
            }
        }
    }

    // If we are getting close move to a save ressurrection spot instead of just the corpse.
    if (corpseDist < sPlayerbotAIConfig.ReactDistance)
    {
        if (moveToLeader)
            moveToPos = leaderPos;
        else
        {
            FleeManager manager(bot, reclaimDist, 0.0, urand(0, 1), moveToPos);

            if (manager.isUseful())
            {
                float rx, ry, rz;
                if (manager.CalculateDestination(&rx, &ry, &rz))
                    moveToPos = WorldPosition(moveToPos.GetMapId(), rx, ry, rz, 0.0);
                else if (!moveToPos.GetReachableRandomPointOnGround(bot, reclaimDist, urand(0, 1)))
                    moveToPos = corpsePos;
            }
        }
    }

    // Actual mobing part.
    bool moved = false;

    if (!botAI->AllowActivity(ALL_ACTIVITY))
    {
        uint32 delay = ServerFacade::instance().GetDistance2d(bot, corpse) /
                       bot->GetSpeed(MOVE_RUN);        // Time a bot would take to travel to it's corpse.
        delay = std::min(delay, uint32(10 * MINUTE));  // Cap time to get to corpse at 10 minutes.

        if (deadTime > delay)
        {
            bot->GetMotionMaster()->Clear();
            bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
            bot->TeleportTo(moveToPos.GetMapId(), moveToPos.GetPositionX(), moveToPos.GetPositionY(), moveToPos.GetPositionZ(), 0);
        }

        moved = true;
    }
    else
    {
        if (bot->isMoving())
            moved = true;
        else
        {
            if (deadTime < 10 * MINUTE && dCount < 5)  // Look for corpse up to 30 minutes.
            {
                moved =
                    MoveTo(moveToPos.GetMapId(), moveToPos.GetPositionX(), moveToPos.GetPositionY(), moveToPos.GetPositionZ(), false, false);
            }

            if (!moved)
            {
                moved = botAI->DoSpecificAction("spirit healer", Event(), true);
            }
        }
    }

    return moved;
}

bool FindCorpseAction::isUseful()
{
    if (bot->InBattleground())
        return false;

    return bot->GetCorpse();
}

GraveyardStruct const* SpiritHealerAction::GetGrave(bool startZone)
{
    GraveyardStruct const* ClosestGrave = nullptr;
    GraveyardStruct const* NewGrave = nullptr;

    ClosestGrave = sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());
    GraveyardStruct const* ownGrave = ClosestGrave;

    if (!startZone && ClosestGrave)
        return ClosestGrave;

    if (botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT) && botAI->GetGroupLeader() && botAI->GetGroupLeader() != bot)
    {
        Player* groupLeader = botAI->GetGroupLeader();
        if (groupLeader && groupLeader != bot)
        {
            ClosestGrave = sGraveyard->GetClosestGraveyard(groupLeader, bot->GetTeamId());

            if (ClosestGrave)
                return ClosestGrave;
        }
    }
    else if (startZone && AI_VALUE(uint8, "durability"))
    {
        TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");

        if (travelTarget->getPosition())
        {
            WorldPosition travelPos = *travelTarget->getPosition();
            if (travelPos.GetMapId() != uint32(-1))
            {
                uint32 areaId = 0;
                uint32 zoneId = 0;
                sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), zoneId, areaId, travelPos.GetMapId(), travelPos.GetPositionX(),
                                          travelPos.GetPositionY(), travelPos.GetPositionZ());
                ClosestGrave = sGraveyard->GetClosestGraveyard(travelPos.GetMapId(), travelPos.GetPositionX(), travelPos.GetPositionY(),
                                                               travelPos.GetPositionZ(), bot->GetTeamId(), areaId, zoneId,
                                                               bot->getClass() == CLASS_DEATH_KNIGHT);

                if (ClosestGrave)
                    return ClosestGrave;
            }
        }
    }

    if (ownGrave && bot->GetLevel() > START_ZONE_GRAVEYARD_MAX_LEVEL)
        return ownGrave;

    std::vector<uint32> races;

    if (bot->GetTeamId() == TEAM_ALLIANCE)
        races = {RACE_HUMAN, RACE_DWARF, RACE_GNOME, RACE_NIGHTELF, RACE_DRAENEI};
    else
        races = {RACE_ORC, RACE_TROLL, RACE_TAUREN, RACE_UNDEAD_PLAYER, RACE_BLOODELF};

    float graveDistance = -1;

    WorldPosition botPos(bot);

    for (auto race : races)
    {
        for (uint32 cls = 0; cls < MAX_CLASSES; cls++)
        {
            PlayerInfo const* info = sObjectMgr->GetPlayerInfo(race, cls);
            if (!info)
                continue;

            uint32 areaId = 0;
            uint32 zoneId = 0;
            sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), zoneId, areaId, info->mapId, info->positionX,
                                      info->positionY, info->positionZ);

            NewGrave = sGraveyard->GetClosestGraveyard(info->mapId, info->positionX, info->positionY, info->positionZ,
                                                       bot->GetTeamId(), areaId, zoneId, cls == CLASS_DEATH_KNIGHT);
            if (!NewGrave)
                continue;

            WorldPosition gravePos(NewGrave->Map, NewGrave->x, NewGrave->y, NewGrave->z);

            float newDist = botPos.fDist(gravePos);

            if (graveDistance < 0 || newDist < graveDistance)
            {
                ClosestGrave = NewGrave;
                graveDistance = newDist;
            }
        }
    }

    return ClosestGrave;
}

bool SpiritHealerAction::Execute(Event /*event*/)
{
    // GetGrave() picks world graveyards; teleporting there from a BG removes the bot from it.
    if (bot->InBattleground())
        return false;

    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
    {
        botAI->TellError("I am not a spirit");
        return false;
    }

    uint32 dCount = AI_VALUE(uint32, "death count");
    int64 deadTime = time(nullptr) - corpse->GetGhostTime();

    // Corpse runners use the nearest graveyard's healer, never one in their start zone
    GraveyardStruct const* ClosestGrave =
        GetGrave(!Cortex::RunsToCorpse(bot) &&
                 (dCount > 10 || deadTime > 15 * MINUTE || AI_VALUE(uint8, "durability") < 10));

    if (ClosestGrave && bot->GetDistance2d(ClosestGrave->x, ClosestGrave->y) < sPlayerbotAIConfig.SightDistance)
    {
        GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
        // Corpse runners also find the healer in the grid, and at the graveyard itself come back without one in the
        // list: "nearest npcs" doesn't always hold it, and a ghost on the graveyard spot then teleported onto that
        // spot every tick (26 ghosts in 19 min stood 30 min at Auberdine, Aldrassil, Kargathia, Charred Vale until
        // the random bot manager's rescue). Before the corpse runs its own 60-300 s revive hid this.
        if (Cortex::RunsToCorpse(bot))
        {
            std::list<Creature*> healers;
            for (uint32 entry : {6491u, 29259u})
                bot->GetCreatureListWithEntryInGrid(healers, entry, SPIRIT_HEALER_SEARCH);
            for (Creature* healer : healers)
                npcs.push_back(healer->GetGUID());
            if (healers.empty() &&
                bot->GetExactDist2d(ClosestGrave->x, ClosestGrave->y) <= AT_GRAVEYARD)
                npcs.push_back(ObjectGuid::Empty);  // at the graveyard: as if the healer were there
        }
        for (GuidVector::iterator i = npcs.begin(); i != npcs.end(); i++)
        {
            Unit* unit = i->IsEmpty() ? nullptr : botAI->GetUnit(*i);
            if ((unit && unit->HasNpcFlag(UNIT_NPC_FLAG_SPIRITHEALER)) || i->IsEmpty())
            {
                if (Camped(botAI, bot, corpse))
                {
                    botAI->SetNextCheckDelay(2 * IN_MILLISECONDS);
                    return true;
                }
                // A corpse runner waits for the mobs round the graveyard to move off, however long: after a 5 min cap
                // it came back beside them and died within seconds, over and over (a level 20 at the Hinterlands
                // graveyard to a level 44 owlbeast every 6 min; 73 bots died 6+ times in one spot in 2 h, 16% of
                // Cortex deaths, on 2026-10-21). One that waits 30 min gets the random bot manager's rescue.
                if (Cortex::RunsToCorpse(bot) && HostileNear(bot))
                {
                    if (HealerWaitPassed(bot, corpse))
                        BotTelemetry::instance().Event(botAI, "corpse_run", "healer_wait",
                                                       static_cast<int32>(time(nullptr) - corpse->GetGhostTime()));
                    botAI->SetNextCheckDelay(2 * IN_MILLISECONDS);
                    return true;
                }

                LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> revives at spirit healer", bot->GetGUID().ToString().c_str(),
                          bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
                PlayerbotChatHandler ch(bot);
                bot->ResurrectPlayer(0.5f);
                bot->SpawnCorpseBones();
                context->GetValue<Unit*>("current target")->Set(nullptr);
                bot->SetTarget();
                botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault("hello", "Hello", {}));

                if (dCount > 20)
                    context->GetValue<uint32>("death count")->Set(0);

                return true;
            }
        }
    }

    if (!ClosestGrave)
    {
        return false;
    }

    bool moved = false;

    if (bot->IsWithinLOS(ClosestGrave->x, ClosestGrave->y, ClosestGrave->z))
        moved = MoveNear(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, 0.0);
    else
        moved = MoveTo(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, false, false);

    if (moved)
        return true;

    // if (!IsRealPlayer(botAI->GetMaster()))
    // {
    context->GetValue<uint32>("death count")->Set(dCount + 1);
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    return bot->TeleportTo(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, 0.f);
    // }

    // LOG_INFO("playerbots", "Bot {} {}:{} <{}> can't find a spirit healer", bot->GetGUID().ToString().c_str(),
    //          bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str());

    // botAI->TellError("Cannot find any spirit healer nearby");
    return false;
}

bool SpiritHealerAction::isUseful() { return bot->HasPlayerFlag(PLAYER_FLAGS_GHOST); }
