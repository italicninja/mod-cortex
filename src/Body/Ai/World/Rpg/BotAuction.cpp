/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotAuction.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "AuctionHouseMgr.h"
#include "Bag.h"
#include "BotProfessions.h"
#include "BotTelemetry.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Mail.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "Playerbots.h"
#include "RandomItemMgr.h"
#include "RandomPlayerbotMgr.h"
#include "StatsWeightCalculator.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace BotAuction
{
namespace
{
constexpr float MAILBOX_RANGE = 150.0f;         // mailbox must be this close to the auctioneer
constexpr uint32 AUCTION_MINUTES = 24 * 60;      // listing duration (12, 24 or 48 hours)
constexpr time_t SPOT_CACHE_SECONDS = 30;
constexpr uint64 MIN_LISTING_VALUE = 5 * SILVER;  // estimated stack value worth a listing
constexpr uint32 BAG_RESERVE_COPPER_PER_LEVEL2 = 10;  // money kept back for repairs: level² × this

std::atomic<bool> loaded{false};
Settings settings;
std::vector<Spot> spots;

std::mutex stateMutex;
std::unordered_map<ObjectGuid::LowType, TripState> states;

// Nearest-spot cache, per bot (HoldForAuction runs once per bag item during a vendor sale)
struct SpotCache
{
    int32 spot = -1;
    time_t checkedAt = 0;
    uint32 map = 0;
};
std::unordered_map<ObjectGuid::LowType, SpotCache> spotCache;

time_t Now() { return GameTime::GetGameTime().count(); }

void LoadSettings()
{
    settings.enabled = sConfigMgr->GetOption<bool>("Cortex.Bots.NewRpgAuction", false);
    settings.minLevel = sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgAuctionMinLevel", 8);
    settings.tripMinutes = sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgAuctionTripMinutes", 60);
    settings.maxDistance = sConfigMgr->GetOption<float>("Cortex.Bots.NewRpgAuctionMaxDistance", 4000.0f);
    settings.maxListings = sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgAuctionMaxListings", 8);
    settings.maxActive = sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgAuctionMaxActive", 16);
    settings.pricePct = sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgAuctionPricePct", 100);
    settings.spendPct = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgAuctionSpendPct", 50));
    settings.maxBuys = sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgAuctionMaxBuys", 2);
    settings.firstWandPct =
        std::min<uint32>(100, sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgAuctionFirstWandPct", 0));

    settings.skipZones.clear();
    std::string zones = sConfigMgr->GetOption<std::string>("Cortex.Bots.NewRpgAuctionSkipZones", "1497,1638");
    for (std::string_view token : Acore::Tokenize(zones, ',', false))
    {
        if (Optional<uint32> zone = Acore::StringTo<uint32>(token))
            settings.skipZones.push_back(*zone);
    }
}

template <typename Fn>
void ForEachBagItem(Player* bot, Fn&& fn)
{
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            fn(item);
    }

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = bot->GetBagByPos(bagSlot);
        if (!bag)
            continue;

        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
        {
            if (Item* item = bag->GetItemByPos(slot))
                fn(item);
        }
    }
}

// Slots of the smallest equipped bag; 0 when a bag slot is empty.
uint32 SmallestBag(Player* bot)
{
    uint32 smallest = UINT32_MAX;
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = bot->GetBagByPos(bagSlot);
        smallest = std::min<uint32>(smallest, bag ? bag->GetBagSize() : 0);
    }
    return smallest;
}

uint32 RoundPrice(uint32 copper)
{
    if (copper >= GOLD)
        return copper / SILVER * SILVER;
    if (copper >= SILVER)
        return copper / 5 * 5;
    return std::max<uint32>(copper, 1);
}

// Professions (BotProfessions): share of what a bot can spare that goes on materials and resale per visit.
constexpr int64 MATERIAL_SPEND_PCT = 30;
constexpr uint32 MATERIAL_CRAFTS = 5;               // reagents for this many crafts of a recipe
constexpr size_t MATERIAL_RECIPES_CONSIDERED = 5;
constexpr uint32 MATERIAL_RECIPES_BOUGHT = 2;
constexpr int64 RESELL_MARGIN_PCT = 5;
constexpr uint32 RESELL_BUYS = 3;

int64 SpendBudget(Player* bot)
{
    int64 level = bot->GetLevel();
    int64 spare = static_cast<int64>(bot->GetMoney()) - level * level * BAG_RESERVE_COPPER_PER_LEVEL2;
    return spare > 0 ? spare * settings.spendPct / 100 : 0;
}

// A priest, mage or warlock with nothing in its ranged slot: when its mana runs out it has nothing to fight with.
// A player buys the first wand before most else; a Lesser Magic Wand (about 12 silver) is more than most of these
// bots' budget (on 2026-10-20 none of 66 Cortex priests and one of 71 mages had a wand, priests carried 5 silver
// on average). Its first wand may take NewRpgAuctionFirstWandPct % of all its money, repair reserve or not.
bool FirstWand(Player* bot, ItemTemplate const* proto)
{
    return settings.firstWandPct && proto && proto->Class == ITEM_CLASS_WEAPON &&
           proto->SubClass == ITEM_SUBCLASS_WEAPON_WAND && bot->HasSkill(SKILL_WANDS) &&
           !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
}

int64 FirstWandBudget(Player* bot) { return static_cast<int64>(bot->GetMoney()) * settings.firstWandPct / 100; }

ItemUsage UsageOf(PlayerbotAI* botAI, uint32 entry, int32 randomPropertyId = 0)
{
    std::string qualifier = std::to_string(entry);
    if (randomPropertyId)
        qualifier += "," + std::to_string(randomPropertyId);
    return botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", qualifier)->Get();
}

// Finish a world-thread operation: the bot's map thread picks the result up from its trip state.
void MarkDone(Player* bot)
{
    TripState& state = State(bot);
    state.opDone = true;
}

class AuctionVisitOperation : public PlayerbotOperation
{
public:
    AuctionVisitOperation(ObjectGuid bot, ObjectGuid auctioneer) : _bot(bot), _auctioneer(auctioneer) {}

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindPlayer(_bot);
        if (!bot)
            return false;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        Creature* auctioneer = bot->GetNPCIfCanInteractWith(_auctioneer, UNIT_NPC_FLAG_AUCTIONEER);
        AuctionHouseObject* house = auctioneer ? sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction()) : nullptr;
        if (!botAI || !house)
        {
            MarkDone(bot);
            return false;
        }

        int64 moneyBefore = bot->GetMoney();
        std::vector<Listing> toList = CollectListings(botAI, bot);
        std::unordered_map<uint32, uint32> lowestUnit;
        for (Listing const& listing : toList)
            lowestUnit.emplace(listing.entry, UINT32_MAX);

        uint32 own = 0;
        std::vector<Candidate> candidates;
        uint32 smallestBag = SmallestBag(bot);
        for (auto const& [id, auction] : house->GetAuctions())
        {
            if (auction->owner == bot->GetGUID())
            {
                ++own;
                continue;
            }

            if (!auction->itemCount)
                continue;

            auto it = lowestUnit.find(auction->item_template);
            if (it != lowestUnit.end() && auction->buyout)
                it->second = std::min(it->second, auction->buyout / auction->itemCount);

            if (auction->buyout && auction->buyout <= bot->GetMoney())
                ConsiderBuying(bot, auction, smallestBag, candidates);
        }

        uint32 listed = List(botAI, bot, toList, lowestUnit, own);
        uint32 bought = Buy(botAI, bot, house, candidates);
        // Materials for the skill, then cheap listings to sell on to a vendor, from one shared budget.
        int64 budget = BotProfessions::Enabled() ? BotProfessions::Spare(bot) * MATERIAL_SPEND_PCT / 100 : 0;
        bought += BuyMaterials(botAI, bot, house, budget);
        bought += BuyToResell(botAI, bot, house, budget);

        TripState& state = State(bot);
        state.activeAuctions = own + listed;
        state.bought = bought;
        state.opDone = true;
        BotTelemetry::instance().Event(botAI, "ah", "visit", static_cast<int32>(listed), static_cast<int32>(bought),
                                       static_cast<int32>(static_cast<int64>(bot->GetMoney()) - moneyBefore));
        return true;
    }

    ObjectGuid GetBotGuid() const override { return _bot; }
    std::string GetName() const override { return "AuctionVisitOperation"; }
    bool IsValid() const override { return ObjectAccessor::FindPlayer(_bot) != nullptr; }

private:
    struct Listing
    {
        ObjectGuid item;
        uint32 entry;
        uint32 value;  // estimated value of the whole stack, to list the best items first
    };

    struct Candidate
    {
        uint32 auctionId;
        uint32 entry;
        int32 randomPropertyId;
        uint32 buyout;
    };

    static std::vector<Listing> CollectListings(PlayerbotAI* botAI, Player* bot)
    {
        std::vector<Listing> listings;
        ForEachBagItem(bot, [&](Item* item)
        {
            if (WantsToList(botAI, item))
                listings.push_back({item->GetGUID(), item->GetEntry(), UnitValue(item->GetTemplate()) * item->GetCount()});
        });
        std::sort(listings.begin(), listings.end(),
                  [](Listing const& a, Listing const& b) { return a.value > b.value; });
        return listings;
    }

    static void ConsiderBuying(Player* bot, AuctionEntry const* auction, uint32 smallestBag,
                               std::vector<Candidate>& candidates)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(auction->item_template);
        if (!proto || proto->RequiredLevel > bot->GetLevel() || bot->CanUseItem(proto) != EQUIP_ERR_OK)
            return;

        switch (proto->Class)
        {
            case ITEM_CLASS_ARMOR:
                if (!sRandomItemMgr.CanEquipArmor(proto, bot->getClass(), bot->GetLevel()))
                    return;
                break;
            case ITEM_CLASS_WEAPON:
                if (!sRandomItemMgr.CanEquipWeapon(proto, bot->getClass()))
                    return;
                break;
            case ITEM_CLASS_CONTAINER:
                if (proto->SubClass != ITEM_SUBCLASS_CONTAINER || proto->ContainerSlots <= smallestBag)
                    return;
                break;
            default:
                return;
        }

        int32 randomPropertyId = 0;
        if (Item* item = sAuctionMgr->GetAItem(auction->item_guid))
            randomPropertyId = item->GetItemRandomPropertyId();

        candidates.push_back({auction->Id, proto->ItemId, randomPropertyId, auction->buyout});
    }

    uint32 List(PlayerbotAI* botAI, Player* bot, std::vector<Listing> const& toList,
                std::unordered_map<uint32, uint32> const& lowestUnit, uint32 own)
    {
        uint32 listed = 0;
        for (Listing const& listing : toList)
        {
            if (listed >= settings.maxListings || own + listed >= settings.maxActive)
                break;

            Item* item = bot->GetItemByGuid(listing.item);
            if (!item)
                continue;

            ItemTemplate const* proto = item->GetTemplate();
            uint32 count = item->GetCount();
            uint32 entry = proto->ItemId;

            // Undercut the cheapest other seller a little, but never below 1.2 × the vendor price
            uint32 unit = UnitValue(proto);
            auto lowest = lowestUnit.find(entry);
            if (lowest != lowestUnit.end() && lowest->second < unit)
                unit = std::max<uint32>(lowest->second * 97 / 100, proto->SellPrice * 6 / 5);
            unit = std::max<uint32>(1, unit * urand(92, 108) / 100);

            uint32 buyout = RoundPrice(static_cast<uint32>(
                std::min<uint64>(static_cast<uint64>(unit) * count, MAX_MONEY_AMOUNT / 2)));
            uint32 bid = std::max<uint32>(1, buyout * urand(70, 85) / 100);

            WorldPacket packet(CMSG_AUCTION_SELL_ITEM, 8 + 4 + 8 + 4 + 4 + 4 + 4);
            packet << _auctioneer << uint32(1) << listing.item << count << bid << buyout << AUCTION_MINUTES;
            bot->GetSession()->HandleAuctionSellItem(packet);

            if (bot->GetItemByGuid(listing.item))
                continue;  // refused (deposit, stack size); stays in the bags

            ++listed;
            BotTelemetry::instance().Event(botAI, "ah", "list", static_cast<int32>(entry), static_cast<int32>(count),
                                           static_cast<int32>(buyout));
        }
        return listed;
    }

    // An auction found earlier, by id: a buyout removes the entry, so it is looked up again before buying.
    struct Offer
    {
        uint32 id;
        uint32 entry;
        uint32 count;
        uint32 buyout;
    };

    // Buyout through the stock handler; false when gone, changed or refused (same account, no money).
    bool BuyOut(Player* bot, AuctionHouseObject* house, Offer const& offer)
    {
        AuctionEntry* auction = house->GetAuction(offer.id);
        if (!auction || auction->buyout != offer.buyout)
            return false;
        uint32 moneyBefore = bot->GetMoney();
        WorldPacket packet(CMSG_AUCTION_PLACE_BID, 8 + 4 + 4);
        packet << _auctioneer << offer.id << offer.buyout;
        bot->GetSession()->HandleAuctionPlaceBid(packet);
        return bot->GetMoney() < moneyBefore;
    }

    // Reagents for the recipes most likely to raise the bot's skill, at no more than a fair price (half again what
    // bots list them at). A recipe is bought for only when all it lacks fits the budget.
    uint32 BuyMaterials(PlayerbotAI* botAI, Player* bot, AuctionHouseObject* house, int64& budget)
    {
        if (budget <= 0)
            return 0;
        std::vector<BotProfessions::Shopping> list = BotProfessions::ShoppingList(bot, MATERIAL_CRAFTS);
        if (list.size() > MATERIAL_RECIPES_CONSIDERED)
            list.resize(MATERIAL_RECIPES_CONSIDERED);
        if (list.empty())
            return 0;

        std::unordered_map<uint32, std::vector<Offer>> offers;
        for (auto const& entry : list)
            for (auto const& [item, count] : entry.missing)
                offers.emplace(item, std::vector<Offer>());
        for (auto const& [id, auction] : house->GetAuctions())
        {
            auto it = offers.find(auction->item_template);
            if (it == offers.end() || !auction->buyout || !auction->itemCount || auction->owner == bot->GetGUID())
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(auction->item_template);
            if (!proto || auction->buyout / auction->itemCount > UnitValue(proto) * 3 / 2)
                continue;
            it->second.push_back({auction->Id, auction->item_template, auction->itemCount, auction->buyout});
        }
        for (auto& [item, found] : offers)
            std::sort(found.begin(), found.end(),
                      [](Offer const& a, Offer const& b)
                      {
                          return static_cast<uint64>(a.buyout) * b.count < static_cast<uint64>(b.buyout) * a.count;
                      });

        uint32 bought = 0;
        uint32 recipes = 0;
        std::unordered_set<uint32> used;  // auctions already picked for an earlier recipe
        for (auto const& entry : list)
        {
            if (recipes >= MATERIAL_RECIPES_BOUGHT)
                break;
            std::vector<Offer> picks;
            int64 cost = 0;
            bool complete = true;
            for (auto const& [item, count] : entry.missing)
            {
                uint32 got = 0;
                for (Offer const& offer : offers[item])
                {
                    if (got >= count)
                        break;
                    if (used.count(offer.id))
                        continue;
                    picks.push_back(offer);
                    got += offer.count;
                    cost += offer.buyout;
                }
                complete = complete && got >= count;
            }
            if (!complete || cost > budget)
                continue;

            ++recipes;
            for (Offer const& offer : picks)
            {
                used.insert(offer.id);
                if (!BuyOut(bot, house, offer))
                    continue;
                ++bought;
                budget -= offer.buyout;
                BotTelemetry::instance().Event(botAI, "ah", "buy_material", static_cast<int32>(offer.entry),
                                               static_cast<int32>(offer.buyout), static_cast<int32>(entry.spellId));
            }
        }
        return bought;
    }

    // Listings below what a vendor pays for them: bought to sell on, as players do.
    uint32 BuyToResell(PlayerbotAI* botAI, Player* bot, AuctionHouseObject* house, int64& budget)
    {
        if (budget <= 0)
            return 0;
        std::vector<std::pair<Offer, int64>> deals;  // with the profit at the vendor
        for (auto const& [id, auction] : house->GetAuctions())
        {
            if (!auction->buyout || !auction->itemCount || auction->owner == bot->GetGUID() || auction->buyout > budget)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(auction->item_template);
            if (!proto || !proto->SellPrice)
                continue;
            int64 vendor = static_cast<int64>(proto->SellPrice) * auction->itemCount;
            int64 profit = vendor - static_cast<int64>(auction->buyout) * (100 + RESELL_MARGIN_PCT) / 100;
            if (profit > 0)
                deals.push_back({{auction->Id, auction->item_template, auction->itemCount, auction->buyout}, profit});
        }
        std::sort(deals.begin(), deals.end(), [](auto const& a, auto const& b) { return a.second > b.second; });

        uint32 bought = 0;
        for (auto const& [offer, profit] : deals)
        {
            if (bought >= RESELL_BUYS)
                break;
            if (offer.buyout > budget || !BuyOut(bot, house, offer))
                continue;
            ++bought;
            budget -= offer.buyout;
            BotTelemetry::instance().Event(botAI, "ah", "buy_resell", static_cast<int32>(offer.entry),
                                           static_cast<int32>(offer.buyout), static_cast<int32>(profit));
        }
        return bought;
    }

    uint32 Buy(PlayerbotAI* botAI, Player* bot, AuctionHouseObject* house, std::vector<Candidate> const& candidates)
    {
        int64 budget = SpendBudget(bot);
        int64 wandBudget = FirstWandBudget(bot);
        if ((!budget && !wandBudget) || candidates.empty() || !settings.maxBuys)
            return 0;

        StatsWeightCalculator calculator(bot);
        calculator.SetItemSetBonus(false);
        calculator.SetOverflowPenalty(false);
        if (sRandomPlayerbotMgr.IsSpecPvp(bot->GetGUID().GetCounter(), bot->getClass()))
            calculator.SetPvpSpec(true);

        auto slotScore = [&](uint8 slot) -> float
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            return item ? calculator.CalculateItem(item->GetEntry(), item->GetItemRandomPropertyId()) : 0.0f;
        };

        struct Scored
        {
            Candidate const* candidate;
            uint8 slot;
            float gain;
        };
        std::vector<Scored> scored;
        for (Candidate const& candidate : candidates)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(candidate.entry);
            if (!proto || candidate.buyout > (FirstWand(bot, proto) ? std::max(budget, wandBudget) : budget))
                continue;

            if (proto->Class == ITEM_CLASS_CONTAINER)
            {
                // Bags first: fresh-start bots only have a backpack. More slots, then cheaper, wins.
                scored.push_back({&candidate, INVENTORY_SLOT_BAG_START,
                                  1000000.0f + proto->ContainerSlots * 1000.0f - candidate.buyout / 100.0f});
                continue;
            }

            uint8 slot = botAI->FindEquipSlot(proto, NULL_SLOT, true);
            if (slot == NULL_SLOT)
                continue;

            float newScore = calculator.CalculateItem(candidate.entry, candidate.randomPropertyId);
            float oldScore = slotScore(slot);
            if (slot == EQUIPMENT_SLOT_FINGER1 || slot == EQUIPMENT_SLOT_TRINKET1)
                oldScore = std::min(oldScore, slotScore(slot + 1));

            if (newScore <= 0.0f || newScore <= oldScore * sPlayerbotAIConfig.EquipUpgradeThreshold)
                continue;

            scored.push_back({&candidate, slot, newScore - oldScore});
        }

        std::sort(scored.begin(), scored.end(), [](Scored const& a, Scored const& b) { return a.gain > b.gain; });

        uint32 bought = 0;
        std::unordered_set<uint8> slotsTaken;
        for (Scored const& entry : scored)
        {
            if (bought >= settings.maxBuys)
                break;

            Candidate const& candidate = *entry.candidate;
            bool firstWand = FirstWand(bot, sObjectMgr->GetItemTemplate(candidate.entry));
            if (candidate.buyout > (firstWand ? std::max(budget, wandBudget) : budget) || slotsTaken.count(entry.slot))
                continue;

            // The full usage check (unique-equipped, two-handers, dual wield) only for what would be bought
            if (entry.slot != INVENTORY_SLOT_BAG_START)
            {
                ItemUsage usage = UsageOf(botAI, candidate.entry, candidate.randomPropertyId);
                if (usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_REPLACE)
                    continue;
            }

            AuctionEntry* auction = house->GetAuction(candidate.auctionId);
            if (!auction || auction->buyout != candidate.buyout)
                continue;

            uint32 moneyBefore = bot->GetMoney();
            WorldPacket packet(CMSG_AUCTION_PLACE_BID, 8 + 4 + 4);
            packet << _auctioneer << candidate.auctionId << candidate.buyout;
            bot->GetSession()->HandleAuctionPlaceBid(packet);
            if (bot->GetMoney() >= moneyBefore)
                continue;  // refused (e.g. another character on the same account listed it)

            ++bought;
            budget -= candidate.buyout;
            wandBudget -= candidate.buyout;
            slotsTaken.insert(entry.slot);
            BotTelemetry::instance().Event(botAI, "ah", firstWand ? "buy_wand" : "buy", static_cast<int32>(candidate.entry),
                                           static_cast<int32>(candidate.buyout), static_cast<int32>(entry.gain));
        }
        return bought;
    }

    ObjectGuid _bot;
    ObjectGuid _auctioneer;
};

class MailboxVisitOperation : public PlayerbotOperation
{
public:
    MailboxVisitOperation(ObjectGuid bot, ObjectGuid mailbox) : _bot(bot), _mailbox(mailbox) {}

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindPlayer(_bot);
        if (!bot)
            return false;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI || !bot->GetGameObjectIfCanInteractWith(_mailbox, GAMEOBJECT_TYPE_MAILBOX))
        {
            MarkDone(bot);
            return false;
        }

        struct Pending
        {
            uint32 id;
            std::vector<ObjectGuid::LowType> items;
        };
        std::vector<Pending> pending;
        time_t now = Now();
        for (Mail const* mail : bot->GetMails())
        {
            if (!mail || mail->messageType != MAIL_AUCTION || mail->state == MAIL_STATE_DELETED ||
                mail->deliver_time > now || mail->COD)
                continue;

            Pending entry{mail->messageID, {}};
            for (MailItemInfo const& info : mail->items)
                entry.items.push_back(info.item_guid);
            pending.push_back(std::move(entry));
        }

        int64 moneyBefore = bot->GetMoney();
        uint32 itemsTaken = 0;
        WorldSession* session = bot->GetSession();
        for (Pending const& entry : pending)
        {
            Mail* mail = bot->GetMail(entry.id);
            if (mail && mail->money)
            {
                WorldPacket packet(CMSG_MAIL_TAKE_MONEY, 8 + 4);
                packet << _mailbox << entry.id;
                session->HandleMailTakeMoney(packet);
            }

            for (ObjectGuid::LowType itemGuid : entry.items)
            {
                WorldPacket packet(CMSG_MAIL_TAKE_ITEM, 8 + 4 + 4);
                packet << _mailbox << entry.id << uint32(itemGuid);
                session->HandleMailTakeItem(packet);
                mail = bot->GetMail(entry.id);
                if (mail && std::any_of(mail->items.begin(), mail->items.end(),
                                        [&](MailItemInfo const& info) { return info.item_guid == itemGuid; }))
                    break;  // bags full: leave the rest for the next trip
                ++itemsTaken;
            }

            mail = bot->GetMail(entry.id);
            if (mail && mail->state != MAIL_STATE_DELETED && !mail->money && mail->items.empty())
            {
                WorldPacket packet(CMSG_MAIL_DELETE, 8 + 4 + 4);
                packet << _mailbox << entry.id << uint32(mail->mailTemplateId);
                session->HandleMailDelete(packet);
            }
        }

        TripState& state = State(bot);
        state.equipPending = itemsTaken > 0;
        state.opDone = true;
        if (!pending.empty())
            BotTelemetry::instance().Event(botAI, "ah", "mail",
                                           static_cast<int32>(static_cast<int64>(bot->GetMoney()) - moneyBefore),
                                           static_cast<int32>(itemsTaken), static_cast<int32>(pending.size()));
        return true;
    }

    ObjectGuid GetBotGuid() const override { return _bot; }
    std::string GetName() const override { return "MailboxVisitOperation"; }
    bool IsValid() const override { return ObjectAccessor::FindPlayer(_bot) != nullptr; }

private:
    ObjectGuid _bot;
    ObjectGuid _mailbox;
};
}  // namespace

Settings const& Config()
{
    static Settings const disabled;
    return loaded.load(std::memory_order_acquire) ? settings : disabled;
}

void LoadCatalog()
{
    if (loaded.load(std::memory_order_acquire))
        return;

    LoadSettings();

    struct Mailbox
    {
        ObjectGuid::LowType spawn;
        uint32 map;
        float x, y, z;
    };
    std::vector<Mailbox> mailboxes;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        GameObjectTemplate const* goInfo = sObjectMgr->GetGameObjectTemplate(data.id);
        if (goInfo && goInfo->type == GAMEOBJECT_TYPE_MAILBOX)
            mailboxes.push_back({spawnId, data.mapid, data.posX, data.posY, data.posZ});
    }

    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(data.id);
        if (!cInfo || !(cInfo->npcflag & UNIT_NPC_FLAG_AUCTIONEER) ||
            !AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(cInfo->faction))
            continue;

        Spot spot;
        spot.auctioneerSpawn = spawnId;
        spot.map = data.mapid;
        spot.x = data.posX;
        spot.y = data.posY;
        spot.z = data.posZ;
        spot.factionTemplate = cInfo->faction;

        float best = MAILBOX_RANGE * MAILBOX_RANGE;
        for (Mailbox const& box : mailboxes)
        {
            float dx = box.x - spot.x, dy = box.y - spot.y, dz = box.z - spot.z;
            float dist = dx * dx + dy * dy + dz * dz;
            if (box.map != spot.map || dist >= best)
                continue;

            best = dist;
            spot.mailboxSpawn = box.spawn;
            spot.mailX = box.x;
            spot.mailY = box.y;
            spot.mailZ = box.z;
        }
        if (!spot.mailboxSpawn)
            continue;  // sales money only arrives by mail

        spot.zone = sMapMgr->GetZoneId(data.phaseMask, data.mapid, data.posX, data.posY, data.posZ);
        spots.push_back(spot);
    }

    LOG_INFO("server.loading", ">> Playerbots auction trips: {} ({} auctioneers with a mailbox nearby)",
             settings.enabled ? "on" : "off", spots.size());
    loaded.store(true, std::memory_order_release);
}

std::vector<Spot> const& Spots() { return spots; }

TripState& State(Player* bot)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return states[bot->GetGUID().GetCounter()];
}

void Forget(Player* bot)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    states.erase(bot->GetGUID().GetCounter());
    spotCache.erase(bot->GetGUID().GetCounter());
}

int32 NearestSpot(Player* bot)
{
    time_t now = Now();
    SpotCache* cache;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        cache = &spotCache[bot->GetGUID().GetCounter()];
    }
    if (cache->map == bot->GetMapId() && now < cache->checkedAt + SPOT_CACHE_SECONDS)
        return cache->spot;

    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    int32 best = -1;
    float bestDistance = Config().maxDistance;
    for (size_t i = 0; botFaction && i < spots.size(); ++i)
    {
        Spot const& spot = spots[i];
        if (spot.map != bot->GetMapId() ||
            std::find(settings.skipZones.begin(), settings.skipZones.end(), spot.zone) != settings.skipZones.end())
            continue;

        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(spot.factionTemplate);
        if (!faction || botFaction->IsHostileTo(*faction))
            continue;

        float distance = bot->GetExactDist(spot.x, spot.y, spot.z);
        if (distance < bestDistance)
        {
            best = static_cast<int32>(i);
            bestDistance = distance;
        }
    }

    cache->spot = best;
    cache->checkedAt = now;
    cache->map = bot->GetMapId();
    return best;
}

bool Listable(Item* item)
{
    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
    if (!proto || item->IsSoulBound() || !item->CanBeTraded() || item->IsNotEmptyBag() ||
        proto->HasFlag(ITEM_FLAG_CONJURED) || item->GetUInt32Value(ITEM_FIELD_DURATION) || !proto->SellPrice ||
        proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM ||
        proto->Quality < ITEM_QUALITY_NORMAL)
        return false;

    switch (proto->Class)
    {
        case ITEM_CLASS_ARMOR:
        case ITEM_CLASS_WEAPON:
            return proto->Quality >= ITEM_QUALITY_UNCOMMON;  // white gear isn't worth a deposit
        case ITEM_CLASS_TRADE_GOODS:
        case ITEM_CLASS_RECIPE:
        case ITEM_CLASS_GEM:
            return true;
        case ITEM_CLASS_CONTAINER:
            return proto->SubClass == ITEM_SUBCLASS_CONTAINER;
        case ITEM_CLASS_CONSUMABLE:
            // Enchanting scrolls and mod-forge's crafting orbs (wowproject docs/forge-currency.md)
            return proto->SubClass == ITEM_SUBCLASS_ITEM_ENHANCEMENT;
        case ITEM_CLASS_MISC:
            return proto->Quality >= ITEM_QUALITY_UNCOMMON || proto->SubClass == ITEM_SUBCLASS_JUNK_PET ||
                   proto->SubClass == ITEM_SUBCLASS_JUNK_MOUNT;
        default:
            return false;
    }
}

bool WantsToList(PlayerbotAI* botAI, Item* item)
{
    // Below this the 1 silver minimum deposit (lost if it expires) eats the sale: vendor it instead
    if (!Listable(item) ||
        static_cast<uint64>(UnitValue(item->GetTemplate())) * item->GetCount() < MIN_LISTING_VALUE)
        return false;

    return UsageOf(botAI, item->GetEntry()) == ITEM_USAGE_AH;
}

bool HoldForAuction(PlayerbotAI* botAI, Item* item)
{
    Settings const& cfg = Config();
    Player* bot = botAI->GetBot();
    if (!cfg.enabled || bot->GetLevel() < cfg.minLevel || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    TripState& state = State(bot);
    time_t now = Now();
    if (now < state.giveUpUntil || state.activeAuctions >= cfg.maxActive)
        return false;

    // Bags full and the next trip still a while off: sell to the vendor rather than stop looting
    if (state.phase == Phase::NONE && now < state.lastTripAt + cfg.tripMinutes * MINUTE &&
        botAI->GetAiObjectContext()->GetValue<uint8>("bag space")->Get() >= sPlayerbotAIConfig.NewRpgUpkeepBagPct)
        return false;

    return NearestSpot(bot) >= 0 && WantsToList(botAI, item);
}

uint32 UnitValue(ItemTemplate const* proto)
{
    // Percent of the vendor price by quality: poor, common, uncommon, rare, epic, legendary, artifact, heirloom
    static constexpr uint32 QUALITY_PCT[MAX_ITEM_QUALITY] = {100, 300, 400, 600, 1000, 1500, 1500, 1500};

    uint64 value = static_cast<uint64>(proto->SellPrice) * QUALITY_PCT[std::min<uint32>(proto->Quality, 7)] / 100;
    if (proto->Class == ITEM_CLASS_TRADE_GOODS)
        value = value * 3 / 2;
    else if (proto->Class == ITEM_CLASS_RECIPE)
        value *= 2;

    value = value * Config().pricePct / 100;
    return static_cast<uint32>(std::clamp<uint64>(value, 1, MAX_MONEY_AMOUNT / 1000));
}

bool QueueAuctionVisit(Player* bot, ObjectGuid auctioneer)
{
    return PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<AuctionVisitOperation>(bot->GetGUID(), auctioneer));
}

bool QueueMailboxVisit(Player* bot, ObjectGuid mailbox)
{
    return PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<MailboxVisitOperation>(bot->GetGUID(), mailbox));
}

bool HasCollectibleMail(Player* bot)
{
    time_t now = Now();
    for (Mail const* mail : bot->GetMails())
    {
        if (mail && mail->messageType == MAIL_AUCTION && mail->state != MAIL_STATE_DELETED &&
            mail->deliver_time <= now && !mail->COD && (mail->money || !mail->items.empty()))
            return true;
    }
    return false;
}
}  // namespace BotAuction
