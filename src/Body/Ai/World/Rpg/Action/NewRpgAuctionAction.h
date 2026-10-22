/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGAUCTIONACTION_H
#define PLAYERBOTS_NEWRPGAUCTIONACTION_H

#include "NewRpgBaseAction.h"

namespace BotAuction
{
struct Spot;
struct TripState;
}  // namespace BotAuction

// Auction house trip (Cortex.Bots.NewRpgAuction, see BotAuction.h): with loot worth listing, auction mail waiting or
// money to spend, walk to the nearest usable auctioneer, collect mail, sell and buy, collect what was bought.
class NewRpgAuctionAction : public NewRpgBaseAction
{
public:
    NewRpgAuctionAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg auction") {}
    bool isUseful() override;
    bool Execute(Event event) override;

private:
    static constexpr time_t CHECK_INTERVAL_SECONDS = 60;
    static constexpr time_t TRIP_TIMEOUT_SECONDS = 40 * MINUTE;
    static constexpr time_t OPERATION_TIMEOUT_SECONDS = 15;
    static constexpr time_t GIVE_UP_SECONDS = 3 * HOUR;
    static constexpr float ARRIVE_DISTANCE = 4.0f;
    static constexpr float APPROACH_DISTANCE = 30.0f;

    enum TripReason : uint32
    {
        REASON_MAIL = 0x1,  // auction mail to collect
        REASON_SELL = 0x2,  // loot worth listing
        REASON_SHOP = 0x4   // money to spend on upgrades
    };

    uint32 TripReasons();
    // Walk to dest, or to the object once it is in sight. True once within reach.
    bool Approach(WorldObject* object, float x, float y, float z);
    bool AtMailbox(BotAuction::TripState& state, BotAuction::Spot const& spot);
    bool AtAuctioneer(BotAuction::TripState& state, BotAuction::Spot const& spot);
    void EndTrip(BotAuction::TripState& state, char const* outcome, bool giveUp);
};

#endif
