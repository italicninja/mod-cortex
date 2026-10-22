/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTCRAFTACTION_H
#define PLAYERBOTS_BOTCRAFTACTION_H

#include <ctime>
#include <vector>

#include "NewRpgBaseAction.h"
#include "ObjectGuid.h"

class SpellInfo;

// Crafting for skill (Playerbots.Professions, docs/professions.md): every few minutes a random bot with a crafting
// profession looks for a recipe that can still raise its skill and that it has the materials, tools and, for an
// anvil or forge, the focus for. If there is one it stops and crafts until nothing is left to make, as a player
// does between quests. Before crafting, enchanters disenchant gear they have no use for and scribes mill herbs, so
// the recipes have dust, essence and pigment.
class BotCraftAction : public NewRpgBaseAction
{
public:
    BotCraftAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "bot craft") {}
    bool isUseful() override;
    bool Execute(Event event) override;

private:
    struct Recipe
    {
        SpellInfo const* spell = nullptr;
        Item* target = nullptr;      // enchanting: the bot's own gear
        GameObject* focus = nullptr; // anvil, forge
        int32 chance = 0;
    };

    Recipe Best();
    // A bag item to disenchant or mill now, and the spell for it; nullptr when there is none.
    Item* PrepTarget(uint32& spellId);
    void EndSession();

    time_t nextCheck = 0;
    time_t sessionEnd = 0;
    uint32 crafted = 0;
    time_t prepAt = 0;                   // last disenchant or mill cast: its loot is taken before the next
    std::vector<ObjectGuid> prepFailed;  // items a disenchant or mill cast failed on: not tried again
};

#endif
