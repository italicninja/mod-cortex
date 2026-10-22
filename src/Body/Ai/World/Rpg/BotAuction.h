/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTAUCTION_H
#define PLAYERBOTS_BOTAUCTION_H

#include "Common.h"
#include "ObjectGuid.h"
#include <vector>

class Item;
class Player;
class PlayerbotAI;
struct ItemTemplate;

// Auction house trips for new rpg random bots (Cortex.Bots.NewRpgAuction).
//
// A bot with loot worth listing, auction mail waiting, or money to spend walks to the nearest auctioneer it may use
// (own faction or neutral, same map, within NewRpgAuctionMaxDistance). On the way it stops at the mailbox next to
// the auction house to collect sales money and returned items; at the auctioneer it lists BoE gear, trade goods,
// recipes and bags it has no use for, and buys gear upgrades and bags within its budget; then it goes back to the
// mailbox for what it won. All auction house and mail work runs on the world thread through the stock packet
// handlers (deposit, cut, mail delays, same-account rules), queued with PlayerbotWorldThreadProcessor.
namespace BotAuction
{
struct Settings
{
    bool enabled = false;
    uint32 minLevel = 8;
    uint32 tripMinutes = 60;
    float maxDistance = 4000.0f;
    std::vector<uint32> skipZones;
    uint32 maxListings = 8;
    uint32 maxActive = 16;
    uint32 pricePct = 100;
    uint32 spendPct = 50;
    uint32 maxBuys = 2;
    uint32 firstWandPct = 0;
};

Settings const& Config();

// An auctioneer spawn bots can trade at, with the mailbox nearest to it.
struct Spot
{
    ObjectGuid::LowType auctioneerSpawn = 0;
    uint32 map = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32 factionTemplate = 0;
    uint32 zone = 0;
    ObjectGuid::LowType mailboxSpawn = 0;
    float mailX = 0.0f;
    float mailY = 0.0f;
    float mailZ = 0.0f;
};

enum class Phase : uint8
{
    NONE,
    TO_MAILBOX,        // collect auction mail before trading
    TO_AUCTIONEER,
    TO_MAILBOX_AFTER,  // collect what was bought
    WAITING            // a world-thread operation is running
};

// Per-bot trip state. Only the bot's own map thread and the world thread (while maps are idle) touch it.
struct TripState
{
    Phase phase = Phase::NONE;
    Phase afterWait = Phase::NONE;
    int32 spot = -1;
    time_t startedAt = 0;
    time_t waitSince = 0;
    time_t lastTripAt = 0;
    time_t nextCheckAt = 0;
    time_t giveUpUntil = 0;
    uint32 activeAuctions = 0;  // own listings at the last visit
    uint32 bought = 0;          // auctions won during this visit
    bool opDone = false;
    bool equipPending = false;  // run "equip upgrade" on the bot's next tick
};

// Fill the auctioneer/mailbox list from spawn data. World thread, once, after the world is loaded.
void LoadCatalog();
std::vector<Spot> const& Spots();

TripState& State(Player* bot);
void Forget(Player* bot);

// Nearest usable spot for the bot, or -1.
int32 NearestSpot(Player* bot);

// Whether a bag item could go on the auction house at all (tradeable, priced, and of a kind bots list).
bool Listable(Item* item);
// Listable and of no use to the bot (item usage "auction house").
bool WantsToList(PlayerbotAI* botAI, Item* item);
// Vendor selling keeps the item for the next auction trip (SellAction "vendor").
bool HoldForAuction(PlayerbotAI* botAI, Item* item);

// Asking price for one unit before market adjustment (vendor price × quality and kind multipliers).
uint32 UnitValue(ItemTemplate const* proto);

// Queue the world-thread work at an auctioneer / mailbox. The result lands in State(bot).
bool QueueAuctionVisit(Player* bot, ObjectGuid auctioneer);
bool QueueMailboxVisit(Player* bot, ObjectGuid mailbox);

// Auction mail (sales money, won or expired items) that can be taken now.
bool HasCollectibleMail(Player* bot);
}  // namespace BotAuction

#endif
