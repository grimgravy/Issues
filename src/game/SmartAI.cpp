/*
* Copyright (C) 2008-2010 TrinityCore <http://www.trinitycore.org/>
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the
* Free Software Foundation; either version 2 of the License, or (at your
* option) any later version.
*
* This program is distributed in the hope that it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License along
* with this program. If not, see <http://www.gnu.org/licenses/>.
*/


#include "DatabaseEnv.h"
#include "ObjectMgr.h"
#include "ObjectDefines.h"
#include "GridDefines.h"
#include "GridNotifiers.h"
#include "SpellMgr.h"
#include "GridNotifiersImpl.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Unit.h"
#include "ScriptedCreature.h"
#include "ScriptMgr.h"

#include "SmartAI.h"

SmartAI::SmartAI(Creature* c) : CreatureAI(c)
{
    // copy script to local (protection for table reload)

    mWayPoints = NULL;
    mEscortState = SMART_ESCORT_NONE;
    mCurrentWPID = 0;//first wp id is 1 !!
    mWPReached = false;
    mOOCReached = false;
    mWPPauseTimer = 0;
    mLastWP = NULL;

    mCanRepeatPath = false;

    // spawn in run mode
    me->SetWalk(false);
    mRun = false;

    mLastOOCPos = me->GetPosition();

    mCanAutoAttack = true;
    mCanCombatMove = true;

    mForcedPaused = false;
    mLastWPIDReached = 0;

    mEscortQuestID = 0;

    mDespawnTime = 0;
    mDespawnState = 0;

    mEscortInvokerCheckTimer = 1000;
    mFollowGuid = 0;
    mFollowDist = 0;
    mFollowAngle = 0;
    mFollowCredit = 0;
    mFollowArrivedEntry = 0;
    mFollowCreditType = 0;
    mFollowArrivedTimer = 0;
    mInvincibilityHpLevel = 0;

    mJustReset = false;

    m_preventMoveHome = false;
}

void SmartAI::UpdateDespawn(const uint32 diff)
{
    if (mDespawnState <= 1 || mDespawnState > 3)
        return;

    if (mDespawnTime < diff)
    {
        if (mDespawnState == 2)
        {
            me->SetVisibility(VISIBILITY_OFF);
            mDespawnTime = 1000;
            mDespawnState++;
        }
        else
            me->ForcedDespawn();
    } else mDespawnTime -= diff;
}

WayPoint* SmartAI::GetNextWayPoint()
{
    if (!mWayPoints || mWayPoints->empty())
        return NULL;

    mCurrentWPID++;
    WPPath::const_iterator itr = mWayPoints->find(mCurrentWPID);
    if (itr != mWayPoints->end())
    {
        mLastWP = (*itr).second;
        if (mLastWP->id != mCurrentWPID)
        {
            TC_LOG_ERROR("FIXME","SmartAI::GetNextWayPoint: Got not expected waypoint id %u, expected %u", mLastWP->id, mCurrentWPID);
        }
        return (*itr).second;
    }
    return NULL;
}


void SmartAI::GenerateWayPointArray(Movement::PointsArray* points)
{
    if (!mWayPoints || mWayPoints->empty())
        return;

    // Flying unit, just fill array
    if (me->m_movementInfo.HasMovementFlag((MovementFlags)(MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_DISABLE_GRAVITY)))
    {
        // xinef: first point in vector is unit real position
        points->clear();
        points->push_back(G3D::Vector3(me->GetPositionX(), me->GetPositionY(), me->GetPositionZ()));
        uint32 wpCounter = mCurrentWPID;
        WPPath::const_iterator itr;
        while ((itr = mWayPoints->find(wpCounter++)) != mWayPoints->end())
        {
            WayPoint* wp = (*itr).second;
            points->push_back(G3D::Vector3(wp->x, wp->y, wp->z));
        }
    }
    else
    {
        for (float size = 1.0f; size; size *= 0.5f)
        {
            std::vector<G3D::Vector3> pVector;
            // xinef: first point in vector is unit real position
            pVector.push_back(G3D::Vector3(me->GetPositionX(), me->GetPositionY(), me->GetPositionZ()));
            uint32 length = (mWayPoints->size() - mCurrentWPID)*size;

            uint32 cnt = 0;
            uint32 wpCounter = mCurrentWPID;
            WPPath::const_iterator itr;
            while ((itr = mWayPoints->find(wpCounter++)) != mWayPoints->end() && cnt++ <= length)
            {
                WayPoint* wp = (*itr).second;
                pVector.push_back(G3D::Vector3(wp->x, wp->y, wp->z));
            }

            if (pVector.size() > 2) // more than source + dest
            {
                G3D::Vector3 middle = (pVector[0] + pVector[pVector.size() - 1]) / 2.f;
                G3D::Vector3 offset;

                bool continueLoop = false;
                for (uint32 i = 1; i < pVector.size() - 1; ++i)
                {
                    offset = middle - pVector[i];
                    if (fabs(offset.x) >= 0xFF || fabs(offset.y) >= 0xFF || fabs(offset.z) >= 0x7F)
                    {
                        // offset is too big, split points
                        continueLoop = true;
                        break;
                    }
                }
                if (continueLoop)
                    continue;
            }
            // everything ok
            *points = pVector;
            break;
        }
    }
}

void SmartAI::StartPath(bool run, uint32 path, bool repeat, Unit* /*invoker*/)
{
    if (me->IsInCombat())// no wp movement in combat
    {
        TC_LOG_ERROR("FIXME","SmartAI::StartPath: Creature entry %u wanted to start waypoint movement while in combat, ignoring.", me->GetEntry());
        return;
    }
    if (HasEscortState(SMART_ESCORT_ESCORTING))
        StopPath();
    if (path)
        if (!LoadPath(path))
            return;
    if (!mWayPoints || mWayPoints->empty())
        return;

    if (WayPoint* wp = GetNextWayPoint())
    {
        AddEscortState(SMART_ESCORT_ESCORTING);
        mCanRepeatPath = repeat;

        SetRun(run);

        mLastOOCPos = me->GetPosition();
        
        Movement::PointsArray pathPoints;
        GenerateWayPointArray(&pathPoints);

        me->GetMotionMaster()->MoveSplinePath(&pathPoints);
        GetScript()->ProcessEventsFor(SMART_EVENT_WAYPOINT_START, NULL, wp->id, GetScript()->GetPathId());
    }
}

bool SmartAI::LoadPath(uint32 entry)
{
    if (HasEscortState(SMART_ESCORT_ESCORTING))
        return false;
    mWayPoints = sSmartWaypointMgr->GetPath(entry);
    if (!mWayPoints)
    {
        GetScript()->SetPathId(0);
        return false;
    }
    GetScript()->SetPathId(entry);
    return true;
}

void SmartAI::PausePath(uint32 delay, bool forced)
{
    if (!HasEscortState(SMART_ESCORT_ESCORTING))
        return;
    if (HasEscortState(SMART_ESCORT_PAUSED))
    {
        TC_LOG_ERROR("FIXME","SmartAI::StartPath: Creature entry %u wanted to pause waypoint movement while already paused, ignoring.", me->GetEntry());
        return;
    }
    AddEscortState(SMART_ESCORT_PAUSED);
    mLastOOCPos = me->GetPosition();
    mWPPauseTimer = delay;
    if (forced && !mWPReached)
    {
        mForcedPaused = forced;
        SetRun(mRun);
        if (me->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE) == ESCORT_MOTION_TYPE)
            me->GetMotionMaster()->MovementExpired();
        me->StopMoving();//force stop
        me->GetMotionMaster()->MoveIdle();//force stop
    }
    GetScript()->ProcessEventsFor(SMART_EVENT_WAYPOINT_PAUSED, NULL, mLastWP->id, GetScript()->GetPathId());
}

void SmartAI::StopPath(uint32 DespawnTime, uint32 quest, bool fail)
{
    if (!HasEscortState(SMART_ESCORT_ESCORTING))
        return;

    if (quest)
        mEscortQuestID = quest;

    SetDespawnTime(DespawnTime);

    if (me->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE) == ESCORT_MOTION_TYPE)
        me->GetMotionMaster()->MovementExpired();

    mLastOOCPos = me->GetPosition();
    me->StopMoving();//force stop
    me->GetMotionMaster()->MoveIdle();
    GetScript()->ProcessEventsFor(SMART_EVENT_WAYPOINT_STOPPED, NULL, mLastWP->id, GetScript()->GetPathId());
    EndPath(fail);
}

void SmartAI::EndPath(bool fail)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_WAYPOINT_ENDED, NULL, mLastWP->id, GetScript()->GetPathId());

    RemoveEscortState(SMART_ESCORT_ESCORTING | SMART_ESCORT_PAUSED | SMART_ESCORT_RETURNING);
    mWayPoints = NULL;
    mCurrentWPID = 0;
    mWPPauseTimer = 0;
    mLastWP = NULL;

    if (mCanRepeatPath)
        StartPath(mRun, GetScript()->GetPathId(), true);
    else
        GetScript()->SetPathId(0);
    
    ObjectList* targets = GetScript()->GetTargetList(SMART_ESCORT_TARGETS);
    if (targets && mEscortQuestID)
    {
        if (targets->size() == 1 && GetScript()->IsPlayer((*targets->begin())))
        {
            Player* player = (*targets->begin())->ToPlayer();
            if (!fail && player->IsAtGroupRewardDistance(me) && !player->GetCorpse())
                player->GroupEventHappens(mEscortQuestID, me);
           
            if (fail && player->GetQuestStatus(mEscortQuestID) == QUEST_STATUS_INCOMPLETE)
                player->FailQuest(mEscortQuestID);

            if (Group* group = player->GetGroup())
            {
                for (GroupReference* groupRef = group->GetFirstMember(); groupRef != NULL; groupRef = groupRef->next())
                {
                    Player* groupGuy = groupRef->GetSource();

                    if (!fail && groupGuy->IsAtGroupRewardDistance(me) && !groupGuy->GetCorpse())
                        groupGuy->AreaExploredOrEventHappens(mEscortQuestID);
                    if (fail && groupGuy->GetQuestStatus(mEscortQuestID) == QUEST_STATUS_INCOMPLETE)
                        groupGuy->FailQuest(mEscortQuestID);
                }
            }
        }else
        {
            for (ObjectList::iterator iter = targets->begin(); iter != targets->end(); ++iter)
            {
                if (GetScript()->IsPlayer((*iter)))
                {
                    Player* player = (*iter)->ToPlayer();
                    if (!fail && player->IsAtGroupRewardDistance(me) && !player->GetCorpse())
                        player->AreaExploredOrEventHappens(mEscortQuestID);
                    if (fail && player->GetQuestStatus(mEscortQuestID) == QUEST_STATUS_INCOMPLETE)
                        player->FailQuest(mEscortQuestID);
                }
            }
        }
    }
    if (mDespawnState == 1)
        StartDespawn();
}

void SmartAI::ResumePath()
{
    //mWPReached = false;
    SetRun(mRun);
    if (mLastWP)
        me->GetMotionMaster()->MovePoint(mLastWP->id, mLastWP->x, mLastWP->y, mLastWP->z);
}

void SmartAI::ReturnToLastOOCPos()
{
    SetRun(mRun);
    me->GetMotionMaster()->MovePoint(SMART_ESCORT_LAST_OOC_POINT, mLastOOCPos);
}

void SmartAI::UpdatePath(const uint32 diff)
{
    if (!HasEscortState(SMART_ESCORT_ESCORTING))
        return;
    if (mEscortInvokerCheckTimer < diff)
    {
        if (!IsEscortInvokerInRange())
        {
            StopPath(mDespawnTime, mEscortQuestID, true);
        }
        mEscortInvokerCheckTimer = 1000;
    } else mEscortInvokerCheckTimer -= diff;
    // handle pause
    if (HasEscortState(SMART_ESCORT_PAUSED))
    {
        if (mWPPauseTimer < diff)
        {
            if (!me->IsInCombat() && !HasEscortState(SMART_ESCORT_RETURNING) && (mWPReached || mLastWPIDReached == SMART_ESCORT_LAST_OOC_POINT || mForcedPaused))
            {
                GetScript()->ProcessEventsFor(SMART_EVENT_WAYPOINT_RESUMED, NULL, mLastWP->id, GetScript()->GetPathId());
                RemoveEscortState(SMART_ESCORT_PAUSED);
                if (mForcedPaused)// if paused between 2 wps resend movement
                {
                    ResumePath();
                    mWPReached = false;
                    mForcedPaused = false;
                }
                if (mLastWPIDReached == SMART_ESCORT_LAST_OOC_POINT)
                    mWPReached = true;
            }
            mWPPauseTimer = 0;
        } else {
            mWPPauseTimer -= diff;
        }
    }
    if (HasEscortState(SMART_ESCORT_RETURNING))
    {
        if (mOOCReached)//reached OOC WP
        {
            mOOCReached = false;
            RemoveEscortState(SMART_ESCORT_RETURNING);
            if (!HasEscortState(SMART_ESCORT_PAUSED))
                ResumePath();
        }
    }
    if ((!me->HasReactState(REACT_PASSIVE) && me->IsInCombat()) || HasEscortState(SMART_ESCORT_PAUSED | SMART_ESCORT_RETURNING))
        return;

    // handle next wp
    if (mWPReached)//reached WP
    {
        mWPReached = false;
        if (mCurrentWPID == GetWPCount())
        {
            EndPath();
        }
        else if (WayPoint* wp = GetNextWayPoint())
        {
            SetRun(mRun);
            me->GetMotionMaster()->MovePoint(wp->id, wp->x, wp->y, wp->z);
        }
    }
}

void SmartAI::UpdateAI(const uint32 diff)
{
    GetScript()->OnUpdate(diff);
    UpdatePath(diff);
    UpdateDespawn(diff);

    /// @todo move to void
    if (mFollowGuid)
    {
        if (mFollowArrivedTimer < diff)
        {
            if (me->FindNearestCreature(mFollowArrivedEntry, INTERACTION_DISTANCE, true))
            {
                StopFollow();
                return;
            }

            mFollowArrivedTimer = 1000;
        }
        else
            mFollowArrivedTimer -= diff;
    }

    if (!UpdateVictim())
        return;

    if (mCanAutoAttack)
        DoMeleeAttackIfReady();
}

bool SmartAI::IsEscortInvokerInRange()
{
    ObjectList* targets = GetScript()->GetTargetList(SMART_ESCORT_TARGETS);
    if (targets)
    {
        if (targets->size() == 1 && GetScript()->IsPlayer((*targets->begin())))
        {
            Player* player = (*targets->begin())->ToPlayer();
            if (me->GetDistance(player) <= SMART_ESCORT_MAX_PLAYER_DIST)
                        return true;

            if (Group* group = player->GetGroup())
            {
                for (GroupReference* groupRef = group->GetFirstMember(); groupRef != NULL; groupRef = groupRef->next())
                {
                    Player* groupGuy = groupRef->GetSource();

                    if (me->GetDistance(groupGuy) <= SMART_ESCORT_MAX_PLAYER_DIST)
                        return true;
                }
            }
        }else
        {
            for (ObjectList::iterator iter = targets->begin(); iter != targets->end(); ++iter)
            {
                if (GetScript()->IsPlayer((*iter)))
                {
                    if (me->GetDistance((*iter)->ToPlayer()) <= SMART_ESCORT_MAX_PLAYER_DIST)
                        return true;
                }
            }
        }
    }
    return true;//escort targets were not set, ignore range check
}

void SmartAI::MovepointReached(uint32 id)
{
    // override the id, path can be resumed any time and counter will reset
    // mCurrentWPID holds proper id

    // xinef: both point movement and escort generator can enter this function
    if (id == SMART_ESCORT_LAST_OOC_POINT)
    {
        mOOCReached = true;
        return;
    }

    mLastWPIDReached = id;
    mWPReached = true;

    if (id != SMART_ESCORT_LAST_OOC_POINT && mLastWPIDReached != id)
        GetScript()->ProcessEventsFor(SMART_EVENT_WAYPOINT_REACHED, NULL, id);

    if (mLastWP)
    {
        me->SetPosition(mLastWP->x, mLastWP->y, mLastWP->z, me->GetOrientation());
        me->SetHomePosition(me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), me->GetOrientation());
    }

    if (HasEscortState(SMART_ESCORT_PAUSED))
    {
        if (me->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE) == ESCORT_MOTION_TYPE)
            me->GetMotionMaster()->MovementExpired();

        me->StopMovingOnCurrentPos();
        me->GetMotionMaster()->MoveIdle();
    }
    // Xinef: Can be unset in ProcessEvents
    else if (HasEscortState(SMART_ESCORT_ESCORTING) && me->GetMotionMaster()->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE)
    {
        mWPReached = false;
        if (mCurrentWPID == GetWPCount())
            EndPath();
        else if (GetNextWayPoint())
        {
            SetRun(mRun);
            // xinef: if we have reached waypoint, and there is no working spline movement it means our splitted array has ended, make new one
            if (me->movespline->Finalized())
                ResumePath();
        }
    }
}

void SmartAI::MovementInform(uint32 MovementType, uint32 Data)
{
    if ((MovementType == POINT_MOTION_TYPE && Data == SMART_ESCORT_LAST_OOC_POINT) || MovementType == FOLLOW_MOTION_TYPE)
        me->ClearUnitState(UNIT_STATE_EVADE);

    GetScript()->ProcessEventsFor(SMART_EVENT_MOVEMENTINFORM, NULL, MovementType, Data);
    if (MovementType != POINT_MOTION_TYPE || !HasEscortState(SMART_ESCORT_ESCORTING))
        return;

    if (MovementType == ESCORT_MOTION_TYPE || (MovementType == POINT_MOTION_TYPE && Data == SMART_ESCORT_LAST_OOC_POINT))
        MovepointReached(Data);
}

void SmartAI::RemoveAuras()
{
    Unit::AuraMap& auras = me->GetAuras();
    for(Unit::AuraMap::iterator itr = auras.begin(); itr != auras.end();)
    {
        if (!itr->second->IsPassive() && itr->second->GetCasterGUID() != me->GetGUID())
            me->RemoveAura(itr);
        else
            itr++;
    }
    /*
    /// @fixme: duplicated logic in CreatureAI::_EnterEvadeMode (could use RemoveAllAurasExceptType)
    Unit::AuraApplicationMap& appliedAuras = me->GetAppliedAuras();
    for (Unit::AuraApplicationMap::iterator iter = appliedAuras.begin(); iter != appliedAuras.end();)
    {
        Aura const* aura = iter->second->GetBase();
        if (!aura->IsPassive() && !aura->HasEffectType(SPELL_AURA_CLONE_CASTER) && aura->GetCasterGUID() != me->GetGUID())
            me->RemoveAura(iter);
        else
            ++iter;
    }
    */
}

void SmartAI::EnterEvadeMode()
{
    if (!me->IsAlive() || me->IsInEvadeMode())
        return;

    RemoveAuras();

    me->DeleteThreatList();
    me->CombatStop(true);
    me->SetLootRecipient(NULL);
    me->ResetPlayerDamageReq();
    me->SetLastDamagedTime(0);

    GetScript()->ProcessEventsFor(SMART_EVENT_EVADE);//must be after aura clear so we can cast spells from db

    SetRun(mRun);
    
    if(m_preventMoveHome)
        return;
    
    me->InitCreatureAddon();

    if (HasEscortState(SMART_ESCORT_ESCORTING))
    {
        AddEscortState(SMART_ESCORT_RETURNING);
        ReturnToLastOOCPos();
    }
    else if (mFollowGuid)
    {
        if (Unit* target = ObjectAccessor::GetUnit(*me, mFollowGuid))
            me->GetMotionMaster()->MoveFollow(target, mFollowDist, mFollowAngle);
    }
    else {
        me->GetMotionMaster()->MoveTargetedHome();
    }

    if (!HasEscortState(SMART_ESCORT_ESCORTING))//dont mess up escort movement after combat
        SetRun(mRun);
}

void SmartAI::MoveInLineOfSight(Unit* who)
{
    if (!who)
        return;

    GetScript()->OnMoveInLineOfSight(who);

    CreatureAI::MoveInLineOfSight(who);
}

void SmartAI::JustRespawned()
{
    mDespawnTime = 0;
    mDespawnState = 0;
    mEscortState = SMART_ESCORT_NONE;
    me->SetVisibility(VISIBILITY_ON);
    if (me->GetFaction() != me->GetCreatureTemplate()->faction)
        me->RestoreFaction();
    GetScript()->ProcessEventsFor(SMART_EVENT_RESPAWN);
    mJustReset = true;
    JustReachedHome();
    mFollowGuid = 0;//do not reset follower on Reset(), we need it after combat evade
    mFollowDist = 0;
    mFollowAngle = 0;
    mFollowCredit = 0;
    mFollowArrivedTimer = 1000;
    mFollowArrivedEntry = 0;
    mFollowCreditType = 0;
}

int SmartAI::Permissible(const Creature* creature)
{
    if (creature->GetAIName() == SMARTAI_AI_NAME)
        return PERMIT_BASE_SPECIAL;
    return PERMIT_BASE_NO;
}

void SmartAI::JustReachedHome()
{
    GetScript()->OnReset();

    if (!mJustReset)
    {
        GetScript()->ProcessEventsFor(SMART_EVENT_REACHED_HOME);

        if (!UpdateVictim() && me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE && me->GetWaypointPathId())
            me->GetMotionMaster()->MovePath(me->GetWaypointPathId());
    }

    mJustReset = false;
}

void SmartAI::EnterCombat(Unit* enemy)
{
    me->InterruptNonMeleeSpells(false); // must be before ProcessEvents
    GetScript()->ProcessEventsFor(SMART_EVENT_AGGRO, enemy);
    mLastOOCPos = me->GetPosition();
    SetRun(mRun);
    if (me->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE) == POINT_MOTION_TYPE)
        me->GetMotionMaster()->MovementExpired();
}

void SmartAI::JustDied(Unit* killer)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_DEATH, killer);
	if (HasEscortState(SMART_ESCORT_ESCORTING))
	{
		EndPath(true);
		me->StopMoving(); //force stop
		me->GetMotionMaster()->MoveIdle();
	}
}

void SmartAI::KilledUnit(Unit* victim)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_KILL, victim);
}

void SmartAI::AttackedUnitDied(Unit* attacked)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_ATTACKED_UNIT_DIED, attacked);
}

void SmartAI::JustSummoned(Creature* creature)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_SUMMONED_UNIT, creature);
}

void SmartAI::AttackStart(Unit* who)
{
    // xinef: dont allow charmed npcs to act on their own
    if (me->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED))
    {
        if (who && mCanAutoAttack)
            me->Attack(who, true);
        return;
    }

    if(!me->IsInCombat())
        EnterCombat(who);

    if (who && me->Attack(who, me->IsWithinMeleeRange(who)))
    {
        if (mCanCombatMove)
        {
            SetRun(mRun);
            MovementGeneratorType type = me->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE);
            if (type == ESCORT_MOTION_TYPE || type == POINT_MOTION_TYPE)
            {
                me->GetMotionMaster()->MovementExpired();
                me->StopMoving();
            }
            me->GetMotionMaster()->MoveChase(who);
        }
    }
}

void SmartAI::SpellHit(Unit* unit, const SpellInfo* spellInfo)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_SPELLHIT, unit, 0, 0, false, spellInfo);
}

void SmartAI::SpellHitTarget(Unit* target, const SpellInfo* spellInfo)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_SPELLHIT_TARGET, target, 0, 0, false, spellInfo);
}

void SmartAI::DamageTaken(Unit* doneBy, uint32& damage)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_DAMAGED, doneBy, damage);
    if (mInvincibilityHpLevel && (damage >= me->GetHealth() - mInvincibilityHpLevel))
    {
        damage = 0;
        me->SetHealth(mInvincibilityHpLevel);
    }
}

void SmartAI::HealReceived(Unit* doneBy, uint32& addhealth)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_RECEIVE_HEAL, doneBy, addhealth);
}

void SmartAI::ReceiveEmote(Player* player, uint32 textEmote)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_RECEIVE_EMOTE, player, textEmote);
}

void SmartAI::IsSummonedBy(Unit* summoner)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_JUST_SUMMONED, summoner);
}

void SmartAI::DamageDealt(Unit* doneTo, uint32& damage, DamageEffectType /*damagetype*/)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_DAMAGED_TARGET, doneTo, damage);
}

void SmartAI::SummonedCreatureDespawn(Creature* unit)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_SUMMON_DESPAWNED, unit);
}

void SmartAI::UpdateAIWhileCharmed(const uint32 /*diff*/) { }

void SmartAI::CorpseRemoved(uint32& respawnDelay)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_CORPSE_REMOVED, NULL, respawnDelay);
}

void SmartAI::PassengerBoarded(Unit* who, int8 seatId, bool apply)
{
#ifndef LICH_KING
    TC_LOG_ERROR("misc","SmartAI::PassengerBoarded was called while core isn't compiled for LK");
#endif
    GetScript()->ProcessEventsFor(apply ? SMART_EVENT_PASSENGER_BOARDED : SMART_EVENT_PASSENGER_REMOVED, who, uint32(seatId), 0, apply);
}

void SmartAI::InitializeAI()
{
    GetScript()->OnInitialize(me);
    if (!me->IsDead())
    mJustReset = true;
    JustReachedHome();
    GetScript()->ProcessEventsFor(SMART_EVENT_RESPAWN);
}

void SmartAI::OnCharmed(Unit* charmer, bool apply)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_CHARMED, NULL, 0, 0, apply);

    if (!apply && !me->IsInEvadeMode() && me->GetUInt64Value(UNIT_FIELD_CHARMEDBY))
        if (Unit* charmer = ObjectAccessor::GetUnit(*me, me->GetUInt64Value(UNIT_FIELD_CHARMEDBY)))
            AttackStart(charmer);
}

void SmartAI::DoAction(int32 param)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_ACTION_DONE, NULL, param);
}

uint32 SmartAI::GetData(uint32 /*id*/) const
{
    return 0;
}

void SmartAI::SetData(uint32 id, uint32 value)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_DATA_SET, NULL, id, value);
}

void SmartAI::SetGUID(uint64 /*guid*/, int32 /*id*/) { }

uint64 SmartAI::GetGUID(int32 /*id*/) const
{
    return 0;
}

void SmartAI::SetRun(bool run)
{
    me->SetWalk(!run);
    mRun = run;
}

void SmartAI::SetFly(bool fly)
{
    me->SetCanFly(fly);
}

void SmartAI::SetSwim(bool swim)
{
    if (swim)
        me->AddUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
    else
        me->RemoveUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
}

void SmartAI::sOnGossipHello(Player* player)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_GOSSIP_HELLO, player);
}

void SmartAI::sGossipSelect(Player* player, uint32 sender, uint32 action)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_GOSSIP_SELECT, player, sender, action);
}

void SmartAI::sOnGossipSelectCode(Player* /*player*/, uint32 /*sender*/, uint32 /*action*/, const char* /*code*/) { }

void SmartAI::sQuestAccept(Player* player, Quest const* quest)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_ACCEPTED_QUEST, player, quest->GetQuestId());
}

void SmartAI::sQuestReward(Player* player, Quest const* quest, uint32 opt)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_REWARD_QUEST, player, quest->GetQuestId(), opt);
}

bool SmartAI::sOnDummyEffect(Unit* caster, uint32 spellId, uint32 effIndex)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_DUMMY_EFFECT, caster, spellId, effIndex);
    return true;
}

void SmartAI::SetCombatMove(bool on)
{
    if (mCanCombatMove == on)
        return;
    mCanCombatMove = on;
    if (!HasEscortState(SMART_ESCORT_ESCORTING))
    {
        if (on && me->GetVictim())
        {
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE)
            {
                SetRun(mRun);
                me->GetMotionMaster()->MoveChase(me->GetVictim());
                me->CastStop();
            }
        }
        else if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != WAYPOINT_MOTION_TYPE)
        {
            if (me->HasUnitState(UNIT_STATE_CONFUSED_MOVE | UNIT_STATE_FLEEING_MOVE))
                return;

            me->GetMotionMaster()->MovementExpired();
            me->GetMotionMaster()->Clear(true);
            me->StopMoving();
            me->GetMotionMaster()->MoveIdle();
        }
    }
}

void SmartAI::SetFollow(Unit* target, float dist, float angle, uint32 credit, uint32 end, uint32 creditType)
{
    if (!target)
    {
        StopFollow();
        return;
    }

    SetRun(mRun);
    mFollowGuid = target->GetGUID();
    mFollowDist = dist >= 0.0f ? dist : PET_FOLLOW_DIST;
    mFollowAngle = angle >= 0.0f ? angle : static_cast<float>(M_PI/2); //me->GetFollowAngle();
    mFollowArrivedTimer = 1000;
    mFollowCredit = credit;
    mFollowArrivedEntry = end;
    me->GetMotionMaster()->MoveFollow(target, mFollowDist, mFollowAngle);
    mFollowCreditType = creditType;
}

void SmartAI::StopFollow()
{
    if (Player* player = ObjectAccessor::GetPlayer(*me, mFollowGuid))
    {
        if (!mFollowCreditType)
            player->RewardPlayerAndGroupAtEvent(mFollowCredit, me);
        else
            player->GroupEventHappens(mFollowCredit, me);
    }

    mFollowGuid = 0;
    mFollowDist = 0;
    mFollowAngle = 0;
    mFollowCredit = 0;
    mFollowArrivedTimer = 1000;
    mFollowArrivedEntry = 0;
    mFollowCreditType = 0;
    SetDespawnTime(5000);
    me->StopMoving();
    me->GetMotionMaster()->MoveIdle();
    StartDespawn();
    GetScript()->ProcessEventsFor(SMART_EVENT_FOLLOW_COMPLETED);
}

void SmartAI::SetScript9(SmartScriptHolder& e, uint32 entry, Unit* invoker)
{
    if (invoker)
        GetScript()->mLastInvoker = invoker->GetGUID();
    GetScript()->SetScript9(e, entry);
}

/*
void SmartAI::sOnGameEvent(bool start, uint16 eventId)
{
    GetScript()->ProcessEventsFor(start ? SMART_EVENT_GAME_EVENT_START : SMART_EVENT_GAME_EVENT_END, NULL, eventId);
}*/

void SmartAI::OnSpellClick(Unit* clicker, bool& result)
{
    if (!result)
        return;

    GetScript()->ProcessEventsFor(SMART_EVENT_ON_SPELLCLICK, clicker);
}

void SmartAI::FriendlyKilled(Creature const* c, float range)
{
    GetScript()->ProcessEventsFor( SMART_EVENT_FRIENDLY_KILLED, (Unit*)c );
}

int SmartGameObjectAI::Permissible(const GameObject* g)
{
    if (g->GetAIName() == "SmartGameObjectAI")
        return PERMIT_BASE_SPECIAL;
    return PERMIT_BASE_NO;
}

void SmartGameObjectAI::UpdateAI(uint32 diff)
{
    GetScript()->OnUpdate(diff);
}

void SmartGameObjectAI::InitializeAI()
{
    GetScript()->OnInitialize(go);
    GetScript()->ProcessEventsFor(SMART_EVENT_RESPAWN);
    //Reset();
}

void SmartGameObjectAI::Reset()
{
    GetScript()->OnReset();
}

// Called when a player opens a gossip dialog with the gameobject.
bool SmartGameObjectAI::OnGossipHello(Player* player)
{
    TC_LOG_DEBUG("scripts.ai","SmartGameObjectAI::GossipHello");
    GetScript()->ProcessEventsFor(SMART_EVENT_GOSSIP_HELLO, player, 0, 0, false, NULL, go);
    return false;
}

// Called when a player selects a gossip item in the gameobject's gossip menu.
bool SmartGameObjectAI::OnGossipSelect(Player* player, uint32 sender, uint32 action)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_GOSSIP_SELECT, player, sender, action, false, NULL, go);
    return false;
}

// Called when a player selects a gossip with a code in the gameobject's gossip menu.
bool SmartGameObjectAI::OnGossipSelectCode(Player* /*player*/, uint32 /*sender*/, uint32 /*action*/, const char* /*code*/)
{
    return false;
}

// Called when a player accepts a quest from the gameobject.
bool SmartGameObjectAI::OnQuestAccept(Player* player, Quest const* quest)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_ACCEPTED_QUEST, player, quest->GetQuestId(), 0, false, NULL, go);
    return false;
}

// Called when a player selects a quest reward.
bool SmartGameObjectAI::QuestReward(Player* player, Quest const* quest, uint32 opt)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_REWARD_QUEST, player, quest->GetQuestId(), opt, false, NULL, go);
    return false;
}

// Called when the gameobject is destroyed (destructible buildings only).
void SmartGameObjectAI::Destroyed(Player* player, uint32 eventId)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_DEATH, player, eventId, 0, false, NULL, go);
}

void SmartGameObjectAI::SetData(uint32 id, uint32 value)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_DATA_SET, NULL, id, value);
}

void SmartGameObjectAI::SetScript9(SmartScriptHolder& e, uint32 entry, Unit* invoker)
{
    if (invoker)
        GetScript()->mLastInvoker = invoker->GetGUID();
    GetScript()->SetScript9(e, entry);
}

void SmartGameObjectAI::OnGameEvent(bool start, uint16 eventId)
{
    GetScript()->ProcessEventsFor(start ? SMART_EVENT_GAME_EVENT_START : SMART_EVENT_GAME_EVENT_END, NULL, eventId);
}

void SmartGameObjectAI::OnStateChanged(GOState state, Unit* unit)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_GO_STATE_CHANGED, unit, state);
}

void SmartGameObjectAI::OnLootStateChanged(LootState state, Unit* unit)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_GO_LOOT_STATE_CHANGED, unit, state);
}

void SmartGameObjectAI::EventInform(uint32 eventId)
{
    GetScript()->ProcessEventsFor(SMART_EVENT_GO_EVENT_INFORM, NULL, eventId);
}

class SmartTrigger : public AreaTriggerScript
{
    public:

        SmartTrigger() : AreaTriggerScript("SmartTrigger") { }

        bool OnTrigger(Player* player, AreaTriggerEntry const* trigger)
        {
            if (!player->IsAlive())
                return false;

            TC_LOG_DEBUG("scripts.ai", "AreaTrigger %u is using SmartTrigger script", trigger->id);
            SmartScript script;
            script.OnInitialize(NULL, trigger);
            script.ProcessEventsFor(SMART_EVENT_AREATRIGGER_ONTRIGGER, player, trigger->id);
            return true;
        }
};

void AddSC_SmartScripts()
{
    new SmartTrigger();
}