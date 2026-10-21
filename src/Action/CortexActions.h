/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 * Originally written as part of the wowproject patches to mod-playerbots.
 */

#ifndef MOD_CORTEX_CORTEXACTIONS_H
#define MOD_CORTEX_CORTEXACTIONS_H

#include <functional>
#include <list>

#include "AttackAction.h"
#include "CortexTravel.h"
#include "NewRpgBaseAction.h"

class CortexMemory;
struct CortexCluster;
struct CortexPlan;
struct CortexTask;
class Transport;

// Re-plans on an event (quest progress, death, level, interaction) or a backstop timer.
class CortexThinkAction : public NewRpgBaseAction
{
public:
    CortexThinkAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "cortex think") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// Carries out the plan's trip (CortexTravel): walks to each leg's start, waits for the boat or lift, boards, rides,
// steps off, talks to flight masters, takes portals, casts teleports. Shared by the step and the ride action.
class CortexTravelAction : public NewRpgBaseAction
{
public:
    CortexTravelAction(PlayerbotAI* botAI, std::string const name) : NewRpgBaseAction(botAI, name) {}

protected:
    // Plans a trip to (map, x, y, z) and starts it; false when there is none worth taking.
    bool StartTrip(CortexPlan& plan, uint32 map, float x, float y, float z, uint32 mapArea, bool allowDirect,
                   char const* why);
    // One step of the active trip. False when it did nothing this tick; `failed` once the trip gave up (after
    // re-planning around the failed link where it could).
    bool TripStep(CortexPlan& plan, bool& failed);

private:
    bool Approach(CortexTrip& trip, CortexLeg const& leg);
    bool Wait(CortexTrip& trip, CortexLeg const& leg);
    bool Board(CortexTrip& trip, CortexLeg const& leg);
    bool Ride(CortexTrip& trip, CortexLeg const& leg);
    bool Exit(CortexTrip& trip, CortexLeg const& leg);
    bool Activate(CortexTrip& trip, CortexLeg const& leg);
    bool Arrive(CortexTrip& trip, CortexLeg const& leg);
    GameObject* FindTransport(CortexTrip& trip, CortexLeg const& leg);
    Creature* FindFlightMaster(uint32 node);
    bool Still(CortexTrip& trip, GameObject* transport);
    void SetPhase(CortexTrip& trip, CortexTripPhase phase);
    void LegDone(CortexTrip& trip);
    // `replan`: try a way round the failed link; false when the walk itself is the problem.
    void Fail(CortexTrip& trip, char const* reason, bool replan = true);
    void StepTo(float x, float y, float z);
    // Makes the bot the transport's passenger once it stands on its deck (the ray test playerbots' once-a-second
    // check uses, so that check doesn't undo it); true when it is.
    bool Attach(GameObject* transport);
    // Off the transport: stops a walk in deck coordinates first, or the bot would land at those as world ones.
    void Detach(Transport* transport);

    bool tripFailed = false;     // set by Fail during a step
    CortexPlan* tripPlan = nullptr;  // the plan of the step being run (Fail marks the link bad on it)
};

// Keeps a bot at the pier or lift landing and aboard until it steps off, and through casts and teleports: above
// the needs and trips (vendor, trainer, auction) that would walk it off the deck.
class CortexRideAction : public CortexTravelAction
{
public:
    CortexRideAction(PlayerbotAI* botAI) : CortexTravelAction(botAI, "cortex ride") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// Carries out the current plan step: talk to quest givers in reach, hand in, work a cluster, pick up, grind.
class CortexStepAction : public CortexTravelAction
{
public:
    CortexStepAction(PlayerbotAI* botAI) : CortexTravelAction(botAI, "cortex step") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    bool Retreat(CortexPlan& plan);
    bool Errand();
    bool LearnFlightPoint(CortexPlan& plan);
    bool TalkToQuestGivers(CortexPlan& plan);
    bool HandIn(CortexPlan& plan, CortexMemory& memory);
    bool Work(CortexPlan& plan, CortexMemory& memory);
    bool PickUp(CortexPlan& plan, CortexMemory& memory);
    bool Grind(CortexPlan& plan, CortexMemory& memory);

    ObjectGuid ChooseQuestGiver(CortexPlan& plan);
    bool Search(CortexPlan& plan, CortexCluster& cluster);
    bool UseQuestObject(CortexPlan& plan, CortexCluster const& cluster, CortexMemory const& memory, bool scan);
    GameObject* FindQuestObject(CortexPlan const& plan);
    bool ReturnToSparkle(CortexPlan& plan, CortexCluster const& cluster, CortexMemory const& memory);
    bool PickSparkle(CortexPlan& plan, CortexCluster const& cluster, CortexMemory const& memory);
    void EndSparkle(CortexPlan& plan, char const* outcome, uint32 skipMs);
    bool UseQuestItems(CortexPlan& plan, CortexCluster& cluster);
    bool TalkForQuest(CortexPlan& plan, CortexCluster const& cluster);
    bool VendorErrand(CortexPlan& plan, CortexMemory& memory, CortexCluster const& cluster);
    void ShelveCluster(CortexPlan& plan, CortexCluster const& cluster, char const* reason);
    Creature* FindItemTarget(CortexPlan& plan, CortexTask const& task);
    GameObject* FindNearbyObject(std::function<bool(GameObject*)> const& match);
    void UseItemOn(Item* item, uint32 spellId, WorldObject* target);
    bool CanPull();
    Unit* FindHuntTarget(std::function<bool(Creature*)> const& wanted);
    bool Hunt(CortexPlan& plan, Unit* target);
    bool MoveToGround(CortexPlan& plan, uint32 map, float x, float y, bool& stuck, uint32 mapArea = 0);
    // Walks there, or takes a trip (flights, boats, lifts, portals) when walking can't get there or is much slower.
    bool Travel(CortexPlan& plan, uint32 map, float x, float y, float z, bool& stuck, uint32 mapArea = 0);
    // A short walk to a random spot round the bot that a path reaches (MoveRandomNear without its straight-line
    // collision test, which fails on any slope or tree and left grinders standing).
    bool LookAround(float range);

    bool sawWanted = false;  // the last FindHuntTarget saw a creature it wanted, pullable or not
};

// Hunter taming: a hunter that knows Tame Beast and has no pet tames a beast it comes across while questing, as a
// player does once the trainer's taming quests are done.
class CortexTameAction : public NewRpgBaseAction
{
public:
    CortexTameAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "cortex tame") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    void Settle(CortexPlan& plan);
    void Skip(CortexPlan& plan, char const* reason);
    void WatchPet(CortexPlan& plan);
    void ReportPet(CortexPlan& plan, char const* why, int32 a, int32 b, int32 c);
    Creature* FindBeast(CortexPlan& plan);
    bool Suitable(CortexPlan& plan, Creature* creature);
};

// Gathering trips: a herbalist or miner walks to a node it remembers and can gather, as a player heads for the herbs
// it knows between quests. Nodes in sight are left to "new rpg gather" (playerbots' node detours) and the loot
// strategy; this only brings the bot back to where it saw one.
class CortexGatherAction : public NewRpgBaseAction
{
public:
    CortexGatherAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "cortex gather") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    bool Pick(CortexPlan& plan, CortexMemory const& memory);
    bool NodeThere(CortexPlan const& plan);
    void End(CortexPlan& plan, char const* failReason, uint32 skipMs);
};

// Back from a death: steps away from the hostiles in sight before "new rpg recover" sits it down (CortexRegroup.cpp).
class CortexRegroupAction : public NewRpgBaseAction
{
public:
    CortexRegroupAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "cortex regroup") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    uint32 _moves = 0;
    uint32 _hearthTries = 0;
};

// Attacks the target Cortex picked (CortexPlan::huntTarget); the class AI fights from there.
class CortexAttackAction : public AttackAction
{
public:
    CortexAttackAction(PlayerbotAI* botAI) : AttackAction(botAI, "cortex attack") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
