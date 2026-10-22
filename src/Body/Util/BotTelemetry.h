/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BOTTELEMETRY_H
#define _PLAYERBOT_BOTTELEMETRY_H

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "Define.h"

class Player;
class PlayerbotAI;

// Per-bot bookkeeping for BotTelemetry::Update, kept on the PlayerbotAI.
struct BotTelemetryState
{
    uint32 nextSampleMs{0};
    uint32 nextFarSampleMs{0};
    uint32 rpgStartT{0};
    uint32 rpgStatus{0};
    uint32 rpgStatusSinceMs{0};
    uint32 deadSinceMs{0};
    uint8 level{0};
    bool nearPlayer{false};
};

// Structured event log for tuning bot behaviour. Rows are batched and written asynchronously to
// acore_playerbots.playerbots_telemetry, then pruned after Playerbots.Telemetry.RetentionHours.
// Safe to call from map threads: no queries, one mutex around the batch.
//
// Events (column use):
//   sample   a=xp, b=hp%, c=flags (SampleFlag); every SampleNearMs near a real player, else SampleFarMs
//   state    info=new rpg status, a=previous status, b=ms spent in it
//   quest    info=accept|complete|reward|drop|shelve_noprog|shelve_reward, a=quest id
//   death    a=ms since the last state change
//   revive   a=ms spent dead
//   level    a=new level
//   stuck_tp a=distance to the destination
//   chat     info=channel, a=length, b=text hash
//   action   info=action name, a=relevance*100 (only for bots near a real player)
//   ptrace   real (non-bot) players only, every Telemetry.PlayerTraceMs while they move: a=movement flags,
//            b=hp%, c=flags (SampleFlag subset); state "player". Kept past RetentionHours (navigation atlas).
class BotTelemetry
{
public:
    enum SampleFlag : uint32
    {
        SAMPLE_COMBAT = 0x01,
        SAMPLE_DEAD = 0x02,
        SAMPLE_MOVING = 0x04,
        SAMPLE_GROUPED = 0x08,
        SAMPLE_REAL_GROUP = 0x10,
        SAMPLE_ACTIVE = 0x20,
        SAMPLE_NEAR_PLAYER = 0x40,
        SAMPLE_RANDOM_BOT = 0x80,
        SAMPLE_IN_FLIGHT = 0x100,
        SAMPLE_EXT_ARM = 0x200,  // in an extension's A/B arm (PlayerbotsExtensions telemetry provider)
    };

    static BotTelemetry& instance();

    void Event(PlayerbotAI* botAI, std::string_view event, std::string_view info = {}, int32 a = 0, int32 b = 0,
               int32 c = 0);
    void Chat(PlayerbotAI* botAI, std::string_view channel, std::string const& msg);
    void Action(PlayerbotAI* botAI, std::string const& name, float relevance);
    // Called every UpdateAI tick; O(1) unless a sample is due.
    void Update(PlayerbotAI* botAI);
    // Called from the player update hook for every player; does nothing for bots or when tracing is off.
    void PlayerTrace(Player* player);
    // Reads Playerbots.Telemetry.PlayerTraceMs (called from PlayerbotAIConfig::Initialize).
    static void LoadConfig();

    static char const* StatusName(uint32 status);

private:
    struct Row
    {
        uint64 ts;
        uint32 guid;
        std::string event;
        std::string state;
        std::string info;
        int32 a;
        int32 b;
        int32 c;
        uint32 map;
        uint32 zone;
        float x;
        float y;
        float z;
        uint8 level;
    };

    void Push(Row&& row);
    void Write(std::vector<Row> const& rows);

    std::mutex _mutex;
    std::vector<Row> _rows;
    uint32 _lastFlushMs{0};
    uint64 _lastPruneTs{0};
};

#endif
