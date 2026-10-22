/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include "CortexChatHook.h"

#include <vector>

namespace
{
    // Function-local static: modules register from their loaders, whose order against this file's static
    // initialisation isn't defined.
    std::vector<CortexChatHook::ChatFilter>& Filters()
    {
        static std::vector<CortexChatHook::ChatFilter> filters;
        return filters;
    }
}

void CortexChatHook::AddChatFilter(ChatFilter filter) { Filters().push_back(std::move(filter)); }

bool CortexChatHook::ClaimsChat(Player* bot, Player* from, uint32 type, std::string const& msg)
{
    for (auto const& filter : Filters())
        if (filter.claims && filter.claims(bot, from, type, msg))
            return true;
    return false;
}

bool CortexChatHook::SilencesChatReply(Player* bot, uint32 type)
{
    for (auto const& filter : Filters())
        if (filter.silences && filter.silences(bot, type))
            return true;
    return false;
}
