/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotAIConfig.h"
#include "BisListMgr.h"
#include "BotTelemetry.h"
#include "Config.h"
#include "NewRpgInfo.h"
#include "PlayerbotDungeonRepository.h"
#include "BotProfessions.h"
#include "PlayerbotFactory.h"
#include "PlayerbotGuildMgr.h"
#include "Playerbots.h"
#include "RandomItemMgr.h"
#include "RandomPlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"
#include "Talentspec.h"
#include "TravelMgr.h"
#include <cctype>
#include <iostream>
#include <sstream>

template <class T>
void LoadList(std::string const value, T& list)
{
    std::vector<std::string> ids = split(value, ',');
    for (std::vector<std::string>::iterator i = ids.begin(); i != ids.end(); i++)
    {
        uint32 id = atoi((*i).c_str());
        // if (!id)
        //     continue;
        list.push_back(id);
    }
}

template <class T>
void LoadSet(std::string const value, T& set)
{
    std::vector<std::string> ids = split(value, ',');
    for (std::vector<std::string>::iterator i = ids.begin(); i != ids.end(); i++)
    {
        uint32 id = atoi((*i).c_str());
        // if (!id)
        //     continue;
        set.insert(id);
    }
}

template <class T>
void LoadListString(std::string const value, T& list)
{
    std::vector<std::string> strings = split(value, ',');
    for (std::vector<std::string>::iterator i = strings.begin(); i != strings.end(); i++)
    {
        std::string const string = *i;
        if (string.empty())
            continue;

        list.push_back(string);
    }
}

// Parses a comma-separated, whitespace-tolerant bot name list (as used by both
// Cortex.Bots.LevelBrackets.ExcludeNames and Cortex.Bots.ResetBotLevel.ExcludeNames) into out.
static void ParseLevelMgrExcludeNames(std::string const& csv, std::vector<std::string>& out)
{
    out.clear();
    std::istringstream f(csv);
    std::string s;
    while (getline(f, s, ','))
    {
        s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());
        if (!s.empty())
            out.push_back(s);
    }
}

bool PlayerbotAIConfig::Initialize()
{
    LOG_INFO("server.loading", "Initializing mod-playerbots, based on AI Playerbots by ike3 and the original Playerbots by blueboy");

    Enabled = sConfigMgr->GetOption<bool>("Cortex.Bots.Enabled", true);
    if (!Enabled)
    {
        LOG_INFO("server.loading", "Playerbots Module is disabled in playerbots.conf");
        return false;
    }

    GlobalCoolDown = sConfigMgr->GetOption<int32>("Cortex.Bots.GlobalCooldown", 500);
    MaxWaitForMove = sConfigMgr->GetOption<int32>("Cortex.Bots.MaxWaitForMove", 5000);
    DisableMoveSplinePath = sConfigMgr->GetOption<int32>("Cortex.Bots.DisableMoveSplinePath", 0);
    MaxMovementSearchTime = sConfigMgr->GetOption<int32>("Cortex.Bots.MaxMovementSearchTime", 3);
    ExpireActionTime = sConfigMgr->GetOption<int32>("Cortex.Bots.ExpireActionTime", 5000);
    DispelAuraDuration = sConfigMgr->GetOption<int32>("Cortex.Bots.DispelAuraDuration", 700);
    ReactDelay = sConfigMgr->GetOption<int32>("Cortex.Bots.ReactDelay", 100);
    DynamicReactDelay = sConfigMgr->GetOption<bool>("Cortex.Bots.DynamicReactDelay", true);
    PassiveDelay = sConfigMgr->GetOption<int32>("Cortex.Bots.PassiveDelay", 10000);
    RepeatDelay = sConfigMgr->GetOption<int32>("Cortex.Bots.RepeatDelay", 2000);
    ErrorDelay = sConfigMgr->GetOption<int32>("Cortex.Bots.ErrorDelay", 100);
    RpgDelay = sConfigMgr->GetOption<int32>("Cortex.Bots.RpgDelay", 10000);
    SitDelay = sConfigMgr->GetOption<int32>("Cortex.Bots.SitDelay", 20000);
    ReturnDelay = sConfigMgr->GetOption<int32>("Cortex.Bots.ReturnDelay", 2000);
    LootDelay = sConfigMgr->GetOption<int32>("Cortex.Bots.LootDelay", 1000);
    DisabledWithoutRealPlayerLoginDelay = sConfigMgr->GetOption<int32>("Cortex.Bots.DisabledWithoutRealPlayerLoginDelay", 30);
    DisabledWithoutRealPlayerLogoutDelay = sConfigMgr->GetOption<int32>("Cortex.Bots.DisabledWithoutRealPlayerLogoutDelay", 300);

    FarDistance = sConfigMgr->GetOption<float>("Cortex.Bots.FarDistance", 20.0f);
    SightDistance = sConfigMgr->GetOption<float>("Cortex.Bots.SightDistance", 100.0f);
    SpellDistance = sConfigMgr->GetOption<float>("Cortex.Bots.SpellDistance", 28.5f);
    ShootDistance = sConfigMgr->GetOption<float>("Cortex.Bots.ShootDistance", 5.0f);
    HealDistance = sConfigMgr->GetOption<float>("Cortex.Bots.HealDistance", 38.5f);
    LootDistance = sConfigMgr->GetOption<float>("Cortex.Bots.LootDistance", 15.0f);
    FleeDistance = sConfigMgr->GetOption<float>("Cortex.Bots.FleeDistance", 5.0f);
    AggroDistance = sConfigMgr->GetOption<float>("Cortex.Bots.AggroDistance", 22.0f);
    TooCloseDistance = sConfigMgr->GetOption<float>("Cortex.Bots.TooCloseDistance", 5.0f);
    MeleeDistance = sConfigMgr->GetOption<float>("Cortex.Bots.MeleeDistance", 0.75f);
    FollowDistance = sConfigMgr->GetOption<float>("Cortex.Bots.FollowDistance", 1.5f);
    WhisperDistance = sConfigMgr->GetOption<float>("Cortex.Bots.WhisperDistance", 6000.0f);
    ContactDistance = sConfigMgr->GetOption<float>("Cortex.Bots.ContactDistance", 0.45f);
    AoeRadius = sConfigMgr->GetOption<float>("Cortex.Bots.AoeRadius", 10.0f);
    RpgDistance = sConfigMgr->GetOption<float>("Cortex.Bots.RpgDistance", 200.0f);
    GrindDistance = sConfigMgr->GetOption<float>("Cortex.Bots.GrindDistance", 75.0f);
    ReactDistance = sConfigMgr->GetOption<float>("Cortex.Bots.ReactDistance", 150.0f);

    CriticalHealth = sConfigMgr->GetOption<int32>("Cortex.Bots.CriticalHealth", 25);
    LowHealth = sConfigMgr->GetOption<int32>("Cortex.Bots.LowHealth", 45);
    MediumHealth = sConfigMgr->GetOption<int32>("Cortex.Bots.MediumHealth", 65);
    AlmostFullHealth = sConfigMgr->GetOption<int32>("Cortex.Bots.AlmostFullHealth", 85);
    LowMana = sConfigMgr->GetOption<int32>("Cortex.Bots.LowMana", 15);
    MediumMana = sConfigMgr->GetOption<int32>("Cortex.Bots.MediumMana", 40);
    HighMana = sConfigMgr->GetOption<int32>("Cortex.Bots.HighMana", 65);
    AutoSaveMana = sConfigMgr->GetOption<bool>("Cortex.Bots.AutoSaveMana", true);
    SaveManaThreshold = sConfigMgr->GetOption<int32>("Cortex.Bots.SaveManaThreshold", 60);
    switch (sConfigMgr->GetOption<uint32>("Cortex.Bots.AutoGreaterBlessings", 1))
    {
        case 0:
            AutoGreaterBlessings = AutoPartyBuffMode::DISABLED;
            break;
        case 2:
            AutoGreaterBlessings = AutoPartyBuffMode::GROUP_OR_RAID;
            break;
        case 1:
        default:
            AutoGreaterBlessings = AutoPartyBuffMode::RAID_ONLY;
            break;
    }
    switch (sConfigMgr->GetOption<uint32>("Cortex.Bots.AutoPartyBuffs", 2))
    {
        case 0:
            AutoPartyBuffs = AutoPartyBuffMode::DISABLED;
            break;
        case 1:
            AutoPartyBuffs = AutoPartyBuffMode::RAID_ONLY;
            break;
        case 2:
        default:
            AutoPartyBuffs = AutoPartyBuffMode::GROUP_OR_RAID;
            break;
    }
    TellWhenMissingBuffReagents = sConfigMgr->GetOption<bool>("Cortex.Bots.TellWhenMissingBuffReagents", true);
    MissingBuffReagentMessageCooldown = sConfigMgr->GetOption<uint32>(
        "Cortex.Bots.MissingBuffReagentMessageCooldown", 300);
    ForceRebuffOnReadyCheck = sConfigMgr->GetOption<bool>("Cortex.Bots.ForceRebuffOnReadyCheck", false);
    ForceRebuffMarginSecs = std::min(sConfigMgr->GetOption<uint32>("Cortex.Bots.ForceRebuffMarginSecs", 60), 3600u);
    AutoAvoidAoe = sConfigMgr->GetOption<bool>("Cortex.Bots.AutoAvoidAoe", true);
    MaxAoeAvoidRadius = sConfigMgr->GetOption<float>("Cortex.Bots.MaxAoeAvoidRadius", 15.0f);
    LoadSet<std::set<uint32>>(sConfigMgr->GetOption<std::string>("Cortex.Bots.AoeAvoidSpellWhitelist", "50759,57491,13810,29946"),
                              AoeAvoidSpellWhitelist);
    TellWhenAvoidAoe = sConfigMgr->GetOption<bool>("Cortex.Bots.TellWhenAvoidAoe", false);

    RandomGearLoweringChance = sConfigMgr->GetOption<float>("Cortex.Bots.RandomGearLoweringChance", 0.0f);
    RandomGearQualityLimit = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomGearQualityLimit", 3);
    RandomGearScoreLimit = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomGearScoreLimit", 0);
    PreferClassArmorType  = sConfigMgr->GetOption<bool>("Cortex.Bots.PreferClassArmorType", false);
    PreferredSpecWeapons  = sConfigMgr->GetOption<bool>("Cortex.Bots.PreferredSpecWeapons", false);

    RandomBotMinLevelChance = sConfigMgr->GetOption<float>("Cortex.Bots.RandomBotMinLevelChance", 0.1f);
    RandomBotMaxLevelChance = sConfigMgr->GetOption<float>("Cortex.Bots.RandomBotMaxLevelChance", 0.1f);
    RandomBotRpgChance = sConfigMgr->GetOption<float>("Cortex.Bots.RandomBotRpgChance", 0.20f);

    IterationsPerTick = sConfigMgr->GetOption<int32>("Cortex.Bots.IterationsPerTick", 10);

    AllowAccountBots = sConfigMgr->GetOption<bool>("Cortex.Bots.AllowAccountBots", true);
    AllowGuildBots = sConfigMgr->GetOption<bool>("Cortex.Bots.AllowGuildBots", true);
    AllowTrustedAccountBots = sConfigMgr->GetOption<bool>("Cortex.Bots.AllowTrustedAccountBots", true);
    DisabledWithoutRealPlayer = sConfigMgr->GetOption<bool>("Cortex.Bots.DisabledWithoutRealPlayer", false);
    RandomBotGuildNearby = sConfigMgr->GetOption<bool>("Cortex.Bots.RandomBotGuildNearby", false);
    RandomBotInvitePlayer = sConfigMgr->GetOption<bool>("Cortex.Bots.RandomBotInvitePlayer", false);
    InviteChat = sConfigMgr->GetOption<bool>("Cortex.Bots.InviteChat", false);

    RandomBotMapsAsString = sConfigMgr->GetOption<std::string>("Cortex.Bots.RandomBotMaps", "0,1,530,571");
    LoadList<std::vector<uint32>>(RandomBotMapsAsString, RandomBotMaps);
    ProbTeleToBankers = sConfigMgr->GetOption<float>("Cortex.Bots.ProbTeleToBankers", 0.25f);
    EnableWeightTeleToCityBankers = sConfigMgr->GetOption<bool>("Cortex.Bots.EnableWeightTeleToCityBankers", false);
    WeightTeleToStormwind = sConfigMgr->GetOption<int>("Cortex.Bots.TeleToStormwindWeight", 2);
    WeightTeleToIronforge = sConfigMgr->GetOption<int>("Cortex.Bots.TeleToIronforgeWeight", 1);
    WeightTeleToDarnassus = sConfigMgr->GetOption<int>("Cortex.Bots.TeleToDarnassusWeight", 1);
    WeightTeleToExodar = sConfigMgr->GetOption<int>("Cortex.Bots.TeleToExodarWeight", 1);
    WeightTeleToOrgrimmar = sConfigMgr->GetOption<int>("Cortex.Bots.TeleToOrgrimmarWeight", 2);
    WeightTeleToUndercity = sConfigMgr->GetOption<int>("Cortex.Bots.TeleToUndercityWeight", 1);
    WeightTeleToThunderBluff = sConfigMgr->GetOption<int>("Cortex.Bots.TeleToThunderBluffWeight", 1);
    WeightTeleToSilvermoonCity = sConfigMgr->GetOption<int>("Cortex.Bots.TeleToSilvermoonCityWeight", 1);
    WeightTeleToShattrathCity = sConfigMgr->GetOption<int>("Cortex.Bots.TeleToShattrathCityWeight", 1);
    WeightTeleToDalaran = sConfigMgr->GetOption<int>("Cortex.Bots.TeleToDalaranWeight", 1);
    LoadList<std::vector<uint32>>(
        sConfigMgr->GetOption<std::string>("Cortex.Bots.RandomBotQuestItems",
                                           "5175,5176,5177,5178,6948,11000,12382,13704,16309"),
        RandomBotQuestItems);
    LoadList<std::vector<uint32>>(sConfigMgr->GetOption<std::string>("Cortex.Bots.RandomBotSpellIds", "54197"),
                                  RandomBotSpellIds);
    LoadList<std::vector<uint32>>(
        sConfigMgr->GetOption<std::string>("Cortex.Bots.PvpProhibitedZoneIds",
                                           "2255,656,2361,2362,2363,976,35,2268,3425,392,541,1446,3828,3712,3738,3565,"
                                           "3539,3623,4152,3988,4658,4284,4418,4436,4275,4323,4395,3703,4298,3951"),
        PvpProhibitedZoneIds);
    LoadList<std::vector<uint32>>(
        sConfigMgr->GetOption<std::string>("Cortex.Bots.PvpProhibitedAreaIds",
                                           "976,35,392,2268,4161,4010,4317,4312,3649,3887,3958,3724,4080,3938,3754,3786,"
                                           "3973,4085,4086,4087,4088,251"),
        PvpProhibitedAreaIds);
    FastReactInBG = sConfigMgr->GetOption<bool>("Cortex.Bots.FastReactInBG", true);
    LoadList<std::vector<uint32>>(
        sConfigMgr->GetOption<std::string>("Cortex.Bots.RandomBotQuestIds", "3802,5505,6502,7761,7848,10277,10285,11492,"
                                           "13188,13189,24499,24511,24710,24712"),
        RandomBotQuestIds);

    LoadSet<std::set<uint32>>(
        sConfigMgr->GetOption<std::string>("Cortex.Bots.DisallowedGameObjects",
                                           "176213,17155,2656,74448,19020,3719,3658,3705,3706,105579,75293,2857,"
                                           "179490,141596,160836,160845,179516,176224,181085,176112,128308,128403,"
                                           "165739,165738,175245,175970,176325,176327,123329,2560"),
        DisallowedGameObjects);
    LoadSet<std::set<uint32>>(
        sConfigMgr->GetOption<std::string>("Cortex.Bots.AttunementQuests", "10279,10277,10282,10283,10284,10285,10296,"
                                           "10297,10298,11481,11482,11488,11490,11492,10901,10888,10445,10985"),
        AttunementQuests);

    LoadSet<std::set<uint32>>(
        sConfigMgr->GetOption<std::string>("Cortex.Bots.UnobtainableItems", "12468,44869,44870,46978"),
        UnobtainableItems);

    BotAutologin = sConfigMgr->GetOption<bool>("Cortex.Bots.BotAutologin", false);
    RandomBotAutologin = sConfigMgr->GetOption<bool>("Cortex.Bots.RandomBotAutologin", true);
    MinRandomBots = sConfigMgr->GetOption<int32>("Cortex.Bots.MinRandomBots", 500);
    MaxRandomBots = sConfigMgr->GetOption<int32>("Cortex.Bots.MaxRandomBots", 500);
    RandomBotUpdateInterval = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotUpdateInterval", 20);
    RandomBotCountChangeMinInterval =
        sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotCountChangeMinInterval", 30 * MINUTE);
    RandomBotCountChangeMaxInterval =
        sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotCountChangeMaxInterval", 2 * HOUR);
    MinRandomBotInWorldTime = sConfigMgr->GetOption<int32>("Cortex.Bots.MinRandomBotInWorldTime", 2 * HOUR);
    MaxRandomBotInWorldTime = sConfigMgr->GetOption<int32>("Cortex.Bots.MaxRandomBotInWorldTime", 14 * 24 * HOUR);
    MinRandomBotRandomizeTime = sConfigMgr->GetOption<int32>("Cortex.Bots.MinRandomBotRandomizeTime", 2 * HOUR);
    MaxRandomBotRandomizeTime = sConfigMgr->GetOption<int32>("Cortex.Bots.MaxRandomBotRandomizeTime", 14 * 24 * HOUR);
    MinRandomBotChangeStrategyTime =
        sConfigMgr->GetOption<int32>("Cortex.Bots.MinRandomBotChangeStrategyTime", 30 * MINUTE);
    MaxRandomBotChangeStrategyTime =
        sConfigMgr->GetOption<int32>("Cortex.Bots.MaxRandomBotChangeStrategyTime", 2 * HOUR);
    MinRandomBotReviveTime = sConfigMgr->GetOption<int32>("Cortex.Bots.MinRandomBotReviveTime", MINUTE);
    MaxRandomBotReviveTime = sConfigMgr->GetOption<int32>("Cortex.Bots.MaxRandomBotReviveTime", 5 * MINUTE);
    MinRandomBotTeleportInterval = sConfigMgr->GetOption<int32>("Cortex.Bots.MinRandomBotTeleportInterval", 1 * HOUR);
    MaxRandomBotTeleportInterval = sConfigMgr->GetOption<int32>("Cortex.Bots.MaxRandomBotTeleportInterval", 5 * HOUR);
    PermanentlyInWorldTime =
        sConfigMgr->GetOption<int32>("Cortex.Bots.PermanentlyInWorldTime", 1 * YEAR);
    RandomBotTeleportDistance = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotTeleportDistance", 100);
    RandomBotsPerInterval = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotsPerInterval", 60);
    RandomBotPrintStatsInterval = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotPrintStatsInterval", 300);
    MinRandomBotsPriceChangeInterval =
        sConfigMgr->GetOption<int32>("Cortex.Bots.MinRandomBotsPriceChangeInterval", 2 * HOUR);
    MaxRandomBotsPriceChangeInterval =
        sConfigMgr->GetOption<int32>("Cortex.Bots.MaxRandomBotsPriceChangeInterval", 48 * HOUR);
    RandomBotJoinLfg = sConfigMgr->GetOption<bool>("Cortex.Bots.RandomBotJoinLfg", true);
    RandomBotJoinLfgOutleveledExpansion =
        sConfigMgr->GetOption<bool>("Cortex.Bots.RandomBotJoinLfgOutleveledExpansion", true);

    RestrictHealerDPS = sConfigMgr->GetOption<bool>("Cortex.Bots.HealerDPSMapRestriction", false);
    LoadList<std::vector<uint32>>(
        sConfigMgr->GetOption<std::string>("Cortex.Bots.RestrictedHealerDPSMaps",
                                             "33,34,36,43,47,48,70,90,109,129,209,229,230,329,349,389,429,1001,1004,"
                                             "1007,269,540,542,543,545,546,547,552,553,554,555,556,557,558,560,585,574,"
                                             "575,576,578,595,599,600,601,602,604,608,619,632,650,658,668,409,469,509,"
                                             "531,532,534,544,548,550,564,565,580,249,533,603,615,616,624,631,649,724"),
        RestrictedHealerDPSMaps);

    //////////////////////////// ICC

    EnableICCBuffs = sConfigMgr->GetOption<bool>("Cortex.Bots.EnableICCBuffs", true);

    //////////////////////////// Professions
    ClassMatchingProfessionChance =
        std::min<uint32>(100, sConfigMgr->GetOption<uint32>("Cortex.Bots.ClassMatchingProfessionChance", 30));
    FishingDistanceFromMaster = sConfigMgr->GetOption<float>("Cortex.Bots.FishingDistanceFromMaster", 10.0f);
    EndFishingWithMaster = sConfigMgr->GetOption<float>("Cortex.Bots.EndFishingWithMaster", 30.0f);
    FishingDistance = sConfigMgr->GetOption<float>("Cortex.Bots.FishingDistance", 40.0f);
    EnableFishingWithMaster = sConfigMgr->GetOption<bool>("Cortex.Bots.EnableFishingWithMaster", true);
    //////////////////////////// CHAT
    EnableBroadcasts = sConfigMgr->GetOption<bool>("Cortex.Bots.EnableBroadcasts", true);
    RandomBotTalk = sConfigMgr->GetOption<bool>("Cortex.Bots.RandomBotTalk", false);
    RandomBotEmote = sConfigMgr->GetOption<bool>("Cortex.Bots.RandomBotEmote", false);
    RandomBotSuggestDungeons = sConfigMgr->GetOption<bool>("Cortex.Bots.RandomBotSuggestDungeons", true);
    RandomBotSayWithoutMaster = sConfigMgr->GetOption<bool>("Cortex.Bots.RandomBotSayWithoutMaster", false);
    AnnounceConsumableUse = sConfigMgr->GetOption<bool>("Cortex.Bots.AnnounceConsumableUse", true);
    EnablePersonas = sConfigMgr->GetOption<bool>("Cortex.Bots.EnablePersonas", true);
    PersonaTextChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("Cortex.Bots.PersonaTextChance", 100));

    TelemetryEnabled = sConfigMgr->GetOption<bool>("Cortex.Bots.Telemetry.Enable", false);
    TelemetryActions = sConfigMgr->GetOption<bool>("Cortex.Bots.Telemetry.Actions", true);
    TelemetryNearRadius = sConfigMgr->GetOption<uint32>("Cortex.Bots.Telemetry.NearRadius", 150);
    TelemetrySampleNearMs =
        std::max<uint32>(500, sConfigMgr->GetOption<uint32>("Cortex.Bots.Telemetry.SampleNearMs", 2000));
    TelemetrySampleFarMs =
        std::max<uint32>(5000, sConfigMgr->GetOption<uint32>("Cortex.Bots.Telemetry.SampleFarMs", 60000));
    TelemetryRetentionHours =
        std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Cortex.Bots.Telemetry.RetentionHours", 48));

    // broadcastChanceMaxValue is used in urand(1, broadcastChanceMaxValue) for broadcasts,
    // lowering it will increase the chance, setting it to 0 will disable broadcasts
    // for internal use, not intended to be change by the user
    BroadcastChanceMaxValue = EnableBroadcasts ? 30000 : 0;

    // all broadcast chances should be in range 1-broadcastChanceMaxValue, value of 0 will disable this particular
    // broadcast setting value to max does not guarantee the broadcast, as there are some internal randoms as well
    BroadcastToGuildGlobalChance = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastToGuildGlobalChance", 30000);
    BroadcastToWorldGlobalChance = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastToWorldGlobalChance", 30000);
    BroadcastToGeneralGlobalChance = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastToGeneralGlobalChance", 30000);
    BroadcastToTradeGlobalChance = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastToTradeGlobalChance", 30000);
    BroadcastToLFGGlobalChance = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastToLFGGlobalChance", 30000);
    BroadcastToLocalDefenseGlobalChance =
        sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastToLocalDefenseGlobalChance", 30000);
    BroadcastToWorldDefenseGlobalChance =
        sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastToWorldDefenseGlobalChance", 30000);
    BroadcastToGuildRecruitmentGlobalChance =
        sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastToGuildRecruitmentGlobalChance", 30000);

    BroadcastChanceLootingItemPoor = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceLootingItemPoor", 30);
    BroadcastChanceLootingItemNormal =
        sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceLootingItemNormal", 300);
    BroadcastChanceLootingItemUncommon =
        sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceLootingItemUncommon", 10000);
    BroadcastChanceLootingItemRare = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceLootingItemRare", 20000);
    BroadcastChanceLootingItemEpic = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceLootingItemEpic", 30000);
    BroadcastChanceLootingItemLegendary =
        sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceLootingItemLegendary", 30000);
    BroadcastChanceLootingItemArtifact =
        sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceLootingItemArtifact", 30000);

    BroadcastChanceQuestAccepted = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceQuestAccepted", 6000);
    BroadcastChanceQuestUpdateObjectiveCompleted =
        sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceQuestUpdateObjectiveCompleted", 300);
    BroadcastChanceQuestUpdateObjectiveProgress =
        sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceQuestUpdateObjectiveProgress", 300);
    BroadcastChanceQuestUpdateFailedTimer =
        sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceQuestUpdateFailedTimer", 300);
    BroadcastChanceQuestUpdateComplete =
        sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceQuestUpdateComplete", 1000);
    BroadcastChanceQuestTurnedIn = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceQuestTurnedIn", 10000);

    BroadcastChanceKillNormal = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceKillNormal", 30);
    BroadcastChanceKillElite = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceKillElite", 300);
    BroadcastChanceKillRareelite = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceKillRareelite", 3000);
    BroadcastChanceKillWorldboss = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceKillWorldboss", 20000);
    BroadcastChanceKillRare = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceKillRare", 10000);
    BroadcastChanceKillUnknown = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceKillUnknown", 100);
    BroadcastChanceKillPet = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceKillPet", 10);
    BroadcastChanceKillPlayer = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceKillPlayer", 30);

    BroadcastChanceLevelupGeneric = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceLevelupGeneric", 20000);
    BroadcastChanceLevelupTenX = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceLevelupTenX", 30000);
    BroadcastChanceLevelupMaxLevel = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceLevelupMaxLevel", 30000);

    BroadcastChanceSuggestInstance = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceSuggestInstance", 5000);
    BroadcastChanceSuggestQuest = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceSuggestQuest", 10000);
    BroadcastChanceSuggestGrindMaterials =
        sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceSuggestGrindMaterials", 5000);
    BroadcastChanceSuggestGrindReputation =
        sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceSuggestGrindReputation", 5000);
    BroadcastChanceSuggestSell = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceSuggestSell", 300);
    BroadcastChanceSuggestSomething =
        sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceSuggestSomething", 30000);

    BroadcastChanceSuggestSomethingToxic =
        sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceSuggestSomethingToxic", 0);

    BroadcastChanceSuggestToxicLinks = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceSuggestToxicLinks", 0);
    ToxicLinksPrefix = sConfigMgr->GetOption<std::string>("Cortex.Bots.ToxicLinksPrefix", "gnomes");

    BroadcastChanceSuggestThunderfury =
        sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceSuggestThunderfury", 1);

    // does not depend on global chance
    BroadcastChanceGuildManagement = sConfigMgr->GetOption<int32>("Cortex.Bots.BroadcastChanceGuildManagement", 30000);

    ToxicLinksRepliesChance = sConfigMgr->GetOption<int32>("Cortex.Bots.ToxicLinksRepliesChance", 30);    // 0-100
    ThunderfuryRepliesChance = sConfigMgr->GetOption<int32>("Cortex.Bots.ThunderfuryRepliesChance", 40);  // 0-100
    GuildRepliesRate = sConfigMgr->GetOption<int32>("Cortex.Bots.GuildRepliesRate", 100);                 // 0-100

    RandomBotJoinBG = sConfigMgr->GetOption<bool>("Cortex.Bots.RandomBotJoinBG", true);
    RandomBotAutoJoinBG = sConfigMgr->GetOption<bool>("Cortex.Bots.RandomBotAutoJoinBG", false);

    RandomBotAutoJoinArenaBracket = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotAutoJoinArenaBracket", 14);

    RandomBotAutoJoinWSBrackets = sConfigMgr->GetOption<std::string>("Cortex.Bots.RandomBotAutoJoinWSBrackets", "7");
    RandomBotAutoJoinABBrackets = sConfigMgr->GetOption<std::string>("Cortex.Bots.RandomBotAutoJoinABBrackets", "6");
    RandomBotAutoJoinAVBrackets = sConfigMgr->GetOption<std::string>("Cortex.Bots.RandomBotAutoJoinAVBrackets", "3");
    RandomBotAutoJoinEYBrackets = sConfigMgr->GetOption<std::string>("Cortex.Bots.RandomBotAutoJoinEYBrackets", "2");
    RandomBotAutoJoinICBrackets = sConfigMgr->GetOption<std::string>("Cortex.Bots.RandomBotAutoJoinICBrackets", "1");

    RandomBotAutoJoinBGWSCount = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotAutoJoinBGWSCount", 1);
    RandomBotAutoJoinBGABCount = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotAutoJoinBGABCount", 1);
    RandomBotAutoJoinBGAVCount = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotAutoJoinBGAVCount", 0);
    RandomBotAutoJoinBGEYCount = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotAutoJoinBGEYCount", 1);
    RandomBotAutoJoinBGICCount = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotAutoJoinBGICCount", 0);

    RandomBotAutoJoinBGRatedArena2v2Count =
        sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotAutoJoinBGRatedArena2v2Count", 0);
    RandomBotAutoJoinBGRatedArena3v3Count =
        sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotAutoJoinBGRatedArena3v3Count", 0);
    RandomBotAutoJoinBGRatedArena5v5Count =
        sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotAutoJoinBGRatedArena5v5Count", 0);
    LogInGroupOnly = sConfigMgr->GetOption<bool>("Cortex.Bots.LogInGroupOnly", true);
    LogValuesPerTick = sConfigMgr->GetOption<bool>("Cortex.Bots.LogValuesPerTick", false);
    SummonAtInnkeepersEnabled = sConfigMgr->GetOption<bool>("Cortex.Bots.SummonAtInnkeepersEnabled", true);
    RandomBotMinLevel = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotMinLevel", 1);
    RandomBotMaxLevel = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotMaxLevel", 80);
    if (RandomBotMaxLevel > sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        RandomBotMaxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    // Bracket defaults (below) derive from randomBotMaxLevel, so this must run after it is read.
    LoadRandomBotLevelConfig();

    RandomBotTeleLowerLevel = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotTeleLowerLevel", 1);
    RandomBotTeleHigherLevel = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotTeleHigherLevel", 3);
    OpenGoSpell = sConfigMgr->GetOption<int32>("Cortex.Bots.OpenGoSpell", 6477);

    // Zones for NewRpgStrategy teleportation brackets
    std::vector<uint32> zoneIds = {
        // Classic WoW - Low-level zones
        1, 12, 14, 85, 141, 215, 3430, 3524,
        // Classic WoW - Mid-level zones
        17, 38, 40, 130, 148, 3433, 3525,
        // Classic WoW - High-level zones
        10, 11, 44, 267, 331, 400, 406,
        // Classic WoW - Higher-level zones
        3, 8, 15, 16, 33, 45, 47, 51, 357, 405, 440,
        // Classic WoW - Top-level zones
        4, 28, 46, 139, 361, 490, 618, 1377,
        // The Burning Crusade - Zones
        3483, 3518, 3519, 3520, 3521, 3522, 3523, 4080,
        // Wrath of the Lich King - Zones
        65, 66, 67, 210, 394, 495, 2817, 3537, 3711, 4197
    };

    for (uint32 zoneId : zoneIds)
    {
        std::string setting = "Cortex.Bots.ZoneBracket." + std::to_string(zoneId);
        std::string value = sConfigMgr->GetOption<std::string>(setting, "");

        if (!value.empty())
        {
            size_t commaPos = value.find(',');
            if (commaPos != std::string::npos)
            {
                uint32 minLevel = atoi(value.substr(0, commaPos).c_str());
                uint32 maxLevel = atoi(value.substr(commaPos + 1).c_str());
                ZoneBrackets[zoneId] = std::make_pair(minLevel, maxLevel);
            }
        }
    }

    RandomChangeMultiplier = sConfigMgr->GetOption<float>("Cortex.Bots.RandomChangeMultiplier", 1.0);

    RandomBotCombatStrategies = sConfigMgr->GetOption<std::string>("Cortex.Bots.RandomBotCombatStrategies", "");
    RandomBotNonCombatStrategies = sConfigMgr->GetOption<std::string>("Cortex.Bots.RandomBotNonCombatStrategies", "");
    CombatStrategies = sConfigMgr->GetOption<std::string>("Cortex.Bots.CombatStrategies", "");
    NonCombatStrategies = sConfigMgr->GetOption<std::string>("Cortex.Bots.NonCombatStrategies", "");
    ReactStrategies = sConfigMgr->GetOption<std::string>("Cortex.Bots.ReactStrategies", "");
    RandomBotReactStrategies = sConfigMgr->GetOption<std::string>("Cortex.Bots.RandomBotReactStrategies", "");
    ApplyInstanceStrategies = sConfigMgr->GetOption<bool>("Cortex.Bots.ApplyInstanceStrategies", true);

    CommandPrefix = sConfigMgr->GetOption<std::string>("Cortex.Bots.CommandPrefix", "");
    CommandSeparator = sConfigMgr->GetOption<std::string>("Cortex.Bots.CommandSeparator", "\\\\");

    CommandServerPort = sConfigMgr->GetOption<int32>("Cortex.Bots.CommandServerPort", 8888);
    PerfMonEnabled = sConfigMgr->GetOption<bool>("Cortex.Bots.PerfMonEnabled", false);

    UseGroundMountAtMinLevel = sConfigMgr->GetOption<int32>("Cortex.Bots.UseGroundMountAtMinLevel", 20);
    UseFastGroundMountAtMinLevel = sConfigMgr->GetOption<int32>("Cortex.Bots.UseFastGroundMountAtMinLevel", 40);
    UseFlyMountAtMinLevel = sConfigMgr->GetOption<int32>("Cortex.Bots.UseFlyMountAtMinLevel", 60);
    UseFastFlyMountAtMinLevel = sConfigMgr->GetOption<int32>("Cortex.Bots.UseFastFlyMountAtMinLevel", 70);

    // stagger bot flightpath takeoff
    BotTaxiDelayMin = sConfigMgr->GetOption<uint32>("Cortex.Bots.BotTaxiDelayMinMs", 350);
    BotTaxiDelayMax = sConfigMgr->GetOption<uint32>("Cortex.Bots.BotTaxiDelayMaxMs", 5000);
    BotTaxiGapMs = sConfigMgr->GetOption<uint32>("Cortex.Bots.BotTaxiGapMs", 200);
    BotTaxiGapJitterMs = sConfigMgr->GetOption<uint32>("Cortex.Bots.BotTaxiGapJitterMs", 100);

    LOG_INFO("server.loading", "Loading TalentSpecs...");

    for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
    {
        if (cls == 10)
        {
            continue;
        }
        for (uint32 spec = 0; spec < MAX_SPECNO; ++spec)
        {
            std::ostringstream os;
            os << "Cortex.Bots.PremadeSpecName." << cls << "." << spec;
            PremadeSpecName[cls][spec] = sConfigMgr->GetOption<std::string>(os.str().c_str(), "", false);
            os.str("");
            os.clear();
            os << "Cortex.Bots.PremadeSpecGlyph." << cls << "." << spec;
            PremadeSpecGlyph[cls][spec] = sConfigMgr->GetOption<std::string>(os.str().c_str(), "", false);
            std::vector<std::string> splitSpecGlyph = split(PremadeSpecGlyph[cls][spec], ',');
            for (std::string& split : splitSpecGlyph)
            {
                if (split.size() != 0)
                {
                    ParsedSpecGlyph[cls][spec].push_back(atoi(split.c_str()));
                }
            }
            for (uint32 level = 0; level < MAX_LEVEL; ++level)
            {
                std::ostringstream os;
                os << "Cortex.Bots.PremadeSpecLink." << cls << "." << spec << "." << level;
                PremadeSpecLink[cls][spec][level] = sConfigMgr->GetOption<std::string>(os.str().c_str(), "", false);
                ParsedSpecLinkOrder[cls][spec][level] = ParseTempTalentsOrder(cls, PremadeSpecLink[cls][spec][level]);
            }
        }
        for (uint32 spec = 0; spec < 3; ++spec)
        {
            for (uint32 points = 0; points < 21; ++points)
            {
                std::ostringstream os;
                os << "Cortex.Bots.PremadeHunterPetLink." << spec << "." << points;
                PremadeHunterPetLink[spec][points] = sConfigMgr->GetOption<std::string>(os.str().c_str(), "", false);
                ParsedHunterPetLinkOrder[spec][points] =
                    ParseTempPetTalentsOrder(spec, PremadeHunterPetLink[spec][points]);
            }
        }
        for (uint32 spec = 0; spec < MAX_SPECNO; ++spec)
        {
            std::ostringstream os;
            os << "Cortex.Bots.RandomClassSpecProb." << cls << "." << spec;
            uint32 def;
            if (spec <= 1)
                def = 33;
            else if (spec == 2)
                def = 34;
            else
                def = 0;
            RandomClassSpecProb[cls][spec] = sConfigMgr->GetOption<uint32>(os.str().c_str(), def, false);
            os.str("");
            os.clear();
            os << "Cortex.Bots.RandomClassSpecIndex." << cls << "." << spec;
            RandomClassSpecIndex[cls][spec] = sConfigMgr->GetOption<uint32>(os.str().c_str(), spec, false);
        }
    }

    BotCheats.clear();
    LoadListString<std::vector<std::string>>(sConfigMgr->GetOption<std::string>("Cortex.Bots.BotCheats", "food,taxi,raid"),
                                             BotCheats);

    BotCheatMask = 0;

    if (std::find(BotCheats.begin(), BotCheats.end(), "food") != BotCheats.end())
        BotCheatMask |= (uint32)BotCheatMask::food;
    if (std::find(BotCheats.begin(), BotCheats.end(), "taxi") != BotCheats.end())
        BotCheatMask |= (uint32)BotCheatMask::taxi;
    if (std::find(BotCheats.begin(), BotCheats.end(), "gold") != BotCheats.end())
        BotCheatMask |= (uint32)BotCheatMask::gold;
    if (std::find(BotCheats.begin(), BotCheats.end(), "health") != BotCheats.end())
        BotCheatMask |= (uint32)BotCheatMask::health;
    if (std::find(BotCheats.begin(), BotCheats.end(), "mana") != BotCheats.end())
        BotCheatMask |= (uint32)BotCheatMask::mana;
    if (std::find(BotCheats.begin(), BotCheats.end(), "power") != BotCheats.end())
        BotCheatMask |= (uint32)BotCheatMask::power;
    if (std::find(BotCheats.begin(), BotCheats.end(), "raid") != BotCheats.end())
        BotCheatMask |= (uint32)BotCheatMask::raid;

    LoadListString<std::vector<std::string>>(sConfigMgr->GetOption<std::string>("Cortex.Bots.AllowedLogFiles", ""),
                                             AllowedLogFiles);
    EnableAutoTradeOnItemMention = sConfigMgr->GetOption<bool>("Cortex.Bots.EnableAutoTradeOnItemMention", true);
    LoadListString<std::vector<std::string>>(sConfigMgr->GetOption<std::string>("Cortex.Bots.TradeActionExcludedPrefixes", ""),
                                             TradeActionExcludedPrefixes);

    WorldBuffs.clear();
    LoadWorldBuff();
    LOG_INFO("playerbots", "Loading World Buff Feature...");

    RandomBotAccountPrefix = sConfigMgr->GetOption<std::string>("Cortex.Bots.RandomBotAccountPrefix", "rndbot");
    RandomBotAccountCount = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotAccountCount", 0);
    DeleteRandomBotAccounts = sConfigMgr->GetOption<bool>("Cortex.Bots.DeleteRandomBotAccounts", false);
    RandomBotGuildCount = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotGuildCount", 20);
    RandomBotGuildSizeMax = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotGuildSizeMax", 15);
    RandomBotGuildFounderChance = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotGuildFounderChance", 5);
    DeleteRandomBotGuilds = sConfigMgr->GetOption<bool>("Cortex.Bots.DeleteRandomBotGuilds", false);

    BotSendMailEnabled = sConfigMgr->GetOption<bool>("Cortex.Bots.BotSendMailEnabled", true);

    GuildTaskEnabled = sConfigMgr->GetOption<bool>("Cortex.Bots.EnableGuildTasks", false);
    MinGuildTaskChangeTime = sConfigMgr->GetOption<int32>("Cortex.Bots.MinGuildTaskChangeTime", 3 * 24 * 3600);
    MaxGuildTaskChangeTime = sConfigMgr->GetOption<int32>("Cortex.Bots.MaxGuildTaskChangeTime", 4 * 24 * 3600);
    MinGuildTaskAdvertisementTime = sConfigMgr->GetOption<int32>("Cortex.Bots.MinGuildTaskAdvertisementTime", 300);
    MaxGuildTaskAdvertisementTime = sConfigMgr->GetOption<int32>("Cortex.Bots.MaxGuildTaskAdvertisementTime", 12 * 3600);
    MinGuildTaskRewardTime = sConfigMgr->GetOption<int32>("Cortex.Bots.MinGuildTaskRewardTime", 300);
    MaxGuildTaskRewardTime = sConfigMgr->GetOption<int32>("Cortex.Bots.MaxGuildTaskRewardTime", 3600);
    GuildTaskAdvertCleanupTime = sConfigMgr->GetOption<int32>("Cortex.Bots.GuildTaskAdvertCleanupTime", 300);
    GuildTaskKillTaskDistance = sConfigMgr->GetOption<int32>("Cortex.Bots.GuildTaskKillTaskDistance", 2000);
    TargetPosRecalcDistance = sConfigMgr->GetOption<float>("Cortex.Bots.TargetPosRecalcDistance", 0.1f);

    //cosmetics
    switch (sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotShowHelmet", 1))
    {
        case 0:
            RandomBotShowHelmet = ShowHideCosmetic::ALWAYS_HIDE;
            break;
        case 2:
            RandomBotShowHelmet = ShowHideCosmetic::RANDOMIZE;
            break;
        case 1:
        default:
            RandomBotShowHelmet = ShowHideCosmetic::ALWAYS_SHOW;
            break;
    }
    switch (sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotShowCloak", 1))
    {
        case 0:
            RandomBotShowCloak = ShowHideCosmetic::ALWAYS_HIDE;
            break;
        case 2:
            RandomBotShowCloak = ShowHideCosmetic::RANDOMIZE;
            break;
        case 1:
        default:
            RandomBotShowCloak = ShowHideCosmetic::ALWAYS_SHOW;
            break;
    }

    // SPP switches
    EnableGreet = sConfigMgr->GetOption<bool>("Cortex.Bots.EnableGreet", true);
    SummonWhenGroup = sConfigMgr->GetOption<bool>("Cortex.Bots.SummonWhenGroup", true);
    RandomBotFixedLevel = sConfigMgr->GetOption<bool>("Cortex.Bots.RandomBotFixedLevel", false);
    DisableRandomLevels = sConfigMgr->GetOption<bool>("Cortex.Bots.DisableRandomLevels", false);
    RandomBotFreshStart = sConfigMgr->GetOption<bool>("Cortex.Bots.RandomBotFreshStart", false);
    RandomBotRandomPassword = sConfigMgr->GetOption<bool>("Cortex.Bots.RandomBotRandomPassword", true);
    DowngradeMaxLevelBot = sConfigMgr->GetOption<bool>("Cortex.Bots.DowngradeMaxLevelBot", true);
    EquipAndSpecPersistence = sConfigMgr->GetOption<bool>("Cortex.Bots.EquipAndSpecPersistence", true);
    EquipAndSpecPersistenceLevel = sConfigMgr->GetOption<int32>("Cortex.Bots.EquipAndSpecPersistenceLevel", 1);
    GroupInvitationPermission = sConfigMgr->GetOption<int32>("Cortex.Bots.GroupInvitationPermission", 1);
    AllowBattlegroundCommands = sConfigMgr->GetOption<bool>("Cortex.Bots.AllowBattlegroundCommands", false);
    KeepAltsInGroup = sConfigMgr->GetOption<bool>("Cortex.Bots.KeepAltsInGroup", false);
    AllowSummonInCombat = sConfigMgr->GetOption<bool>("Cortex.Bots.AllowSummonInCombat", true);
    AllowSummonWhenMasterIsDead = sConfigMgr->GetOption<bool>("Cortex.Bots.AllowSummonWhenMasterIsDead", true);
    AllowSummonWhenBotIsDead = sConfigMgr->GetOption<bool>("Cortex.Bots.AllowSummonWhenBotIsDead", true);
    ReviveBotWhenSummoned = sConfigMgr->GetOption<int32>("Cortex.Bots.ReviveBotWhenSummoned", 1);
    BotRepairWhenSummon = sConfigMgr->GetOption<bool>("Cortex.Bots.BotRepairWhenSummon", true);
    AutoInitOnly = sConfigMgr->GetOption<bool>("Cortex.Bots.AutoInitOnly", false);
    ResetInstanceIdForAltBots = sConfigMgr->GetOption<bool>("Cortex.Bots.ResetInstanceIdForAltBots", false);
    AutoInitEquipLevelLimitRatio = sConfigMgr->GetOption<float>("Cortex.Bots.AutoInitEquipLevelLimitRatio", 1.0);

    MaxAddedBots = sConfigMgr->GetOption<int32>("Cortex.Bots.MaxAddedBots", 40);
    AddClassCommand = sConfigMgr->GetOption<int32>("Cortex.Bots.AddClassCommand", 1);
    AddClassAccountPoolSize = sConfigMgr->GetOption<int32>("Cortex.Bots.AddClassAccountPoolSize", 50);
    AddClassRandomCharacter = sConfigMgr->GetOption<bool>("Cortex.Bots.AddClassRandomCharacter", false);
    MaintenanceCommand = sConfigMgr->GetOption<int32>("Cortex.Bots.MaintenanceCommand", 1);

    AltMaintenanceAttunementQs = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenanceAttunementQuests", true);
    AltMaintenanceBags = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenanceBags", true);
    AltMaintenanceAmmo = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenanceAmmo", true);
    AltMaintenanceFood = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenanceFood", true);
    AltMaintenanceReagents = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenanceReagents", true);
    AltMaintenanceConsumables = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenanceConsumables", true);
    AltMaintenancePotions = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenancePotions", true);
    AltMaintenanceTalentTree = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenanceTalentTree", true);
    AltMaintenancePet = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenancePet", true);
    AltMaintenancePetTalents = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenancePetTalents", true);
    AltMaintenanceClassSpells = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenanceClassSpells", true);
    AltMaintenanceAvailableSpells = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenanceAvailableSpells", true);
    AltMaintenanceSkills = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenanceSkills", true);
    AltMaintenanceReputation = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenanceReputation", true);
    AltMaintenanceSpecialSpells = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenanceSpecialSpells", true);
    AltMaintenanceMounts = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenanceMounts", true);
    AltMaintenanceGlyphs = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenanceGlyphs", true);
    AltMaintenanceKeyring = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenanceKeyring", true);
    AltMaintenanceGemsEnchants = sConfigMgr->GetOption<bool>("Cortex.Bots.AltMaintenanceGemsEnchants", true);

    AutoGearCommand = sConfigMgr->GetOption<int32>("Cortex.Bots.AutoGearCommand", 1);
    AutoGearCommandAltBots = sConfigMgr->GetOption<int32>("Cortex.Bots.AutoGearCommandAltBots", 1);
    AutoGearBisCommand = sConfigMgr->GetOption<int32>("Cortex.Bots.AutoGearBisCommand", 0);
    AutoGearQualityLimit = sConfigMgr->GetOption<int32>("Cortex.Bots.AutoGearQualityLimit", 3);
    AutoGearScoreLimit = sConfigMgr->GetOption<int32>("Cortex.Bots.AutoGearScoreLimit", 0);

    RandomBotXPRate = sConfigMgr->GetOption<float>("Cortex.Bots.RandomBotXPRate", 1.0);
    RandomBotAllianceRatio = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotAllianceRatio", 50);
    RandomBotHordeRatio = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotHordeRatio", 50);
    DisableDeathKnightLogin = sConfigMgr->GetOption<bool>("Cortex.Bots.DisableDeathKnightLogin", 0);
    LimitTalentsExpansion = sConfigMgr->GetOption<bool>("Cortex.Bots.LimitTalentsExpansion", 0);
    BotActiveAlone = sConfigMgr->GetOption<int32>("Cortex.Bots.BotActiveAlone", 10);
    BotActiveAloneDurationSeconds = sConfigMgr->GetOption<int32>("Cortex.Bots.BotActiveAloneDurationSeconds", 30);
    BotActiveAloneForceWhenInRadius = sConfigMgr->GetOption<uint32>("Cortex.Bots.BotActiveAloneForceWhenInRadius", 150);
    BotActiveAloneForceWhenInZone = sConfigMgr->GetOption<bool>("Cortex.Bots.BotActiveAloneForceWhenInZone", 1);
    BotActiveAloneForceWhenInMap = sConfigMgr->GetOption<bool>("Cortex.Bots.BotActiveAloneForceWhenInMap", 0);
    BotActiveAloneForceWhenIsFriend = sConfigMgr->GetOption<bool>("Cortex.Bots.BotActiveAloneForceWhenIsFriend", 0);
    BotActiveAloneForceWhenInGuild = sConfigMgr->GetOption<bool>("Cortex.Bots.BotActiveAloneForceWhenInGuild", 1);
    BotActiveAloneSmartScale = sConfigMgr->GetOption<bool>("Cortex.Bots.botActiveAloneSmartScale", 1);
    BotActiveAloneSmartScaleDiffLimitFloor = sConfigMgr->GetOption<uint32>("Cortex.Bots.botActiveAloneSmartScaleDiffLimitfloor", 50);
    BotActiveAloneSmartScaleDiffLimitCeiling = sConfigMgr->GetOption<uint32>("Cortex.Bots.botActiveAloneSmartScaleDiffLimitCeiling", 200);
    BotActiveAloneSmartScaleWhenMinLevel = sConfigMgr->GetOption<uint32>("Cortex.Bots.botActiveAloneSmartScaleWhenMinLevel", 1);
    BotActiveAloneSmartScaleWhenMaxLevel = sConfigMgr->GetOption<uint32>("Cortex.Bots.botActiveAloneSmartScaleWhenMaxLevel", 80);

    RandomBotsWalkingRPG = sConfigMgr->GetOption<bool>("Cortex.Bots.RandombotsWalkingRPG", false);
    RandomBotsWalkingRPGInDoors = sConfigMgr->GetOption<bool>("Cortex.Bots.RandombotsWalkingRPG.InDoors", false);
    MinEnchantingBotLevel = sConfigMgr->GetOption<int32>("Cortex.Bots.MinEnchantingBotLevel", 60);
    LimitEnchantExpansion = sConfigMgr->GetOption<int32>("Cortex.Bots.LimitEnchantExpansion", 1);
    LimitGearExpansion = sConfigMgr->GetOption<int32>("Cortex.Bots.LimitGearExpansion", 1);
    RandomBotStartingLevel = sConfigMgr->GetOption<int32>("Cortex.Bots.RandombotStartingLevel", 1);
    EnablePeriodicOnlineOffline = sConfigMgr->GetOption<bool>("Cortex.Bots.EnablePeriodicOnlineOffline", false);
    EnableRandomBotTrading = sConfigMgr->GetOption<int32>("Cortex.Bots.EnableRandomBotTrading", 1);
    PeriodicOnlineOfflineRatio = sConfigMgr->GetOption<float>("Cortex.Bots.PeriodicOnlineOfflineRatio", 2.0);
    GearScoreCheck = sConfigMgr->GetOption<bool>("Cortex.Bots.GearScoreCheck", false);
    RandomBotPreQuests = sConfigMgr->GetOption<bool>("Cortex.Bots.PreQuests", false);

    // SPP automation
    FreeMethodLoot = sConfigMgr->GetOption<bool>("Cortex.Bots.FreeMethodLoot", false);
    LootNeedRollLevel = sConfigMgr->GetOption<int32>("Cortex.Bots.LootNeedRollLevel", 1);
    LootRollRecipe = sConfigMgr->GetOption<bool>("Cortex.Bots.LootRollRecipe", false);
    LootRollDisenchant = sConfigMgr->GetOption<bool>("Cortex.Bots.LootRollDisenchant", false);
    LootGreedRollLevel = sConfigMgr->GetOption<bool>("Cortex.Bots.LootGreedRollLevel", false);
    AutoPickReward = sConfigMgr->GetOption<std::string>("Cortex.Bots.AutoPickReward", "yes");
    AutoEquipUpgradeLoot = sConfigMgr->GetOption<bool>("Cortex.Bots.AutoEquipUpgradeLoot", true);
    EquipUpgradeThreshold = sConfigMgr->GetOption<float>("Cortex.Bots.EquipUpgradeThreshold", 1.1f);
    TwoRoundsGearInit = sConfigMgr->GetOption<bool>("Cortex.Bots.TwoRoundsGearInit", false);
    SyncQuestWithPlayer = sConfigMgr->GetOption<bool>("Cortex.Bots.SyncQuestWithPlayer", true);
    SyncQuestForPlayer = sConfigMgr->GetOption<bool>("Cortex.Bots.SyncQuestForPlayer", false);
    DropObsoleteQuests = sConfigMgr->GetOption<bool>("Cortex.Bots.DropObsoleteQuests", true);
    AllowLearnTrainerSpells = sConfigMgr->GetOption<bool>("Cortex.Bots.AllowLearnTrainerSpells", true);
    AutoPickTalents = sConfigMgr->GetOption<bool>("Cortex.Bots.AutoPickTalents", true);
    AutoUpgradeEquip = sConfigMgr->GetOption<bool>("Cortex.Bots.AutoUpgradeEquip", true);
    HunterWolfPet = sConfigMgr->GetOption<int32>("Cortex.Bots.HunterWolfPet", 0);
    DefaultPetStance = sConfigMgr->GetOption<int32>("Cortex.Bots.DefaultPetStance", 1);
    PetChatCommandDebug = sConfigMgr->GetOption<bool>("Cortex.Bots.PetChatCommandDebug", 0);
    AutoLearnTrainerSpells = sConfigMgr->GetOption<bool>("Cortex.Bots.AutoLearnTrainerSpells", true);
    AutoLearnQuestSpells = sConfigMgr->GetOption<bool>("Cortex.Bots.AutoLearnQuestSpells", true);
    AutoTeleportForLevel = sConfigMgr->GetOption<bool>("Cortex.Bots.AutoTeleportForLevel", false);
    AutoDoQuests = sConfigMgr->GetOption<bool>("Cortex.Bots.AutoDoQuests", true);
    EnableNewRpgStrategy = sConfigMgr->GetOption<bool>("Cortex.Bots.EnableNewRpgStrategy", true);
    NewRpgQuestCare = sConfigMgr->GetOption<bool>("Cortex.Bots.NewRpgQuestCare", false);
    NewRpgQuestMaxLevelAbove = sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgQuestMaxLevelAbove", 1);
    NewRpgQuestMaxDeaths = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgQuestMaxDeaths", 2));
    NewRpgQuestNoProgressSeconds =
        std::max<uint32>(60, sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgQuestNoProgressSeconds", 240));
    NewRpgQuestRevisitLevels = sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgQuestRevisitLevels", 2);
    NewRpgQuestRevisitMinutes = sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgQuestRevisitMinutes", 45);
    NewRpgRecover = sConfigMgr->GetOption<bool>("Cortex.Bots.NewRpgRecover", false);
    NewRpgRecoverHealthPct =
        std::min<uint32>(95, sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgRecoverHealthPct", 60));
    NewRpgRecoverManaPct = std::min<uint32>(95, sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgRecoverManaPct", 40));
    NewRpgRecoverMaxSeconds = sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgRecoverMaxSeconds", 150);
    NewRpgCrossZone = sConfigMgr->GetOption<bool>("Cortex.Bots.NewRpgCrossZone", false);
    NewRpgWalkUnstick = sConfigMgr->GetOption<bool>("Cortex.Bots.NewRpgWalkUnstick", false);
    NewRpgUnreachableMinutes = sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgUnreachableMinutes", 30);
    NewRpgUpkeep = sConfigMgr->GetOption<bool>("Cortex.Bots.NewRpgUpkeep", false);
    NewRpgUpkeepBagPct = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgUpkeepBagPct", 80));
    NewRpgUpkeepDurabilityPct =
        std::min<uint32>(100, sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgUpkeepDurabilityPct", 50));
    NewRpgUpkeepBuy = sConfigMgr->GetOption<bool>("Cortex.Bots.NewRpgUpkeepBuy", true);
    NewRpgGearCheckSeconds = sConfigMgr->GetOption<uint32>("Cortex.Bots.NewRpgGearCheckSeconds", 0);
    OffRoleStatPenalty = sConfigMgr->GetOption<bool>("Cortex.Bots.OffRoleStatPenalty", true);
    NewRpgQuestTidy = sConfigMgr->GetOption<bool>("Cortex.Bots.NewRpgQuestTidy", false);
    BotProfessions::LoadConfig();
    BotTelemetry::LoadConfig();

    RpgStatusProbWeight[RPG_WANDER_RANDOM] = sConfigMgr->GetOption<int32>("Cortex.Bots.RpgStatusProbWeight.WanderRandom", 15);
    RpgStatusProbWeight[RPG_WANDER_NPC] = sConfigMgr->GetOption<int32>("Cortex.Bots.RpgStatusProbWeight.WanderNpc", 20);
    RpgStatusProbWeight[RPG_GO_GRIND] = sConfigMgr->GetOption<int32>("Cortex.Bots.RpgStatusProbWeight.GoGrind", 15);
    RpgStatusProbWeight[RPG_GO_CAMP] = sConfigMgr->GetOption<int32>("Cortex.Bots.RpgStatusProbWeight.GoCamp", 10);
    RpgStatusProbWeight[RPG_DO_QUEST] = sConfigMgr->GetOption<int32>("Cortex.Bots.RpgStatusProbWeight.DoQuest", 60);
    RpgStatusProbWeight[RPG_TRAVEL_FLIGHT] = sConfigMgr->GetOption<int32>("Cortex.Bots.RpgStatusProbWeight.TravelFlight", 15);
    RpgStatusProbWeight[RPG_REST] = sConfigMgr->GetOption<int32>("Cortex.Bots.RpgStatusProbWeight.Rest", 5);
    RpgStatusProbWeight[RPG_OUTDOOR_PVP] = sConfigMgr->GetOption<int32>("Cortex.Bots.RpgStatusProbWeight.OutdoorPvp", 10);
    RpgStatusProbWeight[RPG_DO_GATHER] = sConfigMgr->GetOption<int32>("Cortex.Bots.RpgStatusProbWeight.DoGather", 15);

    SyncLevelWithPlayers = sConfigMgr->GetOption<bool>("Cortex.Bots.SyncLevelWithPlayers", false);
    RandomBotConcentrateInPlayerZone =
        sConfigMgr->GetOption<bool>("Cortex.Bots.RandomBotConcentrateInPlayerZone", false);
    RandomBotGroupNearby = sConfigMgr->GetOption<bool>("Cortex.Bots.RandomBotGroupNearby", false);

    // arena
    RandomBotArenaTeam2v2Count = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotArenaTeam2v2Count", 10);
    RandomBotArenaTeam3v3Count = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotArenaTeam3v3Count", 10);
    RandomBotArenaTeam5v5Count = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotArenaTeam5v5Count", 5);
    DeleteRandomBotArenaTeams = sConfigMgr->GetOption<bool>("Cortex.Bots.DeleteRandomBotArenaTeams", false);
    RandomBotArenaTeamMaxRating = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotArenaTeamMaxRating", 2000);
    RandomBotArenaTeamMinRating = sConfigMgr->GetOption<int32>("Cortex.Bots.RandomBotArenaTeamMinRating", 1000);

    SelfBotLevel = sConfigMgr->GetOption<int32>("Cortex.Bots.SelfBotLevel", 1);

    RandomPlayerbotFactory::CreateRandomBots();
    if (World::IsStopped())
    {
        return true;
    }

    // Assign account types after accounts are created
    sRandomPlayerbotMgr.AssignAccountTypes();

    if (sPlayerbotAIConfig.Enabled)
    {
        sRandomPlayerbotMgr.Init();
    }

    PlayerbotGuildMgr::instance().Init();
    sRandomPlayerbotMgr.InitArenaTeams();
    sRandomItemMgr.Init();
    sRandomItemMgr.InitAfterAhBot();
    sBisListMgr->LoadAll();
    PlayerbotTextMgr::instance().LoadBotTexts();
    PlayerbotTextMgr::instance().LoadBotTextChance();
    PlayerbotTextMgr::instance().LoadPersonas();
    PlayerbotFactory::Init();

    AiObjectContext::BuildAllSharedContexts();

    if (sPlayerbotAIConfig.RandomBotSuggestDungeons)
    {
        PlayerbotDungeonRepository::instance().LoadDungeonSuggestions();
    }
    sTravelMgr.Init();

    ExcludedHunterPetFamilies.clear();
    LoadList<std::vector<uint32>>(sConfigMgr->GetOption<std::string>("Cortex.Bots.ExcludedHunterPetFamilies", ""), ExcludedHunterPetFamilies);

    LOG_INFO("server.loading", "---------------------------------------");
    LOG_INFO("server.loading", "       mod-playerbots initialized      ");
    LOG_INFO("server.loading", "---------------------------------------");

    return true;
}

// Loads Cortex.Bots.LevelBrackets.* and Cortex.Bots.ResetBotLevel.* (see RandomBotLevelMgr). Also
// re-run on ".reload config" via RandomBotLevelWorldScript::OnAfterConfigLoad. Bracket
// bounds/percentages are only the as-configured values here; RandomBotLevelMgr::LoadConfig()
// copies them into its own working state, since dynamic distribution and clamp/rebalance mutate
// percentages at runtime.
void PlayerbotAIConfig::LoadRandomBotLevelConfig()
{
    // ---- Level brackets ----
    LevelBracketsEnabled = sConfigMgr->GetOption<bool>("Cortex.Bots.LevelBrackets.Enabled", false);
    LevelBracketsIgnoreGuildWithRealPlayers =
        sConfigMgr->GetOption<bool>("Cortex.Bots.LevelBrackets.IgnoreGuildBotsWithRealPlayers", true);
    LevelBracketsIgnoreArenaTeamBots =
        sConfigMgr->GetOption<bool>("Cortex.Bots.LevelBrackets.IgnoreArenaTeamBots", true);

    LevelBracketsCheckFrequency = sConfigMgr->GetOption<uint32>("Cortex.Bots.LevelBrackets.CheckFrequency", 300);
    LevelBracketsFlaggedCheckFrequency =
        sConfigMgr->GetOption<uint32>("Cortex.Bots.LevelBrackets.CheckFlaggedFrequency", 15);
    LevelBracketsDynamicDistribution =
        sConfigMgr->GetOption<bool>("Cortex.Bots.LevelBrackets.Dynamic.UseDynamicDistribution", false);
    LevelBracketsRealPlayerWeight =
        sConfigMgr->GetOption<float>("Cortex.Bots.LevelBrackets.Dynamic.RealPlayerWeight", 1.0f);
    LevelBracketsSyncFactions = sConfigMgr->GetOption<bool>("Cortex.Bots.LevelBrackets.Dynamic.SyncFactions", false);
    LevelBracketsIgnoreFriendListed = sConfigMgr->GetOption<bool>("Cortex.Bots.LevelBrackets.IgnoreFriendListed", true);
    LevelBracketsFlaggedProcessLimit =
        sConfigMgr->GetOption<uint32>("Cortex.Bots.LevelBrackets.FlaggedProcessLimit", 5);

    ParseLevelMgrExcludeNames(sConfigMgr->GetOption<std::string>("Cortex.Bots.LevelBrackets.ExcludeNames", ""),
        LevelBracketsExcludeNames);

    LevelBracketsNumRanges =
        static_cast<uint8>(sConfigMgr->GetOption<uint32>("Cortex.Bots.LevelBrackets.NumRanges", 9));
    LevelBracketsAlliance.resize(LevelBracketsNumRanges);
    LevelBracketsHorde.resize(LevelBracketsNumRanges);

    for (uint8 i = 0; i < LevelBracketsNumRanges; ++i)
    {
        std::string idx = std::to_string(i + 1);
        uint32 defaultLower = (i == 0 ? 1 : i * 10);
        uint32 defaultUpper = (i < LevelBracketsNumRanges - 1 ? i * 10 + 9 : RandomBotMaxLevel);
        LevelBracketsAlliance[i].Lower = static_cast<uint8>(
            sConfigMgr->GetOption<uint32>("Cortex.Bots.LevelBrackets.Alliance.Range" + idx + ".Lower", defaultLower));
        LevelBracketsAlliance[i].Upper = static_cast<uint8>(
            sConfigMgr->GetOption<uint32>("Cortex.Bots.LevelBrackets.Alliance.Range" + idx + ".Upper", defaultUpper));
        LevelBracketsAlliance[i].Pct = static_cast<uint8>(
            sConfigMgr->GetOption<uint32>("Cortex.Bots.LevelBrackets.Alliance.Range" + idx + ".Pct", 11));
    }

    for (uint8 i = 0; i < LevelBracketsNumRanges; ++i)
    {
        std::string idx = std::to_string(i + 1);
        uint32 defaultLower = (i == 0 ? 1 : i * 10);
        uint32 defaultUpper = (i < LevelBracketsNumRanges - 1 ? i * 10 + 9 : RandomBotMaxLevel);
        LevelBracketsHorde[i].Lower = static_cast<uint8>(
            sConfigMgr->GetOption<uint32>("Cortex.Bots.LevelBrackets.Horde.Range" + idx + ".Lower", defaultLower));
        LevelBracketsHorde[i].Upper = static_cast<uint8>(
            sConfigMgr->GetOption<uint32>("Cortex.Bots.LevelBrackets.Horde.Range" + idx + ".Upper", defaultUpper));
        LevelBracketsHorde[i].Pct = static_cast<uint8>(
            sConfigMgr->GetOption<uint32>("Cortex.Bots.LevelBrackets.Horde.Range" + idx + ".Pct", 11));
    }

    // A mismatch forcibly disables SyncFactions and logs an error; it never brings the server down.
    if (LevelBracketsSyncFactions)
    {
        for (uint8 i = 0; i < LevelBracketsNumRanges; ++i)
        {
            if (LevelBracketsAlliance[i].Lower != LevelBracketsHorde[i].Lower ||
                LevelBracketsAlliance[i].Upper != LevelBracketsHorde[i].Upper)
            {
                LOG_ERROR("server.loading",
                    "[RandomBotLevelMgr] Bracket mismatch detected between factions at index {}. Alliance: {}-{}, "
                    "Horde: {}-{}. SyncFactions requires both bracket count and min/max levels to match exactly; "
                    "forcibly disabling SyncFactions for this session. Check your configuration.",
                    i, LevelBracketsAlliance[i].Lower, LevelBracketsAlliance[i].Upper,
                    LevelBracketsHorde[i].Lower, LevelBracketsHorde[i].Upper);
                LevelBracketsSyncFactions = false;
                break;
            }
        }
    }

    // ---- Level reset ----
    ResetBotLevelEnabled = sConfigMgr->GetOption<bool>("Cortex.Bots.ResetBotLevel.Enabled", false);

    ResetBotLevelMaxLevel =
        static_cast<uint8>(sConfigMgr->GetOption<uint32>("Cortex.Bots.ResetBotLevel.MaxLevel", 80));
    if ((ResetBotLevelMaxLevel < 2 || ResetBotLevelMaxLevel > 80) && ResetBotLevelMaxLevel != 0)
    {
        LOG_ERROR("server.loading",
            "[RandomBotLevelMgr] Invalid Cortex.Bots.ResetBotLevel.MaxLevel value: {}. Using default value 80.",
            ResetBotLevelMaxLevel);
        ResetBotLevelMaxLevel = 80;
    }

    ResetBotLevelResetTo =
        static_cast<uint8>(sConfigMgr->GetOption<uint32>("Cortex.Bots.ResetBotLevel.ResetToLevel", 1));
    if (ResetBotLevelResetTo < 1 || (ResetBotLevelMaxLevel > 0 && ResetBotLevelResetTo >= ResetBotLevelMaxLevel))
    {
        LOG_ERROR("server.loading",
            "[RandomBotLevelMgr] Invalid Cortex.Bots.ResetBotLevel.ResetToLevel value: {}. Using default value 1.",
            ResetBotLevelResetTo);
        ResetBotLevelResetTo = 1;
    }

    ResetBotLevelSkipFrom =
        static_cast<uint8>(sConfigMgr->GetOption<uint32>("Cortex.Bots.ResetBotLevel.SkipFromLevel", 0));
    if (ResetBotLevelSkipFrom > 80 || (ResetBotLevelMaxLevel > 0 && ResetBotLevelSkipFrom >= ResetBotLevelMaxLevel))
    {
        LOG_ERROR("server.loading",
            "[RandomBotLevelMgr] Invalid Cortex.Bots.ResetBotLevel.SkipFromLevel value: {}. Using default value 0 "
            "(disabled).",
            ResetBotLevelSkipFrom);
        ResetBotLevelSkipFrom = 0;
    }

    ResetBotLevelSkipTo = static_cast<uint8>(sConfigMgr->GetOption<uint32>("Cortex.Bots.ResetBotLevel.SkipToLevel", 1));
    if (ResetBotLevelSkipTo < 1 || ResetBotLevelSkipTo > 80 ||
        (ResetBotLevelMaxLevel > 0 && ResetBotLevelSkipTo > ResetBotLevelMaxLevel))
    {
        LOG_ERROR("server.loading",
            "[RandomBotLevelMgr] Invalid Cortex.Bots.ResetBotLevel.SkipToLevel value: {}. Using default value 1.",
            ResetBotLevelSkipTo);
        ResetBotLevelSkipTo = 1;
    }

    ResetBotLevelChance =
        static_cast<uint8>(sConfigMgr->GetOption<uint32>("Cortex.Bots.ResetBotLevel.ResetChance", 100));
    if (ResetBotLevelChance > 100)
    {
        LOG_ERROR("server.loading",
            "[RandomBotLevelMgr] Invalid Cortex.Bots.ResetBotLevel.ResetChance value: {}. Using default value 100.",
            ResetBotLevelChance);
        ResetBotLevelChance = 100;
    }

    ResetBotLevelScaledChance = sConfigMgr->GetOption<bool>("Cortex.Bots.ResetBotLevel.ScaledChance", false);

    ResetBotLevelRestrictTimePlayed =
        sConfigMgr->GetOption<bool>("Cortex.Bots.ResetBotLevel.RestrictTimePlayed", false);
    ResetBotLevelMinTimePlayed = sConfigMgr->GetOption<uint32>("Cortex.Bots.ResetBotLevel.MinTimePlayed", 86400);
    ResetBotLevelPlayedTimeCheckFrequency =
        sConfigMgr->GetOption<uint32>("Cortex.Bots.ResetBotLevel.PlayedTimeCheckFrequency", 864);

    ResetBotLevelIgnoreGuildWithRealPlayers =
        sConfigMgr->GetOption<bool>("Cortex.Bots.ResetBotLevel.IgnoreGuildBotsWithRealPlayers", false);

    ParseLevelMgrExcludeNames(sConfigMgr->GetOption<std::string>("Cortex.Bots.ResetBotLevel.ExcludeNames", ""),
        ResetBotLevelExcludeNames);
}

bool PlayerbotAIConfig::IsInRandomAccountList(uint32 id)
{
    return find(RandomBotAccounts.begin(), RandomBotAccounts.end(), id) != RandomBotAccounts.end();
}

bool PlayerbotAIConfig::IsInRandomQuestItemList(uint32 id)
{
    return find(RandomBotQuestItems.begin(), RandomBotQuestItems.end(), id) != RandomBotQuestItems.end();
}

bool PlayerbotAIConfig::IsPvpProhibited(uint32 zoneId, uint32 areaId)
{
    return IsInPvpProhibitedZone(zoneId) || IsInPvpProhibitedArea(areaId) || IsInPvpProhibitedZone(areaId);
}

bool PlayerbotAIConfig::IsInPvpProhibitedZone(uint32 id)
{
    return find(PvpProhibitedZoneIds.begin(), PvpProhibitedZoneIds.end(), id) != PvpProhibitedZoneIds.end();
}

bool PlayerbotAIConfig::IsInPvpProhibitedArea(uint32 id)
{
    return find(PvpProhibitedAreaIds.begin(), PvpProhibitedAreaIds.end(), id) != PvpProhibitedAreaIds.end();
}

bool PlayerbotAIConfig::IsRestrictedHealerDPSMap(uint32 mapId) const
{
    return RestrictHealerDPS &&
            std::find(RestrictedHealerDPSMaps.begin(), RestrictedHealerDPSMaps.end(), mapId) != RestrictedHealerDPSMaps.end();
}

std::string const PlayerbotAIConfig::GetTimestampStr()
{
    time_t t = time(nullptr);
    tm* aTm = localtime(&t);
    //       YYYY   year
    //       MM     month (2 digits 01-12)
    //       DD     day (2 digits 01-31)
    //       HH     hour (2 digits 00-23)
    //       MM     minutes (2 digits 00-59)
    //       SS     seconds (2 digits 00-59)
    // Sized for the widest output snprintf can produce for these int conversions, so the
    // result is never truncated.
    char buf[128];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d-%02d-%02d", aTm->tm_year + 1900, aTm->tm_mon + 1, aTm->tm_mday, aTm->tm_hour,
             aTm->tm_min, aTm->tm_sec);
    return std::string(buf);
}

bool PlayerbotAIConfig::OpenLog(std::string const fileName, char const* mode)
{
    if (!HasLog(fileName))
        return false;

    auto logFileIt = LogFiles.find(fileName);
    if (logFileIt == LogFiles.end())
    {
        LogFiles.insert(std::make_pair(fileName, std::make_pair(nullptr, false)));
        logFileIt = LogFiles.find(fileName);
    }

    FILE* file = logFileIt->second.first;
    bool fileOpen = logFileIt->second.second;

    if (fileOpen)  // close log file
        fclose(file);

    std::string m_logsDir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!m_logsDir.empty())
    {
        if ((m_logsDir.at(m_logsDir.length() - 1) != '/') && (m_logsDir.at(m_logsDir.length() - 1) != '\\'))
            m_logsDir.append("/");
    }

    file = fopen((m_logsDir + fileName).c_str(), mode);
    fileOpen = true;

    logFileIt->second.first = file;
    logFileIt->second.second = fileOpen;

    return true;
}

void PlayerbotAIConfig::Log(std::string const fileName, char const* str, ...)
{
    if (!str)
        return;

    std::lock_guard<std::mutex> guard(LogMutex);

    if (!IsLogOpen(fileName) && !OpenLog(fileName, "a"))
        return;

    FILE* file = LogFiles.find(fileName)->second.first;

    va_list ap;
    va_start(ap, str);
    vfprintf(file, str, ap);
    fprintf(file, "\n");
    va_end(ap);
    fflush(file);

    fflush(stdout);
}

void PlayerbotAIConfig::LoadWorldBuff()
{
    std::string matrix = sConfigMgr->GetOption<std::string>("Cortex.Bots.WorldBuffMatrix", "", true);
    if (matrix.empty())
        return;

    std::istringstream entryStream(matrix);
    std::string entry;

    while (std::getline(entryStream, entry, ';'))
    {

        entry.erase(0, entry.find_first_not_of(" \t\r\n"));
        entry.erase(entry.find_last_not_of(" \t\r\n") + 1);

        size_t firstColon = entry.find(':');
        size_t secondColon = entry.find(':', firstColon + 1);

        if (firstColon == std::string::npos || secondColon == std::string::npos)
        {
            LOG_ERROR("playerbots", "Malformed entry: [{}]", entry);
            continue;
        }

        std::string metaPart = entry.substr(firstColon + 1, secondColon - firstColon - 1);
        std::string spellPart = entry.substr(secondColon + 1);

        std::vector<uint32> ids;
        std::istringstream metaStream(metaPart);
        std::string token;
        while (std::getline(metaStream, token, ','))
        {
            try {
                ids.push_back(static_cast<uint32>(std::stoi(token)));
            } catch (...) {
                LOG_ERROR("playerbots", "Invalid meta token in [{}]", entry);
                break;
            }
        }

        if (ids.size() != 5)
        {
            LOG_ERROR("playerbots", "Entry [{}] has incomplete meta block", entry);
            continue;
        }

        std::istringstream spellStream(spellPart);
        while (std::getline(spellStream, token, ','))
        {
            try {
                uint32 spellId = static_cast<uint32>(std::stoi(token));
                WorldBuff wb = { spellId, ids[0], ids[1], ids[2], ids[3], ids[4] };
                WorldBuffs.push_back(wb);
            } catch (...) {
                LOG_ERROR("playerbots", "Invalid spell ID in [{}]", entry);
            }
        }
    }
}

static std::vector<std::string> Split(std::string const& str, std::string const& pattern)
{
    std::vector<std::string> res;
    if (str == "")
        return res;
    // Also add separators to string connections to facilitate intercepting the last paragraph.
    std::string strs = str + pattern;
    size_t pos = strs.find(pattern);

    while (pos != strs.npos)
    {
        std::string temp = strs.substr(0, pos);
        res.push_back(temp);
        // Remove the split string and split the remaining string
        strs = strs.substr(pos + 1, strs.size());
        pos = strs.find(pattern);
    }

    return res;
}

std::vector<std::vector<uint32>> PlayerbotAIConfig::ParseTempTalentsOrder(uint32 cls, std::string tab_link)
{
    // check bad link
    uint32 classMask = 1 << (cls - 1);
    std::vector<std::vector<uint32>> res;
    std::vector<std::string> tab_links = Split(tab_link, "-");
    std::map<uint32, std::vector<TalentEntry const*>> spells;
    std::vector<std::vector<std::vector<uint32>>> orders(3);
    for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
    {
        TalentEntry const* talentInfo = sTalentStore.LookupEntry(i);
        if (!talentInfo)
            continue;

        TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
        if (!talentTabInfo)
            continue;

        if ((classMask & talentTabInfo->ClassMask) == 0)
            continue;

        spells[talentTabInfo->tabpage].push_back(talentInfo);
    }
    for (int tab = 0; tab < 3; tab++)
    {
        if (tab_links.size() <= (size_t)tab)
        {
            break;
        }
        std::sort(spells[tab].begin(), spells[tab].end(),
                  [&](TalentEntry const* lhs, TalentEntry const* rhs)
                  { return lhs->Row != rhs->Row ? lhs->Row < rhs->Row : lhs->Col < rhs->Col; });
        for (uint32 i = 0; i < tab_links[tab].size(); i++)
        {
            if (i >= spells[tab].size())
            {
                break;
            }
            int lvl = tab_links[tab][i] - '0';
            if (lvl == 0)
                continue;
            orders[tab].push_back({(uint32)tab, spells[tab][i]->Row, spells[tab][i]->Col, (uint32)lvl});
        }
    }
    // sort by talent tab size
    std::sort(orders.begin(), orders.end(), [&](auto& lhs, auto& rhs) { return lhs.size() > rhs.size(); });
    for (auto& order : orders)
    {
        res.insert(res.end(), order.begin(), order.end());
    }
    return res;
}

std::vector<std::vector<uint32>> PlayerbotAIConfig::ParseTempPetTalentsOrder(uint32 spec, std::string tab_link)
{
    // check bad link
    // uint32 classMask = 1 << (cls - 1);
    std::vector<TalentEntry const*> spells;
    std::vector<std::vector<uint32>> orders;
    for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
    {
        TalentEntry const* talentInfo = sTalentStore.LookupEntry(i);
        if (!talentInfo)
            continue;

        TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
        if (!talentTabInfo)
            continue;

        if (!((1 << spec) & talentTabInfo->petTalentMask))
            continue;
        // skip some duplicate spells like dash/dive
        if (talentInfo->TalentID == 2201 || talentInfo->TalentID == 2208 || talentInfo->TalentID == 2219 ||
            talentInfo->TalentID == 2203)
            continue;

        spells.push_back(talentInfo);
    }
    std::sort(spells.begin(), spells.end(),
              [&](TalentEntry const* lhs, TalentEntry const* rhs)
              { return lhs->Row != rhs->Row ? lhs->Row < rhs->Row : lhs->Col < rhs->Col; });
    for (uint32 i = 0; i < tab_link.size(); i++)
    {
        if (i >= spells.size())
        {
            break;
        }
        int lvl = tab_link[i] - '0';
        if (lvl == 0)
            continue;
        orders.push_back({spells[i]->Row, spells[i]->Col, (uint32)lvl});
    }
    // sort by talent tab size
    std::sort(orders.begin(), orders.end(), [&](auto& lhs, auto& rhs) { return lhs.size() > rhs.size(); });

    return orders;
}
