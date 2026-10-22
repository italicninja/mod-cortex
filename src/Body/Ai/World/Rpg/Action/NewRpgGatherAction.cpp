/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgGatherAction.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "BotProfessions.h"
#include "BotTelemetry.h"
#include "CellImpl.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "LootObjectStack.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "UpdateFields.h"

namespace
{
    constexpr time_t SCAN_SECONDS = 4;
    constexpr time_t IDLE_SCAN_SECONDS = MINUTE;     // no gathering skill (or no pick) yet, or bags nearly full
    constexpr time_t DETOUR_SECONDS = 2 * MINUTE;     // the whole detour: walk, gather, loot
    constexpr time_t STALL_SECONDS = 20;              // not 3 yd closer in this long: unreachable after all
    constexpr float STALL_PROGRESS = 3.0f;
    constexpr time_t SKIP_SECONDS = 15 * MINUTE;      // a node the bot couldn't reach or open
    constexpr time_t CLAIM_SECONDS = 150;
    constexpr float NEAR_RANGE = 20.0f;               // a node this close is no detour: the cooldown doesn't apply
    constexpr float HOSTILE_RANGE = 15.0f;            // a hostile this close to a node keeps the bot away from it
    constexpr uint8 HOSTILE_LEVELS_BELOW = 4;         // ... unless it is this many levels below the bot
    constexpr float MAX_HEIGHT_DIFF = 20.0f;
    constexpr float PATH_SLACK = 1.6f;                // the path may be this much longer than GatherRange
    constexpr float PATH_END_RANGE = 5.0f;            // and must end this close to the node
    constexpr uint32 PATH_CHECKS = 3;                 // path tests per scan, nearest nodes first
    constexpr uint8 MAX_ATTEMPTS = 4;
    constexpr uint8 MAX_BAG_USE = 80;                 // percent, the loot strategy's limit for gathering
    constexpr float OPEN_RANGE = INTERACTION_DISTANCE - 2.0f;  // OpenLootAction's reach
    constexpr size_t CLAIMS_PRUNE_SIZE = 4096;
    constexpr uint32 LOS_CHECKS = 6;                  // line of sight tests per scan, for nodes the bot doesn't track
    // Resource tracking. Both spells come with the skill (SkillLineAbility, learned on skill learn) and are added
    // again at every login from the skill, not stored in character_spell. Their aura's misc value is the lock type.
    constexpr uint32 SPELL_FIND_HERBS = 2383;
    constexpr uint32 SPELL_FIND_MINERALS = 2580;
    constexpr float MAX_TRACK_RANGE = 100.0f;
    constexpr time_t TRACK_CHOICE_SECONDS = 10 * MINUTE;  // a bot with both re-decides what to track this often
    constexpr time_t TRACK_RETRY_SECONDS = MINUTE;        // after a failed cast

    // Nodes bots are on their way to, so two bots don't race for one. Map threads: locked.
    std::mutex sClaimMutex;
    std::unordered_map<ObjectGuid, std::pair<ObjectGuid, time_t>> sClaims;

    time_t Now() { return GameTime::GetGameTime().count(); }

    bool ClaimedByOther(ObjectGuid node, ObjectGuid bot, time_t now)
    {
        std::lock_guard<std::mutex> lock(sClaimMutex);
        auto it = sClaims.find(node);
        return it != sClaims.end() && it->second.first != bot && it->second.second > now;
    }

    bool Claim(ObjectGuid node, ObjectGuid bot, time_t now)
    {
        std::lock_guard<std::mutex> lock(sClaimMutex);
        if (sClaims.size() > CLAIMS_PRUNE_SIZE)
            std::erase_if(sClaims, [now](auto const& claim) { return claim.second.second <= now; });
        auto it = sClaims.find(node);
        if (it != sClaims.end() && it->second.first != bot && it->second.second > now)
            return false;
        sClaims[node] = std::make_pair(bot, now + CLAIM_SECONDS);
        return true;
    }

    void Release(ObjectGuid node, ObjectGuid bot)
    {
        std::lock_guard<std::mutex> lock(sClaimMutex);
        auto it = sClaims.find(node);
        if (it != sClaims.end() && it->second.first == bot)
            sClaims.erase(it);
    }
}

uint32 NewRpgGatherAction::NodeSkill(GameObjectTemplate const* info, uint32* required)
{
    // Quest-loot objects behind a herb or mining lock (Hyacinth Mushroom) are quest items, not nodes.
    if (!info || info->type != GAMEOBJECT_TYPE_CHEST || info->IsGameObjectForQuests())
        return 0;
    LockEntry const* lock = sLockStore.LookupEntry(info->GetLockId());
    if (!lock)
        return 0;
    for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
    {
        if (lock->Type[i] != LOCK_KEY_SKILL ||
            (lock->Index[i] != LOCKTYPE_HERBALISM && lock->Index[i] != LOCKTYPE_MINING))
            continue;
        if (required)
            *required = std::max(1u, lock->Skill[i]);
        return static_cast<uint32>(SkillByLockType(LockType(lock->Index[i])));
    }
    return 0;
}

bool NewRpgGatherAction::CanGather(Player* bot, uint32 skill, uint32 required)
{
    if (!skill || !bot->HasSkill(skill) || bot->GetSkillValue(skill) < required)
        return false;
    return skill != SKILL_MINING || bot->HasItemTotemCategory(TC_MINING_PICK);
}

// Set by the SPELL_AURA_TRACK_RESOURCES handler: bit (misc value - 1), the misc value being the lock type the
// minimap shows (Find Herbs LOCKTYPE_HERBALISM, Find Minerals LOCKTYPE_MINING). The client reads the same field.
uint32 NewRpgGatherAction::TrackedSkill(Player* bot)
{
    if (!BotProfessions::GatherTracking())
        return 0;
    uint32 tracked = bot->GetUInt32Value(PLAYER_TRACK_RESOURCES);
    if (tracked & (1u << (LOCKTYPE_HERBALISM - 1)))
        return SKILL_HERBALISM;
    if (tracked & (1u << (LOCKTYPE_MINING - 1)))
        return SKILL_MINING;
    return 0;
}

// The server sends every object within the map's visibility range; tracking shows the nodes among them.
float NewRpgGatherAction::TrackRange(Player* bot)
{
    return std::min(bot->GetMap()->GetVisibilityRange(), MAX_TRACK_RANGE);
}

// Looting already under way, or the loot strategy has something in its own (short) reach, the node included.
bool NewRpgGatherAction::LootBusy()
{
    return bot->GetLootGUID() || AI_VALUE(LootObjectStack*, "available loot")->IsLootPending() ||
           AI_VALUE(bool, "can loot") || AI_VALUE(bool, "has available loot");
}

bool NewRpgGatherAction::isUseful()
{
    if (!BotProfessions::Gathering() || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() ||
        bot->GetMap()->Instanceable() || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    if (trackSpell)
        return true;

    time_t now = Now();
    if (target)
    {
        if (now - startedAt >= DETOUR_SECONDS)
        {
            End("timeout");
            return false;
        }
        if (LootBusy())
        {
            handedOver = true;
            return false;
        }
        return true;
    }

    if (now < nextScan || LootBusy())
        return false;
    nextScan = now + SCAN_SECONDS;

    trackSpell = TrackingToCast(now);
    if (trackSpell)
        return true;

    if ((!CanGather(bot, SKILL_HERBALISM, 1) && !CanGather(bot, SKILL_MINING, 1)) ||
        AI_VALUE(uint8, "bag space") > MAX_BAG_USE)
    {
        nextScan = now + IDLE_SCAN_SECONDS;
        return false;
    }

    bool viaTracking = false;
    GameObject* node = FindNode(now, viaTracking);
    if (!node || !Claim(node->GetGUID(), bot->GetGUID(), now))
        return false;

    target = node->GetGUID();
    entry = node->GetEntry();
    skill = NodeSkill(node->GetGOInfo());
    startedAt = now;
    progressAt = now;
    bestDist = bot->GetDistance(node);
    attempts = 0;
    opened = false;
    handedOver = false;
    BotTelemetry::instance().Event(botAI, "gather", viaTracking ? "gather_detour_track" : "gather_detour",
                                   static_cast<int32>(entry), static_cast<int32>(skill),
                                   static_cast<int32>(bot->GetExactDist(node)));
    return true;
}

bool NewRpgGatherAction::Execute(Event /*event*/)
{
    if (trackSpell)
        return CastTracking(Now());
    if (!target)
        return false;

    time_t now = Now();
    GameObject* go = botAI->GetGameObject(target);
    if (!go || !go->isSpawned() || go->GetGoState() != GO_STATE_READY || go->getLootState() != GO_READY)
    {
        // Gone: gathered by this bot (its loot is counted as gather_done when it arrives) or by someone else first.
        End(opened || handedOver ? nullptr : "taken");
        return false;
    }

    // Time spent fighting or with the loot strategy doesn't count as stalling.
    if (now - lastStep > SCAN_SECONDS)
        progressAt = now;
    lastStep = now;

    float dist = bot->GetDistance(go);
    if (dist > OPEN_RANGE)
    {
        if (dist + STALL_PROGRESS < bestDist)
        {
            bestDist = dist;
            progressAt = now;
        }
        else if (now - progressAt >= STALL_SECONDS)
        {
            skipped[target] = now + SKIP_SECONDS;
            End("unreach");
            return false;
        }
        return MoveWorldObjectTo(target, 1.0f) || bot->isMoving();
    }

    // Holds the bot still while the gathering cast runs.
    if (bot->IsNonMeleeSpellCast(false))
        return true;
    if (bot->isMoving())
    {
        bot->StopMoving();
        return true;
    }
    if (bot->IsMounted())
    {
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        return true;
    }
    if (!AI_VALUE(LootObjectStack*, "available loot")->CanAttemptLoot(target))
        return false;

    LootObject loot(bot, target);
    if (attempts >= MAX_ATTEMPTS || loot.IsEmpty() || !loot.IsLootPossible(bot))
    {
        skipped[target] = now + SKIP_SECONDS;
        End("cant");
        return false;
    }
    ++attempts;
    context->GetValue<LootObject>("loot target")->Set(loot);
    if (!botAI->DoSpecificAction("open loot", Event(), true))
        return false;
    opened = true;
    return true;
}

// The tracking spell to cast now, or 0: the one for the bot's gathering skill when no tracking is up. A bot with both
// tracks minerals when it has a pick and its mining is the lower skill, else herbs, and re-decides at most every
// TRACK_CHOICE_SECONDS. Find Herbs and Find Minerals can be cast while mounted (SPELL_ATTR0_ALLOW_WHILE_MOUNTED) and
// are instant (PlayerbotAI::CastSpell refuses while flying). Only one tracking aura can be up (SPELL_SPECIFIC_TRACKER),
// so another one (a hunter's creature tracking, which its strategy keeps up; Find Fish; Find Treasure) is left alone.
uint32 NewRpgGatherAction::TrackingToCast(time_t now)
{
    if (!BotProfessions::GatherTracking() || now < nextTrackTry || bot->IsFlying())
        return 0;
    bool herbalist = bot->HasSkill(SKILL_HERBALISM);
    bool miner = bot->HasSkill(SKILL_MINING);
    if (!herbalist && !miner)
        return 0;

    uint32 tracked = TrackedSkill(bot);
    if (!tracked && (bot->HasAuraType(SPELL_AURA_TRACK_RESOURCES) || bot->HasAuraType(SPELL_AURA_TRACK_CREATURES) ||
                     bot->HasAuraType(SPELL_AURA_TRACK_STEALTHED)))
        return 0;

    uint32 wanted = herbalist ? SKILL_HERBALISM : SKILL_MINING;
    if (herbalist && miner)
    {
        if (!trackSkill || now - trackChosenAt >= TRACK_CHOICE_SECONDS)
        {
            trackSkill = bot->HasItemTotemCategory(TC_MINING_PICK) &&
                                 bot->GetSkillValue(SKILL_MINING) < bot->GetSkillValue(SKILL_HERBALISM)
                             ? SKILL_MINING
                             : SKILL_HERBALISM;
            trackChosenAt = now;
        }
        wanted = trackSkill;
    }
    if (tracked == wanted)
        return 0;
    return wanted == SKILL_HERBALISM ? SPELL_FIND_HERBS : SPELL_FIND_MINERALS;
}

bool NewRpgGatherAction::CastTracking(time_t now)
{
    uint32 spell = trackSpell;
    trackSpell = 0;
    uint32 spellSkill = spell == SPELL_FIND_HERBS ? SKILL_HERBALISM : SKILL_MINING;
    if (bot->IsNonMeleeSpellCast(false))
    {
        nextTrackTry = now + SCAN_SECONDS;
        return false;
    }

    // Granted with Apprentice herbalism or mining. A bot whose skill was set without the skill-learn path (and so
    // without its rewarded spells) learns it the same way, as a temporary skill spell like the core adds at login.
    bool learned = false;
    if (!bot->HasSpell(spell))
    {
        bot->learnSpell(spell, true, true);
        if (!bot->HasSpell(spell))
        {
            nextTrackTry = now + TRACK_RETRY_SECONDS;
            return false;
        }
        learned = true;
    }

    if (!botAI->CastSpell(spell, bot))
    {
        nextTrackTry = now + TRACK_RETRY_SECONDS;
        return false;
    }
    BotTelemetry::instance().Event(botAI, "gather", "track_on", static_cast<int32>(spell),
                                   static_cast<int32>(spellSkill), learned ? 1 : 0);
    return true;
}

void NewRpgGatherAction::End(char const* failReason)
{
    time_t now = Now();
    if (failReason)
    {
        BotTelemetry::instance().Event(botAI, "gather", std::string("gather_fail_") + failReason,
                                       static_cast<int32>(entry), static_cast<int32>(skill),
                                       static_cast<int32>(now - startedAt));
        if (AI_VALUE(LootObject, "loot target").guid == target)
            context->GetValue<LootObject>("loot target")->Set(LootObject());
    }
    Release(target, bot->GetGUID());
    target.Clear();
    entry = 0;
    skill = 0;
    attempts = 0;
    opened = false;
    handedOver = false;
    nextDetour = now + BotProfessions::GatherCooldown();
}

// The nearest node the bot knows of (in NEAR_RANGE while the cooldown runs) it can gather and walk to, that no other
// bot is heading for and no hostile stands next to. A node of the kind its tracking shows is known out to TrackRange
// without line of sight, as on the minimap; any other node only within GatherRange and in line of sight.
// viaTracking: the node was known only through tracking (out of sight, or past GatherRange).
GameObject* NewRpgGatherAction::FindNode(time_t now, bool& viaTracking)
{
    viaTracking = false;
    std::erase_if(skipped, [now](auto const& node) { return node.second <= now; });

    float range = BotProfessions::GatherRange();
    bool detourDue = now >= nextDetour;
    float sightRange = detourDue ? range : std::min(range, NEAR_RANGE);
    uint32 tracked = TrackedSkill(bot);
    float trackRange = !tracked ? 0.0f : detourDue ? std::max(range, TrackRange(bot)) : sightRange;
    float searchRange = std::max(sightRange, trackRange);
    std::list<GameObject*> objects;
    Acore::GameObjectInRangeCheck check(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), searchRange);
    Acore::GameObjectListSearcher<Acore::GameObjectInRangeCheck> searcher(bot, objects, check);
    Cell::VisitObjects(bot, searcher, searchRange);

    std::vector<std::tuple<float, GameObject*, bool>> candidates;
    for (GameObject* go : objects)
    {
        if (!go->isSpawned() || go->GetGoState() != GO_STATE_READY || go->getLootState() != GO_READY ||
            go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_IN_USE | GO_FLAG_NOT_SELECTABLE))
            continue;
        // Within LootDistance the loot strategy's own gathering pickup has it.
        float dist = bot->GetExactDist(go);
        if (dist <= sPlayerbotAIConfig.LootDistance)
            continue;
        uint32 required = 0;
        uint32 nodeSkill = NodeSkill(go->GetGOInfo(), &required);
        if (!nodeSkill || !CanGather(bot, nodeSkill, required) || skipped.count(go->GetGUID()) ||
            sPlayerbotAIConfig.DisallowedGameObjects.contains(go->GetEntry()) ||
            std::fabs(go->GetPositionZ() - bot->GetPositionZ()) > MAX_HEIGHT_DIFF ||
            ClaimedByOther(go->GetGUID(), bot->GetGUID(), now))
            continue;
        bool isTracked = nodeSkill == tracked;
        if (dist > (isTracked ? trackRange : sightRange))
            continue;
        candidates.emplace_back(dist, go, isTracked);
    }
    if (candidates.empty())
        return nullptr;
    std::sort(candidates.begin(), candidates.end(),
              [](auto const& a, auto const& b) { return std::get<0>(a) < std::get<0>(b); });

    uint32 pathChecks = 0;
    uint32 losChecks = 0;
    for (auto const& [dist, go, isTracked] : candidates)
    {
        if (HostileNear(go))
            continue;
        if (!isTracked)
        {
            if (losChecks >= LOS_CHECKS)
                continue;
            ++losChecks;
            if (!bot->IsWithinLOSInMap(go))
                continue;
        }
        if (pathChecks++ >= PATH_CHECKS)
            break;
        if (Reachable(go, isTracked ? std::max(range, trackRange) : range))
        {
            viaTracking = isTracked && (dist > range || !bot->IsWithinLOSInMap(go));
            return go;
        }
        skipped[go->GetGUID()] = now + SKIP_SECONDS;
    }
    return nullptr;
}

bool NewRpgGatherAction::HostileNear(GameObject* go)
{
    uint32 level = bot->GetLevel();
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "all targets"))
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || !unit->IsHostileTo(bot) ||
            uint32(unit->GetLevel()) + HOSTILE_LEVELS_BELOW < level)
            continue;
        if (unit->GetExactDist(go) <= HOSTILE_RANGE)
            return true;
    }
    return false;
}

// A walkable path that ends at the node and isn't much longer than the detour range (not a node up a cliff).
bool NewRpgGatherAction::Reachable(GameObject* go, float range)
{
    PathGenerator path(bot);
    path.CalculatePath(go->GetPositionX(), go->GetPositionY(), go->GetPositionZ());
    PathType type = path.GetPathType();
    if (type & PATHFIND_NOT_USING_PATH)
        return true;
    if (type & (PATHFIND_NOPATH | PATHFIND_SHORT))
        return false;
    G3D::Vector3 const& end = path.GetActualEndPosition();
    return go->GetExactDist(end.x, end.y, end.z) <= PATH_END_RANGE &&
           path.getPathLength() <= range * PATH_SLACK;
}
