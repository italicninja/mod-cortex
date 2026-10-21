/*
 * This file is part of the mod-cortex module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

// Regroup after a death (plan.md B3): a bot back from its corpse has half its health and stands where it died. It
// used to sit down to recover right there; 28% of PvE deaths of corpse runners on 10-20 came within 2 min of a revive,
// three quarters to the same kind of mob, a median 28 s after it. A player steps away from the camp first. While a
// hostile that could pull it is in sight within THREAT_RANGE, the bot walks to the nearby spot farthest from the
// hostiles it sees and from where it died; then "new rpg recover" sits it down.

#include <cmath>
#include <list>

#include "BotTelemetry.h"
#include "CortexMemory.h"
#include "CortexTravel.h"
#include "GameTime.h"
#include "CellImpl.h"
#include "Cortex.h"
#include "CortexActions.h"
#include "CortexQuests.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"

namespace
{
constexpr uint32 REGROUP_MS = 90 * IN_MILLISECONDS;  // how long after a revive
constexpr float SCAN_RANGE = 60.0f;                   // hostiles it takes into account
constexpr float THREAT_RANGE = 30.0f;                 // one this near could pull it, or within its aggro range
constexpr float AGGRO_MARGIN = 10.0f;                 // plus this
constexpr int32 GREY_LEVELS = 6;                      // this far below the bot a mob doesn't pull on its own
constexpr float SPOT_DISTANCES[] = {35.0f, 50.0f};
constexpr uint32 SPOT_DIRECTIONS = 8;
constexpr float SPOT_WINDING = 1.6f;  // a path at most this much longer than the straight line
constexpr float SAFE_SCORE = 35.0f;   // a spot this far from every hostile is good enough
constexpr uint32 MAX_MOVES = 2;
// Out of its depth: the second death within OUTCLASSED_WINDOW to something OUTCLASSED_LEVELS or more above it. Walking
// back out past the same mobs killed bots again and again (level 17s in Searing Gorge and Felwood, 11 deaths each in
// 18 min); a player hearths out. Before the corpse runs (bots 0066) playerbots' revive teleport did it for them.
constexpr uint32 OUTCLASSED_WINDOW = 15 * MINUTE;
constexpr int8 OUTCLASSED_LEVELS = 5;
constexpr uint32 OUTCLASSED_DEATHS = 2;
// Or caught in a loop: the third death within LOOP_RADIUS in OUTCLASSED_WINDOW, whatever killed it. Redridge's spirit
// healer at (-9473,-3009) stands beside a Blackrock camp; six level 19-20 bots died there 12-19 times in 19 min,
// each revive 40 yd from the mobs and the next walk through them.
constexpr float LOOP_RADIUS = 150.0f;
constexpr uint32 LOOP_DEATHS = 3;
constexpr uint32 HEARTH_TRIES = 3;

struct Hostile
{
    float x, y;
};

// Hostile creatures in sight that would pull the bot on their own: alive, not fighting, not grey to it. `threat`: one
// of them is within THREAT_RANGE or its aggro range of the bot (a mob well above it pulls from 45 yd).
std::vector<Hostile> Hostiles(Player* bot, float& nearest, bool& threat)
{
    threat = false;
    std::list<Unit*> units;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, SCAN_RANGE);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, units, check);
    Cell::VisitObjects(bot, searcher, SCAN_RANGE);

    std::vector<Hostile> hostiles;
    nearest = SCAN_RANGE + 1.0f;
    for (Unit* unit : units)
    {
        Creature* creature = unit->ToCreature();
        if (!creature || !creature->IsHostileTo(bot) || creature->IsInCombat() || creature->IsPet() ||
            creature->IsTotem() || creature->IsCivilian() ||
            static_cast<int32>(creature->GetLevel()) + GREY_LEVELS < static_cast<int32>(bot->GetLevel()) ||
            !bot->IsWithinLOSInMap(creature))
            continue;
        hostiles.push_back({creature->GetPositionX(), creature->GetPositionY()});
        float dist = bot->GetExactDist2d(creature);
        nearest = std::min(nearest, dist);
        if (dist <= std::max(THREAT_RANGE, creature->GetAggroRange(bot) + AGGRO_MARGIN))
            threat = true;
    }
    return hostiles;
}
}  // namespace

bool CortexRegroupAction::isUseful()
{
    if (!Cortex::Regrouping())
        return false;
    CortexPlan* plan = Cortex::Plan(bot);
    if (!plan || !plan->revivedMs)
        return false;
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->GetGroup() ||
        getMSTimeDiff(plan->revivedMs, getMSTime()) >= REGROUP_MS)
    {
        plan->revivedMs = 0;
        _moves = 0;
        _hearthTries = 0;
        return false;
    }
    return true;
}

bool CortexRegroupAction::Execute(Event /*event*/)
{
    CortexPlan* plan = Cortex::Plan(bot);
    if (!plan)
        return false;
    if (bot->isMoving())
        return true;

    if (bot->IsNonMeleeSpellCast(false))
        return true;

    // Out of its depth here: home by hearthstone, the first thing after coming back (before anything pulls it).
    if (_hearthTries < HEARTH_TRIES && CortexTravel::Hearth() && CortexTravel::HearthReady(bot))
    {
        ++_hearthTries;
        CortexMemory* memory = Cortex::Memory(bot);
        uint32 now = static_cast<uint32>(GameTime::GetGameTime().count());
        uint32 outclassed = memory ? memory->DeathsOutclassed(now - OUTCLASSED_WINDOW, OUTCLASSED_LEVELS) : 0;
        uint32 looping = memory ? memory->DeathsNear(bot->GetMapId(), plan->deathX, plan->deathY, LOOP_RADIUS,
                                                     now - OUTCLASSED_WINDOW)
                                : 0;
        if ((outclassed >= OUTCLASSED_DEATHS || looping >= LOOP_DEATHS) &&
            (bot->GetMapId() != bot->m_homebindMapId ||
             bot->GetExactDist2d(bot->m_homebindX, bot->m_homebindY) > 300.0f))
        {
            // c: what stood in the way when it failed: 1 sitting, 2 shapeshifted, 4 mounted, 8 moving, 16 in combat
            uint32 state = (bot->IsSitState() ? 1 : 0) | (bot->GetShapeshiftForm() != FORM_NONE ? 2 : 0) |
                           (bot->IsMounted() ? 4 : 0) | (bot->isMoving() ? 8 : 0) | (bot->IsInCombat() ? 16 : 0);
            if (bot->isMoving())
                bot->StopMoving();
            if (bot->IsSitState())
                bot->SetStandState(UNIT_STAND_STATE_STAND);
            if (bot->IsMounted())
                bot->Dismount();
            botAI->RemoveShapeshift();
            bool cast = botAI->DoSpecificAction("hearthstone", Event(), true);
            BotTelemetry::instance().Event(botAI, "regroup", cast ? "hearth" : "hearth_fail",
                                           static_cast<int32>(outclassed * 100 + looping),
                                           static_cast<int32>(bot->GetExactDist2d(bot->m_homebindX, bot->m_homebindY)),
                                           static_cast<int32>(state));
            if (cast)
            {
                plan->dirty = true;
                return true;
            }
            if (_hearthTries < HEARTH_TRIES)
            {
                botAI->SetNextCheckDelay(2 * IN_MILLISECONDS);
                return true;  // try again in a moment, standing
            }
        }
    }

    float nearest = 0.0f;
    bool threat = false;
    std::vector<Hostile> hostiles = Hostiles(bot, nearest, threat);
    if (!threat || _moves >= MAX_MOVES)
    {
        BotTelemetry::instance().Event(botAI, "regroup", _moves ? "done" : "clear", static_cast<int32>(hostiles.size()),
                                       static_cast<int32>(nearest), static_cast<int32>(_moves));
        plan->revivedMs = 0;
        _moves = 0;
        _hearthTries = 0;
        return false;
    }

    // The spot whose nearest hostile (or the death spot, which keeps whatever killed it in mind) is farthest.
    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const z = bot->GetPositionZ();
    float bestScore = nearest;
    float bx = 0.0f, by = 0.0f, bz = 0.0f;
    bool found = false;
    for (float distance : SPOT_DISTANCES)
    {
        for (uint32 i = 0; i < SPOT_DIRECTIONS; ++i)
        {
            float angle = static_cast<float>(i) * 2.0f * static_cast<float>(M_PI) / SPOT_DIRECTIONS;
            float sx = x + distance * std::cos(angle);
            float sy = y + distance * std::sin(angle);
            float score = std::hypot(sx - plan->deathX, sy - plan->deathY);
            for (Hostile const& h : hostiles)
                score = std::min(score, std::hypot(sx - h.x, sy - h.y));
            if (score <= bestScore + 5.0f)
                continue;
            float sz = CortexQuests::GroundZ(bot, sx, sy, z);
            if (sz <= INVALID_HEIGHT)
                continue;
            PathGenerator path(bot);
            path.CalculatePath(sx, sy, sz);
            if (path.GetPathType() != PATHFIND_NORMAL || path.getPathLength() > distance * SPOT_WINDING + 10.0f)
                continue;
            G3D::Vector3 const& end = path.GetActualEndPosition();
            if (bot->GetMap()->IsInWater(bot->GetPhaseMask(), end.x, end.y, end.z, bot->GetCollisionHeight()))
                continue;
            bestScore = score;
            bx = end.x;
            by = end.y;
            bz = end.z;
            found = true;
        }
        if (found && bestScore >= SAFE_SCORE)
            break;
    }

    if (!found)
    {
        BotTelemetry::instance().Event(botAI, "regroup", "none", static_cast<int32>(hostiles.size()),
                                       static_cast<int32>(nearest), static_cast<int32>(_moves));
        plan->revivedMs = 0;
        _moves = 0;
        _hearthTries = 0;
        return false;
    }

    ++_moves;
    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);
    BotTelemetry::instance().Event(botAI, "regroup", "move", static_cast<int32>(hostiles.size()),
                                   static_cast<int32>(nearest), static_cast<int32>(bestScore));
    return MoveTo(bot->GetMapId(), bx, by, bz, false, false, false, true);
}
