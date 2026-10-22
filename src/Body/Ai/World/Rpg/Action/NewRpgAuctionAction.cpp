/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgAuctionAction.h"

#include "Bag.h"
#include "BotAuction.h"
#include "BotTelemetry.h"
#include "Creature.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Item.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"

namespace
{
time_t Now() { return GameTime::GetGameTime().count(); }

Creature* SpawnedCreature(Player* bot, ObjectGuid::LowType spawnId)
{
    auto bounds = bot->GetMap()->GetCreatureBySpawnIdStore().equal_range(spawnId);
    for (auto it = bounds.first; it != bounds.second; ++it)
    {
        if (it->second && it->second->IsInWorld() && it->second->IsAlive())
            return it->second;
    }
    return nullptr;
}

GameObject* SpawnedGameObject(Player* bot, ObjectGuid::LowType spawnId)
{
    auto bounds = bot->GetMap()->GetGameObjectBySpawnIdStore().equal_range(spawnId);
    for (auto it = bounds.first; it != bounds.second; ++it)
    {
        if (it->second && it->second->IsInWorld() && it->second->isSpawned())
            return it->second;
    }
    return nullptr;
}
}  // namespace

uint32 NewRpgAuctionAction::TripReasons()
{
    uint32 reasons = 0;
    if (BotAuction::HasCollectibleMail(bot))
        reasons |= REASON_MAIL;

    // Loot worth listing: worth about level² × 5 copper, or the bags are filling up with it
    uint64 heldValue = 0;
    uint32 held = 0;
    auto visit = [&](Item* item)
    {
        if (BotAuction::WantsToList(botAI, item))
        {
            ++held;
            heldValue += static_cast<uint64>(BotAuction::UnitValue(item->GetTemplate())) * item->GetCount();
        }
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            visit(item);
    }
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = bot->GetBagByPos(bagSlot);
        for (uint32 slot = 0; bag && slot < bag->GetBagSize(); ++slot)
        {
            if (Item* item = bag->GetItemByPos(slot))
                visit(item);
        }
    }

    uint64 level = bot->GetLevel();
    if (held && (heldValue >= level * level * 5 || AI_VALUE(uint8, "bag space") >= sPlayerbotAIConfig.NewRpgUpkeepBagPct))
        reasons |= REASON_SELL;

    // Shopping on its own: about level² × 10 copper to spend, and only every other trip interval
    BotAuction::TripState& state = BotAuction::State(bot);
    uint64 spare = bot->GetMoney() > level * level * 10 ? bot->GetMoney() - level * level * 10 : 0;
    uint64 budget = spare * BotAuction::Config().spendPct / 100;
    if (budget >= level * level * 10 && Now() >= state.lastTripAt + 2 * BotAuction::Config().tripMinutes * MINUTE)
        reasons |= REASON_SHOP;

    return reasons;
}

bool NewRpgAuctionAction::isUseful()
{
    BotAuction::Settings const& cfg = BotAuction::Config();
    if (!cfg.enabled || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->GetGroup() ||
        bot->GetLevel() < cfg.minLevel || bot->GetMap()->Instanceable() || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    BotAuction::TripState& state = BotAuction::State(bot);
    if (state.phase != BotAuction::Phase::NONE)
        return true;

    time_t now = Now();
    // Trip state lives in memory: after a restart, spread the first trips over one interval instead of sending
    // every bot at once.
    if (!state.lastTripAt)
        state.lastTripAt = now - static_cast<time_t>(urand(0, BotAuction::Config().tripMinutes * MINUTE));

    if (now < state.nextCheckAt)
        return false;

    state.nextCheckAt = now + CHECK_INTERVAL_SECONDS;
    if (now < state.giveUpUntil || now < state.lastTripAt + cfg.tripMinutes * MINUTE)
        return false;

    uint32 reasons = TripReasons();
    if (!reasons)
        return false;

    int32 spotIndex = BotAuction::NearestSpot(bot);
    if (spotIndex < 0)
        return false;

    BotAuction::Spot const& spot = BotAuction::Spots()[spotIndex];
    state.spot = spotIndex;
    state.startedAt = now;
    state.bought = 0;
    state.opDone = false;
    state.phase = (reasons & REASON_MAIL) ? BotAuction::Phase::TO_MAILBOX : BotAuction::Phase::TO_AUCTIONEER;
    BotTelemetry::instance().Event(botAI, "ah", "trip", static_cast<int32>(bot->GetExactDist(spot.x, spot.y, spot.z)),
                                   static_cast<int32>(reasons), static_cast<int32>(spot.auctioneerSpawn));
    return true;
}

void NewRpgAuctionAction::EndTrip(BotAuction::TripState& state, char const* outcome, bool giveUp)
{
    time_t now = Now();
    BotTelemetry::instance().Event(botAI, "ah", outcome, static_cast<int32>(state.phase),
                                   static_cast<int32>(now - state.startedAt), static_cast<int32>(state.bought));
    state.phase = BotAuction::Phase::NONE;
    state.lastTripAt = now;
    if (giveUp)
        state.giveUpUntil = now + GIVE_UP_SECONDS;
}

bool NewRpgAuctionAction::Approach(WorldObject* object, float x, float y, float z)
{
    if (object && bot->GetDistance(object) <= ARRIVE_DISTANCE - 1.0f)
        return true;
    if (!object && bot->GetExactDist(x, y, z) <= ARRIVE_DISTANCE)
        return true;

    if (object && bot->GetDistance(object) <= APPROACH_DISTANCE)
    {
        MoveWorldObjectTo(object->GetGUID(), ARRIVE_DISTANCE / 2);
        return false;
    }

    WorldPosition dest(bot->GetMapId(), x, y, z);
    MoveFarTo(dest);

    // Walk unstick gave up on the spot (the trip isn't an rpg status, so its ChangeToIdle doesn't end it): end
    // the trip. Hearthing out of a dead end isn't a failure of the trip, so no long cooldown then.
    uint32 unreachableMs = sPlayerbotAIConfig.NewRpgUnreachableMinutes * MINUTE * IN_MILLISECONDS;
    if (sPlayerbotAIConfig.NewRpgWalkUnstick && botAI->rpgInfo.IsUnreachable(dest, unreachableMs))
        EndTrip(BotAuction::State(bot), "give_up", !bot->IsNonMeleeSpellCast(false));
    return false;
}

bool NewRpgAuctionAction::AtMailbox(BotAuction::TripState& state, BotAuction::Spot const& spot)
{
    BotAuction::Phase next =
        state.phase == BotAuction::Phase::TO_MAILBOX ? BotAuction::Phase::TO_AUCTIONEER : BotAuction::Phase::NONE;

    GameObject* mailbox = SpawnedGameObject(bot, spot.mailboxSpawn);
    if (!Approach(mailbox, spot.mailX, spot.mailY, spot.mailZ))
        return true;

    if (!mailbox)
    {
        // At its spawn point but not spawned: carry on without the mail
        if (next == BotAuction::Phase::NONE)
            EndTrip(state, "done", false);
        else
            state.phase = next;
        return true;
    }

    if (bot->isMoving())
    {
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
    }
    bot->SetFacingToObject(mailbox);

    state.opDone = false;
    state.waitSince = Now();
    state.afterWait = next;
    state.phase = BotAuction::Phase::WAITING;
    if (!BotAuction::QueueMailboxVisit(bot, mailbox->GetGUID()))
        state.opDone = true;
    return true;
}

bool NewRpgAuctionAction::AtAuctioneer(BotAuction::TripState& state, BotAuction::Spot const& spot)
{
    Creature* auctioneer = SpawnedCreature(bot, spot.auctioneerSpawn);
    if (!Approach(auctioneer, spot.x, spot.y, spot.z))
        return true;

    if (!auctioneer)
    {
        EndTrip(state, "give_up", true);
        return true;
    }

    if (bot->isMoving())
    {
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
    }
    bot->SetFacingToObject(auctioneer);

    state.opDone = false;
    state.waitSince = Now();
    state.afterWait = BotAuction::Phase::TO_MAILBOX_AFTER;
    state.phase = BotAuction::Phase::WAITING;
    if (!BotAuction::QueueAuctionVisit(bot, auctioneer->GetGUID()))
        state.opDone = true;
    return true;
}

bool NewRpgAuctionAction::Execute(Event /*event*/)
{
    BotAuction::TripState& state = BotAuction::State(bot);
    if (state.phase == BotAuction::Phase::NONE || state.spot < 0 ||
        state.spot >= static_cast<int32>(BotAuction::Spots().size()))
    {
        state.phase = BotAuction::Phase::NONE;
        return false;
    }

    BotAuction::Spot const& spot = BotAuction::Spots()[state.spot];
    time_t now = Now();
    if (bot->GetMapId() != spot.map || now > state.startedAt + TRIP_TIMEOUT_SECONDS)
    {
        EndTrip(state, "give_up", true);
        return false;
    }

    if (state.equipPending)
    {
        state.equipPending = false;
        botAI->DoSpecificAction("equip upgrade", Event(), true);
    }

    switch (state.phase)
    {
        case BotAuction::Phase::WAITING:
            if (!state.opDone)
            {
                if (now > state.waitSince + OPERATION_TIMEOUT_SECONDS)
                    EndTrip(state, "give_up", true);
                return true;
            }

            // Nothing bought: no reason to go back to the mailbox
            if (state.afterWait == BotAuction::Phase::TO_MAILBOX_AFTER && !state.bought)
                state.afterWait = BotAuction::Phase::NONE;

            if (state.afterWait == BotAuction::Phase::NONE)
                EndTrip(state, "done", false);
            else
                state.phase = state.afterWait;
            return true;
        case BotAuction::Phase::TO_MAILBOX:
        case BotAuction::Phase::TO_MAILBOX_AFTER:
            return AtMailbox(state, spot);
        case BotAuction::Phase::TO_AUCTIONEER:
            return AtAuctioneer(state, spot);
        default:
            return false;
    }
}
