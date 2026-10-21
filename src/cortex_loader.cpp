/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include "Chat.h"
#include "Player.h"
#include "PlayerScript.h"
#include "WorldSession.h"

void AddCortexScripts();

// The version this source is tagged with in https://github.com/italicninja/mod-cortex (bump it with each tag).
constexpr char const* CORTEX_VERSION = "v0.1.0";

namespace
{
    // Tells each real player at login that the realm runs this module, like mod-playerbots does. GPL-2.0: the link
    // must lead to the module's public source.
    class CortexLoginScript : public PlayerScript
    {
    public:
        CortexLoginScript() : PlayerScript("CortexLoginScript", {PLAYERHOOK_ON_LOGIN}) { }

        void OnPlayerLogin(Player* player) override
        {
            if (player->GetSession()->IsHeadless())
                return;
            std::string line = "|cff00ff00This server runs with |cff00ccffmod-cortex|r |cffffd100";
            line += CORTEX_VERSION;
            line += "|r |cffcccccchttps://github.com/italicninja/mod-cortex|r";
            ChatHandler(player->GetSession()).SendSysMessage(line);
        }
    };
}

// Called by the core's module loader (Add<module name with '-' as '_'>Scripts).
void Addmod_cortexScripts()
{
    AddCortexScripts();
    new CortexLoginScript();
}
