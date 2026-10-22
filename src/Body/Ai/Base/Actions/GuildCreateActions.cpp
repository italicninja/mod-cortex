/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildCreateActions.h"
#include "ArenaTeam.h"
#include "BudgetValues.h"
#include "Event.h"
#include "BotLevels.h"
#include "GuildMgr.h"
#include "ObjectMgr.h"
#include "PetitionMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotFactory.h"
#include "ServerFacade.h"
#include "SharedDefines.h"

namespace GuildCharter
{
    static constexpr uint32 CHARTER_ITEM = 5863;

    Item* Get(Player* bot) { return bot->GetItemByEntry(CHARTER_ITEM); }

    uint32 Signatures(Player* bot)
    {
        Item* charter = Get(bot);
        if (!charter)
            return 0;

        ::Signatures const* signatures = sPetitionMgr->GetSignature(charter->GetGUID());
        return signatures ? signatures->signatureMap.size() : 0;
    }

    bool ReadyToTurnIn(Player* bot)
    {
        return !bot->GetGuildId() && Get(bot) && Signatures(bot) >= sWorld->getIntConfig(CONFIG_MIN_PETITION_SIGNS);
    }

    // Guild masters are petitioners that also design tabards. Arena organizers are plain
    // petitioners, and buying there makes an arena charter.
    Creature* NearbyGuildMaster(PlayerbotAI* botAI, Player* bot)
    {
        GuidVector npcs = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get();
        for (ObjectGuid const& guid : npcs)
        {
            Creature* creature = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_PETITIONER);
            if (creature && creature->IsTabardDesigner())
                return creature;
        }

        return nullptr;
    }

    struct GuildMasterSpawn
    {
        uint32 mapId;
        float x, y, z;
        uint32 faction;
    };

    bool FindGuildMaster(Player* bot, WorldPosition& pos)
    {
        static std::vector<GuildMasterSpawn> const spawns = []
        {
            std::vector<GuildMasterSpawn> list;
            uint32 const flags = UNIT_NPC_FLAG_PETITIONER | UNIT_NPC_FLAG_TABARDDESIGNER;
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
                if (tmpl && (tmpl->npcflag & flags) == flags)
                    list.push_back({data.mapid, data.posX, data.posY, data.posZ, tmpl->faction});
            }
            LOG_INFO("playerbots", "Found {} guild master spawns for bot guild charters", list.size());
            return list;
        }();

        FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
        if (!botFaction)
            return false;

        // A guild master the bot can't walk to is skipped: up a lift (Thunder Bluff, Undercity; MoveFarTo gives up
        // on it at once), or given up on by its walk unstick in the last hour. The errand held the tick for it and
        // asked MoveFarTo again every tick: bots stood in Mulgore and Tirisfal for hours ("levels" "lift_only").
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);

        GuildMasterSpawn const* nearest = nullptr;
        float nearestDist = 0.0f;
        for (GuildMasterSpawn const& spawn : spawns)
        {
            if (spawn.mapId != bot->GetMapId())
                continue;

            FactionTemplateEntry const* npcFaction = sFactionTemplateStore.LookupEntry(spawn.faction);
            if (!npcFaction || !npcFaction->IsFriendlyTo(*botFaction))
                continue;

            float dist = bot->GetExactDist(spawn.x, spawn.y, spawn.z);
            if (nearest && dist >= nearestDist)
                continue;

            if (BotLevels::LiftOnly(bot, spawn.mapId, spawn.x, spawn.y, spawn.z) ||
                (botAI && botAI->rpgInfo.IsUnreachable(WorldPosition(spawn.mapId, spawn.x, spawn.y, spawn.z),
                                                       HOUR * IN_MILLISECONDS)))
                continue;

            nearest = &spawn;
            nearestDist = dist;
        }

        if (!nearest)
            return false;

        pos = WorldPosition(nearest->mapId, nearest->x, nearest->y, nearest->z);
        return true;
    }

    // Turns the charter in at a guild master in reach. Returns true once the guild exists.
    static bool TurnIn(PlayerbotAI* botAI, Player* bot)
    {
        Item* charter = Get(bot);
        if (!charter || !NearbyGuildMaster(botAI, bot))
            return false;

        Petition const* petition = sPetitionMgr->GetPetition(charter->GetGUID());
        std::string const name = petition ? petition->petitionName : "";

        WorldPacket data(CMSG_TURN_IN_PETITION, 8);
        data << charter->GetGUID();
        bot->GetSession()->HandleTurnInPetitionOpcode(data);

        if (!bot->GetGuildId())
        {
            // Most likely the name was taken while the charter was out. Drop it so the bot can
            // buy a new one with another name instead of retrying forever.
            if (!name.empty() && sGuildMgr->GetGuildByName(name))
            {
                LOG_INFO("playerbots", "Bot {} dropped charter <{}>: name already taken", bot->GetName(), name);
                bot->DestroyItemCount(CHARTER_ITEM, 1, true);
            }
            return false;
        }

        // Ensure that bot has at least 10g for HandleSetEmblem can be managed core side
        // (EMBLEM_PRICE = 10 * GOLD in core)
        static constexpr uint32 REQUIRED = 10 * GOLD;
        uint32 have = bot->GetMoney();               // actual money earned by bot in copper
        if (have < REQUIRED && !sPlayerbotAIConfig.RandomBotFreshStart)
        {
            bot->ModifyMoney(int32(REQUIRED - have)); // add only the missing amount to bot to reach 10g
        }

        Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId());

        uint32 st, cl, br, bc, bg;
        bg = urand(0, 51);
        bc = urand(0, 17);
        cl = urand(0, 17);
        br = urand(0, 7);
        st = urand(0, 180);
        EmblemInfo emblemInfo(st, cl, br, bc, bg);

        guild->HandleSetEmblem(emblemInfo); // official core handling

        // LANG_GUILD_VETERAN -> can invite
        guild->HandleSetRankInfo(2, {}, GR_RIGHT_GCHATLISTEN | GR_RIGHT_GCHATSPEAK | GR_RIGHT_INVITE);

        LOG_INFO("playerbots", "Bot {} founded guild <{}> with {} members", bot->GetName(), guild->GetName(),
                 guild->GetMemberCount());
        return true;
    }
}

bool BuyPetitionAction::Execute(Event /*event*/)
{
    Creature* pCreature = GuildCharter::NearbyGuildMaster(botAI, bot);
    if (!pCreature)
        return false;

    std::string const guildName = RandomPlayerbotFactory::CreateRandomGuildName();
    if (guildName.empty())
        return false;

    WorldPacket data(CMSG_PETITION_BUY);

    data << pCreature->GetGUID();
    data << uint32(0);
    data << uint64(0);
    data << guildName.c_str();
    data << std::string("");
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);
    data << uint16(0);
    data << uint32(0);
    data << uint32(0);
    data << uint32(0);

    for (uint8 i = 0; i < 10; ++i)
        data << std::string("");

    data << uint32(0);  // index
    data << uint32(0);

    bot->GetSession()->HandlePetitionBuyOpcode(data);

    if (GuildCharter::Get(bot))
        LOG_INFO("playerbots", "Bot {} bought a guild charter for <{}>", bot->GetName(), guildName);

    return true;
}

bool BuyPetitionAction::isUseful() { return canBuyPetition(bot); };

bool BuyPetitionAction::canBuyPetition(Player* bot)
{
    if (bot->GetGuildId())
        return false;

    if (bot->GetGuildIdInvited())
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    AiObjectContext* context = botAI->GetAiObjectContext();

    if (GuildCharter::Get(bot))
        return false;

    // Only a few bots want to lead a guild. Everyone else signs and joins.
    if (!botAI->IsGuildFounder())
        return false;

    if (!botAI->HasStrategy("guild", BOT_STATE_NON_COMBAT))
        return false;

    uint32 cost = sWorld->getIntConfig(CONFIG_CHARTER_COST_GUILD);

    if (AI_VALUE2(uint32, "free money for", uint32(NeedMoneyFor::guild)) < cost)
        return false;

    return true;
}

bool PetitionOfferAction::Execute(Event event)
{
    Item* charter = GuildCharter::Get(bot);
    if (!charter)
        return false;

    ObjectGuid guid = event.getObject();

    Player* master = GetMaster();
    if (!master)
    {
        if (!guid)
            guid = bot->GetTarget();
    }
    else
    {
        if (!guid)
            guid = master->GetTarget();
    }

    if (!guid)
        return false;

    Player* player = ObjectAccessor::FindPlayer(guid);

    if (!player || player == bot)
        return false;

    Signatures const* signatures = sPetitionMgr->GetSignature(charter->GetGUID());
    if (!signatures)
        return false;

    // Bots share accounts and may sign once per character (see OnPlayerBeforePetitionSign).
    // Real players are held to the core's one signature per account.
    bool const isBot = GET_PLAYERBOT_AI(player) != nullptr;
    uint32 const accountId = player->GetSession()->GetAccountId();
    for (auto const& [signer, signerAccount] : signatures->signatureMap)
    {
        if (signer == player->GetGUID() || (!isBot && signerAccount == accountId))
            return false;
    }

    WorldPacket data(CMSG_OFFER_PETITION);

    data << uint32(0);
    data << charter->GetGUID();
    data << guid;

    bot->GetSession()->HandleOfferPetitionOpcode(data);

    return true;
}

bool PetitionOfferAction::isUseful() { return !bot->GetGuildId(); }

bool PetitionOfferNearbyAction::Execute(Event /*event*/)
{
    uint32 found = 0;
    time_t const now = time(nullptr);

    if (offeredUntil.size() > 500)
    {
        for (auto it = offeredUntil.begin(); it != offeredUntil.end();)
            it = it->second <= now ? offeredUntil.erase(it) : std::next(it);
    }

    GuidVector nearGuids = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest friendly players")->Get();
    for (auto& i : nearGuids)
    {
        Player* player = ObjectAccessor::FindPlayer(i);

        if (!player || player == bot)
            continue;

        if (player->GetGuildId())
            continue;

        if (player->GetGuildIdInvited())
            continue;

        auto offered = offeredUntil.find(i);
        if (offered != offeredUntil.end() && offered->second > now)
            continue;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);

        if (botAI)
        {
            if (botAI->GetGuilderType() == GuilderType::SOLO && !botAI->HasGameClientMaster())   // Do not invite solo players.
                continue;

            if (IsRealPlayer(botAI->GetMaster()))   // Do not invite bots that belong to an active player.
                continue;

            if (GuildCharter::Get(player))   // Busy founding their own guild.
                continue;
        }
        else
        {
            if (!sPlayerbotAIConfig.RandomBotGuildNearby)
                continue;
        }

        if (ServerFacade::instance().GetDistance2d(bot, player) > sPlayerbotAIConfig.SightDistance)
            continue;

        // A real player gets the signature dialog. Ask again after 15 minutes, a bot after 5.
        offeredUntil[i] = now + (botAI ? 5 * MINUTE : 15 * MINUTE);

        if (PetitionOfferAction::Execute(Event("petition offer nearby", i)))
            found++;
    }

    return found > 0;
}

bool PetitionOfferNearbyAction::isUseful()
{
    return !bot->GetGuildId() && botAI->IsGuildFounder() && GuildCharter::Get(bot) &&
           GuildCharter::Signatures(bot) < sWorld->getIntConfig(CONFIG_MIN_PETITION_SIGNS);
}

bool PetitionTurnInAction::Execute(Event /*event*/)
{
    if (!GuildCharter::Get(bot))
        return false;

    if (GuildCharter::NearbyGuildMaster(botAI, bot))
    {
        GuildCharter::TurnIn(botAI, bot);
        return true;
    }

    TravelTarget* oldTarget = context->GetValue<TravelTarget*>("travel target")->Get();

    // Select a new target to travel to.
    TravelTarget newTarget = TravelTarget(botAI);

    bool foundTarget = SetNpcFlagTarget(&newTarget, {UNIT_NPC_FLAG_PETITIONER});

    if (!foundTarget || !newTarget.isActive())
        return false;

    newTarget.setRadius(INTERACTION_DISTANCE);

    setNewTarget(&newTarget, oldTarget);

    return true;
}

bool PetitionTurnInAction::isUseful()
{
    bool inCity = false;
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    return inCity && GuildCharter::ReadyToTurnIn(bot) &&
           !context->GetValue<TravelTarget*>("travel target")->Get()->isTraveling();
}

bool GuildCharterErrandAction::IsNeeded(Player* bot)
{
    if (bot->GetGuildId())
        return false;

    // Checked every tick, so bail out on the cheap test first. Only founders buy charters.
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !botAI->IsGuildFounder())
        return false;

    if (GuildCharter::Get(bot))
        return GuildCharter::ReadyToTurnIn(bot);

    return BuyPetitionAction::canBuyPetition(bot);
}

bool GuildCharterErrandAction::Execute(Event /*event*/)
{
    if (GuildCharter::NearbyGuildMaster(botAI, bot))
    {
        if (GuildCharter::Get(bot))
            return GuildCharter::TurnIn(botAI, bot);

        return botAI->DoSpecificAction("buy petition", Event(), true);
    }

    WorldPosition dest;
    if (!GuildCharter::FindGuildMaster(bot, dest))
        return false;

    MoveFarTo(dest);
    // Hold the tick even while the last move is still running, so the rpg status
    // actions below don't steer the bot somewhere else.
    return true;
}

bool GuildCharterErrandAction::isUseful() { return IsNeeded(bot); }

bool BuyTabardAction::Execute(Event /*event*/)
{
    bool canBuy = botAI->DoSpecificAction("buy", Event("buy tabard", "Hitem:5976:"), true);
    if (canBuy && AI_VALUE2(uint32, "item count", chat->FormatQItem(5976)))
        return true;

    TravelTarget* oldTarget = context->GetValue<TravelTarget*>("travel target")->Get();

    // Select a new target to travel to.
    TravelTarget newTarget = TravelTarget(botAI);

    bool foundTarget = SetNpcFlagTarget(&newTarget, {UNIT_NPC_FLAG_TABARDDESIGNER}, "Tabard Vendor", {5976});

    if (!foundTarget || !newTarget.isActive())
        return false;

    newTarget.setRadius(INTERACTION_DISTANCE);

    setNewTarget(&newTarget, oldTarget);

    return true;
};

bool BuyTabardAction::isUseful()
{
    bool inCity = false;
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    return inCity && bot->GetGuildId() && !AI_VALUE2(uint32, "item count", chat->FormatQItem(5976)) &&
           AI_VALUE2(uint32, "free money for", uint32(NeedMoneyFor::guild)) >= 10000 &&
           !context->GetValue<TravelTarget*>("travel target")->Get()->isTraveling();
}
