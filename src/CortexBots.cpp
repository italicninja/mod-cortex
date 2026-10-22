/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include "CortexBots.h"

#include <algorithm>
#include <cctype>

#include "BotTelemetry.h"
#include "ChatTriggerContext.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSecurity.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"

namespace
{
    PlayerbotAI* BotAI(Player* player)
    {
        PlayerbotAI* botAI = player ? GET_PLAYERBOT_AI(player) : nullptr;
        return botAI && botAI->IsBotAI() ? botAI : nullptr;
    }
}

bool CortexBots::IsBot(Player* player) { return BotAI(player) != nullptr; }

bool CortexBots::IsRealPlayer(Player* player) { return ::IsRealPlayer(player); }

std::vector<std::string> CortexBots::CommandNames()
{
    std::vector<std::string> names;
    ChatTriggerContext triggers;
    for (std::string name : triggers.supports())
    {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        names.push_back(std::move(name));
    }
    return names;
}

std::string const& CortexBots::CommandPrefix() { return sPlayerbotAIConfig.CommandPrefix; }

std::string const& CortexBots::CommandSeparator() { return sPlayerbotAIConfig.CommandSeparator; }

bool CortexBots::IsAllowedCommand(Player* bot, std::string const& msg)
{
    PlayerbotAI* botAI = BotAI(bot);
    return botAI && botAI->IsAllowedCommand(msg);
}

bool CortexBots::TakesCommandsFrom(Player* bot, Player* from)
{
    PlayerbotAI* botAI = BotAI(bot);
    return botAI && botAI->GetSecurity()->LevelFor(from) >= PLAYERBOT_SECURITY_ALLOW_ALL;
}

bool CortexBots::Whisper(Player* bot, std::string const& text, std::string const& to)
{
    PlayerbotAI* botAI = BotAI(bot);
    return botAI && botAI->Whisper(text, to);
}

bool CortexBots::Say(Player* bot, std::string const& text)
{
    PlayerbotAI* botAI = BotAI(bot);
    return botAI && botAI->Say(text);
}

bool CortexBots::SayToParty(Player* bot, std::string const& text)
{
    PlayerbotAI* botAI = BotAI(bot);
    return botAI && botAI->SayToParty(text);
}

bool CortexBots::SayToGuild(Player* bot, std::string const& text)
{
    PlayerbotAI* botAI = BotAI(bot);
    return botAI && botAI->SayToGuild(text);
}

void CortexBots::ChatTelemetry(Player* bot, std::string const& channel, std::string const& text)
{
    if (PlayerbotAI* botAI = BotAI(bot))
        BotTelemetry::instance().Chat(botAI, channel, text);
}

std::string CortexBots::Persona(Player* bot) { return PlayerbotTextMgr::instance().GetPersona(bot); }

std::unordered_map<std::string, std::string> CortexBots::PersonaDescriptions()
{
    std::unordered_map<std::string, std::string> descriptions;
    if (QueryResult result = PlayerbotsDatabase.Query("SELECT persona, description FROM cortex_personas"))
        do
        {
            Field* fields = result->Fetch();
            descriptions[fields[0].Get<std::string>()] = fields[1].Get<std::string>();
        } while (result->NextRow());
    return descriptions;
}

std::string CortexBots::AreaName(AreaTableEntry const* area)
{
    return area ? PlayerbotAI::GetLocalizedAreaName(area) : std::string();
}
