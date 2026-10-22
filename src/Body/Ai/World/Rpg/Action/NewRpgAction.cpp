/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgAction.h"
#include "AreaDefines.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "Config.h"
#include "DBCStores.h"
#include "GossipDef.h"
#include "IVMapMgr.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "WaypointMovementGenerator.h"
#include "G3D/Vector2.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "BudgetValues.h"
#include "HunterPetFood.h"

// Class quests are never abandoned (NewRpgBaseAction.cpp).
bool KeepsQuest(Quest const* quest);

namespace
{
// Where each quest is turned in: spawn positions of its creature and game object enders, built once. Quest POIs
// only have x / y and the z new rpg gives them comes from the top surface, which for an ender indoors is the roof:
// "Bitter Rivals" (310) is turned in at a barrel inside Thunderbrew Distillery (z 393) under a POI z of 429, and
// 173 of 186 "shelve_reward" in 6 hours were bots waiting on that roof.
struct EnderSpot
{
    uint32 map;
    float x, y, z;
};
std::once_flag enderOnce;
std::unordered_map<uint32, std::vector<EnderSpot>> enderSpots;

void BuildEnderSpots()
{
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        auto bounds = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(data.id);
        for (auto it = bounds.first; it != bounds.second; ++it)
            enderSpots[it->second].push_back({data.mapid, data.posX, data.posY, data.posZ});
    }
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        auto bounds = sObjectMgr->GetGOQuestInvolvedRelationBounds(data.id);
        for (auto it = bounds.first; it != bounds.second; ++it)
            enderSpots[it->second].push_back({data.mapid, data.posX, data.posY, data.posZ});
    }
}

// The quest's ender spawn on `map` within `range` yards (2D) of (x, y), nearest first; false if none.
bool EnderNear(uint32 questId, uint32 map, float x, float y, float range, EnderSpot& out)
{
    std::call_once(enderOnce, BuildEnderSpots);
    auto it = enderSpots.find(questId);
    if (it == enderSpots.end())
        return false;
    float best = range * range;
    bool found = false;
    for (EnderSpot const& spot : it->second)
    {
        float dx = spot.x - x, dy = spot.y - y;
        if (spot.map == map && dx * dx + dy * dy < best)
        {
            best = dx * dx + dy * dy;
            out = spot;
            found = true;
        }
    }
    return found;
}

// Reward POI waits that ended without a turn-in, per bot and quest. The second one abandons the quest: "Bitter
// Rivals" (310) can't be turned in by bots at its barrel at all, and they came back to it every 45 minutes.
std::mutex rewardFailMutex;
std::unordered_map<uint64, uint8> rewardFails;

bool SecondRewardFail(Player* bot, uint32 questId)
{
    std::lock_guard<std::mutex> lock(rewardFailMutex);
    return ++rewardFails[(static_cast<uint64>(bot->GetGUID().GetCounter()) << 32) | questId] >= 2;
}
}  // namespace

void TellRpgStatusAction::WhisperStatusChange(Player* owner, std::string const& statusName)
{
    std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        RPG_STATUS_CHANGED_KEY, RPG_STATUS_CHANGED_DEFAULT,
        {{"%status", statusName}});
    bot->Whisper(msg, LANG_UNIVERSAL, owner);
}

bool TellRpgStatusAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    if (text.empty())
    {
        std::string out = botAI->rpgInfo.ToString();
        bot->Whisper(out.c_str(), LANG_UNIVERSAL, owner);
        return true;
    }

    Player* master = botAI->GetMaster();
    bool isMaster = master && master->GetGUID() == owner->GetGUID();
    bool isGM = owner->GetSession() && owner->GetSession()->GetSecurity() >= SEC_GAMEMASTER;
    if (!isMaster && !isGM)
    {
        std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "rpg_debug_permission_error",
            "Only your master or a GM can change my rpg status.", {});
        bot->Whisper(msg, LANG_UNIVERSAL, owner);
        return false;
    }

    std::string name = text;
    uint32 questId = 0;
    static std::string const doQuestPrefix = "do quest ";
    size_t doQuestPos = text.find(doQuestPrefix);
    if (doQuestPos != std::string::npos)
    {
        name = "do quest";
        std::string idStr = text.substr(doQuestPos + doQuestPrefix.length());
        try
        {
            questId = static_cast<uint32>(std::stoul(idStr));
        }
        catch (std::exception const&)
        {
            questId = 0;
        }
    }

    NewRpgStatus status = NewRpgInfo::StatusFromString(name);
    NewRpgInfo& info = botAI->rpgInfo;

    if (status == RPG_IDLE)
    {
        info.ChangeToIdle();
        WhisperStatusChange(owner, "IDLE");
        return true;
    }
    else if (status == RPG_REST)
    {
        info.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        WhisperStatusChange(owner, "REST");
        return true;
    }
    else if (status == RPG_WANDER_RANDOM)
    {
        info.ChangeToWanderRandom();
        WhisperStatusChange(owner, "WANDER_RANDOM");
        return true;
    }
    else if (status == RPG_WANDER_NPC)
    {
        info.ChangeToWanderNpc();
        WhisperStatusChange(owner, "WANDER_NPC");
        return true;
    }
    else if (status == RPG_GO_GRIND)
    {
        WorldPosition pos = SelectRandomGrindPos(bot);
        if (pos == WorldPosition())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_grind_pos_error", "No grind position available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToGoGrind(pos);
        WhisperStatusChange(owner, "GO_GRIND");
        return true;
    }
    else if (status == RPG_GO_CAMP)
    {
        WorldPosition pos = SelectRandomCampPos(bot);
        if (pos == WorldPosition())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_camp_pos_error", "No camp position available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToGoCamp(pos);
        WhisperStatusChange(owner, "GO_CAMP");
        return true;
    }
    else if (status == RPG_TRAVEL_FLIGHT)
    {
        uint32 flightMasterEntry = 0;
        WorldPosition flightMasterPos;
        std::vector<uint32> path;
        if (!SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_flight_path_error", "No flight path available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, std::move(path));
        WhisperStatusChange(owner, "TRAVEL_FLIGHT");
        return true;
    }
    else if (status == RPG_OUTDOOR_PVP)
    {
        info.ChangeToOutdoorPvp();
        WhisperStatusChange(owner, "OUTDOOR_PVP");
        return true;
    }
    else if (status == RPG_DO_GATHER)
    {
        if (!botAI->HasSkill(SKILL_HERBALISM) && !botAI->HasSkill(SKILL_MINING))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_gather_no_profession_error", "I have neither Herbalism nor Mining, so I can't gather.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToDoGather();
        WhisperStatusChange(owner, "DO_GATHER");
        return true;
    }
    else if (status == RPG_DO_QUEST)
    {
        if (!questId)
        {
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 qid = bot->GetQuestSlotQuestId(slot);
                if (!qid)
                    continue;
                std::vector<POIInfo> poi;
                if (GetQuestPOIPosAndObjectiveIdx(qid, poi, true))
                {
                    questId = qid;
                    break;
                }
            }
        }
        if (!questId)
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_quest_error", "No quest available; use 'do quest <id>'.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatus questStatus = bot->GetQuestStatus(questId);
        if (!quest || (questStatus != QUEST_STATUS_INCOMPLETE && questStatus != QUEST_STATUS_COMPLETE))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_invalid_quest_error", "Invalid quest %quest_id",
                {{"%quest_id", std::to_string(questId)}});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToDoQuest(questId, quest);
        WhisperStatusChange(owner, "DO_QUEST " + std::to_string(questId));
        return true;
    }

    std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "rpg_unknown_status_error",
        "Unknown rpg status. Options: idle, rest, wander random, wander npc, "
        "go grind, go camp, do quest [<id>], travel flight, outdoor pvp, do gather.", {});
    bot->Whisper(msg, LANG_UNIVERSAL, owner);
    return false;
}

bool StartRpgDoQuestAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    PlayerbotChatHandler ch(owner);
    uint32 questId = ch.extractQuestId(text);
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (quest)
    {
        botAI->rpgInfo.ChangeToDoQuest(questId, quest);
        bot->Whisper("Start to do quest " + std::to_string(questId), LANG_UNIVERSAL, owner);
        return true;
    }
    bot->Whisper("Invalid quest " + text, LANG_UNIVERSAL, owner);
    return false;
}

bool NewRpgStatusUpdateAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus status = info.GetStatus();
    switch (status)
    {
        case RPG_IDLE:
            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_DO_QUEST,
                                       RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP, RPG_DO_GATHER});

        case RPG_GO_GRIND:
        {
            auto& data = std::get<NewRpgInfo::GoGrind>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_GRIND -> WANDER_RANDOM
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderRandom();
                return true;
            }
            break;
        }
        case RPG_GO_CAMP:
        {
            auto& data = std::get<NewRpgInfo::GoCamp>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_CAMP -> WANDER_NPC
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderNpc();
                return true;
            }
            break;
        }
        case RPG_WANDER_RANDOM:
        {
            // WANDER_RANDOM -> IDLE
            if (info.HasStatusPersisted(statusWanderRandomDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_WANDER_NPC:
        {
            if (info.HasStatusPersisted(statusWanderNpcDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_QUEST:
        {
            // DO_QUEST -> IDLE
            if (info.HasStatusPersisted(statusDoQuestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            auto& data = std::get<NewRpgInfo::TravelFlight>(info.data);
            if (data.inFlight && !bot->IsInFlight())
            {
                // flight arrival
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_REST:
        {
            // REST -> IDLE
            if (info.HasStatusPersisted(statusRestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_OUTDOOR_PVP:
        {
            if (info.HasStatusPersisted(statusOutDoorPvPDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_GATHER:
        {
            // DO_GATHER -> IDLE
            if (info.HasStatusPersisted(statusDoGatherDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}

bool NewRpgGoGrindAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;
    if (auto* data = std::get_if<NewRpgInfo::GoGrind>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        // Small nudge so the next tick's MoveFarTo starts from a
        // slightly different position. Kept small so it doesn't look
        // like the bot is abandoning its destination.
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgGoCampAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    if (auto* data = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgWanderRandomAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    return MoveRandomNear();
}

bool NewRpgWanderNpcAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::WanderNpc>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    if (!data.npcOrGo)
    {
        // No npc can be found, switch to IDLE
        ObjectGuid npcOrGo = ChooseNpcOrGameObjectToInteract();
        if (npcOrGo.IsEmpty())
        {
            info.ChangeToIdle();
            return true;
        }
        data.npcOrGo = npcOrGo;
        data.lastReach = 0;
        return true;
    }

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, data.npcOrGo);
    if (object && IsWithinInteractionDist(object))
    {
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            if (bot->CanInteractWithQuestGiver(object))
                InteractWithNpcOrGameObjectForQuest(data.npcOrGo);
            return true;
        }

        if (data.lastReach && GetMSTimeDiffToNow(data.lastReach) < npcStayTime)
            return false;

        // has reached the npc for more than `npcStayTime`, select the next target
        data.npcOrGo = ObjectGuid();
        data.lastReach = 0;
    }
    else
    {
        if (MoveWorldObjectTo(data.npcOrGo))
            return true;
        // NPC pathing failed (random offset in a wall, mmap hiccup, etc).
        // Take a small random step so the next tick retries from a
        // different spot instead of staring at the NPC from afar.
        return MoveRandomNear(15.0f);
    }

    return true;
}

bool NewRpgDoQuestAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::DoQuest>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    uint32 questId = data.questId;
    uint8 questStatus = bot->GetQuestStatus(questId);
    switch (questStatus)
    {
        case QUEST_STATUS_INCOMPLETE:
            return DoIncompleteQuest(data);
        case QUEST_STATUS_COMPLETE:
            return DoCompletedQuest(data);
        default:
            break;
    }
    info.ChangeToIdle();
    return true;
}

bool NewRpgDoQuestAction::DoIncompleteQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    if (data.pos != WorldPosition())
    {
        /// @TODO: extract to a new function
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has completed
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);
        bool completed = true;
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] < quest->RequiredNpcOrGoCount[currentObjective])
                completed = false;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] <
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                completed = false;
        }
        // the current objective is completed, clear and find a new objective later
        if (completed)
        {
            data.lastReachPOI = 0;
            data.pos = WorldPosition();
            data.objectiveIdx = 0;
        }
    }
    if (data.pos == WorldPosition())
    {
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo))
        {
            // can't find a poi pos to go, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        uint32 rndIdx = urand(0, poiInfo.size() - 1);
        G3D::Vector2 nearestPoi = poiInfo[rndIdx].pos;
        int32 objectiveIdx = poiInfo[rndIdx].objectiveIdx;

        float dx = nearestPoi.x, dy = nearestPoi.y;

        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = objectiveIdx;
    }

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        // Long-range sampler couldn't land a candidate — nudge the
        // bot a short distance so the next tick retries from a
        // different position instead of sitting idle.
        return MoveRandomNear(10.0f);
    }
    // Now we are near the quest objective
    // kill mobs and looting quest should be done automatically by grind strategy

    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        data.progressAtReach = QuestObjectiveProgress(questId, data.objectiveIdx);
        return true;
    }
    // Quest care checks for progress made since arriving, on a shorter window
    uint32 stayTime = poiStayTime;
    if (sPlayerbotAIConfig.NewRpgQuestCare)
        stayTime = sPlayerbotAIConfig.NewRpgQuestNoProgressSeconds * IN_MILLISECONDS;
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= stayTime)
    {
        bool hasProgression = false;
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has progression
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] != 0 && quest->RequiredNpcOrGoCount[currentObjective])
                hasProgression = true;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] != 0 &&
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                hasProgression = true;
        }
        if (sPlayerbotAIConfig.NewRpgQuestCare)
            hasProgression = QuestObjectiveProgress(questId, currentObjective) > data.progressAtReach;

        if (!hasProgression)
        {
            // we has reach the poi for more than 5 mins but no progession
            // may not be able to complete this quest, marked as abandoned
            ShelveQuest(questId, "shelve_noprog");
            if (sPlayerbotAIConfig.NewRpgQuestCare)
                LeaveQuestToGrind();
            else
                botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        // clear and select another poi later
        data.lastReachPOI = 0;
        data.pos = WorldPosition();
        data.objectiveIdx = 0;
        return true;
    }

    // At the POI: keep the bot actively placed but avoid large
    // random 20yd hops that look like pacing back and forth. A small
    // ~8yd wander reads as the bot looking around while grind/loot
    // strategies do their work.
    return MoveRandomNear(8.0f);
}

bool NewRpgDoQuestAction::DoCompletedQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    Quest const* quest = data.quest;

    if (data.objectiveIdx != -1)
    {
        // if quest is completed, back to poi with -1 idx to reward
        BroadcastHelper::BroadcastQuestUpdateComplete(botAI, bot, quest);
        botAI->rpgStatistic.questCompleted++;
        BotTelemetry::instance().Event(botAI, "quest", "complete", questId);
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
        {
            // can't find a poi pos to reward, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return false;
        }
        assert(poiInfo.size() > 0);
        // now we get the place to get rewarded
        float dx = poiInfo[0].pos.x, dy = poiInfo[0].pos.y;
        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        // Head for the ender itself when one stands near the POI (indoors, its z isn't the top surface's)
        EnderSpot ender;
        if (EnderNear(questId, bot->GetMapId(), dx, dy, 60.0f, ender))
            pos = WorldPosition(ender.map, ender.x, ender.y, ender.z);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = -1;
    }

    if (data.pos == WorldPosition())
        return false;

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    // Now we are near the qoi of reward
    // the quest should be rewarded by SearchQuestGiverAndAcceptOrReward
    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        // e.g. Can not reward quest to gameobjects
        uint16 slot = bot->FindQuestSlot(questId);
        if (SecondRewardFail(bot, questId) && slot < MAX_QUEST_LOG_SIZE &&
            !KeepsQuest(sObjectMgr->GetQuestTemplate(questId)))
        {
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << static_cast<uint8>(slot);
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            botAI->rpgStatistic.questDropped++;
            BotTelemetry::instance().Event(botAI, "quest", "drop_unturnable", questId);
        }
        else
            ShelveQuest(questId, "shelve_reward");
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }
    return false;
}

bool NewRpgTravelFlightAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::TravelFlight>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;
    if (bot->IsInFlight())
    {
        data.inFlight = true;
        ContinueCrossMapTaxi();
        return false;
    }

    if (bot->GetDistance(data.flightMasterPos) > INTERACTION_DISTANCE)
        return MoveFarTo(data.flightMasterPos);

    Creature* flightMaster = bot->FindNearestCreature(data.flightMasterEntry, INTERACTION_DISTANCE * 3);
    if (!flightMaster || !flightMaster->IsAlive())
    {
        info.ChangeToIdle();
        return true;
    }
    if (bot->GetDistance(flightMaster) > INTERACTION_DISTANCE)
        return MoveFarTo(flightMaster);

    std::vector<uint32> nodes = data.path;

    botAI->RemoveShapeshift();
    if (bot->IsMounted())
        bot->Dismount();

    bot->GetSession()->SendLearnNewTaxiNode(flightMaster);

    if (!bot->ActivateTaxiPathTo(nodes, flightMaster, 0))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} active taxi path {} (from {} to {}) failed", bot->GetName(),
                  flightMaster->GetEntry(), nodes.empty() ? 0 : nodes.front(), nodes.empty() ? 0 : nodes.back());
        info.ChangeToIdle();
        return true;
    }
    return true;
}

void NewRpgTravelFlightAction::ContinueCrossMapTaxi()
{
    if (bot->IsBeingTeleported())
        return;

    if (!bot->movespline->Finalized())
        return;

    MotionMaster* mm = bot->GetMotionMaster();
    if (!mm || mm->GetCurrentMovementGeneratorType() != FLIGHT_MOTION_TYPE)
        return;

    // Check if we are at our destination.
    uint32 nextDest = bot->m_taxi.GetTaxiDestination();
    if (!nextDest)
        return;

    // Confirm next node needs different map.
    TaxiNodesEntry const* nextNode = sTaxiNodesStore.LookupEntry(nextDest);
    if (!nextNode || nextNode->map_id == bot->GetMapId())
        return;

    FlightPathMovementGenerator* flight = dynamic_cast<FlightPathMovementGenerator*>(mm->top());
    if (!flight)
        return;

    LOG_DEBUG("playerbots", "[New RPG] {} continuing taxi across map boundary (next node {} on map {})",
              bot->GetName(), nextDest, nextNode->map_id);

    flight->SetCurrentNodeAfterTeleport();

    if (flight->HasArrived())
        return;

    TaxiPathNodeEntry const* node = flight->GetPath()[flight->GetCurrentNode()];
    flight->SkipCurrentNode();

    bot->TeleportTo(nextNode->map_id, node->x, node->y, node->z, bot->GetOrientation(), TELE_TO_NOT_LEAVE_TAXI);
}

namespace
{
// What a recover used (bit 1 food, 2 drink), for its "recover_end" row.
std::mutex recoverUseMutex;
std::unordered_map<ObjectGuid::LowType, uint8> recoverUse;

void NoteRecoverUse(Player* bot, uint8 bits)
{
    std::lock_guard<std::mutex> lock(recoverUseMutex);
    recoverUse[bot->GetGUID().GetCounter()] |= bits;
}

// One row per rest: info why it ended (done, timeout, combat, dead, flight, group) and what it ate or drank (food,
// drink, both, none); a ms sat, b health %, c mana % at the end.
void EndRecover(PlayerbotAI* botAI, Player* bot, NewRpgInfo& info, char const* why, uint32 mana)
{
    if (!info.recoverSinceMs)
        return;
    uint8 used = 0;
    {
        std::lock_guard<std::mutex> lock(recoverUseMutex);
        auto it = recoverUse.find(bot->GetGUID().GetCounter());
        if (it != recoverUse.end())
        {
            used = it->second;
            recoverUse.erase(it);
        }
    }
    static char const* const USED[] = {"none", "food", "drink", "both"};
    BotTelemetry::instance().Event(botAI, "recover_end", std::string(why) + " " + USED[used & 3],
                                   static_cast<int32>(GetMSTimeDiffToNow(info.recoverSinceMs)),
                                   static_cast<int32>(bot->GetHealthPct()), static_cast<int32>(mana));
}
}  // namespace

uint32 NewRpgRecoverAction::ManaPct()
{
    if (bot->getPowerType() != POWER_MANA || !bot->GetMaxPower(POWER_MANA))
        return 100;

    return bot->GetPower(POWER_MANA) * 100 / bot->GetMaxPower(POWER_MANA);
}

bool NewRpgRecoverAction::isUseful()
{
    if (!sPlayerbotAIConfig.NewRpgRecover)
        return false;

    NewRpgInfo& info = botAI->rpgInfo;
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->GetGroup())
    {
        if (info.recoverSinceMs)
            EndRecover(botAI, bot, info,
                       !bot->IsAlive() ? "dead" : (bot->IsInCombat() ? "combat" : (bot->IsInFlight() ? "flight" : "group")),
                       ManaPct());
        info.recoverSinceMs = 0;
        return false;
    }

    uint32 health = static_cast<uint32>(bot->GetHealthPct());
    uint32 mana = ManaPct();

    if (!info.recoverSinceMs)
        return health < sPlayerbotAIConfig.NewRpgRecoverHealthPct || mana < sPlayerbotAIConfig.NewRpgRecoverManaPct;

    bool recovered = health >= RECOVERED_HEALTH_PCT && mana >= RECOVERED_MANA_PCT;
    uint32 maxMs = sPlayerbotAIConfig.NewRpgRecoverMaxSeconds * IN_MILLISECONDS;
    bool tooLong = GetMSTimeDiffToNow(info.recoverSinceMs) >= maxMs;
    if (recovered || tooLong)
    {
        EndRecover(botAI, bot, info, recovered ? "done" : "timeout", mana);
        info.recoverSinceMs = 0;
        bot->SetStandState(UNIT_STAND_STATE_STAND);
        return false;
    }

    return true;
}

bool NewRpgRecoverAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    uint32 health = static_cast<uint32>(bot->GetHealthPct());
    uint32 mana = ManaPct();

    if (!info.recoverSinceMs)
    {
        info.recoverSinceMs = getMSTime() | 1;
        BotTelemetry::instance().Event(botAI, "recover", {}, static_cast<int32>(health), static_cast<int32>(mana));
    }

    // Eating and drinking already sit the bot down and are much faster than resting.
    if (bot->HasAuraType(SPELL_AURA_MOD_REGEN))
        NoteRecoverUse(bot, 1);
    if (bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN))
        NoteRecoverUse(bot, 2);
    if (!bot->HasAuraType(SPELL_AURA_MOD_REGEN) && health < RECOVERED_HEALTH_PCT &&
        botAI->DoSpecificAction("food", Event(), true))
    {
        NoteRecoverUse(bot, 1);
        return true;
    }

    if (!bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN) && mana < RECOVERED_MANA_PCT &&
        botAI->DoSpecificAction("drink", Event(), true))
    {
        NoteRecoverUse(bot, 2);
        return true;
    }

    if (bot->isMoving())
    {
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
    }

    if (bot->getStandState() != UNIT_STAND_STATE_SIT)
        bot->SetStandState(UNIT_STAND_STATE_SIT);

    botAI->SetNextCheckDelay(SIT_CHECK_DELAY_MS);
    return true;
}

namespace
{
// Equipped item entries, to tell whether a gear pass changed anything.
std::array<uint32, EQUIPMENT_SLOT_END> EquippedEntries(Player* bot)
{
    std::array<uint32, EQUIPMENT_SLOT_END> entries{};
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            entries[slot] = item->GetEntry();
    }
    return entries;
}

using EquippedArray = std::array<uint32, EQUIPMENT_SLOT_END>;

int32 ChangedSlots(EquippedArray const& before, EquippedArray const& after)
{
    int32 changed = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (before[slot] != after[slot])
            ++changed;
    }
    return changed;
}
}  // namespace

bool NewRpgGearCheckAction::isUseful()
{
    if (!sPlayerbotAIConfig.NewRpgGearCheckSeconds || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight())
        return false;

    NewRpgInfo& info = botAI->rpgInfo;
    if (info.gearCheckedLevel != bot->GetLevel())
        return true;

    return static_cast<int32>(getMSTime() - info.gearNextCheckMs) >= 0;
}

bool NewRpgGearCheckAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    info.gearCheckedLevel = bot->GetLevel();
    info.gearNextCheckMs = getMSTime() + sPlayerbotAIConfig.NewRpgGearCheckSeconds * IN_MILLISECONDS;

    auto before = EquippedEntries(bot);
    botAI->DoSpecificAction("equip upgrade", Event(), true);
    int32 changed = ChangedSlots(before, EquippedEntries(bot));
    if (changed)
        BotTelemetry::instance().Event(botAI, "gear", "equip", changed);

    if (sPlayerbotAIConfig.NewRpgQuestTidy)
        changed += static_cast<int32>(TidyQuestLog());

    // Never hold the tick: the status update still runs this tick if nothing changed
    return changed > 0;
}

bool NewRpgUpkeepAction::NeedsRepair()
{
    return AI_VALUE(uint8, "durability") < sPlayerbotAIConfig.NewRpgUpkeepDurabilityPct &&
           AI_VALUE(bool, "can repair");
}

bool NewRpgUpkeepAction::NeedsSelling()
{
    return AI_VALUE(uint8, "bag space") >= sPlayerbotAIConfig.NewRpgUpkeepBagPct && AI_VALUE(bool, "can sell");
}

static uint32 MinAmmo() { return sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgUpkeepAmmo", 200); }

static bool SellsAmmo(Creature* vendor, uint32 ammoSubclass, uint8 level)
{
    VendorItemData const* items = vendor->GetVendorItems();
    if (!items)
        return false;

    for (VendorItem const* item : items->m_items)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->item);
        if (proto && proto->Class == ITEM_CLASS_PROJECTILE && proto->SubClass == ammoSubclass &&
            proto->RequiredLevel <= level)
            return true;
    }
    return false;
}

// Ammo subclass a hunter needs to buy (ITEM_SUBCLASS_ARROW / ITEM_SUBCLASS_BULLET), or 0. Fresh-start bots are
// never handed ammo, so without this they run dry and can't shoot.
uint32 NewRpgUpkeepAction::NeedsAmmo()
{
    if (!sPlayerbotAIConfig.NewRpgUpkeepBuy || bot->getClass() != CLASS_HUNTER)
        return 0;

    Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (!ranged)
        return 0;

    uint32 ammoSubclass = 0;
    switch (ranged->GetTemplate()->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_GUN:
            ammoSubclass = ITEM_SUBCLASS_BULLET;
            break;
        case ITEM_SUBCLASS_WEAPON_BOW:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            ammoSubclass = ITEM_SUBCLASS_ARROW;
            break;
        default:
            return 0;
    }

    // Raw money, not "free money for" ammo: that holds back 1g for a guild charter, which a low-level bot never
    // has spare. level² copper covers a stack of the ammo sold at each level (Rough Arrow 10c ... Jagged 10s).
    uint32 level = bot->GetLevel();
    if (AI_VALUE2(uint32, "item count", "ammo") >= MinAmmo() || bot->GetMoney() < level * level)
        return 0;

    return ammoSubclass;
}

// The best ammo of this subclass the bot can use and afford, up to 4 × NewRpgUpkeepAmmo rounds, then put in the
// ammo slot. "buy vendor" won't buy it for a poor bot (see NeedsAmmo).
void NewRpgUpkeepAction::BuyAmmo(Creature* vendor, uint32 ammoSubclass)
{
    VendorItemData const* items = vendor->GetVendorItems();
    if (!items)
        return;

    ItemTemplate const* best = nullptr;
    uint32 bestSlot = 0;
    for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* item = items->GetItem(slot);
        ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
        if (!proto || proto->Class != ITEM_CLASS_PROJECTILE || proto->SubClass != ammoSubclass ||
            proto->RequiredLevel > bot->GetLevel() || proto->BuyPrice > bot->GetMoney())
            continue;

        if (!best || proto->ItemLevel > best->ItemLevel)
        {
            best = proto;
            bestSlot = slot;
        }
    }
    if (!best)
        return;

    uint32 const target = 4 * MinAmmo();
    for (uint32 i = 0; i < 10 && bot->GetItemCount(best->ItemId) < target; ++i)
    {
        if (!bot->BuyItemFromVendorSlot(vendor->GetGUID(), bestSlot, best->ItemId, 1, NULL_BAG, NULL_SLOT))
            break;
    }

    if (bot->HasItemCount(best->ItemId, 1))
        bot->SetAmmo(best->ItemId);
}

// Pet food: a hunter that can feed its pet tops up to PET_FOOD_STOCK pieces it eats at any upkeep vendor once it
// has fewer than PET_FOOD_MIN, from a quarter of its money; it only makes a trip for it below PET_FOOD_LOW.
static constexpr uint32 PET_FOOD_LOW = 3;
static constexpr uint32 PET_FOOD_MIN = 10;
static constexpr uint32 PET_FOOD_STOCK = 20;

static bool WantsPetFood(Player* bot, uint32 below = PET_FOOD_MIN)
{
    if (!sPlayerbotAIConfig.NewRpgUpkeepBuy || bot->getClass() != CLASS_HUNTER ||
        !bot->HasSpell(HunterPetFood::SPELL_FEED_PET))
        return false;
    Pet* pet = HunterPetFood::HunterPet(bot);
    uint32 level = bot->GetLevel();
    return pet && bot->GetMoney() >= 4 * level * level && HunterPetFood::Count(bot, pet) < below;
}

// The vendor's best food for the pet the bot can afford, or -1.
static int32 PetFoodSlot(Player* bot, Pet* pet, Creature* vendor)
{
    VendorItemData const* items = vendor->GetVendorItems();
    int32 best = -1;
    uint32 bestBenefit = 0;
    uint32 bestLevel = 0;
    for (uint32 slot = 0; items && slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* item = items->GetItem(slot);
        ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
        if (!proto || item->ExtendedCost || proto->RequiredLevel > bot->GetLevel() ||
            proto->BuyPrice > bot->GetMoney() / 4)
            continue;
        uint32 benefit = HunterPetFood::Benefit(pet, proto);
        if (benefit > bestBenefit || (benefit && benefit == bestBenefit && proto->ItemLevel < bestLevel))
        {
            best = static_cast<int32>(slot);
            bestBenefit = benefit;
            bestLevel = proto->ItemLevel;
        }
    }
    return best;
}

static void BuyPetFood(PlayerbotAI* botAI, Player* bot, Creature* vendor)
{
    Pet* pet = HunterPetFood::HunterPet(bot);
    if (!pet || !WantsPetFood(bot))
        return;
    int32 slot = PetFoodSlot(bot, pet, vendor);
    if (slot < 0)
        return;
    VendorItem const* item = vendor->GetVendorItems()->GetItem(slot);
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->item);
    uint32 had = HunterPetFood::Count(bot, pet);
    uint64 budget = bot->GetMoney() / 4;
    uint64 spent = 0;
    uint32 stack = std::max<uint32>(1, proto->BuyCount);
    for (uint32 i = 0; i < PET_FOOD_STOCK && HunterPetFood::Count(bot, pet) < PET_FOOD_STOCK &&
                       spent + proto->BuyPrice <= budget;
         i += stack)
    {
        if (!bot->BuyItemFromVendorSlot(vendor->GetGUID(), slot, item->item, 1, NULL_BAG, NULL_SLOT))
            break;
        spent += proto->BuyPrice;
    }
    BotTelemetry::instance().Event(botAI, "upkeep", "pet_food", static_cast<int32>(item->item),
                                   static_cast<int32>(HunterPetFood::Count(bot, pet) - had),
                                   static_cast<int32>(spent));
}

// Water: a mana user keeps a stack (WATER_STOCK) of drinks that suit its level, the best kind it can afford. It tops
// up at any upkeep vendor that sells some once it has fewer, and makes a trip for it below WATER_LOW. Upkeep was only
// ever started by selling, repair or ammo and went to the nearest (repair) vendor, which almost never sells drinks:
// 412 of 594 casters at level 5+ carried none. Water is bought before other optional purchases, from what is free
// for consumables after repair and ammo, and at least a quarter of the bot's money.
static constexpr uint32 WATER_LOW = 5;
static constexpr uint32 WATER_STOCK = 20;
static constexpr uint32 WATER_LEVEL_SPAN = 15;  // a drink 15+ levels below the bot restores too little to count

static bool IsDrink(ItemTemplate const* proto)
{
    return proto && proto->Class == ITEM_CLASS_CONSUMABLE &&
           (proto->SubClass == ITEM_SUBCLASS_CONSUMABLE || proto->SubClass == ITEM_SUBCLASS_FOOD) &&
           proto->Spells[0].SpellCategory == 59;  // drink (11 is food), as ItemUsageValue::GetConsumableType
}

static bool DrinkSuits(Player* bot, ItemTemplate const* proto)
{
    uint32 level = bot->GetLevel();
    return IsDrink(proto) && proto->RequiredLevel <= level && proto->RequiredLevel + WATER_LEVEL_SPAN >= level;
}

// Drinks in the bags that suit the bot's level.
static uint32 DrinkCount(Player* bot)
{
    uint32 count = 0;
    HunterPetFood::ForEachBagItem(bot, [&](Item* item) {
        if (DrinkSuits(bot, item->GetTemplate()))
            count += item->GetCount();
    });
    return count;
}

static uint32 WaterBudget(PlayerbotAI* botAI, Player* bot)
{
    uint32 spare = botAI->GetAiObjectContext()
                       ->GetValue<uint32>("free money for", static_cast<uint32>(NeedMoneyFor::consumables))
                       ->Get();
    return std::max<uint32>(spare, bot->GetMoney() / 4);
}

static bool WantsWater(PlayerbotAI* botAI, Player* bot, uint32 below)
{
    if (!sPlayerbotAIConfig.NewRpgUpkeepBuy || !bot->GetMaxPower(POWER_MANA))
        return false;
    uint32 level = bot->GetLevel();
    return DrinkCount(bot) < below && WaterBudget(botAI, bot) >= 2 * level * level;
}

// The vendor's best drink for the bot's level whose stack fits the budget, else the best one it can buy some of;
// -1 if none.
static int32 DrinkSlot(Player* bot, Creature* vendor, uint32 budget)
{
    VendorItemData const* items = vendor->GetVendorItems();
    int32 best = -1;
    uint32 bestScore = 0;
    for (uint32 slot = 0; items && slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* item = items->GetItem(slot);
        ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
        if (!proto || item->ExtendedCost || !DrinkSuits(bot, proto) || !proto->BuyPrice || proto->BuyPrice > budget ||
            bot->CanUseItem(proto) != EQUIP_ERR_OK)
            continue;
        uint32 lots = (WATER_STOCK + std::max<uint32>(1, proto->BuyCount) - 1) / std::max<uint32>(1, proto->BuyCount);
        uint32 score = (uint64(lots) * proto->BuyPrice <= budget ? 1000 : 0) + proto->ItemLevel + 1;
        if (score > bestScore)
        {
            best = static_cast<int32>(slot);
            bestScore = score;
        }
    }
    return best;
}

static void BuyWater(PlayerbotAI* botAI, Player* bot, Creature* vendor)
{
    if (!WantsWater(botAI, bot, WATER_STOCK))
        return;
    uint32 budget = WaterBudget(botAI, bot);
    int32 slot = DrinkSlot(bot, vendor, budget);
    if (slot < 0)
        return;
    VendorItem const* item = vendor->GetVendorItems()->GetItem(slot);
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->item);
    uint32 had = DrinkCount(bot);
    uint64 spent = 0;
    for (uint32 i = 0; i < WATER_STOCK && DrinkCount(bot) < WATER_STOCK && spent + proto->BuyPrice <= budget; ++i)
    {
        if (!bot->BuyItemFromVendorSlot(vendor->GetGUID(), slot, item->item, 1, NULL_BAG, NULL_SLOT))
            break;
        spent += proto->BuyPrice;
    }
    BotTelemetry::instance().Event(botAI, "upkeep", "water", static_cast<int32>(item->item),
                                   static_cast<int32>(DrinkCount(bot)) - static_cast<int32>(had),
                                   static_cast<int32>(spent));
}

bool NewRpgUpkeepAction::isUseful()
{
    if (!sPlayerbotAIConfig.NewRpgUpkeep || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() ||
        bot->GetGroup())
        return false;

    NewRpgInfo& info = botAI->rpgInfo;
    uint32 now = getMSTime();
    if (static_cast<int32>(now - info.upkeepNextCheckMs) < 0)
        return false;

    if (NeedsSelling() || NeedsRepair() || NeedsAmmo() || WantsPetFood(bot, PET_FOOD_LOW) ||
        WantsWater(botAI, bot, WATER_LOW))
        return true;

    info.upkeepCampTrips = 0;
    info.upkeepNextCheckMs = now + IDLE_CHECK_MS;
    return false;
}

Creature* NewRpgUpkeepAction::FindVendor(bool wantRepair, uint32 ammoSubclass)
{
    Creature* best = nullptr;
    float bestDistance = FLT_MAX;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest npcs"))
    {
        Creature* creature = ObjectAccessor::GetCreature(*bot, guid);
        if (!creature || !creature->IsAlive() || !creature->IsVendor() || creature->IsHostileTo(bot))
            continue;

        if (wantRepair && !creature->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))
            continue;

        if (ammoSubclass && !SellsAmmo(creature, ammoSubclass, bot->GetLevel()))
            continue;

        float distance = bot->GetDistance(creature);
        if (distance < bestDistance)
        {
            best = creature;
            bestDistance = distance;
        }
    }
    return best;
}

WorldPosition NewRpgUpkeepAction::NearestCamp()
{
    static constexpr float MIN_CAMP_DISTANCE = 50.0f;
    WorldPosition best;
    float bestDistance = FLT_MAX;
    for (WorldLocation const& loc : sTravelMgr.GetTravelHubs(bot))
    {
        if (loc.GetMapId() != bot->GetMapId())
            continue;

        float distance = bot->GetExactDist(loc);
        if (distance < MIN_CAMP_DISTANCE || distance >= bestDistance)
            continue;

        // Same zone only: the nearest camp by straight line can be across a mountain range or a sea
        // (Coldridge Valley -> Northshire), which the bot can't walk to.
        if (bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                     loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        best = WorldPosition(loc);
        bestDistance = distance;
    }
    return best;
}

bool NewRpgUpkeepAction::DoBusiness(Creature* vendor, uint32 ammoSubclass)
{
    uint32 moneyBefore = bot->GetMoney();
    auto gearBefore = EquippedEntries(bot);

    bot->SetFacingToObject(vendor);
    // Equip first so upgrades aren't sold, then sell what is left over
    botAI->DoSpecificAction("equip upgrade", Event(), true);
    botAI->DoSpecificAction("sell", Event("upkeep", "gray"), true);
    botAI->DoSpecificAction("sell", Event("upkeep", "vendor"), true);
    if (vendor->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))
        botAI->DoSpecificAction("repair", Event("upkeep"), true);
    if (ammoSubclass)
        BuyAmmo(vendor, ammoSubclass);
    BuyWater(botAI, bot, vendor);  // before the general "buy", which spends on gear and more
    BuyPetFood(botAI, bot, vendor);
    if (sPlayerbotAIConfig.NewRpgUpkeepBuy)
        botAI->DoSpecificAction("buy", Event("upkeep", "vendor"), true);

    int32 moneyDelta = static_cast<int32>(bot->GetMoney()) - static_cast<int32>(moneyBefore);
    BotTelemetry::instance().Event(botAI, "upkeep", "done", moneyDelta, AI_VALUE(uint8, "bag space"),
                                   ChangedSlots(gearBefore, EquippedEntries(bot)));
    return true;
}

bool NewRpgUpkeepAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    uint32 now = getMSTime();
    bool wantRepair = NeedsRepair();
    bool wantSell = NeedsSelling();
    uint32 ammoSubclass = NeedsAmmo();

    bool wantWater = WantsWater(botAI, bot, WATER_LOW);
    uint32 waterBudget = WaterBudget(botAI, bot);

    // The nearest vendor in sight that sells what only some vendors stock (water, pet food)
    auto nearestSelling = [&](auto&& sells) -> Creature*
    {
        Creature* best = nullptr;
        float bestDistance = FLT_MAX;
        for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest npcs"))
        {
            Creature* creature = ObjectAccessor::GetCreature(*bot, guid);
            if (!creature || !creature->IsAlive() || !creature->IsVendor() || creature->IsHostileTo(bot) ||
                bot->GetDistance(creature) >= bestDistance || !sells(creature))
                continue;
            best = creature;
            bestDistance = bot->GetDistance(creature);
        }
        return best;
    };
    auto sellsWater = [&](Creature* creature) { return DrinkSlot(bot, creature, waterBudget) >= 0; };

    Creature* vendor = nullptr;
    // Low on water and no repair or ammo to buy: a vendor with water, which takes the junk as well. Armorers and
    // weaponsmiths (the repair vendors) almost never sell drinks.
    if (wantWater && !wantRepair && !ammoSubclass)
        vendor = nearestSelling(sellsWater);
    if (!vendor && (wantRepair || wantSell || ammoSubclass))
        vendor = FindVendor(wantRepair, ammoSubclass);
    if (!vendor && ammoSubclass && wantRepair)
        vendor = FindVendor(false, ammoSubclass);  // ammo first: a hunter without it barely fights
    if (!vendor && (wantRepair || wantSell))
        vendor = FindVendor(wantRepair, 0);
    if (!vendor && wantRepair && wantSell)
        vendor = FindVendor(false, 0);  // a plain vendor still clears the bags
    if (!vendor && wantWater)
        vendor = nearestSelling(sellsWater);
    Pet* pet = HunterPetFood::HunterPet(bot);
    if (!vendor && pet && WantsPetFood(bot, PET_FOOD_LOW))
        vendor = nearestSelling([&](Creature* creature) { return PetFoodSlot(bot, pet, creature) >= 0; });

    if (vendor)
    {
        if (bot->GetDistance(vendor) > INTERACTION_DISTANCE)
        {
            info.upkeepNextCheckMs = now;  // keep walking toward it every tick
            return MoveWorldObjectTo(vendor->GetGUID(), INTERACTION_DISTANCE / 2) || bot->isMoving();
        }

        if (bot->isMoving())
        {
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
        }

        DoBusiness(vendor, ammoSubclass);
        info.upkeepCampTrips = 0;
        info.upkeepNextCheckMs = now + DONE_COOLDOWN_MS;
        return true;
    }

    // No vendor in sight: head for the nearest camp and look again from there
    if (info.upkeepCampTrips >= MAX_CAMP_TRIPS)
    {
        BotTelemetry::instance().Event(botAI, "upkeep", "give_up", static_cast<int32>(info.upkeepCampTrips));
        info.upkeepCampTrips = 0;
        info.upkeepNextCheckMs = now + GIVE_UP_COOLDOWN_MS;
        return false;
    }

    if (info.GetStatus() != RPG_GO_CAMP)
    {
        WorldPosition camp = NearestCamp();
        if (camp == WorldPosition())
        {
            info.upkeepNextCheckMs = now + GIVE_UP_COOLDOWN_MS;
            return false;
        }

        ++info.upkeepCampTrips;
        info.ChangeToGoCamp(camp);
        BotTelemetry::instance().Event(botAI, "upkeep", "to_camp", static_cast<int32>(bot->GetExactDist(camp)));
    }

    // Let the GO_CAMP walk run; look for a vendor again shortly
    info.upkeepNextCheckMs = now + WALK_CHECK_MS;
    return false;
}
