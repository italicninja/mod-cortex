/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#ifndef MOD_CORTEX_CORTEXNEEDS_H
#define MOD_CORTEX_CORTEXNEEDS_H

#include <functional>
#include <unordered_map>
#include <vector>

#include "CortexActions.h"
#include "CortexMemory.h"

class Creature;
class GameObject;
class WorldObject;
struct ItemTemplate;

// Needs served from memory (plan.md B2c, docs/bot-ai-overhaul.md §4.3 and §4.5 "Shop"): selling, repairs, ammo and
// pet food, food and drink, the class trainer, profession trainers, the inn, auction mail and the auction house. A
// need goes to the best place the bot remembers seeing (CortexMemory NPCs and mailboxes), walking or by a trip
// (CortexTravel). Only when memory has nothing suitable does playerbots' own action take it (upkeep's look around
// and camp walk, the trainer and auction catalogs), as a player who knows no vendor would ask a guard.
namespace CortexNeeds
{
    void LoadConfig();
    // Cortex.Needs
    bool Enabled();
}

class CortexNeedsAction : public CortexTravelAction
{
public:
    CortexNeedsAction(PlayerbotAI* botAI) : CortexTravelAction(botAI, "cortex needs") {}

    bool Execute(Event event) override;
    bool isUseful() override;

    enum class Need : uint8
    {
        NONE,
        VENDOR,      // sell, repair, ammo, water, pet food: the business itself is upkeep's (run at the vendor)
        FOOD,        // out of food and drink
        TRAINER,     // new class spells or ranks to learn
        PROFESSION,  // a profession to learn, its next rank, or recipes close by
        INN,         // make the inn of this town home
        MAIL,        // auction mail to collect at a mailbox close by
        AUCTION,     // an auction house trip (carried out by playerbots' auction action)
        COUNT
    };

private:
    using NpcMatch = std::function<bool(CortexMemory::Npc const&)>;

    bool Choose(CortexPlan& plan, CortexMemory const& memory, uint32 now);
    bool ChooseVendor(CortexMemory const& memory, uint32 now);
    bool ChooseFood(CortexMemory const& memory, uint32 now);
    bool ChooseTrainer(CortexMemory const& memory, uint32 now);
    bool ChooseProfession(CortexMemory const& memory, uint32 now);
    bool ChooseInn(CortexMemory const& memory, uint32 now);
    bool ChooseMail(CortexMemory const& memory, uint32 now);
    void ChooseAuction(CortexMemory const& memory, uint32 now);

    // The nearest remembered NPC that matches, within range, not skipped, and on ground the bot can walk to or a
    // trip reaches. Null if none.
    CortexMemory::Npc const* NearestNpc(CortexMemory const& memory, float range, NpcMatch const& match,
                                        uint64& keyOut);
    // Not skipped by this bot (absent, unreachable) nor blocked for every bot (UNREACH_STRIKES).
    bool Usable(uint64 memoryKey, uint32 now);

    uint32 VendorWants(uint32& ammoSubclass);
    uint32 WaterBudget();
    // Sells a drink that suits the bot's level and fits the budget (as upkeep's DrinkSlot).
    bool SellsDrink(uint32 vendorEntry, uint32 budget);
    uint32 FoodCount();
    uint32 TrainerSkill(uint32 entry);

    void Start(Need need, uint64 key, uint32 entry, uint32 map, float x, float y, float z, int32 detail);
    void End(uint32 cooldownMs);
    void GiveUp(char const* reason, uint32 skipMs, bool strike);
    void Fallback(Need need, uint32 holdMs, int32 detail);

    bool Walk(CortexPlan& plan, uint32 now);
    WorldObject* FindTarget();
    bool AtTarget(WorldObject* target, uint32 now);
    void DoFood(Creature* vendor);
    void DoTrainer(Creature* trainer);
    void DoProfession(Creature* trainer);
    void DoInn(Creature* innkeeper);
    void DoMail(GameObject* mailbox);

    // The errand under way.
    Need need = Need::NONE;
    uint64 key = 0;  // memory key of the NPC or mailbox
    uint32 entry = 0;
    uint32 map = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32 skill = 0;  // PROFESSION
    uint32 startMs = 0;
    uint32 seenMs = 0;
    uint32 seenAwayMs = 0;  // Cortex::AwayMs at the last step
    float best = 0.0f;  // closest to the target so far, and when
    uint32 bestMs = 0;
    bool tripTried = false;
    uint8 relocations = 0;  // a walker followed from its remembered spot to where it is now

    uint32 nextCheckMs[static_cast<uint8>(Need::COUNT)] = {};
    uint8 checkedLevel = 0;      // TRAINER: nothing (affordable) to learn at this level
    uint32 trainerRecheckMs = 0;
    uint32 innBoundAt = 0;       // unix seconds
    uint64 handoverKey = 0;      // VENDOR: the last vendor handed to upkeep, and when
    uint32 handoverMs = 0;
    uint32 holdUntilMs = 0;      // VENDOR: upkeep walks to a vendor in sight it prefers; no other errand till then
    std::unordered_map<uint64, uint32> skip;           // memory key -> until getMSTime (absent, unreachable)
    std::unordered_map<uint32, uint32> trainerSkills;  // creature entry -> profession it teaches (0 none)
    std::unordered_map<uint32, std::vector<ItemTemplate const*>> vendorDrinks;  // creature entry -> drinks it sells
};

#endif
