/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#ifndef MOD_CORTEX_CHAT_HOOK_H
#define MOD_CORTEX_CHAT_HOOK_H

#include <functional>
#include <string>

#include "Define.h"

class Player;

// The one hook the bot body keeps for another module: mod-cortex-chat answers real players' chat itself (LLM
// replies), and the body is built without it. The rest of what was PlayerbotsExtensions (bots 0053) became direct
// calls into the brain (CortexBody.h) when the body moved into mod-cortex.
namespace CortexChatHook
{
    // `claims`: a real player's whisper, party or guild message to `bot` is chat, not a command, so the body doesn't
    // run it (nor refuse it) for that bot; world thread. `silences`: the bot leaves out its canned reply
    // (ChatReplyAction) to a real player's message of this chat type; the bot's map thread.
    struct ChatFilter
    {
        std::function<bool(Player* bot, Player* from, uint32 type, std::string const& msg)> claims;
        std::function<bool(Player* bot, uint32 type)> silences;
    };

    // From a module's loader.
    void AddChatFilter(ChatFilter filter);

    bool ClaimsChat(Player* bot, Player* from, uint32 type, std::string const& msg);
    bool SilencesChatReply(Player* bot, uint32 type);
}

#endif
