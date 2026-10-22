/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotTextMgr.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotsDatabase.h"
#include "DatabaseEnv.h"
#include "Player.h"
#include "QueryResult.h"    // Required due to a poor implementation by AC
#include "Random.h"
#include "WorldSessionMgr.h"

void PlayerbotTextMgr::replaceAll(std::string& str, std::string const& from, std::string const& to)
{
    if (from.empty())
        return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos)
    {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();  // In case 'to' contains 'from', like replacing 'x' with 'yx'
    }
}

void PlayerbotTextMgr::LoadBotTexts()
{
    LOG_INFO("playerbots", "Loading playerbots texts...");

    uint32 count = 0;
    if (PreparedQueryResult result =
            PlayerbotsDatabase.Query(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_TEXT)))
    {
        do
        {
            std::map<uint32, std::string> text;
            Field* fields = result->Fetch();
            std::string name = fields[0].Get<std::string>();
            text[0] = fields[1].Get<std::string>();
            uint8 sayType = fields[2].Get<uint8>();
            uint8 replyType = fields[3].Get<uint8>();
            for (uint8 i = 1; i < TOTAL_LOCALES; ++i)
            {
                text[i] = fields[i + 3].Get<std::string>();
            }

            botTexts[name].push_back(BotTextEntry(name, text, sayType, replyType));
            ++count;
        } while (result->NextRow());
    }

    LOG_INFO("playerbots", "{} playerbots texts loaded", count);
}

void PlayerbotTextMgr::LoadBotTextChance()
{
    if (botTextChance.empty())
    {
        PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_TEXT_CHANCE);
        PreparedQueryResult results = PlayerbotsDatabase.Query(stmt);
        if (results)
        {
            do
            {
                Field* fields = results->Fetch();
                std::string name = fields[0].Get<std::string>();
                uint32 probability = fields[1].Get<uint32>();

                botTextChance[name] = probability;
            } while (results->NextRow());
        }
    }
}

// general texts

std::string PlayerbotTextMgr::GetBotText(std::string name)
{
    if (botTexts.empty())
    {
        LOG_ERROR("playerbots", "Can't get bot text {}! No bots texts loaded!", name);
        return "";
    }

    if (botTexts[name].empty())
    {
        LOG_ERROR("playerbots", "Can't get bot text {}! No bots texts for this name!", name);
        return "";
    }

    std::vector<BotTextEntry>& list = botTexts[name];
    BotTextEntry textEntry = list[urand(0, list.size() - 1)];
    return !textEntry.m_text[GetLocalePriority()].empty() ? textEntry.m_text[GetLocalePriority()] : textEntry.m_text[0];
}

std::string PlayerbotTextMgr::GetBotText(std::string name, std::map<std::string, std::string> placeholders)
{
    std::string botText = GetBotText(name);
    if (botText.empty())
        return "";

    for (std::map<std::string, std::string>::iterator i = placeholders.begin(); i != placeholders.end(); ++i)
        replaceAll(botText, i->first, i->second);

    return botText;
}

std::string PlayerbotTextMgr::GetBotTextOrDefault(std::string name, std::string defaultText,
    std::map<std::string, std::string> placeholders)
{
    std::string botText = GetBotText(name, placeholders);
    if (botText.empty())
    {
        for (std::map<std::string, std::string>::iterator i = placeholders.begin(); i != placeholders.end(); ++i)
        {
            replaceAll(defaultText, i->first, i->second);
        }
        return defaultText;
    }

    return botText;
}

// personas

namespace
{
    // Persona keys end up in text names and in a formatted query, so keep them to [a-z0-9_]
    bool IsValidPersonaKey(std::string const& key)
    {
        if (key.empty() || key.size() > 32)
            return false;

        for (char c : key)
        {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
                return false;
        }

        return true;
    }
}

void PlayerbotTextMgr::LoadPersonas()
{
    std::lock_guard<std::mutex> lock(personaMutex);
    personas.clear();
    personaWeightTotal = 0;
    botPersonas.clear();

    if (QueryResult result = PlayerbotsDatabase.Query("SELECT persona, weight FROM cortex_personas"))
    {
        do
        {
            Field* fields = result->Fetch();
            std::string persona = fields[0].Get<std::string>();
            uint32 weight = fields[1].Get<uint32>();
            if (!weight || !IsValidPersonaKey(persona))
            {
                LOG_WARN("playerbots", "Skipping bot persona '{}' (weight {}): key must be [a-z0-9_]{{1,32}} with weight > 0", persona, weight);
                continue;
            }

            personas.emplace_back(persona, weight);
            personaWeightTotal += weight;
        } while (result->NextRow());
    }

    if (QueryResult result = PlayerbotsDatabase.Query("SELECT guid, persona FROM cortex_persona"))
    {
        do
        {
            Field* fields = result->Fetch();
            botPersonas[fields[0].Get<uint32>()] = fields[1].Get<std::string>();
        } while (result->NextRow());
    }

    LOG_INFO("playerbots", "{} bot personas loaded, {} bots assigned", personas.size(), botPersonas.size());
}

std::string PlayerbotTextMgr::GetPersona(Player* bot)
{
    if (!bot)
        return "";

    uint32 const guid = bot->GetGUID().GetCounter();
    std::lock_guard<std::mutex> lock(personaMutex);
    if (!personaWeightTotal)
        return "";

    // Keep the stored persona unless it was removed from cortex_personas
    auto it = botPersonas.find(guid);
    if (it != botPersonas.end())
    {
        for (auto const& [persona, weight] : personas)
        {
            if (persona == it->second)
                return persona;
        }
    }

    uint32 roll = urand(1, personaWeightTotal);
    std::string chosen;
    for (auto const& [persona, weight] : personas)
    {
        if (roll <= weight)
        {
            chosen = persona;
            break;
        }
        roll -= weight;
    }

    botPersonas[guid] = chosen;
    PlayerbotsDatabase.Execute("REPLACE INTO cortex_persona (guid, persona) VALUES ({}, '{}')", guid, chosen);
    return chosen;
}

bool PlayerbotTextMgr::GetPersonaText(Player* speaker, std::string const& name, std::string& text,
                                      std::map<std::string, std::string> placeholders)
{
    if (!sPlayerbotAIConfig.EnablePersonas || urand(1, 100) > sPlayerbotAIConfig.PersonaTextChance)
        return false;

    std::string const persona = GetPersona(speaker);
    if (persona.empty())
        return false;

    auto it = botTexts.find(name + ":" + persona);
    if (it == botTexts.end() || it->second.empty())
        return false;

    text = GetBotText(it->first, placeholders);
    return !text.empty();
}

// chat replies

std::string PlayerbotTextMgr::GetBotText(ChatReplyType replyType, std::map<std::string, std::string> placeholders)
{
    if (botTexts.empty())
    {
        LOG_ERROR("playerbots", "Can't get bot text reply {}! No bots texts loaded!", replyType);
        return "";
    }
    if (botTexts["reply"].empty())
    {
        LOG_ERROR("playerbots", "Can't get bot text reply {}! No bots texts replies!", replyType);
        return "";
    }

    std::vector<BotTextEntry>& list = botTexts["reply"];
    std::vector<BotTextEntry> proper_list;
    for (auto text : list)
    {
        if (text.m_replyType == replyType)
            proper_list.push_back(text);
    }

    if (proper_list.empty())
        return "";

    BotTextEntry textEntry = proper_list[urand(0, proper_list.size() - 1)];
    std::string botText =
        !textEntry.m_text[GetLocalePriority()].empty() ? textEntry.m_text[GetLocalePriority()] : textEntry.m_text[0];
    for (auto& placeholder : placeholders)
        replaceAll(botText, placeholder.first, placeholder.second);

    return botText;
}

std::string PlayerbotTextMgr::GetBotText(ChatReplyType replyType, std::string name)
{
    std::map<std::string, std::string> placeholders;
    placeholders["%s"] = name;

    return GetBotText(replyType, placeholders);
}

// probabilities

bool PlayerbotTextMgr::rollTextChance(std::string name)
{
    if (!botTextChance[name])
        return true;

    return urand(0, 100) < botTextChance[name];
}

bool PlayerbotTextMgr::GetBotText(std::string name, std::string& text)
{
    if (!rollTextChance(name))
        return false;

    text = GetBotText(name);
    return !text.empty();
}

bool PlayerbotTextMgr::GetBotText(std::string name, std::string& text, std::map<std::string, std::string> placeholders)
{
    if (!rollTextChance(name))
        return false;

    text = GetBotText(name, placeholders);
    return !text.empty();
}

void PlayerbotTextMgr::AddLocalePriority(uint32 locale)
{
    if (locale >= TOTAL_LOCALES)
    {
        LOG_WARN("playerbots", "Ignoring locale {} for bot texts because it exceeds TOTAL_LOCALES ({})", locale, TOTAL_LOCALES - 1);
        return;
    }

    botTextLocalePriority[locale]++;
}

uint32 PlayerbotTextMgr::GetLocalePriority()
{
    // if no real players online, reset top locale
    uint32 const activeSessions = sWorldSessionMgr->GetActiveSessionCount();
    if (!activeSessions)
    {
        ResetLocalePriority();
        return 0;
    }

    uint32 topLocale = 0;
    for (uint8 i = 0; i < TOTAL_LOCALES; ++i)
    {
        if (botTextLocalePriority[i] > botTextLocalePriority[topLocale])
            topLocale = i;
    }

    return topLocale;
}

void PlayerbotTextMgr::ResetLocalePriority()
{
    for (uint8 i = 0; i < TOTAL_LOCALES; ++i)
    {
        botTextLocalePriority[i] = 0;
    }
}
