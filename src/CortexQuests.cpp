/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 * Originally written as part of the wowproject patches to mod-playerbots.
 */

#include "CortexQuests.h"

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <tuple>
#include <unordered_set>

#include "BotTelemetry.h"
#include "CortexMemory.h"
#include "CortexTravel.h"
#include "BotDanger.h"
#include "Formulas.h"
#include "GameTime.h"
#include "Map.h"
#include "Opcodes.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Cortex.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TravelMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    constexpr float CLUSTER_JOIN_RANGE = 150.0f;
    constexpr float SEARCH_SPACING = 25.0f;
    constexpr float SEARCH_DEDUPE = 15.0f;
    constexpr size_t MAX_SEARCH_POINTS = 80;
    constexpr uint32 FREE_LOG_SLOTS_TO_PICK_UP = 3;
    constexpr uint32 SHELF_SECONDS = 3600;
    constexpr uint32 SHELVES_TO_ABANDON = 2;
    // Times at the turn-in with nobody there to take it before the quest is dropped for good, as new rpg's
    // drop_unturnable does on the second miss (an ender that is never spawned, e.g. a barrel only an event shows).
    constexpr uint8 REWARD_FAILS_TO_DROP = 2;
    constexpr uint32 CLASS_SHELF_SECONDS = 20 * MINUTE;  // a class quest is set aside briefly, then tried again
    constexpr uint32 UNREACH_BACKOFF_STEPS = 3;  // 1 h, 2 h, 4 h, 8 h
    constexpr uint32 DANGER_SHELF_SECONDS = 2 * HOUR;
    constexpr float HAND_IN_NEAR = 250.0f;
    constexpr uint32 COMPLETED_TO_HAND_IN = 3;
    constexpr float HAND_IN_TRIP = 1500.0f;  // finished quests further away don't count towards a hand-in trip
    constexpr float AVOID_RADIUS = 50.0f;
    constexpr uint32 AVOID_SECONDS = 30 * MINUTE;
    constexpr float PICK_UP_RANGE = 2500.0f;
    constexpr uint32 PICK_UP_MEMORY_SECONDS = 6 * HOUR;
    // Givers whose quests are only for their class or trade: nothing new for a levelling bot there.
    constexpr uint32 REVISIT_SKIP_SERVICES =
        CortexMemory::SERVICE_CLASS_TRAINER | CortexMemory::SERVICE_PROFESSION_TRAINER;
    constexpr float GRIND_MIN_RANGE = 60.0f;
    constexpr float GRIND_MAX_RANGE = 1500.0f;
    constexpr uint32 GRIND_MEMORY_SECONDS = 3 * HOUR;
    constexpr uint8 GRIND_MAX_PACK = 3;
    // A player grinds mobs near their level, not ones barely above grey: Silvain (16) kept to Elwynn's level 11-12
    // gnolls, which it found too rarely to gain anything, and never looked further (FarGrindSpot).
    constexpr uint8 GRIND_LEVELS_BELOW = 3;
    constexpr uint32 GRIND_FAR_CHECKS = 8;
    constexpr uint32 SAFE_WALK_CHECKS = 6;  // nearest grind spots or givers looked at for a safe walk there

    constexpr uint32 MAX_AREA_TRIGGER_ID = 20000;
    constexpr uint32 REMOTE_HAND_IN_CHECKS = 3;  // remote turn-ins tested for a trip per re-plan
    constexpr float ENDER_NEAR = 150.0f;         // a quest ender this close to the turn-in area is the one there

    float Dist2d(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

    // Quest -> the area triggers that explore it. ObjectMgr only maps trigger -> quest, so this is built once.
    std::vector<AreaTrigger const*> const& QuestAreaTriggers(uint32 questId)
    {
        static std::unordered_map<uint32, std::vector<AreaTrigger const*>> byQuest;
        static std::once_flag built;
        std::call_once(built,
                       []()
                       {
                           for (uint32 id = 1; id < MAX_AREA_TRIGGER_ID; ++id)
                               if (uint32 quest = sObjectMgr->GetQuestForAreaTrigger(id))
                                   if (AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(id))
                                       if (!sObjectMgr->GetAreaTriggerTeleport(id))
                                           byQuest[quest].push_back(trigger);
                       });
        static std::vector<AreaTrigger const*> const none;
        auto it = byQuest.find(questId);
        return it == byQuest.end() ? none : it->second;
    }

    // Items some vendor sells for gold. Built once from the vendor lists.
    std::unordered_set<uint32> const& VendorGoods()
    {
        static std::unordered_set<uint32> goods;
        static std::once_flag built;
        std::call_once(built,
                       []()
                       {
                           for (auto const& [entry, creature] : *sObjectMgr->GetCreatureTemplates())
                               if (VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(entry))
                                   for (VendorItem const* item : items->m_items)
                                       if (item && !item->ExtendedCost)
                                           goods.insert(item->item);
                       });
        return goods;
    }

    // A player reading "into the mine", "the coven", "their den" knows to look underground.
    bool Underground(Quest const* quest)
    {
        static constexpr char const* WORDS[] = {"mine",   "mines",  "cave",   "caves",   "cavern",      "caverns",
                                                "coven",  "den",    "burrow", "burrows", "grotto",      "tunnel",
                                                "tunnels", "lair",  "crypt",  "barrow",  "underground", "depths"};
        std::string text = quest->GetObjectives() + " " + quest->GetDetails();
        std::string word;
        for (size_t i = 0; i <= text.size(); ++i)
        {
            char c = i < text.size() ? text[i] : ' ';
            if (std::isalpha(static_cast<unsigned char>(c)))
            {
                word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                continue;
            }
            if (!word.empty())
                for (char const* w : WORDS)
                    if (word == w)
                        return true;
            word.clear();
        }
        return false;
    }

    bool NeedsExploring(Player* bot, Quest const* quest)
    {
        if (!quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT))
            return false;
        auto it = bot->getQuestStatusMap().find(quest->GetQuestId());
        return it != bot->getQuestStatusMap().end() && it->second.Status == QUEST_STATUS_INCOMPLETE &&
               !it->second.Explored;
    }

    bool InPolygon(std::vector<std::pair<float, float>> const& poly, float x, float y)
    {
        bool inside = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        {
            auto const& [xi, yi] = poly[i];
            auto const& [xj, yj] = poly[j];
            if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
                inside = !inside;
        }
        return inside;
    }

    void Centre(std::vector<CortexArea> const& areas, float& x, float& y)
    {
        float sx = 0.0f;
        float sy = 0.0f;
        uint32 n = 0;
        for (CortexArea const& area : areas)
            for (auto const& [px, py] : area.points)
            {
                sx += px;
                sy += py;
                ++n;
            }
        x = n ? sx / n : 0.0f;
        y = n ? sy / n : 0.0f;
    }

    std::vector<CortexArea> AreasFor(QuestPOIVector const* pois, uint32 map, int32 objective)
    {
        std::vector<CortexArea> areas;
        if (!pois)
            return areas;
        for (QuestPOI const& poi : *pois)
        {
            if (poi.ObjectiveIndex != objective || poi.MapId != map || poi.points.empty())
                continue;
            CortexArea area;
            for (QuestPOIPoint const& point : poi.points)
                area.points.emplace_back(static_cast<float>(point.x), static_cast<float>(point.y));
            areas.push_back(std::move(area));
        }
        return areas;
    }

    // Every objective area of the quest (not the turn-in), for steps the log doesn't tie to one objective.
    std::vector<CortexArea> AllAreas(QuestPOIVector const* pois, uint32 map)
    {
        std::vector<CortexArea> areas;
        if (!pois)
            return areas;
        for (QuestPOI const& poi : *pois)
            if (poi.ObjectiveIndex >= 0)
                for (CortexArea& area : AreasFor(pois, map, poi.ObjectiveIndex))
                    areas.push_back(std::move(area));
        return areas;
    }

    // An item the quest handed the bot that has a "Use:" spell (start item or quest source item in its bags).
    SpellInfo const* QuestUseItem(Player* bot, Quest const* quest, uint32& item)
    {
        std::vector<uint32> candidates = {quest->GetSrcItemId()};
        for (uint32 drop : quest->ItemDrop)
            candidates.push_back(drop);
        for (uint32 entry : candidates)
        {
            if (!entry || !bot->HasItemCount(entry, 1))
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (!proto)
                continue;
            for (auto const& spell : proto->Spells)
                if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.SpellId))
                    {
                        item = entry;
                        return info;
                    }
        }
        return nullptr;
    }

    bool SpellCreates(SpellInfo const* spell, uint32 item)
    {
        for (SpellEffectInfo const& effect : spell->Effects)
            if (effect.Effect == SPELL_EFFECT_CREATE_ITEM && effect.ItemType == item)
                return true;
        return false;
    }

    // The quest map (WorldMapArea) the objective's areas are drawn on; -2 (the whole quest) takes any objective's.
    uint32 MapAreaFor(QuestPOIVector const* pois, uint32 map, int32 objective)
    {
        if (!pois)
            return 0;
        for (QuestPOI const& poi : *pois)
            if (poi.MapId == map && (poi.ObjectiveIndex == objective || (objective < -1 && poi.ObjectiveIndex >= 0)))
                return poi.AreaId;
        return 0;
    }

    bool IsContinent(uint32 map) { return map == 0 || map == 1 || map == 530 || map == 571; }

    struct EnderSpot
    {
        uint16 map;
        float x;
        float y;
        float z;
    };

    // Quest -> where the creatures that take it in stand. Built once from the spawns.
    std::unordered_map<uint32, std::vector<EnderSpot>> const& EnderSpots()
    {
        static std::unordered_map<uint32, std::vector<EnderSpot>> spots;
        static std::once_flag built;
        std::call_once(built,
                       []()
                       {
                           std::unordered_map<uint32, std::vector<uint32>> questsByEnder;
                           for (auto const& [entry, quest] : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
                               questsByEnder[entry].push_back(quest);
                           for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
                           {
                               auto it = questsByEnder.find(data.id);
                               if (it == questsByEnder.end())
                                   continue;
                               for (uint32 quest : it->second)
                                   spots[quest].push_back({data.mapid, data.posX, data.posY, data.posZ});
                           }
                       });
        return spots;
    }

    // Height of the quest's ender nearest to (x, y) on the map, if one stands near the turn-in area.
    bool EnderZ(uint32 questId, uint32 map, float x, float y, float& z)
    {
        auto it = EnderSpots().find(questId);
        if (it == EnderSpots().end())
            return false;
        float best = ENDER_NEAR;
        bool found = false;
        for (EnderSpot const& spot : it->second)
        {
            float d = Dist2d(spot.x, spot.y, x, y);
            if (spot.map != map || d >= best)
                continue;
            best = d;
            z = spot.z;
            found = true;
        }
        return found;
    }

    CortexTask* AddTask(CortexPlan& plan, uint32 questId, int32 objective, CortexTaskKind kind, int32 target,
                        uint32 map, std::vector<CortexArea> areas)
    {
        if (areas.empty())
            return nullptr;
        CortexTask task;
        task.questId = questId;
        task.objective = objective;
        task.kind = kind;
        task.target = target;
        task.map = map;
        task.areas = std::move(areas);
        Centre(task.areas, task.x, task.y);
        plan.tasks.push_back(std::move(task));
        return &plan.tasks.back();
    }

    void AddUseTask(CortexPlan& plan, uint32 questId, int32 objective, int32 target, uint32 map,
                    std::vector<CortexArea> areas, uint32 item, SpellInfo const* spell)
    {
        if (CortexTask* task =
                AddTask(plan, questId, objective, CortexTaskKind::USE_ITEM, target, map, std::move(areas)))
        {
            task->item = item;
            task->spell = spell->Id;
        }
    }

    void BuildSearchPoints(CortexPlan const& plan, CortexCluster& cluster)
    {
        std::vector<CortexSearchPoint> points;
        auto add = [&points](float x, float y)
        {
            for (CortexSearchPoint const& p : points)
                if (Dist2d(p.x, p.y, x, y) < SEARCH_DEDUPE)
                    return;
            points.push_back({x, y, false});
        };

        for (uint32 index : cluster.tasks)
            for (CortexArea const& area : plan.tasks[index].areas)
            {
                if (area.points.size() < 3)
                {
                    for (auto const& [x, y] : area.points)
                        add(x, y);
                    continue;
                }

                float minX = area.points[0].first, maxX = minX, minY = area.points[0].second, maxY = minY;
                for (auto const& [x, y] : area.points)
                {
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                }
                size_t before = points.size();
                for (float x = minX + SEARCH_SPACING / 2; x <= maxX; x += SEARCH_SPACING)
                    for (float y = minY + SEARCH_SPACING / 2; y <= maxY; y += SEARCH_SPACING)
                        if (InPolygon(area.points, x, y))
                            add(x, y);
                // A sliver narrower than the grid still gets its vertices.
                if (points.size() == before)
                    for (auto const& [x, y] : area.points)
                        add(x, y);
            }

        if (points.size() > MAX_SEARCH_POINTS)
        {
            std::vector<CortexSearchPoint> thinned;
            float step = static_cast<float>(points.size()) / MAX_SEARCH_POINTS;
            for (size_t i = 0; i < MAX_SEARCH_POINTS; ++i)
                thinned.push_back(points[static_cast<size_t>(i * step)]);
            points.swap(thinned);
        }
        if (points.empty())
            points.push_back({cluster.x, cluster.y, false});
        cluster.points = std::move(points);
    }

    void BuildClusters(CortexPlan& plan)
    {
        for (uint32 i = 0; i < plan.tasks.size(); ++i)
        {
            CortexTask const& task = plan.tasks[i];
            if (task.kind == CortexTaskKind::TURN_IN)
                continue;

            CortexCluster* home = nullptr;
            for (CortexCluster& cluster : plan.clusters)
                if (cluster.map == task.map && Dist2d(cluster.x, cluster.y, task.x, task.y) <= CLUSTER_JOIN_RANGE)
                {
                    home = &cluster;
                    break;
                }
            if (!home)
            {
                plan.clusters.emplace_back();
                home = &plan.clusters.back();
                home->map = task.map;
                home->mapArea = task.mapArea;
            }
            home->tasks.push_back(i);
            home->underground = home->underground || task.underground;
            float sx = 0.0f;
            float sy = 0.0f;
            for (uint32 index : home->tasks)
            {
                sx += plan.tasks[index].x;
                sy += plan.tasks[index].y;
            }
            home->x = sx / home->tasks.size();
            home->y = sy / home->tasks.size();
        }

        for (CortexCluster& cluster : plan.clusters)
        {
            for (uint32 index : cluster.tasks)
                for (CortexArea const& area : plan.tasks[index].areas)
                    for (auto const& [x, y] : area.points)
                        cluster.radius = std::max(cluster.radius, Dist2d(cluster.x, cluster.y, x, y));
            BuildSearchPoints(plan, cluster);
        }
    }

    uint32 FreeLogSlots(Player* bot)
    {
        uint32 free = 0;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (!bot->GetQuestSlotQuestId(slot))
                ++free;
        return free;
    }

    // A finished quest to hand in on another continent that some trip reaches, class quests first.
    int32 RemoteHandIn(Player* bot, CortexPlan const& plan)
    {
        uint32 checks = 0;
        for (bool classPass : {true, false})
            for (uint32 i = 0; i < plan.tasks.size() && checks < REMOTE_HAND_IN_CHECKS; ++i)
            {
                CortexTask const& task = plan.tasks[i];
                if (!task.remote || CortexQuests::IsClassQuest(sObjectMgr->GetQuestTemplate(task.questId)) != classPass)
                    continue;
                ++checks;
                if (CortexTravel::Reachable(bot, task.map, task.x, task.y, task.mapArea))
                    return static_cast<int32>(i);
            }
        return -1;
    }

    // The walk from the bot to (x, y) isn't one to avoid (CortexTravel::DangerousWalk): picking a grind spot or a
    // giver past a zone well above the bot only made Travel give up on it, over and over, one spot after another.
    bool SafeWalk(Player* bot, uint32 map, float x, float y)
    {
        return !CortexTravel::Enabled() || !CortexTravel::Recheck() || map != bot->GetMapId() ||
               !CortexTravel::DangerousWalk(bot, bot->GetPositionX(), bot->GetPositionY(), x, y);
    }

    // A quest giver the bot saw at a lower level: a player who has levelled goes back through the towns it knows,
    // since a giver with no "!" (or a grey one) then may have one now. Half the bots that ground had a giver with an
    // open quest for them within 2500 yd, remembered only from before their last level.
    bool WorthRevisit(CortexMemory::Npc const& npc, uint8 level)
    {
        return (npc.services & CortexMemory::SERVICE_QUESTGIVER) && !(npc.services & REVISIT_SKIP_SERVICES) &&
               npc.seenLevel < level &&
               (npc.marker == DIALOG_STATUS_NONE || npc.marker == DIALOG_STATUS_UNAVAILABLE ||
                npc.marker == DIALOG_STATUS_AVAILABLE || npc.marker == DIALOG_STATUS_AVAILABLE_REP);
    }

    bool PickUpDestination(Player* bot, CortexPlan& plan, CortexMemory const& memory)
    {
        if (FreeLogSlots(bot) < FREE_LOG_SLOTS_TO_PICK_UP)
            return false;

        uint32 now = static_cast<uint32>(GameTime::GetGameTime().count());
        uint8 level = static_cast<uint8>(bot->GetLevel());
        bool revisit = Cortex::Revisit();
        // Remembered "!" first (revisit false), then givers last seen at a lower level, nearest first in each.
        std::vector<std::tuple<bool, float, CortexMemory::Npc const*>> givers;
        for (auto const& [key, npc] : memory.Npcs())
        {
            if (npc.map != bot->GetMapId())
                continue;
            bool marked = (npc.marker == DIALOG_STATUS_AVAILABLE || npc.marker == DIALOG_STATUS_AVAILABLE_REP) &&
                          now - npc.lastSeen <= PICK_UP_MEMORY_SECONDS;
            if (!marked && !(revisit && WorthRevisit(npc, level)))
                continue;
            float d = bot->GetExactDist2d(npc.x, npc.y);
            // A giver the bot got stuck walking to (just now, or hearthed out of before): the next one instead.
            if (d < PICK_UP_RANGE && !CortexQuests::IsAvoided(plan, npc.map, npc.x, npc.y) &&
                !memory.SpotBlocked(npc.map, npc.x, npc.y, now))
                givers.emplace_back(!marked, d, &npc);
        }
        std::sort(givers.begin(), givers.end(), [](auto const& a, auto const& b)
                  { return std::tie(std::get<0>(a), std::get<1>(a)) < std::tie(std::get<0>(b), std::get<1>(b)); });
        CortexMemory::Npc const* pick = nullptr;
        bool pickRevisit = false;
        // Safe-walk checks per kind, so a run of unsafe "!"s doesn't use up the look at the revisits.
        uint32 checks[2] = {0, 0};
        for (auto const& [isRevisit, d, npc] : givers)
        {
            if (checks[isRevisit]++ >= SAFE_WALK_CHECKS)
                continue;
            if (SafeWalk(bot, npc->map, npc->x, npc->y))
            {
                pick = npc;
                pickRevisit = isRevisit;
                break;
            }
        }
        if (!pick)
            return false;

        plan.destMap = pick->map;
        plan.destX = pick->x;
        plan.destY = pick->y;
        plan.destZ = pick->z;
        plan.hasDest = true;
        plan.revisit = pickRevisit;
        return true;
    }

    // Nothing the bot remembers is worth grinding: it has outlevelled the place (a level 16 bot with only Elwynn's
    // mobs in memory wandered 60 yd at random there for hours). A player opens the map and heads where its level
    // is: the nearest spot playerbots knows for the level (sTravelMgr's per-level cache of mob cells, what new rpg's
    // go-grind picks from), in another zone if need be, as long as there is a way there and it isn't deadly.
    bool FarGrindSpot(Player* bot, CortexPlan const& plan, CortexMemory const& memory, float& x, float& y, float& z)
    {
        uint32 now = static_cast<uint32>(GameTime::GetGameTime().count());
        std::vector<std::pair<float, WorldLocation const*>> spots;
        for (WorldLocation const& loc : sTravelMgr.GetLocsPerLevelCache(static_cast<uint8>(bot->GetLevel())))
        {
            if (loc.GetMapId() != bot->GetMapId())
                continue;
            float d = bot->GetExactDist2d(loc.GetPositionX(), loc.GetPositionY());
            if (d < GRIND_MIN_RANGE ||
                CortexQuests::IsAvoided(plan, loc.GetMapId(), loc.GetPositionX(), loc.GetPositionY()) ||
                memory.SpotBlocked(loc.GetMapId(), loc.GetPositionX(), loc.GetPositionY(), now) ||
                memory.HasCellTag(loc.GetMapId(), loc.GetPositionX(), loc.GetPositionY(), CortexMemory::CELL_DEATH))
                continue;
            spots.emplace_back(d, &loc);
        }
        std::sort(spots.begin(), spots.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

        uint32 checks = 0;
        for (auto const& [d, loc] : spots)
        {
            if (checks++ >= GRIND_FAR_CHECKS)
                break;
            // A way there: walking (same region), or a trip. Reachable alone is only true when a trip beats the walk,
            // so it turned down every spot a bot could walk to and outlevelled bots stood with nowhere to grind.
            if (BotDanger::IsDangerous(bot, loc->GetMapId(), loc->GetPositionX(), loc->GetPositionY(),
                                       loc->GetPositionZ()) ||
                !SafeWalk(bot, loc->GetMapId(), loc->GetPositionX(), loc->GetPositionY()) ||
                (!CortexTravel::SameRegion(bot, loc->GetMapId(), loc->GetPositionX(), loc->GetPositionY(),
                                           loc->GetPositionZ(), 0) &&
                 !CortexTravel::Reachable(bot, loc->GetMapId(), loc->GetPositionX(), loc->GetPositionY(), 0)))
                continue;
            x = loc->GetPositionX();
            y = loc->GetPositionY();
            z = loc->GetPositionZ();
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                BotTelemetry::instance().Event(botAI, "grind_far", "level spot", static_cast<int32>(d),
                                               static_cast<int32>(spots.size()), static_cast<int32>(checks));
            return true;
        }
        return false;
    }

    bool GrindDestination(Player* bot, CortexPlan& plan, CortexMemory const& memory)
    {
        uint32 now = static_cast<uint32>(GameTime::GetGameTime().count());
        uint8 level = static_cast<uint8>(bot->GetLevel());
        uint8 gray = Acore::XP::GetGrayLevel(level);
        float best = GRIND_MAX_RANGE;
        float bestX = 0.0f;
        float bestY = 0.0f;
        std::vector<std::tuple<float, float, float>> spots;  // distance, x, y
        for (auto const& [key, mob] : memory.Mobs())
        {
            if (mob.map != bot->GetMapId() || mob.elite || mob.maxCount > GRIND_MAX_PACK ||
                now - mob.lastSeen > GRIND_MEMORY_SECONDS || mob.maxLevel <= gray || mob.maxLevel > level + 1 ||
                mob.maxLevel + GRIND_LEVELS_BELOW < level)
                continue;
            float x = 0.0f;
            float y = 0.0f;
            CortexMemory::CellCentre(mob.cell, x, y);
            if (memory.HasCellTag(mob.map, x, y, CortexMemory::CELL_DEATH))
                continue;
            float d = bot->GetExactDist2d(x, y);
            if (d < GRIND_MIN_RANGE || d >= GRIND_MAX_RANGE || CortexQuests::IsAvoided(plan, mob.map, x, y) ||
                memory.SpotBlocked(mob.map, x, y, now))
                continue;
            spots.emplace_back(d, x, y);
        }
        std::sort(spots.begin(), spots.end());
        for (uint32 i = 0; i < spots.size() && i < SAFE_WALK_CHECKS; ++i)
        {
            auto const& [d, x, y] = spots[i];
            if (!SafeWalk(bot, bot->GetMapId(), x, y))
                continue;
            best = d;
            bestX = x;
            bestY = y;
            break;
        }
        float bestZ = bot->GetPositionZ();
        if (best >= GRIND_MAX_RANGE && !FarGrindSpot(bot, plan, memory, bestX, bestY, bestZ))
            return false;

        plan.destMap = bot->GetMapId();
        plan.destX = bestX;
        plan.destY = bestY;
        plan.destZ = CortexQuests::GroundZ(bot, bestX, bestY, bestZ);
        plan.hasDest = true;
        return true;
    }
}

void CortexQuests::Build(Player* bot, CortexPlan& plan, CortexMemory const& memory)
{
    plan.tasks.clear();
    plan.clusters.clear();
    plan.cluster = -1;
    plan.handIn = -1;
    plan.hasDest = false;
    plan.revisit = false;
    plan.goal = CortexGoal::NONE;

    uint32 map = bot->GetMapId();
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || IsShelved(bot, plan, questId))
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        QuestPOIVector const* pois = sObjectMgr->GetQuestPOIVector(questId);
        QuestStatus status = bot->GetQuestStatus(questId);
        if (status == QUEST_STATUS_COMPLETE)
        {
            size_t before = plan.tasks.size();
            AddTask(plan, questId, -1, CortexTaskKind::TURN_IN, 0, map, AreasFor(pois, map, -1));
            // The turn-in is on another continent: kept for when nothing is left to do here.
            if (plan.tasks.size() == before && pois && CortexTravel::CrossMap())
                for (QuestPOI const& poi : *pois)
                {
                    if (poi.ObjectiveIndex != -1 || poi.MapId == map || !IsContinent(poi.MapId))
                        continue;
                    if (CortexTask* task = AddTask(plan, questId, -1, CortexTaskKind::TURN_IN, 0, poi.MapId,
                                                   AreasFor(pois, poi.MapId, -1)))
                        task->remote = true;
                    break;
                }
            continue;
        }
        if (status != QUEST_STATUS_INCOMPLETE)
            continue;

        auto it = bot->getQuestStatusMap().find(questId);
        if (it == bot->getQuestStatusMap().end())
            continue;
        QuestStatusData const& data = it->second;
        size_t before = plan.tasks.size();
        // Every task the quest adds below is underground if its text says so (set after the tasks are added).
        struct UndergroundMark
        {
            CortexPlan& plan;
            size_t from;
            bool underground;
            ~UndergroundMark()
            {
                for (size_t i = from; i < plan.tasks.size(); ++i)
                    plan.tasks[i].underground = underground;
            }
        } mark{plan, before, Underground(quest)};

        // A quest that hands over an item with a "Use:" spell wants it used: on the creature or object the log
        // names when the spell takes a target, to make the item the log asks for, or where the quest happens.
        uint32 useItem = 0;
        SpellInfo const* use = QuestUseItem(bot, quest, useItem);
        bool useOnUnit = use && use->NeedsExplicitUnitTarget();
        bool useOnObject = use && (use->GetExplicitTargetMask() & TARGET_FLAG_GAMEOBJECT_MASK);
        bool usePlanned = false;

        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 npcOrGo = quest->RequiredNpcOrGo[i];
            if (!npcOrGo || data.CreatureOrGOCount[i] >= quest->RequiredNpcOrGoCount[i])
                continue;
            if ((npcOrGo > 0 && useOnUnit) || (npcOrGo < 0 && useOnObject))
            {
                AddUseTask(plan, questId, i, npcOrGo, map, AreasFor(pois, map, i), useItem, use);
                usePlanned = true;
                continue;
            }
            AddTask(plan, questId, i, npcOrGo > 0 ? CortexTaskKind::KILL : CortexTaskKind::USE, npcOrGo, map,
                    AreasFor(pois, map, i));
        }
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            uint32 item = quest->RequiredItemId[i];
            if (!item || data.ItemCount[i] >= quest->RequiredItemCount[i])
                continue;
            int32 objective = QUEST_OBJECTIVES_COUNT + i;
            if (use && !usePlanned && SpellCreates(use, item))
            {
                std::vector<CortexArea> areas = AreasFor(pois, map, objective);
                AddUseTask(plan, questId, objective, 0, map, areas.empty() ? AllAreas(pois, map) : std::move(areas),
                           useItem, use);
                usePlanned = true;
                continue;
            }
            if (CortexTask* task = AddTask(plan, questId, objective, CortexTaskKind::COLLECT, static_cast<int32>(item),
                                          map, AreasFor(pois, map, objective)))
            {
                // Thread, vials, drinks: a player knows to look for these at a vendor.
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
                task->buyable = proto && proto->BuyPrice && proto->Class != ITEM_CLASS_QUEST &&
                                (proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_CONSUMABLE) &&
                                VendorGoods().count(item);
            }
        }
        // An event quest (no area trigger) with a usable item: the event is what the item does (a totem at a
        // well, a rod on a beast, a horn at a lair).
        if (use && !usePlanned && quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) &&
            !data.Explored && QuestAreaTriggers(questId).empty())
        {
            AddUseTask(plan, questId, -2, 0, map, AllAreas(pois, map), useItem, use);
            continue;
        }
        if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) && !data.Explored && pois)
        {
            // Explore and event objectives have no counter of their own. Alone, any non-turn-in area of the quest
            // is it; next to counted objectives, only an area none of them uses (an explore quest's place).
            bool alone = plan.tasks.size() == before;
            if (alone || !QuestAreaTriggers(questId).empty())
                for (QuestPOI const& poi : *pois)
                {
                    int32 index = poi.ObjectiveIndex;
                    bool counted = (index >= 0 && index < QUEST_OBJECTIVES_COUNT && quest->RequiredNpcOrGo[index]) ||
                                   (index >= QUEST_OBJECTIVES_COUNT &&
                                    index < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT &&
                                    quest->RequiredItemId[index - QUEST_OBJECTIVES_COUNT]);
                    if (index < 0 || (!alone && counted))
                        continue;
                    AddTask(plan, questId, index, CortexTaskKind::EXPLORE, 0, map, AreasFor(pois, map, index));
                    break;
                }
        }
    }

    uint32 nowSeconds = static_cast<uint32>(GameTime::GetGameTime().count());
    for (CortexTask& task : plan.tasks)
    {
        task.mapArea = MapAreaFor(sObjectMgr->GetQuestPOIVector(task.questId), task.map, task.objective);
        if (task.kind != CortexTaskKind::TURN_IN)
            continue;
        task.hasZ = EnderZ(task.questId, task.map, task.x, task.y, task.z);
        // The bot got stuck walking to this turn-in: another place the ender stands, if there is one.
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (memory.SpotBlocked(task.map, task.x, task.y, nowSeconds) && OtherEnder(task, memory, x, y, z))
        {
            task.x = x;
            task.y = y;
            task.z = z;
            task.hasZ = true;
            task.mapArea = 0;
        }
    }

    BuildClusters(plan);

    float bx = bot->GetPositionX();
    float by = bot->GetPositionY();
    // Class quests first: the nearest finished one to hand in, else the nearest cluster working one.
    int32 classHandIn = -1;
    int32 classCluster = -1;
    float classHandInDist = FLT_MAX;
    float classClusterDist = FLT_MAX;
    for (uint32 i = 0; i < plan.tasks.size(); ++i)
    {
        CortexTask const& task = plan.tasks[i];
        float d = Dist2d(bx, by, task.x, task.y);
        if (task.kind == CortexTaskKind::TURN_IN && !task.remote && d < classHandInDist &&
            IsClassQuest(sObjectMgr->GetQuestTemplate(task.questId)))
        {
            classHandIn = static_cast<int32>(i);
            classHandInDist = d;
        }
    }
    for (uint32 i = 0; i < plan.clusters.size(); ++i)
    {
        float d = Dist2d(bx, by, plan.clusters[i].x, plan.clusters[i].y);
        if (d >= classClusterDist)
            continue;
        for (uint32 index : plan.clusters[i].tasks)
            if (IsClassQuest(sObjectMgr->GetQuestTemplate(plan.tasks[index].questId)))
            {
                classCluster = static_cast<int32>(i);
                classClusterDist = d;
                break;
            }
    }
    uint32 completed = 0;
    uint32 completedNear = 0;
    float nearestHandIn = FLT_MAX;
    for (uint32 i = 0; i < plan.tasks.size(); ++i)
    {
        CortexTask const& task = plan.tasks[i];
        if (task.kind != CortexTaskKind::TURN_IN || task.remote)
            continue;
        ++completed;
        float d = Dist2d(bx, by, task.x, task.y);
        if (d <= HAND_IN_TRIP)
            ++completedNear;
        if (d < nearestHandIn)
        {
            nearestHandIn = d;
            plan.handIn = static_cast<int32>(i);
        }
    }

    float nearestCluster = FLT_MAX;
    for (uint32 i = 0; i < plan.clusters.size(); ++i)
    {
        float d = Dist2d(bx, by, plan.clusters[i].x, plan.clusters[i].y);
        if (d < nearestCluster)
        {
            nearestCluster = d;
            plan.cluster = static_cast<int32>(i);
        }
    }

    // A player hands in on the way, or when several are done nearby, or when nothing else is left to do.
    if (classHandIn >= 0)
    {
        plan.handIn = classHandIn;
        plan.goal = CortexGoal::HAND_IN;
    }
    else if (classCluster >= 0)
    {
        plan.cluster = classCluster;
        plan.goal = CortexGoal::WORK;
    }
    else if (completed &&
        (plan.clusters.empty() || completedNear >= COMPLETED_TO_HAND_IN || nearestHandIn <= HAND_IN_NEAR ||
         nearestHandIn < nearestCluster))
        plan.goal = CortexGoal::HAND_IN;
    else if (!plan.clusters.empty())
        plan.goal = CortexGoal::WORK;
    else if (int32 remote = RemoteHandIn(bot, plan); remote >= 0)
    {
        plan.handIn = remote;
        plan.goal = CortexGoal::HAND_IN;
    }
    else if (PickUpDestination(bot, plan, memory))
        plan.goal = CortexGoal::PICK_UP;
    else
    {
        GrindDestination(bot, plan, memory);
        plan.goal = CortexGoal::GRIND;
    }

    if (plan.goal != CortexGoal::WORK)
        plan.cluster = -1;
    if (plan.goal != CortexGoal::HAND_IN)
        plan.handIn = -1;
}

uint32 CortexQuests::Progress(Player* bot, CortexPlan const& plan, CortexCluster const& cluster)
{
    uint32 sum = 0;
    for (uint32 index : cluster.tasks)
    {
        CortexTask const& task = plan.tasks[index];
        auto it = bot->getQuestStatusMap().find(task.questId);
        if (it == bot->getQuestStatusMap().end())
            continue;
        QuestStatusData const& data = it->second;
        if (task.kind == CortexTaskKind::COLLECT ||
            (task.kind == CortexTaskKind::USE_ITEM && task.objective >= QUEST_OBJECTIVES_COUNT))
            sum += data.ItemCount[task.objective - QUEST_OBJECTIVES_COUNT];
        else if (task.kind == CortexTaskKind::EXPLORE || (task.kind == CortexTaskKind::USE_ITEM && task.objective < 0))
            sum += data.Explored ? 1 : 0;
        else if (task.objective >= 0 && task.objective < QUEST_OBJECTIVES_COUNT)
            sum += data.CreatureOrGOCount[task.objective];
        if (it->second.Status == QUEST_STATUS_COMPLETE)
            sum += 1000;
    }
    return sum;
}

bool CortexQuests::ClusterHasCompletedTask(Player* bot, CortexPlan const& plan, CortexCluster const& cluster)
{
    for (uint32 index : cluster.tasks)
    {
        CortexTask const& task = plan.tasks[index];
        Quest const* quest = sObjectMgr->GetQuestTemplate(task.questId);
        auto it = bot->getQuestStatusMap().find(task.questId);
        if (!quest || it == bot->getQuestStatusMap().end() || it->second.Status != QUEST_STATUS_INCOMPLETE)
            return true;
        QuestStatusData const& data = it->second;
        if (task.kind == CortexTaskKind::COLLECT ||
            (task.kind == CortexTaskKind::USE_ITEM && task.objective >= QUEST_OBJECTIVES_COUNT))
        {
            uint8 i = static_cast<uint8>(task.objective - QUEST_OBJECTIVES_COUNT);
            if (data.ItemCount[i] >= quest->RequiredItemCount[i])
                return true;
        }
        else if (task.kind == CortexTaskKind::EXPLORE || (task.kind == CortexTaskKind::USE_ITEM && task.objective < 0))
        {
            if (data.Explored)
                return true;
        }
        else if (data.CreatureOrGOCount[task.objective] >= quest->RequiredNpcOrGoCount[task.objective])
            return true;
    }
    return false;
}

bool CortexQuests::InArea(CortexTask const& task, float x, float y, float margin)
{
    for (CortexArea const& area : task.areas)
    {
        if (area.points.size() >= 3 && InPolygon(area.points, x, y))
            return true;
        for (auto const& [px, py] : area.points)
            if (Dist2d(px, py, x, y) <= margin)
                return true;
    }
    return false;
}

bool CortexQuests::OtherEnder(CortexTask const& task, CortexMemory const& memory, float& x, float& y, float& z)
{
    auto it = EnderSpots().find(task.questId);
    if (it == EnderSpots().end())
        return false;
    uint32 now = static_cast<uint32>(GameTime::GetGameTime().count());
    float best = FLT_MAX;
    for (EnderSpot const& spot : it->second)
    {
        float d = Dist2d(spot.x, spot.y, task.x, task.y);
        if (spot.map != task.map || d < ENDER_NEAR || d >= best || memory.SpotBlocked(spot.map, spot.x, spot.y, now))
            continue;
        best = d;
        x = spot.x;
        y = spot.y;
        z = spot.z;
    }
    return best < FLT_MAX;
}

void CortexQuests::Shelve(Player* bot, CortexPlan& plan, uint32 questId, char const* reason)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    CortexMemory* memory = Cortex::Memory(bot);
    if (!memory)
        return;
    CortexMemory::Shelf& shelf = memory->ShelfFor(questId);
    // The only way there is through a zone well above the bot: it will go there when it has the levels, so the
    // quest waits for them (or DANGER_SHELF_SECONDS) and the set-aside doesn't count towards dropping it.
    bool danger = !std::strcmp(reason, "danger");
    if (!danger)
        shelf.count = static_cast<uint8>(std::min<uint32>(shelf.count + 1, 0xFF));
    shelf.untilLevel = bot->GetLevel() + 1;
    // A place it couldn't get to won't come closer on its own (an island, a city lift): wait longer each time.
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    bool classQuest = IsClassQuest(quest);
    // A class quest comes back soon, unless the bot couldn't get there: walking that way again won't help.
    uint32 seconds = SHELF_SECONDS;
    if (danger)
        seconds = DANGER_SHELF_SECONDS;
    else if (!std::strcmp(reason, "unreach"))
        seconds <<= std::min<uint32>(shelf.count - 1, UNREACH_BACKOFF_STEPS);
    else if (classQuest)
        seconds = CLASS_SHELF_SECONDS;
    shelf.untilTime = static_cast<uint32>(GameTime::GetGameTime().count()) + seconds;
    plan.dirty = true;

    // At the turn-in again and nobody there to take it: whoever takes it in doesn't show for the bot.
    bool unturnable = false;
    if (!std::strcmp(reason, "reward"))
    {
        shelf.rewardFails = static_cast<uint8>(std::min<uint32>(shelf.rewardFails + 1, 0xFF));
        unturnable = shelf.rewardFails >= REWARD_FAILS_TO_DROP;
    }

    // A finished quest is kept: a player hands it in later rather than throw the reward away, unless it can't be
    // handed in at all. Class quests are never dropped.
    uint16 slot = bot->FindQuestSlot(questId);
    if (!classQuest && slot < MAX_QUEST_LOG_SIZE &&
        (unturnable ||
         (!danger && shelf.count >= SHELVES_TO_ABANDON && bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)))
    {
        WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
        packet << static_cast<uint8>(slot);
        WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
        removeQuest.Read();
        bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
        memory->Unshelve(questId);
        // Remembered, or the giver's "!" brings the bot straight back to it; one nobody takes in, for good.
        if (unturnable)
            memory->GiveUpForGood(questId);
        else
            memory->GiveUp(questId, static_cast<uint8>(bot->GetLevel()));
        if (botAI)
            BotTelemetry::instance().Event(botAI, "quest", unturnable ? std::string("drop_unturnable")
                                                                      : std::string("drop_") + reason,
                                           questId);
        return;
    }
    if (botAI)
        BotTelemetry::instance().Event(botAI, "quest", std::string("shelve_") + reason, questId);
}

bool CortexQuests::IsClassQuest(Quest const* quest) { return quest && quest->GetRequiredClasses(); }

bool CortexQuests::IsShelved(Player* bot, CortexPlan const& /*plan*/, uint32 questId)
{
    CortexMemory const* memory = Cortex::Memory(bot);
    CortexMemory::Shelf const* shelf = memory ? memory->FindShelf(questId) : nullptr;
    return shelf && bot->GetLevel() < shelf->untilLevel &&
           static_cast<uint32>(GameTime::GetGameTime().count()) < shelf->untilTime;
}

void CortexQuests::Avoid(CortexPlan& plan, uint32 map, float x, float y)
{
    uint32 now = static_cast<uint32>(GameTime::GetGameTime().count());
    std::erase_if(plan.avoid, [now](CortexAvoid const& a) { return a.untilTime <= now; });
    plan.avoid.push_back({map, x, y, now + AVOID_SECONDS});
}

bool CortexQuests::IsAvoided(CortexPlan const& plan, uint32 map, float x, float y)
{
    uint32 now = static_cast<uint32>(GameTime::GetGameTime().count());
    for (CortexAvoid const& a : plan.avoid)
        if (a.untilTime > now && a.map == map && Dist2d(a.x, a.y, x, y) <= AVOID_RADIUS)
            return true;
    return false;
}

void CortexQuests::OutingPlace(CortexPlan const& plan, float& x, float& y)
{
    x = 0.0f;
    y = 0.0f;
    if (plan.goal == CortexGoal::WORK && plan.cluster >= 0)
    {
        x = plan.clusters[plan.cluster].x;
        y = plan.clusters[plan.cluster].y;
    }
    else if (plan.goal == CortexGoal::HAND_IN && plan.handIn >= 0)
    {
        x = plan.tasks[plan.handIn].x;
        y = plan.tasks[plan.handIn].y;
    }
    else if (plan.hasDest)
    {
        x = plan.destX;
        y = plan.destY;
    }
}

char const* CortexQuests::GoalName(CortexGoal goal)
{
    switch (goal)
    {
        case CortexGoal::HAND_IN:
            return "hand_in";
        case CortexGoal::WORK:
            return "work";
        case CortexGoal::PICK_UP:
            return "pick_up";
        case CortexGoal::GRIND:
            return "grind";
        default:
            return "none";
    }
}

void CortexQuests::TouchAreaTriggers(Player* bot)
{
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest || !NeedsExploring(bot, quest))
            continue;
        for (AreaTrigger const* trigger : QuestAreaTriggers(questId))
        {
            if (!bot->IsInAreaTriggerRadius(trigger))
                continue;
            WorldPacket packet(CMSG_AREATRIGGER, 4);
            packet << trigger->entry;
            bot->GetSession()->HandleAreaTriggerOpcode(packet);
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                BotTelemetry::instance().Event(botAI, "quest", "explore", questId, static_cast<int32>(trigger->entry));
            break;
        }
    }
}

bool CortexQuests::VendorSells(uint32 vendorEntry, uint32 item)
{
    VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(vendorEntry);
    if (!items)
        return false;
    for (VendorItem const* entry : items->m_items)
        if (entry && entry->item == item && !entry->ExtendedCost)
            return true;
    return false;
}

float CortexQuests::GroundZ(Player* bot, float x, float y, float refZ)
{
    float z = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, refZ + 30.0f, true, 100.0f);
    if (z <= INVALID_HEIGHT)
        z = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, MAX_HEIGHT, true, MAX_HEIGHT);
    return z;
}
