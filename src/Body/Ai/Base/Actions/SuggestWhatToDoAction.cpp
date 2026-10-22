/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SuggestWhatToDoAction.h"
#include "AiFactory.h"
#include "BroadcastHelper.h"
#include "Channel.h"
#include "ChatHelper.h"
#include "Event.h"
#include "ItemVisitors.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include <functional>

enum eTalkType
{
    /*0x18*/ General = ChannelFlags::CHANNEL_FLAG_GENERAL | ChannelFlags::CHANNEL_FLAG_NOT_LFG,
    /*0x3C*/ Trade = ChannelFlags::CHANNEL_FLAG_CITY | ChannelFlags::CHANNEL_FLAG_GENERAL |
                     ChannelFlags::CHANNEL_FLAG_NOT_LFG | ChannelFlags::CHANNEL_FLAG_TRADE,
    /*0x18*/ LocalDefence = ChannelFlags::CHANNEL_FLAG_GENERAL | ChannelFlags::CHANNEL_FLAG_NOT_LFG,
    /*x038*/ GuildRecruitment =
        ChannelFlags::CHANNEL_FLAG_CITY | ChannelFlags::CHANNEL_FLAG_GENERAL | ChannelFlags::CHANNEL_FLAG_NOT_LFG,
    /*0x50*/ LookingForGroup = ChannelFlags::CHANNEL_FLAG_LFG | ChannelFlags::CHANNEL_FLAG_GENERAL
};

std::map<std::string, uint8> SuggestDungeonAction::instances;

SuggestWhatToDoAction::SuggestWhatToDoAction(PlayerbotAI* botAI, std::string const name)
    : InventoryAction{botAI, name}, _dbc_locale{sWorld->GetDefaultDbcLocale()}
{
    suggestions.push_back(std::bind(&SuggestWhatToDoAction::specificQuest, this));
    suggestions.push_back(std::bind(&SuggestWhatToDoAction::grindReputation, this));
    suggestions.push_back(std::bind(&SuggestWhatToDoAction::something, this));
    suggestions.push_back(std::bind(&SuggestWhatToDoAction::grindMaterials, this));
    suggestions.push_back(std::bind(&SuggestWhatToDoAction::somethingToxic, this));
    suggestions.push_back(std::bind(&SuggestWhatToDoAction::toxicLinks, this));
}

bool SuggestWhatToDoAction::isUseful()
{
    if (!sRandomPlayerbotMgr.IsRandomBot(bot) || bot->GetGroup() || bot->GetInstanceId() || bot->GetBattleground())
        return false;

    std::string qualifier = "suggest what to do";
    time_t lastSaid = AI_VALUE2(time_t, "last said", qualifier);
    return (time(0) - lastSaid) > 30;
}

bool SuggestWhatToDoAction::Execute(Event /*event*/)
{
    uint32 index = rand() % suggestions.size();
    auto fnct_ptr = suggestions[index];
    fnct_ptr();

    std::string const qualifier = "suggest what to do";
    botAI->GetAiObjectContext()->GetValue<time_t>("last said", qualifier)->Set(time(nullptr) + urand(1, 60));

    return true;
}

std::vector<uint32> SuggestWhatToDoAction::GetIncompletedQuests()
{
    std::vector<uint32> result;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);
        if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_NONE)
            result.push_back(questId);
    }

    return result;
}

void SuggestWhatToDoAction::specificQuest()
{
    std::vector<uint32> quests = GetIncompletedQuests();
    if (quests.empty())
        return;

    BroadcastHelper::BroadcastSuggestQuest(botAI, quests, bot);
}

void SuggestWhatToDoAction::grindMaterials()
{
    /*if (bot->GetLevel() <= 5)
        return;

    auto result = CharacterDatabase.Query("SELECT distinct category, multiplier FROM ahbot_category where category not
    in ('other', 'quest', 'trade', 'reagent') and multiplier > 3 order by multiplier desc limit 10"); if (!result)
        return;

    std::map<std::string, double> categories;
    do
    {
        Field* fields = result->Fetch();
        categories[fields[0].Get<std::string>()] = fields[1].Get<float>();
    } while (result->NextRow());

    for (std::map<std::string, double>::iterator i = categories.begin(); i != categories.end(); ++i)
    {
        if (urand(0, 10) < 3)
        {
            std::string name = i->first;
            double multiplier = i->second;

            for (int j = 0; j < ahbot::CategoryList::instance.size(); j++)
            {
                ahbot::Category* category = ahbot::CategoryList::instance[j];
                if (name == category->GetName())
                {
                    std::string item = category->GetLabel();
                    transform(item.begin(), item.end(), item.begin(), ::tolower);
                    std::ostringstream itemout;
                    itemout << "|c0000b000" << item << "|r";
                    item = itemout.str();

                    std::map<std::string, std::string> placeholders;
                    placeholders["%role"] = chat->formatClass(bot, AiFactory::GetPlayerSpecTab(bot));
                    placeholders["%category"] = item;

                    spam(PlayerbotTextMgr::instance().GetBotText("suggest_trade", placeholders), urand(0, 1) ? 0x3C : 0x18, !urand(0, 2), !urand(0,
    3)); return;
                }
            }
        }
    }*/
}

// Reputation factions worth suggesting: the level a bot should be before mentioning them, and the expansion
// that added them (0 classic, 1 TBC, 2 WotLK).
struct ReputationFaction
{
    uint32 factionId;
    uint8 minLevel;
    uint8 expansion;
};

static std::vector<ReputationFaction> const reputationFactions = {
    {529, 60, 0}, // Argent Dawn
    {87, 40, 0}, // Bloodsail Buccaneers
    {910, 60, 0}, // Brood of Nozdormu
    {609, 55, 0}, // Cenarion Circle
    {909, 20, 0}, // Darkmoon Faire
    {749, 60, 0}, // Hydraxian Waterlords
    {349, 20, 0}, // Ravenholdt
    {59, 40, 0}, // Thorium Brotherhood
    {576, 50, 0}, // Timbermaw Hold
    {589, 50, 0}, // Wintersaber Trainers
    {21, 30, 0}, // Booty Bay
    {577, 40, 0}, // Everlook
    {369, 50, 0}, // Gadgetzan
    {470, 20, 0}, // Ratchet

    {1012, 70, 1}, // Ashtongue Deathsworn
    {942, 62, 1}, // Cenarion Expedition
    {933, 65, 1}, // The Consortium
    {946, 66, 1}, // Honor Hold
    {947, 66, 1}, // Thrallmar
    {989, 68, 1}, // Keepers of Time
    {1015, 65, 1}, // Netherwing
    {1038, 65, 1}, // Ogri'la
    {990, 65, 1}, // The Scale of the Sands
    {970, 65, 1}, // Sporeggar
    {922, 10, 1}, // Tranquillien
    {967, 70, 1}, // The Violet Eye

    {1106, 75, 2}, // Argent Crusade
    {1156, 75, 2}, // The Ashen Verdict
    {1073, 72, 2}, // The Kalu'ak
    {1090, 75, 2}, // Kirin Tor
    {1098, 77, 2}, // Knights of the Ebon Blade
    {1119, 78, 2}, // The Sons of Hodir
    {1091, 77, 2}, // The Wyrmrest Accord
};

// A faction the bot's side can't earn: its Faction.dbc row for the bot's race/class is hidden, or starts at
// Hated (-42000) while another row starts higher (e.g. Honor Hold for Horde, Tranquillien for Alliance).
// Matches the race/class row selection in ReputationMgr::GetBaseReputation.
static bool IsOtherSideFaction(Player* bot, FactionEntry const* entry)
{
    uint32 raceMask = bot->getRaceMask();
    uint32 classMask = bot->getClassMask();
    for (int i = 0; i < 4; ++i)
    {
        bool raceMatch = (entry->BaseRepRaceMask[i] & raceMask) ||
                         (!entry->BaseRepRaceMask[i] && entry->BaseRepClassMask[i]);
        bool classMatch = (entry->BaseRepClassMask[i] & classMask) || !entry->BaseRepClassMask[i];
        if (!raceMatch || !classMatch)
            continue;

        if (entry->ReputationFlags[i] & FACTION_FLAG_HIDDEN)
            return true;

        if (entry->BaseRepValue[i] > -42000)
            return false;

        for (int j = 0; j < 4; ++j)
            if (j != i && (entry->BaseRepRaceMask[j] || entry->BaseRepClassMask[j]) &&
                entry->BaseRepValue[j] > entry->BaseRepValue[i])
                return true;

        return false;
    }

    // No row for this race/class: the faction isn't in the bot's reputation list.
    return true;
}

void SuggestWhatToDoAction::grindReputation()
{
    std::vector<std::string> levels;
    levels.push_back("honored");
    levels.push_back("revered");
    levels.push_back("exalted");

    std::vector<std::string> allowedFactions;
    uint8 expansion = bot->GetSession()->Expansion();
    for (auto const& [factionId, minLevel, factionExpansion] : reputationFactions)
    {
        if ((int)bot->GetLevel() < minLevel || factionExpansion > expansion)
            continue;

        FactionEntry const* entry = sFactionStore.LookupEntry(factionId);
        if (!entry || IsOtherSideFaction(bot, entry))
            continue;

        char const* name = entry->name[_dbc_locale];
        allowedFactions.push_back(name && *name ? name : entry->name[0]);
    }

    if (allowedFactions.empty()) return;

    BroadcastHelper::BroadcastSuggestGrindReputation(botAI, levels, allowedFactions, bot);
}

void SuggestWhatToDoAction::something()
{
    BroadcastHelper::BroadcastSuggestSomething(botAI, bot);
}

void SuggestWhatToDoAction::somethingToxic()
{
    BroadcastHelper::BroadcastSuggestSomethingToxic(botAI, bot);
}

void SuggestWhatToDoAction::toxicLinks()
{
    BroadcastHelper::BroadcastSuggestToxicLinks(botAI, bot);
}

void SuggestWhatToDoAction::thunderfury()
{
    BroadcastHelper::BroadcastSuggestThunderfury(botAI, bot);
}

class FindTradeItemsVisitor : public IterateItemsVisitor
{
public:
    FindTradeItemsVisitor(uint32 quality) : IterateItemsVisitor(), quality(quality) {}

    bool Visit(Item* item) override
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (proto->Quality != quality)
            return true;

        if (proto->Class == ITEM_CLASS_TRADE_GOODS && proto->Bonding == NO_BIND)
        {
            if (proto->Quality == ITEM_QUALITY_NORMAL && item->GetCount() > 1 &&
                item->GetCount() == item->GetMaxStackCount())
                stacks.push_back(proto->ItemId);

            items.push_back(proto->ItemId);
            count[proto->ItemId] += item->GetCount();
        }

        return true;
    }

    std::map<uint32, uint32> count;
    std::vector<uint32> stacks;
    std::vector<uint32> items;

private:
    uint32 quality;
};

SuggestDungeonAction::SuggestDungeonAction(PlayerbotAI* botAI) : SuggestWhatToDoAction(botAI, "suggest dungeon") {}

bool SuggestDungeonAction::Execute(Event /*event*/)
{
    // TODO: use PlayerbotDungeonRepository::instance()

    if (!sPlayerbotAIConfig.RandomBotSuggestDungeons || bot->GetGroup())
        return false;

    if (instances.empty())
    {
        instances["Ragefire Chasm"] = 15;
        instances["Deadmines"] = 18;
        instances["Wailing Caverns"] = 18;
        instances["Shadowfang Keep"] = 25;
        instances["Blackfathom Deeps"] = 20;
        instances["Stockade"] = 20;
        instances["Gnomeregan"] = 35;
        instances["Razorfen Kraul"] = 35;
        instances["Maraudon"] = 50;
        instances["Scarlet Monestery"] = 40;
        instances["Uldaman"] = 45;
        instances["Dire Maul"] = 58;
        instances["Scholomance"] = 59;
        instances["Razorfen Downs"] = 40;
        instances["Strathholme"] = 59;
        instances["Zul'Farrak"] = 45;
        instances["Blackrock Depths"] = 55;
        instances["Temple of Atal'Hakkar"] = 55;
        instances["Lower Blackrock Spire"] = 57;

        instances["Hellfire Citidel"] = 65;
        instances["Coilfang Reservoir"] = 65;
        instances["Auchindoun"] = 65;
        instances["Cavens of Time"] = 68;
        instances["Tempest Keep"] = 69;
        instances["Magister's Terrace"] = 70;

        instances["Utgarde Keep"] = 75;
        instances["The Nexus"] = 75;
        instances["Ahn'kahet: The Old Kingdom"] = 75;
        instances["Azjol-Nerub"] = 75;
        instances["Drak'Tharon Keep"] = 75;
        instances["Violet Hold"] = 80;
        instances["Gundrak"] = 77;
        instances["Halls of Stone"] = 77;
        instances["Halls of Lightning"] = 77;
        instances["Oculus"] = 77;
        instances["Utgarde Pinnacle"] = 77;
        instances["Trial of the Champion"] = 80;
        instances["Forge of Souls"] = 80;
        instances["Pit of Saron"] = 80;
        instances["Halls of Reflection"] = 80;
    }

    std::vector<std::string> allowedInstances;
    for (auto it : instances)
    {
        if (bot->GetLevel() >= it.second) allowedInstances.push_back(it.first);
    }

    if (allowedInstances.empty()) return false;

    BroadcastHelper::BroadcastSuggestInstance(botAI, allowedInstances, bot);
    return true;
}

SuggestTradeAction::SuggestTradeAction(PlayerbotAI* botAI) : SuggestWhatToDoAction(botAI, "suggest trade") {}

bool SuggestTradeAction::Execute(Event /*event*/)
{
    uint32 quality = urand(0, 100);
    if (quality > 95)
        quality = ITEM_QUALITY_LEGENDARY;
    else if (quality > 90)
        quality = ITEM_QUALITY_EPIC;
    else if (quality > 75)
        quality = ITEM_QUALITY_RARE;
    else if (quality > 50)
        quality = ITEM_QUALITY_UNCOMMON;
    else
        quality = ITEM_QUALITY_NORMAL;

    uint32 item = 0, count = 0;
    while (quality-- > ITEM_QUALITY_POOR)
    {
        FindTradeItemsVisitor visitor(quality);
        IterateItems(&visitor);
        if (!visitor.stacks.empty())
        {
            uint32 index = urand(0, visitor.stacks.size() - 1);
            item = visitor.stacks[index];
        }

        if (!item)
        {
            if (!visitor.items.empty())
            {
                uint32 index = urand(0, visitor.items.size() - 1);
                item = visitor.items[index];
            }
        }

        if (item)
        {
            count = visitor.count[item];
            break;
        }
    }

    if (!item || !count)
        return false;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    if (!proto)
        return false;

    uint32 price = proto->SellPrice * sRandomPlayerbotMgr.GetSellMultiplier(bot) * count;
    if (!price)
        return false;

    BroadcastHelper::BroadcastSuggestSell(botAI, proto, count, price, bot);
    return true;
}
