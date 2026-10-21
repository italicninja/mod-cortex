/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 * Originally written as part of the wowproject patches to mod-playerbots.
 */

#include "Cortex.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "BotTelemetry.h"
#include "CortexEscape.h"
#include "CortexMemory.h"
#include "CortexNeeds.h"
#include "CortexQuests.h"
#include "CortexTravel.h"
#include "CellImpl.h"
#include "Config.h"
#include "Creature.h"
#include "Engine.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "AiFactory.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "ItemUsageValue.h"
#include "Bag.h"
#include "AiObjectContext.h"
#include "LastMovementValue.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "NewRpgGatherAction.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotsDatabase.h"
#include "QueryResult.h"
#include "QuestDef.h"
#include "Field.h"
#include "RandomPlayerbotMgr.h"
#include "Timer.h"

namespace
{
    bool sEnabled = false;
    bool sActing = false;
    uint32 sRatioPermille = 0;
    bool sPerception = true;
    uint32 sPerceiveMs = 2000;
    bool sWatchdog = true;
    bool sEscape = true;
    uint32 sCorpseRun = 1;  // 0 off, 1 the arm, 2 both arms
    bool sRegroup = true;
    bool sWand = true;
    bool sTidyBags = true;
    bool sTalents = true;
    constexpr uint32 TALENT_CHECK_MS = MINUTE * IN_MILLISECONDS;
    constexpr uint32 TIDY_MS = 5 * MINUTE * IN_MILLISECONDS;
    float sSightRange = 100.0f;
    uint32 sSaveMinutes = 5;
    bool sTaming = true;
    bool sGathering = true;
    float sGatherTripRange = 400.0f;
    bool sSparkles = true;

    // Closer than this a unit counts as seen without a line-of-sight test; past it, at most MAX_LOS_CHECKS
    // tests per scan (nearest first), since each one is a vmap ray cast.
    constexpr float NO_LOS_TEST_RANGE = 30.0f;
    constexpr uint32 MAX_LOS_CHECKS = 16;
    // Remembered nodes checked for being gone take their own few line-of-sight tests per scan.
    constexpr uint32 MAX_NODE_LOS_CHECKS = 4;
    // Quest sparkles get their own tests: a busy camp spends the unit budget before the objects are looked at.
    constexpr uint32 MAX_QUEST_OBJECT_LOS_CHECKS = 4;
    constexpr uint32 MARKER_REFRESH_SECONDS = 300;
    constexpr uint32 STATS_INTERVAL_MS = 5 * MINUTE * IN_MILLISECONDS;
    constexpr float LEARN_FLIGHT_RANGE = 50.0f;  // a flight master passed this close is worth a word

    struct LastFight
    {
        uint32 entry = 0;
        uint8 attackers = 0;
        int8 levelDiff = 0;
    };

    struct CortexState
    {
        bool inArm = false;
        bool loadRequested = false;
        std::atomic<bool> loaded{false};
        CortexMemory memory;
        CortexPlan plan;
        uint32 nextPerceiveMs = 0;
        uint32 nextSaveMs = 0;
        uint32 nextTidyMs = 0;
        uint32 nextTalentMs = 0;
        bool respecced = false;
        bool seenAlive = false;
        bool dead = false;
        uint8 level = 0;
        LastFight fight;

        // Watchdog (Cortex.Watchdog): where the bot has stood since when, and the last action its non-combat
        // engine ran (an ActionExecutionListener writes it, on the bot's map thread).
        uint32 watchMap = 0;
        float watchX = 0.0f;
        float watchY = 0.0f;
        uint32 watchSinceMs = 0;
        uint32 nextWatchMs = 0;
        uint32 nextFrozenMs = 0;
        uint32 nextIdleMs = 0;
        uint32 nextEscapeMs = 0;
        float escapeX = 0.0f;  // where the last escape started from
        float escapeY = 0.0f;
        Engine* watchedEngine = nullptr;
        char lastAction[32] = {};
        uint32 lastActionMs = 0;
        bool lastActionErrand = false;

        // Time away from walking, summed on every AI update (Cortex::AwayMs / ErrandMs): fighting, dead, in flight
        // or recovering; and on an errand (trainer, vendor, auction, crafting, gathering, taming).
        uint32 lastTickMs = 0;
        uint32 awayMs = 0;
        uint32 errandMs = 0;
    };

    struct LoadRequest
    {
        uint32 guid = 0;
        uint8 level = 0;
    };

    // States are created on the bot's map thread and never erased, so pointers stay valid; only the map
    // lookup is locked. A bot's memory is touched by its own map thread, and by the world thread only
    // before `loaded` is set.
    std::mutex sStatesMutex;
    std::unordered_map<uint32, std::unique_ptr<CortexState>> sStates;

    std::mutex sLoadMutex;
    std::vector<LoadRequest> sLoadRequests;

    std::atomic<uint64> sScans{0};
    std::atomic<uint64> sScanMicros{0};
    std::atomic<uint64> sScanUnits{0};
    std::atomic<uint32> sScanMaxMicros{0};
    uint32 sStatsTimer = 0;

    bool ComputeArm(Player const* bot)
    {
        if (!sRandomPlayerbotMgr.IsRandomBot(const_cast<Player*>(bot)))
            return false;

        // Multiplicative hash so neighbouring GUIDs (bots created together) land in both arms.
        uint32 bucket = ((bot->GetGUID().GetCounter() * 2654435761u) >> 8) % 1000;
        return bucket < sRatioPermille;
    }

    CortexState* GetState(Player const* bot, bool create)
    {
        uint32 guid = bot->GetGUID().GetCounter();
        std::lock_guard<std::mutex> lock(sStatesMutex);
        auto it = sStates.find(guid);
        if (it != sStates.end())
            return it->second.get();
        if (!create)
            return nullptr;

        auto state = std::make_unique<CortexState>();
        state->inArm = ComputeArm(bot);
        CortexState* raw = state.get();
        sStates.emplace(guid, std::move(state));
        return raw;
    }

    // Records the last action the bot's non-combat engine ran, for the watchdog. Engines delete their listeners.
    class LastActionListener : public ActionExecutionListener
    {
    public:
        explicit LastActionListener(CortexState* state) : _state(state) {}

        bool Before(Action* /*action*/, Event /*event*/) override { return true; }
        bool AllowExecution(Action* /*action*/, Event /*event*/) override { return true; }
        bool OverrideResult(Action* /*action*/, bool executed, Event /*event*/) override { return executed; }
        void After(Action* action, bool executed, Event /*event*/) override
        {
            if (!executed)
                return;
            std::string const& name = action->getName();
            size_t n = std::min(name.size(), sizeof(_state->lastAction) - 1);
            std::memcpy(_state->lastAction, name.data(), n);
            _state->lastAction[n] = '\0';
            _state->lastActionMs = getMSTime();
            _state->lastActionErrand = IsErrand(name);
        }

        // Actions that take the bot off its walk for a while on business of their own.
        static bool IsErrand(std::string const& name)
        {
            static std::string const errands[] = {"cortex needs",   "new rpg trainer", "new rpg upkeep",
                                                  "new rpg auction", "bot craft",       "cortex tame",
                                                  "new rpg gather",  "cortex gather",   "new rpg gear check"};
            for (std::string const& errand : errands)
                if (name == errand)
                    return true;
            return false;
        }

    private:
        CortexState* _state;
    };

    // Watchdog (Cortex.Watchdog). A bot alive, on the ground and active that hasn't moved WATCH_MOVED in
    // WATCH_FROZEN_MS gets a "frozen" row, and another every WATCH_FROZEN_MS while it lasts:
    //   info  the step's last branch | the last action the engine ran
    //   a     minutes it has stood there, b  distance to its target (-1: none)
    //   c     bits 0-7 path type from the bot to the target (or 20 yd ahead), 8-10 / 11-13 of four probes 30 yd
    //         round the bot: path found / also passes Map::CanReachPositionAndGetValidCoords (MoveRandomNear's
    //         tests), 14 has a target, 15 moving, 16 can't move (CanMove, root, stun), 17 sitting, 18 in water,
    //         19 in combat, 20 trip active, 21 waiting on its last move, 22 plan has a destination, 24-27 rpg status
    // and a "frozen_to" row: a/b/c the target's x/y/z, info the path length, the last move's wait left (s) and the
    // motion generator.
    // A bot that has stood still ESCAPE_AFTER_MS tries to get off a navmesh island (Cortex.Escape, CortexEscape),
    // again every ESCAPE_RETRY_MS while it stays.
    // An errand action that ran this recently is still under way (its moves set the next AI check seconds out).
    constexpr uint32 ERRAND_ACTIVE_MS = 6 * IN_MILLISECONDS;
    constexpr uint32 ESCAPE_AFTER_MS = 3 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 ESCAPE_RETRY_MS = 2 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 WATCH_CHECK_MS = 30 * IN_MILLISECONDS;
    constexpr uint32 WATCH_FROZEN_MS = 10 * MINUTE * IN_MILLISECONDS;
    constexpr float WATCH_MOVED = 10.0f;
    constexpr float WATCH_PROBE = 30.0f;
    // A bot standing at (nearly) full health out of combat gets an "idle" row at most every IDLE_EVERY_MS: what it
    // stands there doing. ~30% of arm samples were idle at full health on 10-20; the samples don't say why.
    //   info  the step's last branch | the last action the engine ran (as "frozen")
    //   a     seconds since it last moved WATCH_MOVED, b  seconds since that action ran, c  bits: 0 sitting, 1 waiting
    //         on its last move, 2 plan has a destination, 3 trip active, 4 mana under 50%
    constexpr uint32 IDLE_EVERY_MS = 2 * MINUTE * IN_MILLISECONDS;

    void NoteIdle(PlayerbotAI* botAI, Player* bot, CortexState& state, uint32 nowMs)
    {
        if (static_cast<int32>(nowMs - state.nextIdleMs) < 0 || !bot->IsAlive() || bot->IsInCombat() ||
            bot->IsInFlight() || bot->isMoving() || !bot->movespline->Finalized() || bot->GetHealthPct() < 95.0f)
            return;
        state.nextIdleMs = nowMs + IDLE_EVERY_MS;

        CortexPlan const& plan = state.plan;
        LastMovement& lastMove = botAI->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get();
        int32 waitLeft = static_cast<int32>(lastMove.msTime + static_cast<uint32>(lastMove.lastdelayTime) - nowMs);
        uint32 flags = 0;
        if (bot->IsSitState())
            flags |= 1u << 0;
        if (waitLeft > 0)
            flags |= 1u << 1;
        if (plan.hasDest)
            flags |= 1u << 2;
        if (plan.trip.active)
            flags |= 1u << 3;
        if (bot->GetMaxPower(POWER_MANA) > 0 && bot->GetPowerPct(POWER_MANA) < 50.0f)
            flags |= 1u << 4;
        std::string info = std::string(plan.step) + (*plan.stepDetail ? ":" : "") + plan.stepDetail + " " +
                           state.lastAction;
        int32 still = state.watchSinceMs ? static_cast<int32>(getMSTimeDiff(state.watchSinceMs, nowMs) / IN_MILLISECONDS)
                                         : 0;
        int32 since = state.lastActionMs ? static_cast<int32>(getMSTimeDiff(state.lastActionMs, nowMs) / IN_MILLISECONDS)
                                         : -1;
        BotTelemetry::instance().Event(botAI, "idle", info, still, since, static_cast<int32>(flags));
    }

    void Watch(PlayerbotAI* botAI, Player* bot, CortexState& state, uint32 nowMs)
    {
        if (static_cast<int32>(nowMs - state.nextWatchMs) < 0)
            return;
        state.nextWatchMs = nowMs + WATCH_CHECK_MS;
        NoteIdle(botAI, bot, state, nowMs);

        if (!state.watchSinceMs || !bot->IsAlive() || bot->IsInFlight() || bot->IsBeingTeleported() ||
            !botAI->AllowActivity() || bot->GetMapId() != state.watchMap ||
            bot->GetExactDist2d(state.watchX, state.watchY) > WATCH_MOVED)
        {
            state.watchMap = bot->GetMapId();
            state.watchX = bot->GetPositionX();
            state.watchY = bot->GetPositionY();
            state.watchSinceMs = nowMs;
            state.nextFrozenMs = nowMs + WATCH_FROZEN_MS;
            state.nextEscapeMs = nowMs + ESCAPE_AFTER_MS;
            return;
        }
        bool escapeDue = sEscape && sActing && static_cast<int32>(nowMs - state.nextEscapeMs) >= 0;
        bool frozenDue = static_cast<int32>(nowMs - state.nextFrozenMs) >= 0;
        if (!escapeDue && !frozenDue)
            return;

        CortexPlan const& plan = state.plan;
        Map* map = bot->GetMap();
        float bx = bot->GetPositionX();
        float by = bot->GetPositionY();
        float bz = bot->GetPositionZ();
        bool hasTarget = false;
        float tx = bx + std::cos(bot->GetOrientation()) * 20.0f;
        float ty = by + std::sin(bot->GetOrientation()) * 20.0f;
        float tz = bz;
        if (plan.stepMap == bot->GetMapId())
        {
            hasTarget = true;
            tx = plan.stepX;
            ty = plan.stepY;
            tz = plan.stepZ;
            if (tz <= INVALID_HEIGHT)
            {
                float ground = map->GetHeight(bot->GetPhaseMask(), tx, ty, bz + 50.0f);
                tz = ground > INVALID_HEIGHT ? ground : bz;
            }
        }
        else if (plan.hasDest && plan.destMap == bot->GetMapId())
        {
            hasTarget = true;
            tx = plan.destX;
            ty = plan.destY;
            tz = plan.destZ;
        }

        if (escapeDue)
        {
            state.nextEscapeMs = nowMs + ESCAPE_RETRY_MS;
            // A walk off that left the bot where it was (something moved it back, or the straight line was blocked):
            // set down this time.
            bool mayWalk = bot->GetExactDist2d(state.escapeX, state.escapeY) > 5.0f;
            if (!plan.trip.active && CortexEscape::Try(botAI, bot, hasTarget, tx, ty, tz, mayWalk))
            {
                state.escapeX = bx;
                state.escapeY = by;
                return;
            }
        }
        if (!frozenDue)
            return;
        state.nextFrozenMs = nowMs + WATCH_FROZEN_MS;

        uint32 const typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
        PathGenerator path(bot);
        path.CalculatePath(tx, ty, tz);
        uint32 flags = static_cast<uint32>(path.GetPathType()) & 0xFF;

        uint32 pathOk = 0;
        uint32 reachOk = 0;
        for (uint32 i = 0; i < 4; ++i)
        {
            float angle = bot->GetOrientation() + static_cast<float>(i) * static_cast<float>(M_PI) / 2.0f;
            float px = bx + std::cos(angle) * WATCH_PROBE;
            float py = by + std::sin(angle) * WATCH_PROBE;
            float pz = bz;
            PathGenerator probe(bot);
            probe.CalculatePath(px, py, pz);
            if (probe.GetPathType() & ~typeOk)
                continue;
            ++pathOk;
            if (map->CanReachPositionAndGetValidCoords(bot, px, py, pz))
                ++reachOk;
        }
        flags |= pathOk << 8;
        flags |= reachOk << 11;

        LastMovement& lastMove = botAI->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get();
        int32 waitLeft = static_cast<int32>(lastMove.msTime + static_cast<uint32>(lastMove.lastdelayTime) - nowMs);
        if (hasTarget)
            flags |= 1u << 14;
        if (bot->isMoving() || !bot->movespline->Finalized())
            flags |= 1u << 15;
        if (!botAI->CanMove() || bot->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED))
            flags |= 1u << 16;
        if (bot->IsSitState())
            flags |= 1u << 17;
        if (bot->IsInWater())
            flags |= 1u << 18;
        if (bot->IsInCombat())
            flags |= 1u << 19;
        if (plan.trip.active)
            flags |= 1u << 20;
        if (waitLeft > 0)
            flags |= 1u << 21;
        if (plan.hasDest)
            flags |= 1u << 22;
        flags |= (static_cast<uint32>(botAI->rpgInfo.GetStatus()) & 0xF) << 24;

        std::string info = std::string(plan.step) + (*plan.stepDetail ? ":" : "") + plan.stepDetail + " " +
                           state.lastAction;
        BotTelemetry::instance().Event(botAI, "frozen", info,
                                       static_cast<int32>(getMSTimeDiff(state.watchSinceMs, nowMs) / MINUTE /
                                                          IN_MILLISECONDS),
                                       hasTarget ? static_cast<int32>(bot->GetExactDist(tx, ty, tz)) : -1,
                                       static_cast<int32>(flags));
        char extra[48];
        std::snprintf(extra, sizeof(extra), "len %d wait %d mm %u", static_cast<int32>(path.getPathLength()),
                      waitLeft / static_cast<int32>(IN_MILLISECONDS),
                      static_cast<uint32>(bot->GetMotionMaster()->GetCurrentMovementGeneratorType()));
        BotTelemetry::instance().Event(botAI, "frozen_to", extra, static_cast<int32>(tx), static_cast<int32>(ty),
                                       static_cast<int32>(tz));
    }

    CortexState* GetLoadedState(Player const* bot)
    {
        if (!sEnabled || !bot)
            return nullptr;
        CortexState* state = GetState(bot, false);
        if (!state || !state->inArm || !state->loaded.load(std::memory_order_acquire))
            return nullptr;
        return state;
    }

    uint32 Now() { return static_cast<uint32>(GameTime::GetGameTime().count()); }

    // A "!" whose every quest the bot has given up on isn't worth the walk, so it isn't remembered as one.
    bool OnlyGivenUpQuests(Player* bot, Creature* creature, CortexMemory const& memory)
    {
        if (!memory.HasGivenUp())
            return false;
        uint8 level = static_cast<uint8>(bot->GetLevel());
        bool givenUp = false;
        auto [begin, end] = sObjectMgr->GetCreatureQuestRelationBounds(creature->GetEntry());
        for (auto it = begin; it != end; ++it)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(it->second);
            if (!quest || bot->GetQuestStatus(quest->GetQuestId()) != QUEST_STATUS_NONE ||
                !bot->CanTakeQuest(quest, false))
                continue;
            if (!memory.GaveUp(quest->GetQuestId(), level))
                return false;
            givenUp = true;
        }
        return givenUp;
    }

    uint32 Services(Creature const* creature)
    {
        uint32 services = 0;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
            services |= CortexMemory::SERVICE_QUESTGIVER;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR_MASK))
            services |= CortexMemory::SERVICE_VENDOR;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR_FOOD))
            services |= CortexMemory::SERVICE_VENDOR | CortexMemory::SERVICE_FOOD;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR_AMMO))
            services |= CortexMemory::SERVICE_AMMO;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR_REAGENT))
            services |= CortexMemory::SERVICE_REAGENT;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))
            services |= CortexMemory::SERVICE_REPAIR;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_TRAINER))
            services |= CortexMemory::SERVICE_TRAINER;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_TRAINER_CLASS))
            services |= CortexMemory::SERVICE_CLASS_TRAINER;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_TRAINER_PROFESSION))
            services |= CortexMemory::SERVICE_PROFESSION_TRAINER;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
            services |= CortexMemory::SERVICE_INNKEEPER;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER))
            services |= CortexMemory::SERVICE_FLIGHTMASTER;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER))
            services |= CortexMemory::SERVICE_AUCTIONEER;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_BANKER))
            services |= CortexMemory::SERVICE_BANKER;
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_SPIRITHEALER))
            services |= CortexMemory::SERVICE_SPIRITHEALER;
        return services;
    }

    // The class a trainer teaches, as a player reads it from the title (<Warrior Trainer>).
    uint8 TrainerClassFromTitle(Creature const* creature)
    {
        static constexpr std::pair<char const*, uint8> CLASS_NAMES[] = {
            {"Warrior", CLASS_WARRIOR}, {"Paladin", CLASS_PALADIN}, {"Hunter", CLASS_HUNTER},
            {"Rogue", CLASS_ROGUE},     {"Priest", CLASS_PRIEST},   {"Shaman", CLASS_SHAMAN},
            {"Mage", CLASS_MAGE},       {"Warlock", CLASS_WARLOCK}, {"Druid", CLASS_DRUID},
        };
        std::string const& title = creature->GetCreatureTemplate()->SubName;
        for (auto const& [name, classId] : CLASS_NAMES)
            if (title.find(name) != std::string::npos)
                return classId;
        return 0;
    }

    // Visible to the bot: not hidden from it, and in line of sight unless close.
    bool Sees(Player* bot, WorldObject* object, uint32& losChecks)
    {
        if (!bot->CanSeeOrDetect(object))
            return false;
        if (bot->GetExactDist(object) <= NO_LOS_TEST_RANGE)
            return true;
        if (losChecks >= MAX_LOS_CHECKS)
            return false;
        ++losChecks;
        return bot->IsWithinLOSInMap(object);
    }

    void Perceive(Player* bot, CortexState& state, uint32 now)
    {
        CortexMemory& memory = state.memory;
        uint32 mapId = bot->GetMapId();
        memory.VisitCell(mapId, bot->GetPositionX(), bot->GetPositionY(), now);

        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck unitCheck(bot, sSightRange);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> unitSearcher(bot, units, unitCheck);
        Cell::VisitObjects(bot, unitSearcher, sSightRange);
        units.sort([bot](Unit* a, Unit* b) { return bot->GetExactDist2dSq(a) < bot->GetExactDist2dSq(b); });

        struct MobGroup
        {
            uint32 entry;
            float x;
            float y;
            uint8 level;
            uint8 count;
            bool elite;
        };
        std::unordered_map<uint64, MobGroup> mobGroups;
        std::unordered_map<uint64, uint32> servicesPerCell;
        uint32 losChecks = 0;

        for (Unit* unit : units)
        {
            Creature* creature = unit->ToCreature();
            if (!creature || creature->IsPet() || creature->IsTotem() || creature->IsGuardian() ||
                creature->IsCritter())
                continue;
            if (!Sees(bot, creature, losChecks))
                continue;

            uint32 services = Services(creature);
            bool enemy = creature->IsHostileTo(bot) || (!creature->IsFriendlyTo(bot) && !services);
            if (enemy)
            {
                uint64 key = (static_cast<uint64>(creature->GetEntry()) << 30) |
                             CortexMemory::CellKey(mapId, creature->GetPositionX(), creature->GetPositionY());
                auto [it, inserted] = mobGroups.try_emplace(
                    key, MobGroup{creature->GetEntry(), creature->GetPositionX(), creature->GetPositionY(),
                                  static_cast<uint8>(creature->GetLevel()), 0, creature->isElite()});
                it->second.count = static_cast<uint8>(std::min<uint32>(it->second.count + 1, 0xFF));
                it->second.level = std::max(it->second.level, static_cast<uint8>(creature->GetLevel()));
                continue;
            }

            // Aboard a ship or zeppelin (Snack-O-Matic IV) it is gone from where it was seen once the ride leaves.
            if (!services || creature->GetTransport())
                continue;

            CortexMemory::Npc npc;
            npc.entry = creature->GetEntry();
            npc.map = static_cast<uint16>(mapId);
            npc.x = creature->GetPositionX();
            npc.y = creature->GetPositionY();
            npc.z = creature->GetPositionZ();
            npc.lastSeen = now;
            npc.services = services;
            if (services & CortexMemory::SERVICE_CLASS_TRAINER)
                npc.trainerClass = TrainerClassFromTitle(creature);
            if (services & CortexMemory::SERVICE_QUESTGIVER)
            {
                npc.marker = static_cast<uint8>(bot->GetQuestDialogStatus(creature));
                if ((npc.marker == DIALOG_STATUS_AVAILABLE || npc.marker == DIALOG_STATUS_AVAILABLE_REP) &&
                    (OnlyGivenUpQuests(bot, creature, memory) ||
                     memory.NothingOffered(npc.entry, static_cast<uint8>(bot->GetLevel()))))
                    npc.marker = DIALOG_STATUS_NONE;
            }
            memory.SeeNpc(npc);

            if (services & CortexMemory::SERVICE_SPIRITHEALER)
                memory.TagCell(mapId, npc.x, npc.y, CortexMemory::CELL_GRAVEYARD, now);
            // A flight master whose flight point the bot doesn't know: the step action walks over and talks to it.
            if ((services & CortexMemory::SERVICE_FLIGHTMASTER) && sActing && CortexTravel::LearnFlights() &&
                !state.plan.learnFlightMaster && !state.plan.ignoredGivers.count(creature->GetGUID()) &&
                bot->GetExactDist(creature) <= LEARN_FLIGHT_RANGE)
            {
                uint32 node = sObjectMgr->GetNearestTaxiNode(npc.x, npc.y, npc.z, mapId, bot->GetTeamId(true));
                if (node && !bot->m_taxi.IsTaximaskNodeKnown(node))
                {
                    state.plan.learnFlightMaster = creature->GetGUID();
                    state.plan.learnSinceMs = getMSTime();
                }
            }
            if (services & ~(CortexMemory::SERVICE_QUESTGIVER | CortexMemory::SERVICE_SPIRITHEALER))
                ++servicesPerCell[CortexMemory::CellKey(mapId, npc.x, npc.y)];
        }

        for (auto const& [key, group] : mobGroups)
            memory.SeeMob(group.entry, mapId, group.x, group.y, group.level, group.count, group.elite, now);

        for (auto const& [cell, count] : servicesPerCell)
            if (count >= 2)
                for (Unit* unit : units)
                    if (CortexMemory::CellKey(mapId, unit->GetPositionX(), unit->GetPositionY()) == cell)
                    {
                        memory.TagCell(mapId, unit->GetPositionX(), unit->GetPositionY(), CortexMemory::CELL_TOWN,
                                       now);
                        break;
                    }

        bool herbalist = sGathering && bot->HasSkill(SKILL_HERBALISM);
        bool miner = sGathering && bot->HasSkill(SKILL_MINING);
        // Nodes of the kind the bot's resource tracking shows are on its minimap: known without line of sight, out
        // to what the server sends it.
        uint32 trackedSkill = (herbalist || miner) ? NewRpgGatherAction::TrackedSkill(bot) : 0;
        float trackRange = trackedSkill ? NewRpgGatherAction::TrackRange(bot) : 0.0f;
        std::vector<uint64> upNodes;
        std::list<GameObject*> objects;
        Acore::GameObjectInRangeCheck objectCheck(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                                  sSightRange);
        Acore::GameObjectListSearcher<Acore::GameObjectInRangeCheck> objectSearcher(bot, objects, objectCheck);
        Cell::VisitObjects(bot, objectSearcher, sSightRange);
        uint32 questLosChecks = MAX_LOS_CHECKS - MAX_QUEST_OBJECT_LOS_CHECKS;
        for (GameObject* object : objects)
        {
            if (!object->isSpawned())
                continue;

            uint8 kind = 0;
            bool tracked = false;
            if (object->GetGoType() == GAMEOBJECT_TYPE_MAILBOX)
                kind = CortexMemory::OBJECT_MAILBOX;
            else if ((object->GetGoType() == GAMEOBJECT_TYPE_CHEST || object->GetGoType() == GAMEOBJECT_TYPE_GOOBER ||
                      object->GetGoType() == GAMEOBJECT_TYPE_SPELL_FOCUS ||
                      object->GetGoType() == GAMEOBJECT_TYPE_GENERIC) &&
                     object->ActivateToQuest(bot))
                kind = CortexMemory::OBJECT_QUEST;
            else if (herbalist || miner)
            {
                uint32 skill = NewRpgGatherAction::NodeSkill(object->GetGOInfo());
                // Only a node that's up counts: one being gathered or just emptied is as good as gone.
                if (((skill == SKILL_HERBALISM && herbalist) || (skill == SKILL_MINING && miner)) &&
                    object->GetGoState() == GO_STATE_READY && object->getLootState() == GO_READY)
                {
                    kind = CortexMemory::OBJECT_NODE;
                    tracked = skill == trackedSkill && bot->GetExactDist(object) <= trackRange;
                }
            }
            // Up nodes, seen or not (out of the line-of-sight budget, say): none of them is missed below.
            if (kind == CortexMemory::OBJECT_NODE)
                upNodes.push_back(CortexMemory::EntryKey(
                    object->GetEntry(), CortexMemory::CellKey(mapId, object->GetPositionX(), object->GetPositionY())));
            if (!kind || !(tracked                                  ? bot->CanSeeOrDetect(object)
                           : kind == CortexMemory::OBJECT_QUEST ? Sees(bot, object, questLosChecks)
                                                                : Sees(bot, object, losChecks)))
                continue;

            CortexMemory::Object seen;
            seen.entry = object->GetEntry();
            seen.map = static_cast<uint16>(mapId);
            seen.x = object->GetPositionX();
            seen.y = object->GetPositionY();
            seen.z = object->GetPositionZ();
            seen.lastSeen = now;
            seen.kind = kind;
            memory.SeeObject(seen);
        }

        // Remembered nodes in a place the bot would see a node now, with none up there: gathered (by the bot itself
        // or someone else) or respawned elsewhere in its spawn pool. Their kind on the tracking minimap out to the
        // tracking range, others in line of sight.
        if (herbalist || miner)
        {
            std::vector<uint64> missed;
            uint32 nodeLosChecks = 0;
            for (auto const& [key, record] : memory.Objects())
            {
                if (record.kind != CortexMemory::OBJECT_NODE || record.map != mapId || record.goneUntil > now ||
                    std::find(upNodes.begin(), upNodes.end(), key) != upNodes.end())
                    continue;
                float d = bot->GetExactDist(record.x, record.y, record.z);
                if (d > sSightRange)
                    continue;
                bool tracked = trackedSkill && d <= trackRange &&
                               NewRpgGatherAction::NodeSkill(sObjectMgr->GetGameObjectTemplate(record.entry)) ==
                                   trackedSkill;
                if (!tracked && d > NO_LOS_TEST_RANGE)
                {
                    if (nodeLosChecks >= MAX_NODE_LOS_CHECKS)
                        continue;
                    ++nodeLosChecks;
                    if (!bot->IsWithinLOS(record.x, record.y, record.z + 2.0f))
                        continue;
                }
                missed.push_back(key);
            }
            for (uint64 key : missed)
                memory.MissObject(key, now);
        }

        sScanUnits.fetch_add(units.size(), std::memory_order_relaxed);
    }

    std::string SaveSql(uint32 guid, uint8 level, CortexMemory const& memory)
    {
        return "REPLACE INTO playerbots_cortex_memory (guid, level, updated, data) VALUES (" + std::to_string(guid) +
               ", " + std::to_string(level) + ", " + std::to_string(Now()) + ", '" + memory.Serialize() + "')";
    }

    void Save(PlayerbotAI* botAI, Player* bot, CortexState& state)
    {
        CortexMemory const& memory = state.memory;
        PlayerbotsDatabase.Execute(SaveSql(bot->GetGUID().GetCounter(), bot->GetLevel(), memory));

        std::string info = "cells " + std::to_string(memory.CellCount()) + " objects " +
                           std::to_string(memory.ObjectCount()) + " dangers " + std::to_string(memory.DangerCount());
        BotTelemetry::instance().Event(botAI, "memory", info, static_cast<int32>(memory.NpcCount()),
                                       static_cast<int32>(memory.MobCount()), static_cast<int32>(memory.DropCount()));
    }

    void LoadRequested()
    {
        std::vector<LoadRequest> requests;
        {
            std::lock_guard<std::mutex> lock(sLoadMutex);
            requests.swap(sLoadRequests);
        }
        if (requests.empty())
            return;

        std::unordered_map<uint32, uint8> levels;
        std::string guids;
        for (LoadRequest const& request : requests)
        {
            levels[request.guid] = request.level;
            if (!guids.empty())
                guids += ',';
            guids += std::to_string(request.guid);
        }

        std::unordered_map<uint32, CortexState*> states;
        {
            std::lock_guard<std::mutex> lock(sStatesMutex);
            for (LoadRequest const& request : requests)
            {
                auto it = sStates.find(request.guid);
                if (it != sStates.end())
                    states[request.guid] = it->second.get();
            }
        }

        if (QueryResult result =
                PlayerbotsDatabase.Query("SELECT guid, level, data FROM playerbots_cortex_memory WHERE guid IN (" +
                                         guids + ")"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 guid = fields[0].Get<uint32>();
                uint8 level = fields[1].Get<uint8>();
                auto it = states.find(guid);
                // A stored level above the bot's means the bot was reset since: it starts with no memory.
                if (it == states.end() || level > levels[guid])
                    continue;
                if (!it->second->memory.Deserialize(fields[2].Get<std::string>()))
                    it->second->memory.Clear();
            } while (result->NextRow());
        }

        for (auto const& [guid, state] : states)
            state->loaded.store(true, std::memory_order_release);
    }

    void LogStats(uint32 diff)
    {
        sStatsTimer += diff;
        if (sStatsTimer < STATS_INTERVAL_MS)
            return;
        sStatsTimer = 0;

        uint64 scans = sScans.exchange(0);
        uint64 micros = sScanMicros.exchange(0);
        uint64 units = sScanUnits.exchange(0);
        uint32 maxMicros = sScanMaxMicros.exchange(0);
        if (!scans)
            return;
        LOG_INFO("playerbots", "Cortex: {} scans in {} s, avg {} us, max {} us, {} units per scan", scans,
                 STATS_INTERVAL_MS / IN_MILLISECONDS, micros / scans, maxMicros, units / scans);
    }
}

void Cortex::LoadConfig()
{
    sEnabled = sConfigMgr->GetOption<bool>("Cortex.Enable", false);
    float ratio = std::clamp(sConfigMgr->GetOption<float>("Cortex.Ratio", 0.5f), 0.0f, 1.0f);
    sRatioPermille = static_cast<uint32>(ratio * 1000.0f + 0.5f);
    sActing = sConfigMgr->GetOption<bool>("Cortex.Act", false);
    sPerception = sConfigMgr->GetOption<bool>("Cortex.Perception", true);
    sPerceiveMs = std::max<uint32>(500, sConfigMgr->GetOption<uint32>("Cortex.PerceiveMs", 2000));
    sSightRange = std::clamp(sConfigMgr->GetOption<float>("Cortex.SightRange", 100.0f), 20.0f, 150.0f);
    sSaveMinutes = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Cortex.SaveMinutes", 5));
    sTaming = sConfigMgr->GetOption<bool>("Cortex.Tame", true);
    sGathering = sConfigMgr->GetOption<bool>("Cortex.Gather", true);
    sGatherTripRange = std::clamp(sConfigMgr->GetOption<float>("Cortex.GatherTripRange", 400.0f), 50.0f, 2000.0f);
    sSparkles = sConfigMgr->GetOption<bool>("Cortex.Sparkles", true);
    sWatchdog = sConfigMgr->GetOption<bool>("Cortex.Watchdog", true);
    sEscape = sConfigMgr->GetOption<bool>("Cortex.Escape", true);
    sCorpseRun = std::min<uint32>(2, sConfigMgr->GetOption<uint32>("Cortex.CorpseRun", 1));
    sRegroup = sConfigMgr->GetOption<bool>("Cortex.Regroup", true);
    sWand = sConfigMgr->GetOption<bool>("Cortex.Wand", true);
    sTidyBags = sConfigMgr->GetOption<bool>("Cortex.TidyBags", true);
    sTalents = sConfigMgr->GetOption<bool>("Cortex.Talents", true);
    CortexTravel::LoadConfig();
    CortexNeeds::LoadConfig();

    // A missing module config leaves Cortex off; this line makes that obvious in the startup log.
    LOG_INFO("server.loading", "Cortex: Enable {}, Ratio {:.2f}, Act {}, Perception {}, PerceiveMs {}, SightRange {}, "
             "SaveMinutes {}, Tame {}, Gather {}, GatherTripRange {}, Sparkles {}, Watchdog {}, Escape {}, CorpseRun {}, Regroup {}, "
             "Wand {}, TidyBags {}, Talents {}",
             sEnabled, ratio, sActing, sPerception, sPerceiveMs, sSightRange, sSaveMinutes, sTaming, sGathering,
             sGatherTripRange, sSparkles, sWatchdog, sEscape, sCorpseRun, sRegroup, sWand, sTidyBags, sTalents);
}

bool Cortex::Enabled() { return sEnabled; }

bool Cortex::InArm(Player const* bot)
{
    if (!sEnabled || !bot)
        return false;
    CortexState* state = GetState(bot, false);
    return state ? state->inArm : ComputeArm(bot);
}

// 2: the control arm too, so the two arms die and come back alike and the A/B compares the rest.
bool Cortex::RunsToCorpse(Player const* bot)
{
    if (!sEnabled || !bot || !sCorpseRun)
        return false;
    return sCorpseRun == 2 || (sActing && InArm(bot));
}

bool Cortex::Regrouping() { return sEnabled && sRegroup && sActing; }

bool Cortex::Wands() { return sEnabled && sWand && sActing; }

namespace
{
    // What has no use for the bot and no vendor price would stay in its bags for good: playerbots sells what has a
    // price and keeps what a skill, a quest or its gun needs, and destroys only for bots that run "maintenance".
    // A player throws it away. On 2026-10-20 Cortex bots carried about 700,000 arrows and bullets (casters'
    // pickings and stacks worse than what the hunter shoots) and some 2,000 stacks of quest items for quests they
    // no longer had. Only ammo, quest items and trade goods; a hearthstone, key or quest starter is never touched.
    // Any quest in the log names the item: to hand in, to use, or given by it. Playerbots' item usage says NONE
    // for a quest item once the bot holds enough of it (it only gates looting), so the first TidyBags destroyed
    // collected quest items before the hand-in and turn-ins halved (0.45 to 0.19 per bot-hour, 2026-10-20).
    bool QuestNames(Player* bot, uint32 item)
    {
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(bot->GetQuestSlotQuestId(slot));
            if (!quest)
                continue;
            if (quest->GetSrcItemId() == item)
                return true;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i] == item)
                    return true;
            for (uint8 i = 0; i < QUEST_SOURCE_ITEM_IDS_COUNT; ++i)
                if (quest->ItemDrop[i] == item)
                    return true;
        }
        return false;
    }

    void TidyBags(PlayerbotAI* botAI, Player* bot)
    {
        std::vector<std::pair<uint8, uint8>> slots;
        auto consider = [&](Item* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (!proto || proto->SellPrice || proto->StartQuest || item->IsInTrade() ||
                (proto->Class != ITEM_CLASS_PROJECTILE && proto->Class != ITEM_CLASS_QUEST &&
                 proto->Class != ITEM_CLASS_TRADE_GOODS))
                return;
            ItemUsage usage =
                botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", item->GetEntry())->Get();
            if (usage == ITEM_USAGE_NONE && !QuestNames(bot, item->GetEntry()))
                slots.emplace_back(item->GetBagSlot(), item->GetSlot());
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* container = bot->GetBagByPos(bag))
                for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                    consider(container->GetItemByPos(static_cast<uint8>(slot)));

        for (auto const& [bag, slot] : slots)
        {
            Item* item = bot->GetItemByPos(bag, slot);
            if (!item)
                continue;
            BotTelemetry::instance().Event(botAI, "destroy", std::to_string(item->GetTemplate()->Class),
                                           static_cast<int32>(item->GetEntry()), static_cast<int32>(item->GetCount()));
            bot->DestroyItem(bag, slot, true);
        }
    }
}

namespace
{
    // Talents for levelling alone: a player levelling a priest takes Spirit Tap (shadow: after a kill, double
    // spirit and mana regeneration while casting) and leaves holy for later. Playerbots' premade holy build has
    // nothing for mana until Holy Concentration at 40-odd, and a holy priest solos with smite; 23 of 66 Cortex
    // priests were holy on 2026-10-20, every priest was broke and drank after most pulls. The other mana specs'
    // premade builds take their regen talents early already (Meditation, Arcane Concentration and Meditation,
    // Improved Life Tap, Moonglow, Elemental Focus, Divine Intellect). The holy priest is respecced to the premade
    // shadow build once; playerbots keeps the bot's main tree at each level-up after that.
    bool LevellingTalents(PlayerbotAI* botAI, Player* bot)
    {
        if (bot->getClass() != CLASS_PRIEST || bot->GetLevel() < 10 || bot->GetGroup() ||
            AiFactory::GetPlayerSpecTab(bot) != PRIEST_TAB_HOLY)
            return false;
        int shadow = -1;
        for (int specNo = 0; specNo < MAX_SPECNO; ++specNo)
            if (sPlayerbotAIConfig.PremadeSpecName[CLASS_PRIEST][specNo] == "shadow pve")
                shadow = specNo;
        if (shadow < 0)
            return false;
        PlayerbotFactory::InitTalentsBySpecNo(bot, shadow, true);
        botAI->ResetStrategies(false);
        BotTelemetry::instance().Event(botAI, "talents", "holy_shadow", static_cast<int32>(bot->GetLevel()),
                                       static_cast<int32>(AiFactory::GetPlayerSpecTab(bot)));
        return true;
    }
}

void Cortex::Update(PlayerbotAI* botAI)
{
    if (!sEnabled || !botAI)
        return;

    Player* bot = botAI->GetBot();
    CortexState* state = GetState(bot, true);
    if (!state->inArm)
        return;

    if (!state->loaded.load(std::memory_order_acquire))
    {
        if (!state->loadRequested)
        {
            state->loadRequested = true;
            std::lock_guard<std::mutex> lock(sLoadMutex);
            sLoadRequests.push_back({bot->GetGUID().GetCounter(), static_cast<uint8>(bot->GetLevel())});
        }
        return;
    }

    uint32 nowMs = getMSTime();
    uint32 now = Now();
    state->level = static_cast<uint8>(bot->GetLevel());

    // Away time: a minute or more between updates is a bot that wasn't in the world, not time spent away.
    uint32 tick = state->lastTickMs ? getMSTimeDiff(state->lastTickMs, nowMs) : 0;
    state->lastTickMs = nowMs;
    if (tick < MINUTE * IN_MILLISECONDS)
    {
        if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || botAI->rpgInfo.recoverSinceMs)
            state->awayMs += tick;
        else if (state->lastActionErrand && getMSTimeDiff(state->lastActionMs, nowMs) < ERRAND_ACTIVE_MS)
            state->errandMs += tick;
    }

    if (bot->IsAlive())
    {
        if (state->dead && state->seenAlive)
            state->plan.revivedMs = nowMs ? nowMs : 1;
        state->seenAlive = true;
        state->dead = false;
        if (bot->IsInCombat())
        {
            Unit* attacker = bot->getAttackerForHelper();
            if (!attacker)
                attacker = bot->GetVictim();
            if (attacker)
            {
                state->fight.entry = attacker->IsPlayer() ? 0 : attacker->GetEntry();
                state->fight.attackers = static_cast<uint8>(std::min<size_t>(bot->getAttackers().size(), 0xFF));
                state->fight.levelDiff = static_cast<int8>(
                    std::clamp<int32>(int32(attacker->GetLevel()) - int32(bot->GetLevel()), -128, 127));
            }
        }
    }
    else if (!state->dead)
    {
        state->dead = true;
        // A bot that logs in dead didn't just die here.
        if (state->seenAlive)
        {
            CortexMemory::Danger death;
            death.map = static_cast<uint16>(bot->GetMapId());
            death.x = bot->GetPositionX();
            death.y = bot->GetPositionY();
            death.time = now;
            state->plan.deathX = death.x;
            state->plan.deathY = death.y;
            death.killer = state->fight.entry;
            death.attackers = state->fight.attackers;
            death.levelDiff = state->fight.levelDiff;
            state->memory.NoteDeath(death);
            if (state->plan.goal == CortexGoal::WORK)
                ++state->plan.clusterDeaths;
            state->plan.dirty = true;
        }
        state->fight = {};
    }

    if (sPerception && bot->IsAlive() && !bot->IsInFlight() && static_cast<int32>(nowMs - state->nextPerceiveMs) >= 0 &&
        botAI->AllowActivity())
    {
        // Spread scans across bots so they don't all land on the same tick.
        state->nextPerceiveMs = nowMs + sPerceiveMs + bot->GetGUID().GetCounter() % 200;
        auto start = std::chrono::steady_clock::now();
        Perceive(bot, *state, now);
        if (sActing)
            CortexQuests::TouchAreaTriggers(bot);
        uint32 micros = static_cast<uint32>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        sScans.fetch_add(1, std::memory_order_relaxed);
        sScanMicros.fetch_add(micros, std::memory_order_relaxed);
        uint32 previous = sScanMaxMicros.load(std::memory_order_relaxed);
        while (micros > previous && !sScanMaxMicros.compare_exchange_weak(previous, micros))
        {
        }
    }

    if (sWatchdog)
        Watch(botAI, bot, *state, nowMs);

    if (sTidyBags && sActing && static_cast<int32>(nowMs - state->nextTidyMs) >= 0 && bot->IsAlive() &&
        !bot->IsInCombat())
    {
        state->nextTidyMs = nowMs + TIDY_MS + bot->GetGUID().GetCounter() % 1000;
        TidyBags(botAI, bot);
    }

    if (sTalents && sActing && static_cast<int32>(nowMs - state->nextTalentMs) >= 0 && bot->IsAlive() &&
        !bot->IsInCombat())
    {
        state->nextTalentMs = nowMs + TALENT_CHECK_MS + bot->GetGUID().GetCounter() % 1000;
        // Once a session: a respec that didn't take isn't tried every minute.
        if (!state->respecced && LevellingTalents(botAI, bot))
            state->respecced = true;
    }

    if (!state->nextSaveMs)
        state->nextSaveMs = nowMs + bot->GetGUID().GetCounter() % (sSaveMinutes * MINUTE * IN_MILLISECONDS) + 1;
    else if (static_cast<int32>(nowMs - state->nextSaveMs) >= 0)
    {
        state->nextSaveMs = nowMs + sSaveMinutes * MINUTE * IN_MILLISECONDS;
        Save(botAI, bot, *state);
    }
}

void Cortex::UpdateWorld(uint32 diff)
{
    if (!sEnabled)
        return;
    LoadRequested();
    LogStats(diff);
}

bool Cortex::Acting() { return sEnabled && sActing; }

bool Cortex::Taming() { return Acting() && sTaming; }

bool Cortex::Gathering() { return Acting() && sGathering; }

float Cortex::GatherTripRange() { return sGatherTripRange; }

bool Cortex::Sparkles() { return Acting() && sSparkles; }

CortexMemory* Cortex::Memory(Player const* bot)
{
    CortexState* state = GetLoadedState(bot);
    return state ? &state->memory : nullptr;
}

CortexPlan* Cortex::Plan(Player const* bot)
{
    CortexState* state = GetLoadedState(bot);
    return state ? &state->plan : nullptr;
}

char const* Cortex::GoalName(Player const* bot)
{
    if (!sActing)
        return nullptr;
    CortexState* state = GetLoadedState(bot);
    return state ? CortexQuests::GoalName(state->plan.goal) : nullptr;
}

std::string Cortex::Describe(Player const* bot)
{
    if (!sActing)
        return {};
    CortexState* state = GetLoadedState(bot);
    if (!state)
        return {};

    CortexPlan const& plan = state->plan;
    std::vector<uint32> quests;
    auto addTask = [&](int32 index)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= plan.tasks.size())
            return;
        uint32 questId = plan.tasks[index].questId;
        if (questId && std::find(quests.begin(), quests.end(), questId) == quests.end())
            quests.push_back(questId);
    };

    std::string text;
    switch (plan.goal)
    {
        case CortexGoal::HAND_IN:
            text = "handing in";
            addTask(plan.handIn);
            break;
        case CortexGoal::WORK:
            text = "working on";
            if (plan.cluster >= 0 && static_cast<std::size_t>(plan.cluster) < plan.clusters.size())
                for (uint32 task : plan.clusters[plan.cluster].tasks)
                    addTask(static_cast<int32>(task));
            break;
        case CortexGoal::PICK_UP:
            return "looking for new quests";
        case CortexGoal::GRIND:
            return "grinding mobs";
        default:
            return {};
    }

    uint8 named = 0;
    for (uint32 questId : quests)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest || named == 3)
            continue;
        text += named ? ", " : " ";
        text += quest->GetTitle();
        ++named;
    }
    return named ? text : std::string();
}

void Cortex::SaveAll()
{
    if (!sEnabled)
        return;

    std::lock_guard<std::mutex> lock(sStatesMutex);
    uint32 saved = 0;
    for (auto const& [guid, state] : sStates)
    {
        if (!state->inArm || !state->level || !state->loaded.load(std::memory_order_acquire))
            continue;
        PlayerbotsDatabase.DirectExecute(SaveSql(guid, state->level, state->memory));
        ++saved;
    }
    LOG_INFO("playerbots", "Cortex: saved memory of {} bots", saved);
}

uint32 Cortex::AwayMs(Player const* bot)
{
    CortexState* state = GetLoadedState(bot);
    return state ? state->awayMs : 0;
}

uint32 Cortex::ErrandMs(Player const* bot)
{
    CortexState* state = GetLoadedState(bot);
    return state ? state->errandMs : 0;
}

void Cortex::WatchActions(Player* bot, Engine* engine)
{
    if (!sEnabled || !sWatchdog || !bot || !engine)
        return;
    CortexState* state = GetState(bot, true);
    if (state->watchedEngine == engine)
        return;
    state->watchedEngine = engine;
    engine->AddActionExecutionListener(new LastActionListener(state));
}

void Cortex::OnKillCredit(Player* player, Creature* killed)
{
    if (!killed)
        return;
    if (CortexState* state = GetLoadedState(player))
    {
        state->memory.NoteKill(killed->GetEntry(), killed->GetMapId(), killed->GetPositionX(), killed->GetPositionY(),
                               Now());
        state->plan.lastKillMs = getMSTime();
    }
}

void Cortex::OnLoot(Player* player, Item* item, uint32 count, ObjectGuid lootGuid)
{
    if (!item)
        return;

    int32 source = 0;
    if (lootGuid.IsCreature())
        source = static_cast<int32>(lootGuid.GetEntry());
    else if (lootGuid.IsGameObject())
        source = -static_cast<int32>(lootGuid.GetEntry());
    if (!source)
        return;

    if (CortexState* state = GetLoadedState(player))
        state->memory.NoteLoot(source, item->GetEntry(), count, Now());
}
