/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include <algorithm>
#include <cmath>
#include <list>

#include "BotTelemetry.h"
#include "CellImpl.h"
#include "Cortex.h"
#include "CortexActions.h"
#include "CortexQuests.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "PetDefines.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"

namespace
{
    constexpr uint32 SPELL_TAME_BEAST = 1515;
    constexpr uint32 SCAN_MS = 5 * IN_MILLISECONDS;
    constexpr float SIGHT = 50.0f;
    constexpr uint8 LEVELS_BELOW = 3;  // a pet this far under the hunter still keeps up; above it can't be tamed
    constexpr float PACK_RANGE = 10.0f;
    constexpr uint32 PACK_LIMIT = 1;
    constexpr float MIN_HEALTH_PCT = 80.0f;  // the beast hits the hunter through the whole channel
    constexpr float STAND_OFF = 6.0f;        // stops this far inside the spell's range, outside most aggro radii
    constexpr uint32 WALK_MS = 60 * IN_MILLISECONDS;
    constexpr uint32 SKIP_MS = 30 * MINUTE * IN_MILLISECONDS;
    constexpr uint8 FAILS_TO_PAUSE = 3;
    constexpr uint32 PAUSE_MS = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 SPELL_REVIVE_PET = 982;
    constexpr uint32 SPELL_FEED_PET = 6991;
    constexpr uint32 PET_CHECK_MS = 30 * IN_MILLISECONDS;
    constexpr uint32 PET_AWAY_MS = 2 * MINUTE * IN_MILLISECONDS;  // Call Pet / Revive Pet fire within seconds
    constexpr uint32 PET_REPORT_MS = 30 * MINUTE * IN_MILLISECONDS;
    // Training the Beast / Beast Training, per race: the hand-in teaches Revive Pet and Feed Pet (no trainer does).
    constexpr uint32 BEAST_TRAINING_QUESTS[] = {6081, 6086, 6089, 6103, 9673, 9675};

    bool Due(uint32 now, uint32 at) { return static_cast<int32>(now - at) >= 0; }

    // A pet out, one dismissed or dead (Call Pet / Revive Pet bring it back), or a quest beast charmed by a Taming Rod.
    bool HasPet(Player* bot)
    {
        if (bot->GetPet() || bot->GetCharmGUID())
            return true;
        PetStable const* stable = bot->GetPetStable();
        return stable && (stable->CurrentPet || stable->GetUnslottedHunterPet());
    }
}

bool CortexTameAction::isUseful()
{
    if (!Cortex::Taming() || bot->getClass() != CLASS_HUNTER || !bot->HasSpell(SPELL_TAME_BEAST))
        return false;
    CortexPlan* plan = Cortex::Plan(bot);
    if (!plan || bot->IsNonMeleeSpellCast(false))  // the channel is still running
        return false;
    if (plan->tameCast)
        Settle(*plan);
    if (HasPet(bot))
    {
        plan->tameTarget.Clear();
        WatchPet(*plan);
        return false;
    }
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->GetMap()->Instanceable())
        return false;
    if (plan->tameTarget)
        return true;

    uint32 now = getMSTime();
    if (!Due(now, plan->nextTameScanMs))
        return false;
    plan->nextTameScanMs = now + SCAN_MS;
    if (bot->GetHealthPct() < MIN_HEALTH_PCT || botAI->rpgInfo.recoverSinceMs)
        return false;

    Creature* beast = FindBeast(*plan);
    if (!beast)
        return false;
    plan->tameTarget = beast->GetGUID();
    plan->tameEntry = beast->GetEntry();
    plan->tameSinceMs = now;
    BotTelemetry::instance().Event(botAI, "tame", "spot", static_cast<int32>(beast->GetEntry()),
                                   static_cast<int32>(beast->GetLevel()),
                                   static_cast<int32>(beast->GetCreatureTemplate()->family));
    return true;
}

bool CortexTameAction::Execute(Event /*event*/)
{
    CortexPlan* plan = Cortex::Plan(bot);
    if (!plan || !plan->tameTarget)
        return false;

    Creature* beast = ObjectAccessor::GetCreature(*bot, plan->tameTarget);
    if (!beast || !Suitable(*plan, beast))
    {
        Skip(*plan, "lost");
        return false;
    }
    if (getMSTimeDiff(plan->tameSinceMs, getMSTime()) > WALK_MS)
    {
        Skip(*plan, "give_up");
        return false;
    }

    SpellInfo const* spell = sSpellMgr->GetSpellInfo(SPELL_TAME_BEAST);
    float range = spell ? spell->GetMaxRange(false, bot) : 30.0f;
    float d = bot->GetExactDist(beast);
    if (d > range - 1.0f)
    {
        // To a spot short of the beast on the line toward the hunter, not next to it: walking up would pull it.
        float angle = beast->GetAngle(bot);
        float reach = std::max(range - STAND_OFF, 5.0f);
        float x = beast->GetPositionX() + std::cos(angle) * reach;
        float y = beast->GetPositionY() + std::sin(angle) * reach;
        float z = CortexQuests::GroundZ(bot, x, y, beast->GetPositionZ());
        return MoveFarTo(WorldPosition(beast->GetMapId(), x, y, z));
    }
    if (!bot->IsWithinLOSInMap(beast))
        return MoveWorldObjectTo(beast->GetGUID(), range / 2);

    if (bot->isMoving())
    {
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
    }
    if (bot->IsMounted())
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
    bot->SetFacingToObject(beast);
    if (!botAI->CastSpell(SPELL_TAME_BEAST, beast))
    {
        Skip(*plan, "cant_cast");
        return false;
    }
    plan->tameCast = true;
    BotTelemetry::instance().Event(botAI, "tame", "cast", static_cast<int32>(beast->GetEntry()),
                                   static_cast<int32>(beast->GetLevel()), static_cast<int32>(d));
    return true;
}

// After the cast: a pet means it took; none means the channel broke (moved, interrupted, the beast died).
void CortexTameAction::Settle(CortexPlan& plan)
{
    plan.tameCast = false;
    Pet* pet = bot->GetPet();
    if (pet && pet->getPetType() == HUNTER_PET)
    {
        plan.tameTarget.Clear();
        plan.tameFails = 0;
        BotTelemetry::instance().Event(botAI, "tame", "done", static_cast<int32>(pet->GetEntry()),
                                       static_cast<int32>(pet->GetLevel()),
                                       static_cast<int32>(pet->GetCreatureTemplate()->family));
        return;
    }
    Skip(plan, "fail");
}

void CortexTameAction::Skip(CortexPlan& plan, char const* reason)
{
    uint32 now = getMSTime();
    if (plan.tameTarget)
        plan.tameSkip[plan.tameTarget] = now + SKIP_MS;
    BotTelemetry::instance().Event(botAI, "tame", reason, static_cast<int32>(plan.tameEntry),
                                   static_cast<int32>(getMSTimeDiff(plan.tameSinceMs, now) / IN_MILLISECONDS));
    plan.tameTarget.Clear();
    plan.tameCast = false;
    if (++plan.tameFails >= FAILS_TO_PAUSE)
    {
        plan.tameFails = 0;
        plan.nextTameScanMs = now + PAUSE_MS;
    }
}

// Upkeep itself is playerbots' hunter "pet" strategy, which Cortex keeps: Call Pet when none is out, Revive Pet when
// it is dead, Feed Pet when unhappy, Mend Pet. This only reports a pet that stays away out of combat or unhappy, and
// why: dead without Revive Pet (only the Training the Beast hand-in teaches it, and a dead pet also blocks taming
// another), dead and not revived, alive and not called, or unhappy without Feed Pet.
void CortexTameAction::WatchPet(CortexPlan& plan)
{
    uint32 now = getMSTime();
    if (!Due(now, plan.nextPetCheckMs))
        return;
    plan.nextPetCheckMs = now + PET_CHECK_MS;

    Pet* pet = bot->GetPet();
    if (pet && pet->IsAlive())
    {
        plan.petAwaySinceMs = 0;
        if (pet->getPetType() == HUNTER_PET && pet->GetHappinessState() == UNHAPPY && !bot->IsInCombat())
            ReportPet(plan, bot->HasSpell(SPELL_FEED_PET) ? "unhappy" : "unhappy_nofeed",
                      static_cast<int32>(pet->GetPower(POWER_HAPPINESS)), 0, 0);
        return;
    }

    PetStable const* stable = bot->GetPetStable();
    PetStable::PetInfo const* info = nullptr;
    if (stable)
        info = stable->CurrentPet ? &*stable->CurrentPet : stable->GetUnslottedHunterPet();
    if (!pet && !info)  // a quest beast on a Taming Rod
    {
        plan.petAwaySinceMs = 0;
        return;
    }
    bool dead = pet ? !pet->IsAlive() : !info->Health;

    if (dead && !bot->HasSpell(SPELL_REVIVE_PET))
    {
        // Nothing brings it back but the hand-in: which one the hunter holds, and in what state.
        uint32 quest = 0;
        for (uint32 id : BEAST_TRAINING_QUESTS)
            if (bot->GetQuestStatus(id) != QUEST_STATUS_NONE)
            {
                quest = id;
                break;
            }
        ReportPet(plan, "dead_norev", static_cast<int32>(quest),
                  quest ? static_cast<int32>(bot->GetQuestStatus(quest)) : -1, static_cast<int32>(bot->GetLevel()));
        return;
    }

    // Away only counts while the strategy could act: alive, out of combat, on the ground and not riding (mounting
    // and flights put the pet away; the core brings it back on landing).
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->IsMounted() || bot->GetVehicle())
    {
        plan.petAwaySinceMs = 0;
        return;
    }
    if (!plan.petAwaySinceMs)
    {
        plan.petAwaySinceMs = now;
        return;
    }
    uint32 away = getMSTimeDiff(plan.petAwaySinceMs, now);
    if (away >= PET_AWAY_MS)
        ReportPet(plan, dead ? "not_revived" : "not_called", static_cast<int32>(away / IN_MILLISECONDS),
                  static_cast<int32>(pet ? pet->GetEntry() : info->CreatureId), stable && stable->CurrentPet ? 1 : 0);
}

void CortexTameAction::ReportPet(CortexPlan& plan, char const* why, int32 a, int32 b, int32 c)
{
    uint32 now = getMSTime();
    if (plan.petReportMs && getMSTimeDiff(plan.petReportMs, now) < PET_REPORT_MS)
        return;
    plan.petReportMs = now;
    BotTelemetry::instance().Event(botAI, "pet", why, a, b, c);
}

// A beast the hunter could tame now and a player would try: tameable, its level or up to three below, not elite,
// nobody's pet or fight or tag, and not standing in a pack that would join in during the channel.
bool CortexTameAction::Suitable(CortexPlan& plan, Creature* creature)
{
    CreatureTemplate const* cInfo = creature->GetCreatureTemplate();
    if (!creature->IsAlive() || creature->IsPet() || creature->IsTotem() || creature->IsGuardian() ||
        creature->isElite() || !cInfo->IsTameable(bot->CanTameExoticPets()) || creature->GetOwnerGUID() ||
        creature->GetCharmerGUID())
        return false;
    if (creature->GetLevel() > bot->GetLevel() || creature->GetLevel() + LEVELS_BELOW < bot->GetLevel())
        return false;
    if ((creature->IsInCombat() && creature->GetVictim() != bot) ||
        (creature->hasLootRecipient() && !creature->isTappedBy(bot)))
        return false;
    auto const& excluded = sPlayerbotAIConfig.ExcludedHunterPetFamilies;
    if (std::find(excluded.begin(), excluded.end(), cInfo->family) != excluded.end())
        return false;
    auto skipped = plan.tameSkip.find(creature->GetGUID());
    if (skipped != plan.tameSkip.end())
    {
        if (!Due(getMSTime(), skipped->second))
            return false;
        plan.tameSkip.erase(skipped);
    }
    return true;
}

Creature* CortexTameAction::FindBeast(CortexPlan& plan)
{
    std::list<Unit*> units;
    Acore::AnyUnitInObjectRangeCheck check(bot, SIGHT);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
    Cell::VisitObjects(bot, searcher, SIGHT);

    Creature* best = nullptr;
    float bestDist = SIGHT;
    for (Unit* unit : units)
    {
        Creature* creature = unit->ToCreature();
        if (!creature)
            continue;
        float d = bot->GetExactDist(creature);
        if (d >= bestDist || !Suitable(plan, creature))
            continue;

        uint32 pack = 0;
        for (Unit* other : units)
            if (other != creature && other->IsAlive() && !other->IsInCombat() && other->IsHostileTo(bot) &&
                other->GetExactDist(creature) <= PACK_RANGE)
                ++pack;
        if (pack > PACK_LIMIT || !bot->IsWithinLOSInMap(creature))
            continue;

        best = creature;
        bestDist = d;
    }
    return best;
}
