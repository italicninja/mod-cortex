/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUILDCREATEACTIONS_H
#define PLAYERBOTS_GUILDCREATEACTIONS_H

#include "ChooseTravelTargetAction.h"
#include "InventoryAction.h"
#include "NewRpgBaseAction.h"

class PlayerbotAI;

class BuyPetitionAction : public InventoryAction
{
public:
    BuyPetitionAction(PlayerbotAI* botAI) : InventoryAction(botAI, "buy petition") {}

    bool Execute(Event event) override;
    bool isUseful() override;

    static bool canBuyPetition(Player* bot);
};

// Guild charter helpers. Signatures come from sPetitionMgr: the core no longer writes
// petition_sign.petitionguid, so counting rows by that column always returned 0.
namespace GuildCharter
{
    Item* Get(Player* bot);
    uint32 Signatures(Player* bot);
    bool ReadyToTurnIn(Player* bot);
    Creature* NearbyGuildMaster(PlayerbotAI* botAI, Player* bot);
    bool FindGuildMaster(Player* bot, WorldPosition& pos);
}

class PetitionOfferAction : public Action
{
public:
    PetitionOfferAction(PlayerbotAI* botAI, std::string const name = "petition offer") : Action(botAI, name) {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class PetitionOfferNearbyAction : public PetitionOfferAction
{
public:
    PetitionOfferNearbyAction(PlayerbotAI* botAI) : PetitionOfferAction(botAI, "petition offer nearby") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    // Don't re-offer to someone who just declined or ignored the charter.
    std::map<ObjectGuid, time_t> offeredUntil;
};

class PetitionTurnInAction : public ChooseTravelTargetAction
{
public:
    PetitionTurnInAction(PlayerbotAI* botAI) : ChooseTravelTargetAction(botAI, "turn in petitn") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class BuyTabardAction : public ChooseTravelTargetAction
{
public:
    BuyTabardAction(PlayerbotAI* botAI) : ChooseTravelTargetAction(botAI, "buy tabard") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// Walks a charter founder to the nearest friendly guild master: first to buy the charter,
// later to turn it in once it has enough signatures. Built on the new rpg movement, since
// ChooseTravelTargetAction's travel targets are not followed when EnableNewRpgStrategy is on.
class GuildCharterErrandAction : public NewRpgBaseAction
{
public:
    GuildCharterErrandAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "guild charter errand") {}

    bool Execute(Event event) override;
    bool isUseful() override;

    static bool IsNeeded(Player* bot);
};

#endif
