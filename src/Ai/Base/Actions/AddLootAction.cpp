/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AddLootAction.h"
#include "CellImpl.h"
#include "Event.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "LootObjectStack.h"
#include "Playerbots.h"
#include "ServerFacade.h"

bool AddLootAction::Execute(Event event)
{
    ObjectGuid guid = event.getObject();
    if (!guid)
        return false;

    return AI_VALUE(LootObjectStack*, "available loot")->Add(guid);
}

bool AddAllLootAction::Execute(Event /*event*/)
{
    bool added = false;

    GuidVector gos = context->GetValue<GuidVector>("nearest game objects")->Get();
    for (GuidVector::iterator i = gos.begin(); i != gos.end(); i++)
        added |= AddLoot(*i);

    GuidVector corpses = context->GetValue<GuidVector>("nearest corpses")->Get();
    for (GuidVector::iterator i = corpses.begin(); i != corpses.end(); i++)
        added |= AddLoot(*i);

    return added;
}

bool AddLootAction::isUseful() { return true; }

bool AddAllLootAction::isUseful() { return true; }

bool AddAllLootAction::AddLoot(ObjectGuid guid) { return AI_VALUE(LootObjectStack*, "available loot")->Add(guid); }

bool AddGatheringLootAction::AddLoot(ObjectGuid guid)
{
    LootObject loot(bot, guid);

    WorldObject* wo = loot.GetWorldObject(bot);
    if (loot.IsEmpty() || !wo)
        return false;

    if (loot.skillId == SKILL_NONE)
        return false;

    if (!loot.IsLootPossible(bot))
        return false;

    // FORK-PATCH(gathering-no-room): a bot with no free slot at all must not take a gathering node as
    // a target. IsLootPossible answers for skill, distance, tools and quest state but knows nothing
    // about bag space, so without this a full bot re-adds the same node every cycle: it mines it,
    // StoreLootAction finds nothing it can store, the object keeps its loot and so never despawns,
    // and the next tick brings it straight back. The bot mines forever and does nothing else.
    //
    // The other half is FORK-PATCH(loot-bag-space) in LootAction.cpp, which decides what can be
    // stored once a node IS being looted. This one keeps a bot that can store NOTHING from starting.
    // Both are needed: that one leaves the loop intact at 100% full, this one alone would still let a
    // bot with one free slot loop on a node it cannot empty.
    //
    // BagSpaceValue is percent-full over the backpack and generic containers, so 100 means genuinely
    // no free slot. Deliberately not the 80 threshold used for what to store -- a bot should still
    // top up partial stacks from a node while it is nearly full; it just must not chase one when it
    // has nowhere to put anything.
    if (AI_VALUE(uint8, "bag space") >= 100)
        return false;

    return AddAllLootAction::AddLoot(guid);
}
