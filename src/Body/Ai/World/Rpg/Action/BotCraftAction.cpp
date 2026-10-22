/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotCraftAction.h"

#include <algorithm>
#include <list>
#include <string>

#include "Bag.h"
#include "BotAuction.h"
#include "BotProfessions.h"
#include "BotTelemetry.h"
#include "CellImpl.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

namespace
{
    constexpr time_t CHECK_SECONDS = 5 * MINUTE;
    constexpr time_t SESSION_SECONDS = 3 * MINUTE;
    constexpr uint32 SESSION_CRAFTS = 20;
    constexpr float FOCUS_RANGE = 30.0f;

    constexpr uint32 CRAFTING[] = {SKILL_BLACKSMITHING, SKILL_ENGINEERING,  SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING,
                                   SKILL_ALCHEMY,       SKILL_TAILORING,    SKILL_ENCHANTING,    SKILL_INSCRIPTION};

    // Disenchanting and milling: free bag slots the loot needs (the item's loot is lost if it can't be stored), how
    // long to wait for the last cast's loot to be taken, and how many failed items to remember.
    constexpr uint32 PREP_FREE_SLOTS = 2;
    constexpr time_t PREP_LOOT_WAIT_SECONDS = 5;
    constexpr size_t PREP_FAILED_MAX = 32;

    time_t Now() { return GameTime::GetGameTime().count(); }

    template <typename Fn>
    void ForEachBagItem(Player* bot, Fn&& fn)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                fn(item);
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(static_cast<uint8>(slot)))
                        fn(item);
    }

    // Empty slots in the backpack and in plain bags (not herb, enchanting or other special bags).
    uint32 FreeSlots(Player* bot)
    {
        uint32 free = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                ++free;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                if (!bag->GetTemplate()->BagFamily)
                    free += bag->GetFreeSlots();
        return free;
    }

    // Gear an enchanter may disenchant: green or blue armor or a weapon in its bags that the gear scoring doesn't
    // want (not an upgrade, not for an empty slot, not for a quest), that it would otherwise sell, list or vendor.
    bool Disenchantable(PlayerbotAI* botAI, Item* item, uint32 skill)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR) ||
            proto->Quality < ITEM_QUALITY_UNCOMMON || proto->Quality > ITEM_QUALITY_RARE || !proto->DisenchantID ||
            proto->RequiredDisenchantSkill == uint32(-1) || proto->RequiredDisenchantSkill > skill ||
            item->IsInTrade() || item->m_lootGenerated)
            return false;
        std::string qualifier = std::to_string(proto->ItemId);
        if (int32 randomPropertyId = item->GetItemRandomPropertyId())
            qualifier += "," + std::to_string(randomPropertyId);
        switch (botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", qualifier)->Get())
        {
            case ITEM_USAGE_NONE:
            case ITEM_USAGE_DISENCHANT:
            case ITEM_USAGE_AH:
            case ITEM_USAGE_VENDOR:
                return true;
            default:
                return false;
        }
    }
}

bool BotCraftAction::isUseful()
{
    if (!BotProfessions::Enabled() || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() ||
        bot->GetMap()->Instanceable() || !sRandomPlayerbotMgr.IsRandomBot(bot))
    {
        sessionEnd = 0;
        return false;
    }
    time_t now = Now();
    if (sessionEnd)
        return true;
    if (now < nextCheck)
        return false;
    nextCheck = now + CHECK_SECONDS;

    bool crafter = false;
    for (uint32 skill : CRAFTING)
        crafter = crafter || bot->HasSkill(skill);
    uint32 prepSpell = 0;
    if (!crafter || (!PrepTarget(prepSpell) && !Best().spell))
        return false;

    sessionEnd = now + SESSION_SECONDS;
    crafted = 0;
    return true;
}

bool BotCraftAction::Execute(Event /*event*/)
{
    // Holds the bot still while the last craft finishes casting.
    if (bot->IsNonMeleeSpellCast(false))
        return true;
    if (Now() >= sessionEnd || crafted >= SESSION_CRAFTS)
    {
        EndSession();
        return false;
    }

    // The last disenchant or mill's loot first: the next cast would release it before it is stored.
    if (bot->GetLootGUID().IsItem() && Now() < prepAt + PREP_LOOT_WAIT_SECONDS)
        return true;

    uint32 prepSpell = 0;
    if (Item* item = PrepTarget(prepSpell))
    {
        if (bot->isMoving())
        {
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
        }
        bool disenchant = prepSpell == BotProfessions::SPELL_DISENCHANT;
        ItemTemplate const* proto = item->GetTemplate();
        int32 entry = static_cast<int32>(proto->ItemId);
        int32 detail = disenchant ? static_cast<int32>(proto->Quality) : static_cast<int32>(item->GetCount());
        ObjectGuid guid = item->GetGUID();
        if (botAI->CastSpell(prepSpell, bot, item))
        {
            ++crafted;
            prepAt = Now();
            int32 skill = static_cast<int32>(bot->GetSkillValue(disenchant ? SKILL_ENCHANTING : SKILL_INSCRIPTION));
            BotTelemetry::instance().Event(botAI, "prof", disenchant ? "disenchant" : "mill", entry, skill, detail);
            return true;
        }
        if (prepFailed.size() >= PREP_FAILED_MAX)
            prepFailed.clear();
        prepFailed.push_back(guid);
        BotTelemetry::instance().Event(botAI, "prof", disenchant ? "disenchant_fail" : "mill_fail", entry,
                                       static_cast<int32>(prepSpell));
    }

    Recipe recipe = Best();
    if (!recipe.spell)
    {
        EndSession();
        return false;
    }
    if (recipe.focus)
    {
        float reach = std::max(2.0f, recipe.focus->GetGOInfo()->spellFocus.dist - 1.0f);
        if (!bot->IsWithinDistInMap(recipe.focus, reach))
            return MoveWorldObjectTo(recipe.focus->GetGUID(), INTERACTION_DISTANCE);
    }

    if (bot->isMoving())
    {
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
    }
    if (!botAI->CastSpell(recipe.spell->Id, bot, recipe.target))
    {
        EndSession();
        return false;
    }
    ++crafted;
    BotTelemetry::instance().Event(botAI, "craft", recipe.target ? "enchant" : "make",
                                   static_cast<int32>(recipe.spell->Id), recipe.chance);
    return true;
}

Item* BotCraftAction::PrepTarget(uint32& spellId)
{
    spellId = 0;
    bool disenchant = BotProfessions::Disenchanting() && bot->HasSkill(SKILL_ENCHANTING) &&
                      bot->HasSpell(BotProfessions::SPELL_DISENCHANT);
    bool mill = BotProfessions::Milling() && bot->HasSkill(SKILL_INSCRIPTION) &&
                bot->HasSpell(BotProfessions::SPELL_MILLING);
    if ((!disenchant && !mill) || FreeSlots(bot) < PREP_FREE_SLOTS)
        return nullptr;
    // An auction visit under way may be listing these very items.
    if (BotAuction::State(bot).phase != BotAuction::Phase::NONE)
        disenchant = false;

    uint32 enchanting = disenchant ? bot->GetSkillValue(SKILL_ENCHANTING) : 0;
    Item* millTarget = nullptr;
    Item* disenchantTarget = nullptr;
    ForEachBagItem(bot,
                   [&](Item* item)
                   {
                       if (std::find(prepFailed.begin(), prepFailed.end(), item->GetGUID()) != prepFailed.end())
                           return;
                       ItemTemplate const* proto = item->GetTemplate();
                       if (mill && !millTarget && proto && proto->HasFlag(ITEM_FLAG_IS_MILLABLE) &&
                           item->GetCount() >= BotProfessions::MILL_COUNT && !item->IsInTrade() &&
                           BotProfessions::MillUseful(bot, proto->ItemId))
                           millTarget = item;
                       // The cheapest first: what a vendor would pay least for.
                       if (disenchant && Disenchantable(botAI, item, enchanting) &&
                           (!disenchantTarget || proto->SellPrice < disenchantTarget->GetTemplate()->SellPrice))
                           disenchantTarget = item;
                   });
    if (millTarget)
    {
        spellId = BotProfessions::SPELL_MILLING;
        return millTarget;
    }
    if (disenchantTarget)
        spellId = BotProfessions::SPELL_DISENCHANT;
    return disenchantTarget;
}

void BotCraftAction::EndSession()
{
    if (crafted)
        BotTelemetry::instance().Event(botAI, "craft", "session", static_cast<int32>(crafted));
    sessionEnd = 0;
    crafted = 0;
}

// The known recipe most likely to raise a skill that the bot can make here and now.
BotCraftAction::Recipe BotCraftAction::Best()
{
    Recipe best;
    std::list<GameObject*> objects;
    bool objectsSearched = false;

    for (auto const& [spellId, known] : bot->GetSpellMap())
    {
        if (!known || known->State == PLAYERSPELL_REMOVED || !known->Active)
            continue;
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
        if (!spell)
            continue;
        int32 chance = BotProfessions::SkillUpChance(bot, spell);
        if (chance <= best.chance)
            continue;

        bool reagents = true;
        for (uint32 i = 0; i < MAX_SPELL_REAGENTS && reagents; ++i)
            if (spell->Reagent[i] > 0 && !bot->HasItemCount(spell->Reagent[i], spell->ReagentCount[i]))
                reagents = false;
        for (uint32 i = 0; i < 2 && reagents; ++i)
            if ((spell->Totem[i] && !bot->HasItemCount(spell->Totem[i], 1)) ||
                (spell->TotemCategory[i] && !bot->HasItemTotemCategory(spell->TotemCategory[i])))
                reagents = false;
        if (!reagents)
            continue;

        Recipe recipe;
        recipe.spell = spell;
        recipe.chance = chance;
        bool makesItem = false;
        bool enchants = false;
        for (SpellEffectInfo const& effect : spell->Effects)
        {
            if (effect.Effect == SPELL_EFFECT_CREATE_ITEM && effect.ItemType)
            {
                ItemPosCountVec dest;
                if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, effect.ItemType, 1) != EQUIP_ERR_OK)
                    break;
                makesItem = true;
            }
            else if (effect.Effect == SPELL_EFFECT_ENCHANT_ITEM)
                enchants = true;
        }
        if (enchants)
        {
            // Its own gear, never over an enchant it already has.
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END && !recipe.target; ++slot)
            {
                Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (item && item->IsFitToSpellRequirements(spell) && !item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT))
                    recipe.target = item;
            }
            if (!recipe.target)
                continue;
        }
        else if (!makesItem)
            continue;

        if (spell->RequiresSpellFocus)
        {
            if (!objectsSearched)
            {
                Acore::GameObjectInRangeCheck check(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                                    FOCUS_RANGE);
                Acore::GameObjectListSearcher<Acore::GameObjectInRangeCheck> searcher(bot, objects, check);
                Cell::VisitObjects(bot, searcher, FOCUS_RANGE);
                objectsSearched = true;
            }
            for (GameObject* go : objects)
                if (go->isSpawned() && go->GetGoType() == GAMEOBJECT_TYPE_SPELL_FOCUS &&
                    go->GetGOInfo()->spellFocus.focusId == spell->RequiresSpellFocus)
                {
                    recipe.focus = go;
                    break;
                }
            if (!recipe.focus)
                continue;
        }
        best = recipe;
    }
    return best;
}
