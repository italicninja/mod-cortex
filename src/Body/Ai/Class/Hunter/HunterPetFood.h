/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_HUNTERPETFOOD_H
#define _PLAYERBOT_HUNTERPETFOOD_H

#include "Bag.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Pet.h"
#include "Player.h"

// Food for a hunter's pet (FeedPetAction, upkeep's vendor trip): what's in its diet and still does it good.
namespace HunterPetFood
{
constexpr uint32 SPELL_FEED_PET = 6991;  // taught by the Training the Beast quest

// The hunter's own tamed pet, out and alive, or null.
inline Pet* HunterPet(Player* bot)
{
    Pet* pet = bot->GetPet();
    return pet && pet->getPetType() == HUNTER_PET && pet->IsAlive() ? pet : nullptr;
}

// Happiness the pet gets from the item (Pet::GetCurrentFoodBenefitLevel), 0 if it won't eat it.
inline uint32 Benefit(Pet* pet, ItemTemplate const* proto)
{
    if (!proto || proto->Class == ITEM_CLASS_QUEST || !proto->FoodType || !pet->HaveInDiet(proto))
        return 0;
    return pet->GetCurrentFoodBenefitLevel(proto->ItemLevel);
}

template <typename F>
void ForEachBagItem(Player* bot, F&& f)
{
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            f(item);
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                if (Item* item = bag->GetItemByPos(static_cast<uint8>(slot)))
                    f(item);
}

// Pieces of food in the bags the pet eats.
inline uint32 Count(Player* bot, Pet* pet)
{
    uint32 count = 0;
    ForEachBagItem(bot, [&](Item* item) {
        if (Benefit(pet, item->GetTemplate()))
            count += item->GetCount();
    });
    return count;
}

// The food that does the pet most good; of those, the lowest level (the cheapest, kept for when it's outgrown).
inline Item* Best(Player* bot, Pet* pet)
{
    Item* best = nullptr;
    uint32 bestBenefit = 0;
    ForEachBagItem(bot, [&](Item* item) {
        ItemTemplate const* proto = item->GetTemplate();
        uint32 benefit = Benefit(pet, proto);
        if (benefit > bestBenefit ||
            (benefit && benefit == bestBenefit && proto->ItemLevel < best->GetTemplate()->ItemLevel))
        {
            best = item;
            bestBenefit = benefit;
        }
    });
    return best;
}
}  // namespace HunterPetFood

#endif
