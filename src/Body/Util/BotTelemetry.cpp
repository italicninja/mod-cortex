/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotTelemetry.h"
#include "BotDanger.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <mutex>
#include <unordered_map>

#include "CellImpl.h"
#include "Config.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "MoveSpline.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotsDatabase.h"
#include "Playerbots.h"
#include "CortexBody.h"
#include "RandomPlayerbotMgr.h"
#include "Timer.h"
#include "WorldSession.h"

namespace
{
// What a bot is working towards in its current rpg status. Kept here, keyed by guid, rather than in
// BotTelemetryState so PlayerbotAI.h (≈590 dependants) doesn't change.
struct Goal
{
    uint32 status{0};
    int32 a{0};
    int32 b{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    bool hasPos{false};

    bool operator==(Goal const&) const = default;
};

std::mutex goalMutex;
std::unordered_map<uint32, Goal> lastGoals;

Goal CurrentGoal(NewRpgInfo const& info, uint32 status)
{
    Goal goal;
    goal.status = status;
    auto setPos = [&goal](WorldPosition const& pos)
    {
        // An unset WorldPosition keeps an invalid map id, so operator bool alone passes it.
        if (!pos || (pos.GetPositionX() == 0.0f && pos.GetPositionY() == 0.0f))
            return;
        goal.x = pos.GetPositionX();
        goal.y = pos.GetPositionY();
        goal.z = pos.GetPositionZ();
        goal.hasPos = true;
    };

    if (auto const* d = std::get_if<NewRpgInfo::GoGrind>(&info.data))
        setPos(d->pos);
    else if (auto const* d = std::get_if<NewRpgInfo::GoCamp>(&info.data))
        setPos(d->pos);
    else if (auto const* d = std::get_if<NewRpgInfo::WanderNpc>(&info.data))
    {
        // Same sign convention as quest_template.RequiredNpcOrGo: negative for a game object.
        int32 entry = static_cast<int32>(d->npcOrGo.GetEntry());
        goal.a = d->npcOrGo.IsGameObject() ? -entry : entry;
    }
    else if (auto const* d = std::get_if<NewRpgInfo::DoQuest>(&info.data))
    {
        goal.a = static_cast<int32>(d->questId);
        goal.b = d->objectiveIdx;
        setPos(d->pos);
    }
    else if (auto const* d = std::get_if<NewRpgInfo::TravelFlight>(&info.data))
    {
        goal.a = static_cast<int32>(d->flightMasterEntry);
        goal.b = d->path.empty() ? 0 : static_cast<int32>(d->path.back());
        setPos(d->flightMasterPos);
    }
    else if (auto const* d = std::get_if<NewRpgInfo::OutdoorPvP>(&info.data))
        goal.a = static_cast<int32>(d->capturePointSpawnId);
    return goal;
}

// Writes a "goal" row when the bot's goal differs from the last one written. Checked every SampleNearMs.
void UpdateGoal(PlayerbotAI* botAI)
{
    uint32 status = botAI->rpgInfo.GetStatus();
    if (status == RPG_IDLE)
        return;

    Player* bot = botAI->GetBot();
    Goal goal = CurrentGoal(botAI->rpgInfo, status);
    {
        std::lock_guard<std::mutex> lock(goalMutex);
        Goal& last = lastGoals[bot->GetGUID().GetCounter()];
        if (last == goal)
            return;
        last = goal;
    }

    if (!goal.hasPos)
    {
        BotTelemetry::instance().Event(botAI, "goal", {}, goal.a, goal.b, -1);
        return;
    }

    int32 yards = static_cast<int32>(bot->GetExactDist(goal.x, goal.y, goal.z));
    BotTelemetry::instance().Event(botAI, "goal", fmt::format("{:.1f} {:.1f} {:.1f}", goal.x, goal.y, goal.z), goal.a,
                                   goal.b, yards);
}

constexpr size_t FLUSH_ROWS = 500;
constexpr uint32 FLUSH_INTERVAL_MS = 5 * IN_MILLISECONDS;
constexpr uint64 PRUNE_INTERVAL_MS = 10 * MINUTE * IN_MILLISECONDS;
constexpr size_t INFO_MAX_LENGTH = 48;

uint64 NowUnixMs()
{
    return static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
}

// Keep only characters that are safe inside a quoted SQL literal.
std::string Sanitize(std::string_view text)
{
    std::string out;
    out.reserve(std::min(text.size(), INFO_MAX_LENGTH));
    for (char ch : text)
    {
        if (out.size() >= INFO_MAX_LENGTH)
            break;

        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == ' ' || ch == '_' || ch == ':' || ch == '.' ||
            ch == '-')
            out.push_back(ch);
    }
    return out;
}
}  // namespace

BotTelemetry& BotTelemetry::instance()
{
    static BotTelemetry instance;
    return instance;
}

char const* BotTelemetry::StatusName(uint32 status)
{
    switch (status)
    {
        case RPG_IDLE:
            return "idle";
        case RPG_GO_GRIND:
            return "go_grind";
        case RPG_GO_CAMP:
            return "go_camp";
        case RPG_WANDER_RANDOM:
            return "wander_rnd";
        case RPG_WANDER_NPC:
            return "wander_npc";
        case RPG_DO_QUEST:
            return "do_quest";
        case RPG_TRAVEL_FLIGHT:
            return "flight";
        case RPG_REST:
            return "rest";
        case RPG_OUTDOOR_PVP:
            return "outdoor_pvp";
        default:
            return "unknown";
    }
}

void BotTelemetry::Event(PlayerbotAI* botAI, std::string_view event, std::string_view info, int32 a, int32 b,
                         int32 c)
{
    if (!sPlayerbotAIConfig.TelemetryEnabled || !botAI)
        return;

    Player* bot = botAI->GetBot();
    if (!bot || !bot->IsInWorld())
        return;

    Row row;
    row.ts = NowUnixMs();
    row.guid = bot->GetGUID().GetCounter();
    row.event = std::string(event);
    if (!Cortex::TelemetryState(bot, row.state))
        row.state = StatusName(botAI->rpgInfo.GetStatus());
    row.info = Sanitize(info);
    row.a = a;
    row.b = b;
    row.c = c;
    row.map = bot->GetMapId();
    row.zone = bot->GetZoneId();
    row.x = bot->GetPositionX();
    row.y = bot->GetPositionY();
    row.z = bot->GetPositionZ();
    row.level = bot->GetLevel();
    Push(std::move(row));
}

void BotTelemetry::Chat(PlayerbotAI* botAI, std::string_view channel, std::string const& msg)
{
    if (!sPlayerbotAIConfig.TelemetryEnabled)
        return;

    int32 hash = static_cast<int32>(std::hash<std::string>{}(msg) & 0x7FFFFFFF);
    Event(botAI, "chat", channel, static_cast<int32>(msg.size()), hash);
}

void BotTelemetry::Action(PlayerbotAI* botAI, std::string const& name, float relevance)
{
    if (!sPlayerbotAIConfig.TelemetryEnabled || !sPlayerbotAIConfig.TelemetryActions || !botAI->telemetry.nearPlayer)
        return;

    Event(botAI, "action", name, static_cast<int32>(relevance * 100.0f));
}

namespace
{
// Who the bot last fought (kept here so PlayerbotAI.h doesn't change): written while it is alive and in combat,
// read by the death row. Bots of one map share a thread, bots of different maps don't, hence the mutex.
struct LastFight
{
    uint32 entry = 0;
    int32 levelDiff = 0;
    uint32 attackers = 0;
};
std::mutex lastFightMutex;
std::unordered_map<ObjectGuid::LowType, LastFight> lastFights;

void NoteFight(Player* bot)
{
    Unit* attacker = bot->getAttackerForHelper();
    if (!attacker)
        attacker = bot->GetVictim();
    if (!attacker)
        return;

    LastFight fight;
    fight.entry = attacker->IsPlayer() ? 0 : attacker->GetEntry();
    fight.levelDiff = static_cast<int32>(attacker->GetLevel()) - static_cast<int32>(bot->GetLevel());
    fight.attackers = static_cast<uint32>(bot->getAttackers().size());
    std::lock_guard<std::mutex> lock(lastFightMutex);
    lastFights[bot->GetGUID().GetCounter()] = fight;
}

LastFight TakeFight(Player* bot)
{
    std::lock_guard<std::mutex> lock(lastFightMutex);
    auto it = lastFights.find(bot->GetGUID().GetCounter());
    if (it == lastFights.end())
        return {};
    LastFight fight = it->second;
    lastFights.erase(it);
    return fight;
}

// Hostile, living, non-critter units within `range` (once per revive).
uint32 HostilesNear(Player* bot, float range)
{
    std::list<Unit*> units;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, range);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, units, check);
    Cell::VisitObjects(bot, searcher, range);
    return static_cast<uint32>(std::count_if(units.begin(), units.end(), [bot](Unit* unit)
                                             { return unit->IsHostileTo(bot) && !unit->IsPlayer(); }));
}
}  // namespace

void BotTelemetry::Update(PlayerbotAI* botAI)
{
    if (!sPlayerbotAIConfig.TelemetryEnabled)
        return;

    Player* bot = botAI->GetBot();
    BotTelemetryState& state = botAI->telemetry;
    uint32 now = getMSTime();

    // NewRpgInfo::ChangeTo* resets startT, so a new startT means a new status. IDLE only lasts until
    // the next status roll, so it isn't recorded.
    if (botAI->rpgInfo.startT != state.rpgStartT)
    {
        state.rpgStartT = botAI->rpgInfo.startT;
        uint32 status = botAI->rpgInfo.GetStatus();
        if (status != RPG_IDLE)
        {
            int32 spent = state.rpgStatusSinceMs ? static_cast<int32>(getMSTimeDiff(state.rpgStatusSinceMs, now)) : 0;
            Event(botAI, "state", StatusName(status), static_cast<int32>(state.rpgStatus), spent);
            state.rpgStatus = status;
            state.rpgStatusSinceMs = now;
        }
    }

    if (!bot->IsAlive())
    {
        if (!state.deadSinceMs)
        {
            state.deadSinceMs = now ? now : 1;
            // state.level is only 0 on the first tick: a bot that logs in dead didn't just die
            if (state.level)
            {
                uint32 sinceState = state.rpgStatusSinceMs ? getMSTimeDiff(state.rpgStatusSinceMs, now) : 0;
                // info "<killer entry> <x> <y>" (entry 0: a player or unknown), b attackers, c killer level - bot level
                LastFight fight = TakeFight(bot);
                BotDanger::NoteDeath(bot);
                std::string where = std::to_string(fight.entry) + " " +
                                    std::to_string(static_cast<int32>(bot->GetPositionX())) + " " +
                                    std::to_string(static_cast<int32>(bot->GetPositionY()));
                Event(botAI, "death", where, static_cast<int32>(sinceState), static_cast<int32>(fight.attackers),
                      fight.levelDiff);
            }
        }
    }
    else if (state.deadSinceMs)
    {
        // b health % on coming back, c hostile mobs within 25 yd
        Event(botAI, "revive", {}, static_cast<int32>(getMSTimeDiff(state.deadSinceMs, now)),
              static_cast<int32>(bot->GetHealthPct()), static_cast<int32>(HostilesNear(bot, 25.0f)));
        state.deadSinceMs = 0;
    }

    if (bot->IsAlive() && bot->IsInCombat())
        NoteFight(bot);

    uint8 level = bot->GetLevel();
    if (level != state.level)
    {
        if (state.level)
            Event(botAI, "level", {}, level);
        state.level = level;
    }

    // Proximity is checked every SampleNearMs. A row is written then if a real player is near,
    // otherwise only once per SampleFarMs. Comparisons are wrap-safe "now < next".
    if (!state.nextSampleMs)
    {
        // Spread each bot's first check over the far interval so logins don't burst.
        uint32 spread = bot->GetGUID().GetCounter() % sPlayerbotAIConfig.TelemetrySampleFarMs;
        state.nextSampleMs = (now + spread % sPlayerbotAIConfig.TelemetrySampleNearMs) | 1;
        state.nextFarSampleMs = (now + spread) | 1;
        return;
    }

    if (static_cast<int32>(now - state.nextSampleMs) < 0)
        return;

    state.nextSampleMs = (now + sPlayerbotAIConfig.TelemetrySampleNearMs) | 1;
    UpdateGoal(botAI);
    state.nearPlayer = botAI->HasPlayerNearby(static_cast<float>(sPlayerbotAIConfig.TelemetryNearRadius));
    if (!state.nearPlayer && static_cast<int32>(now - state.nextFarSampleMs) < 0)
        return;

    state.nextFarSampleMs = (now + sPlayerbotAIConfig.TelemetrySampleFarMs) | 1;

    uint32 flags = 0;
    if (bot->IsInCombat())
        flags |= SAMPLE_COMBAT;
    if (!bot->IsAlive())
        flags |= SAMPLE_DEAD;
    if (bot->isMoving() || !bot->movespline->Finalized())
        flags |= SAMPLE_MOVING;
    if (bot->GetGroup())
    {
        flags |= SAMPLE_GROUPED;
        if (botAI->HasGameClientMaster() || !botAI->GetRealPlayersInGroup().empty())
            flags |= SAMPLE_REAL_GROUP;
    }
    if (botAI->AllowActivity())
        flags |= SAMPLE_ACTIVE;
    if (state.nearPlayer)
        flags |= SAMPLE_NEAR_PLAYER;
    if (sRandomPlayerbotMgr.IsRandomBot(bot))
        flags |= SAMPLE_RANDOM_BOT;
    if (bot->IsInFlight())
        flags |= SAMPLE_IN_FLIGHT;
    if (Cortex::InArm(bot))
        flags |= SAMPLE_EXT_ARM;

    uint32 hpPct = static_cast<uint32>(bot->GetHealthPct());
    Event(botAI, "sample", {}, static_cast<int32>(bot->GetUInt32Value(PLAYER_XP)), static_cast<int32>(hpPct),
          static_cast<int32>(flags));
}

namespace
{
    uint32 sPlayerTraceMs = 0;

    struct PlayerTraceState
    {
        uint32 nextMs{0};
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
        uint32 moveFlags{0};
        uint32 flags{0};
    };

    std::mutex sPlayerTraceMutex;
    std::unordered_map<uint32, PlayerTraceState> sPlayerTraces;
}

void BotTelemetry::LoadConfig()
{
    sPlayerTraceMs = sConfigMgr->GetOption<uint32>("Cortex.Bots.Telemetry.PlayerTraceMs", 0);
    if (sPlayerTraceMs)
        sPlayerTraceMs = std::max<uint32>(250, sPlayerTraceMs);
}

// Real players' movement, for the navigation atlas (docs/bot-ai-overhaul.md §4.11): where a player walks, a
// way exists, including ramps and drops the navmesh misses. Rows only when the player moved 0.5 yd or its
// movement/state flags changed, so standing still costs nothing.
void BotTelemetry::PlayerTrace(Player* player)
{
    if (!sPlayerTraceMs || !sPlayerbotAIConfig.TelemetryEnabled || !player || !player->IsInWorld())
        return;

    WorldSession* session = player->GetSession();
    if (!session || session->IsHeadless() || GET_PLAYERBOT_AI(player))
        return;

    uint32 now = getMSTime();
    uint32 guid = player->GetGUID().GetCounter();
    uint32 moveFlags = player->GetUnitMovementFlags();
    uint32 flags = 0;
    if (player->IsInCombat())
        flags |= SAMPLE_COMBAT;
    if (!player->IsAlive())
        flags |= SAMPLE_DEAD;
    if (player->IsInFlight())
        flags |= SAMPLE_IN_FLIGHT;

    {
        std::lock_guard<std::mutex> lock(sPlayerTraceMutex);
        PlayerTraceState& state = sPlayerTraces[guid];
        if (state.nextMs && static_cast<int32>(now - state.nextMs) < 0)
            return;

        state.nextMs = (now + sPlayerTraceMs) | 1;
        float dx = player->GetPositionX() - state.x;
        float dy = player->GetPositionY() - state.y;
        float dz = player->GetPositionZ() - state.z;
        if (dx * dx + dy * dy + dz * dz < 0.25f && moveFlags == state.moveFlags && flags == state.flags)
            return;

        state.x = player->GetPositionX();
        state.y = player->GetPositionY();
        state.z = player->GetPositionZ();
        state.moveFlags = moveFlags;
        state.flags = flags;
    }

    Row row;
    row.ts = NowUnixMs();
    row.guid = guid;
    row.event = "ptrace";
    row.state = "player";
    row.a = static_cast<int32>(moveFlags);
    row.b = static_cast<int32>(player->GetHealthPct());
    row.c = static_cast<int32>(flags);
    row.map = player->GetMapId();
    row.zone = player->GetZoneId();
    row.x = player->GetPositionX();
    row.y = player->GetPositionY();
    row.z = player->GetPositionZ();
    row.level = player->GetLevel();
    Push(std::move(row));
}

void BotTelemetry::Push(Row&& row)
{
    std::vector<Row> batch;
    bool prune = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _rows.push_back(std::move(row));

        uint32 now = getMSTime();
        if (_rows.size() < FLUSH_ROWS && getMSTimeDiff(_lastFlushMs, now) < FLUSH_INTERVAL_MS)
            return;

        _lastFlushMs = now;
        batch.swap(_rows);

        uint64 nowTs = NowUnixMs();
        if (nowTs - _lastPruneTs >= PRUNE_INTERVAL_MS)
        {
            _lastPruneTs = nowTs;
            prune = true;
        }
    }

    Write(batch);

    if (prune)
    {
        uint64 retentionMs = static_cast<uint64>(sPlayerbotAIConfig.TelemetryRetentionHours) * HOUR * IN_MILLISECONDS;
        uint64 cutoff = NowUnixMs() - retentionMs;
        PlayerbotsDatabase.Execute(
            fmt::format("DELETE FROM cortex_telemetry WHERE ts < {} AND event <> 'ptrace'", cutoff));
    }
}

void BotTelemetry::Write(std::vector<Row> const& rows)
{
    if (rows.empty())
        return;

    std::string sql;
    sql.reserve(rows.size() * 120);
    sql += "INSERT INTO cortex_telemetry (ts, guid, event, state, info, a, b, c, map, zone, x, y, z, level) ";
    sql += "VALUES ";
    bool first = true;
    for (Row const& row : rows)
    {
        if (!first)
            sql += ',';
        first = false;

        sql += fmt::format("({},{},'{}','{}','{}',{},{},{},{},{},{:.1f},{:.1f},{:.1f},{})", row.ts, row.guid,
                           row.event, row.state, row.info, row.a, row.b, row.c, row.map, row.zone, row.x, row.y, row.z,
                           row.level);
    }

    PlayerbotsDatabase.Execute(sql);
}
