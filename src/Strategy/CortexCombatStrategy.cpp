/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include "CortexCombatStrategy.h"

#include <string>
#include <unordered_map>

#include "AiObjectContext.h"
#include "BotTelemetry.h"
#include "Cortex.h"
#include "GenericSpellActions.h"
#include "Multiplier.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"

namespace
{
// A levelling caster with a wand doesn't spend its mana to the last drop on every mob: below WAND_MANA_PCT mana, or
// with the mob nearly dead (FINISH_HEALTH_PCT) and mana not near full, it shoots the wand and keeps its spells for
// the next pull. Fewer and shorter rests: Cortex priests, mages and warlocks spent a tenth of their time sitting
// to drink on 2026-10-20, most of them too poor to buy water. Not with two or more on it, not low on health (then
// it kills as fast as it can), not against a player and not in a group.
constexpr uint32 WAND_MANA_PCT = 50;
constexpr uint32 FINISH_HEALTH_PCT = 25;
constexpr uint32 FINISH_MANA_PCT = 80;
constexpr uint32 WAND_MIN_HEALTH_PCT = 40;
constexpr uint32 RECHECK_MS = 500;
constexpr uint32 TELEMETRY_MS = 10 * MINUTE * IN_MILLISECONDS;

class CortexWandMultiplier : public Multiplier
{
public:
    CortexWandMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "cortex wand") {}

    float GetValue(Action* action) override
    {
        CastSpellAction* cast = dynamic_cast<CastSpellAction*>(action);
        if (!Cortex::Wands() || !cast || dynamic_cast<CastShootAction*>(action) || !Wanding())
            return 1.0f;
        return CostlyDamage(cast->getSpell()) ? 0.0f : 1.0f;
    }

private:
    // Whether the bot shoots its wand now, checked at most every RECHECK_MS.
    bool Wanding()
    {
        uint32 now = getMSTime();
        if (_checkedMs && getMSTimeDiff(_checkedMs, now) < RECHECK_MS)
            return _wanding;
        _checkedMs = now ? now : 1;
        _wanding = false;

        Player* bot = botAI->GetBot();
        Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        ItemTemplate const* proto = ranged ? ranged->GetTemplate() : nullptr;
        if (!proto || proto->Class != ITEM_CLASS_WEAPON || proto->SubClass != ITEM_SUBCLASS_WEAPON_WAND ||
            ranged->IsBroken() || bot->GetGroup() || bot->getPowerType() != POWER_MANA || !bot->GetMaxPower(POWER_MANA))
            return false;
        Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
        if (!target || !target->IsAlive() || target->IsPlayer() || target->GetCharmerOrOwnerPlayerOrPlayerItself() ||
            bot->getAttackers().size() >= 2 || bot->GetHealthPct() < WAND_MIN_HEALTH_PCT)
            return false;

        uint32 mana = bot->GetPower(POWER_MANA) * 100 / bot->GetMaxPower(POWER_MANA);
        bool low = mana < WAND_MANA_PCT;
        bool finish = target->GetHealthPct() <= FINISH_HEALTH_PCT && mana < FINISH_MANA_PCT;
        _wanding = low || finish;
        if (_wanding && (!_reportedMs || getMSTimeDiff(_reportedMs, now) >= TELEMETRY_MS))
        {
            _reportedMs = now ? now : 1;
            BotTelemetry::instance().Event(botAI, "wand", low ? "mana" : "finish", static_cast<int32>(mana),
                                           static_cast<int32>(target->GetHealthPct()),
                                           static_cast<int32>(bot->GetLevel()));
        }
        return _wanding;
    }

    // A damage spell that costs mana: what the wand stands in for. Heals, shields, crowd control and free spells
    // still go through.
    bool CostlyDamage(std::string const& spell)
    {
        auto it = _costly.find(spell);
        if (it != _costly.end())
            return it->second;
        bool costly = false;
        uint32 id = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", spell)->Get();
        if (SpellInfo const* info = id ? sSpellMgr->GetSpellInfo(id) : nullptr)
        {
            if (!info->IsPositive() && info->PowerType == POWER_MANA && (info->ManaCost || info->ManaCostPercentage))
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS && !costly; ++i)
                {
                    SpellEffectInfo const& effect = info->Effects[i];
                    costly = effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE || effect.Effect == SPELL_EFFECT_HEALTH_LEECH ||
                             (effect.Effect == SPELL_EFFECT_APPLY_AURA &&
                              (effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE ||
                               effect.ApplyAuraName == SPELL_AURA_PERIODIC_LEECH));
                }
        }
        // A spell the bot doesn't know yet resolves to 0; looked up again once it may have learned it.
        if (id)
            _costly[spell] = costly;
        return costly;
    }

    uint32 _checkedMs = 0;
    bool _wanding = false;
    uint32 _reportedMs = 0;
    std::unordered_map<std::string, bool> _costly;
};
}  // namespace

void CortexCombatStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    if (Cortex::Acting() && Cortex::Wands())
        multipliers.push_back(new CortexWandMultiplier(botAI));
}
