/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HunterActions.h"
#include "Event.h"
#include "GenericSpellActions.h"
#include "HunterPetFood.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

bool CastViperStingAction::isUseful()
{
    return CastAuraSpellAction::isUseful() && AI_VALUE2(uint8, "mana", "self target") < 50 &&
           AI_VALUE2(uint8, "mana", "current target") >= 30;
}

bool CastAspectOfTheHawkAction::isUseful()
{
    Unit* target = GetTarget();
    return target && !botAI->HasSpell("aspect of the dragonhawk");
}

bool CastArcaneShotAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || !botAI->HasSpell("explosive shot"))
        return false;

    // Armor Penetration rating check - will not cast Arcane Shot above 435 ArP
    int32 armorPenRating =
        bot->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1) + bot->GetUInt32Value(CR_ARMOR_PENETRATION);
    if (armorPenRating > 435)
        return false;

    return true;
}

bool CastImmolationTrapAction::isUseful()
{
    Unit* target = GetTarget();
    return target && !botAI->HasSpell("explosive trap");
}

Value<Unit*>* CastFreezingTrap::GetTargetValue()
{
    return context->GetValue<Unit*>("cc target", "freezing trap");
}

// Feed Pet on food from the bags, as a player does: needs the spell (Training the Beast) and food the pet eats,
// which upkeep buys at vendors. Without either the pet just stays unhappy.
bool FeedPetAction::isUseful()
{
    Pet* pet = HunterPetFood::HunterPet(bot);
    if (!pet || pet->GetHappinessState() == HAPPY || bot->IsInCombat() || pet->IsInCombat() ||
        !bot->HasSpell(HunterPetFood::SPELL_FEED_PET) || bot->HasSpellCooldown(HunterPetFood::SPELL_FEED_PET))
        return false;
    SpellInfo const* spell = sSpellMgr->GetSpellInfo(HunterPetFood::SPELL_FEED_PET);
    if (!spell || (spell->Effects[EFFECT_0].TriggerSpell && pet->HasAura(spell->Effects[EFFECT_0].TriggerSpell)))
        return false;  // still eating the last one
    return HunterPetFood::Best(bot, pet) != nullptr;
}

bool FeedPetAction::Execute(Event /*event*/)
{
    Pet* pet = HunterPetFood::HunterPet(bot);
    SpellInfo const* spell = sSpellMgr->GetSpellInfo(HunterPetFood::SPELL_FEED_PET);
    Item* food = pet ? HunterPetFood::Best(bot, pet) : nullptr;
    if (!food || !spell)
        return false;

    if (bot->isMoving())
        bot->StopMoving();
    SpellCastTargets targets;
    targets.SetItemTarget(food);
    targets.SetUnitTarget(pet);
    return bot->CastSpell(targets, spell, nullptr) == SPELL_CAST_OK;
}

bool CastAutoShotAction::isUseful()
{
    if (botAI->IsInVehicle() && !botAI->IsInVehicle(false, false, true))
        return false;

    if (AI_VALUE(Unit*, "current target") && bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) &&
        bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL)->m_targets.GetUnitTargetGUID() ==
            AI_VALUE(Unit*, "current target")->GetGUID())
    {
        return false;
    }

    return AI_VALUE(uint32, "active spell") != AI_VALUE2(uint32, "spell id", getName());
}

bool CastDisengageAction::Execute(Event event)
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    // can cast spell check passed in isUseful()
    bot->SetOrientation(bot->GetAngle(target));
    return CastSpellAction::Execute(event);
}

bool CastDisengageAction::isUseful()
{
    return !botAI->HasStrategy("trap weave", BOT_STATE_COMBAT);
}

Value<Unit*>* CastScareBeastCcAction::GetTargetValue()
{
    return context->GetValue<Unit*>("cc target", "scare beast");
}

bool CastScareBeastCcAction::Execute(Event /*event*/)
{
    return botAI->CastSpell("scare beast", GetTarget());
}

bool CastWingClipAction::isUseful()
{
    return CastSpellAction::isUseful() && !botAI->HasAura(spell, GetTarget());
}

std::vector<NextAction> CastWingClipAction::getPrerequisites()
{
    return {};
}
