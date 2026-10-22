/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#ifndef MOD_CORTEX_BOTS_H
#define MOD_CORTEX_BOTS_H

#include <string>
#include <unordered_map>
#include <vector>

struct AreaTableEntry;
class Player;

// What other modules (mod-cortex-chat) may ask of the bots, so they don't reach into the body's classes (src/Body).
// World thread or the bot's map thread unless noted.
namespace CortexBots
{
    // A bot the body drives (random bot or a player's bot), not a real player.
    bool IsBot(Player* player);
    // A person at a client: no bot AI of any kind attached (a player's self-bot isn't one).
    bool IsRealPlayer(Player* player);

    // Chat commands: the names the body knows (lowercase), its command prefix and separator from
    // mod_cortex_bots.conf (empty when unset), the few commands anyone may give a bot, and whether `from` may give
    // `bot` any other.
    std::vector<std::string> CommandNames();
    std::string const& CommandPrefix();
    std::string const& CommandSeparator();
    bool IsAllowedCommand(Player* bot, std::string const& msg);
    bool TakesCommandsFrom(Player* bot, Player* from);

    // The bot says `text` (false if it can't: not a bot, no group or guild, receiver offline). Telemetry rows
    // "chat" as the body writes for its own lines.
    bool Whisper(Player* bot, std::string const& text, std::string const& to);
    bool Say(Player* bot, std::string const& text);
    bool SayToParty(Player* bot, std::string const& text);
    bool SayToGuild(Player* bot, std::string const& text);
    // A telemetry "chat" row for a line the caller said itself (a channel).
    void ChatTelemetry(Player* bot, std::string const& channel, std::string const& text);

    // The bot's persona ("" if none), and persona -> description from cortex_personas (a database query: startup).
    std::string Persona(Player* bot);
    std::unordered_map<std::string, std::string> PersonaDescriptions();

    // An area's name in the realm's locale.
    std::string AreaName(AreaTableEntry const* area);
}

#endif
