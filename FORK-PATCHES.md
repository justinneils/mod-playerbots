# Fork patches

Deliberate divergences from `upstream/master` that live only in this fork
(`origin` = `justinneils/mod-playerbots`). **These are never submitted upstream.**

Each patch is marked in the source with a `FORK-PATCH(<id>)` comment, so a merge conflict
shows you what it is and why it exists. After every `git merge upstream/master`, re-run:

```sh
grep -rn "FORK-PATCH(" src/
```

and confirm every id below is still present and still applied. Upstream rewriting the
surrounding code will conflict; upstream rewriting it *without* conflicting is the dangerous
case, which is what this checklist is for.

---

## `loot-bag-space`

**File:** `src/Ai/Base/Actions/LootAction.cpp`, in `StoreLootAction::Execute`

**Upstream:**

```cpp
if (!IsRealPlayer(botAI->GetMaster()) && AI_VALUE(uint8, "bag space") > 80)
```

**Here:**

```cpp
if (AI_VALUE(uint8, "bag space") > 80)
```

**Upstream, in full:**

```cpp
if (!IsRealPlayer(botAI->GetMaster()) && AI_VALUE(uint8, "bag space") > 80)
{
    uint32 maxStack = proto->GetMaxStackSize();
    if (maxStack == 1) continue;
    std::vector<Item*> found = parseItems(chat->FormatItem(proto));
    bool hasFreeStack = false;
    for (auto stack : found)
        if (stack->GetCount() + itemcount < maxStack) { hasFreeStack = true; break; }
    if (!hasFreeStack) continue;
}
```

**Here:**

```cpp
if (AI_VALUE(uint8, "bag space") > 80)
{
    ItemPosCountVec dest;
    if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemid, itemcount) != EQUIP_ERR_OK)
        continue;
}
```

**Why, part 1 — the master check.** `!IsRealPlayer(botAI->GetMaster())` disables the bag-space
check entirely for any bot whose master is a real player — i.e. for every party bot. Such a bot
queues `CMSG_AUTOSTORE_LOOT_ITEM` regardless of free slots and the server rejects the store.

**Why, part 2 — the heuristic itself.** It only ever asked *"can this merge into a partial stack I
already hold"*. It never looked at **free slots**, so a bot with empty slots refused loot it could
trivially store; and `< maxStack` rejected even an exact fit. Observed 2026-08-30 on a tin vein:
Coarse Stone ×10 offered against held stacks of 20 and 15 (max 20), and Shadowgem ×5 against none
held — both refused, while **four slots sat empty** in the bot's second bag. Bag space read 92%.

`CanStoreNewItem` is the authoritative answer — the same question the server asks when the packet
arrives — and it accounts for free slots, partial stacks, bag families and unique limits at once.
A pass here means the store actually consumes the object's loot.

**The shared consequence:** nothing is stored, so the object keeps its loot, never empties and
never despawns. `add gathering loot` re-adds it every cycle and the bot re-mines the same node
forever, doing nothing else — no combat, no follow. Reproduced 2026-08-24/25 on herbalism *and*
mining nodes at 86–100% capacity (12 completed mining casts in 70 seconds, inventory byte-identical
before and after), and again 2026-08-30 at 92% with free slots.

Aggravated by `mod-gathering-scale` at `Scale = 5.0` — Copper Ore rolls 5–20 instead of 1–4, so a
node needs far more free slots than its loot table implies.

`80` is not a new constant: `ShouldSellValue` (`src/Ai/Base/Value/MaintenanceValues.cpp`) uses the
same threshold against the same value, and `BagSpaceValue` returns percent-full.

**This patch alone does not close the loop** — at 100% full there is still nothing to store and the
node is still re-added. `gathering-no-room` below is the other half; keep both.

**If upstream fixes this themselves,** drop this patch rather than merging both.

---

## `gathering-no-room`

**File:** `src/Ai/Base/Actions/AddLootAction.cpp`, in `AddGatheringLootAction::AddLoot`

**Added after the `IsLootPossible` check:**

```cpp
if (AI_VALUE(uint8, "bag space") >= 100)
    return false;
```

**Why:** `LootObject::IsLootPossible` answers for skill, distance, tools, quest state and respawn,
but knows nothing about bag space. A bot with no free slot therefore keeps taking a gathering node
as a target: it mines, `StoreLootAction` finds nothing it can store, the object keeps its loot and
never despawns, and the next tick re-adds it. Same infinite loop as above, reached from the other
direction — which is why `loot-bag-space` alone did not end it.

Deliberately `100` and not the `80` used for what to store: a bot should still top up partial
stacks from a node while it is nearly full. It just must not chase one when it has nowhere to put
anything at all. `BagSpaceValue` is percent-full over the backpack and generic containers, so 100
means genuinely no free slot.

Scoped to `AddGatheringLootAction` on purpose. `IsLootPossible` itself is shared with corpse
looting, travel targeting and the loot triggers, and a bag-space test there would also stop a full
bot looting a quest item off a corpse.

---

## Not catalogued here

Six local commits predate this file and are not documented above (`git log --oneline
upstream/master..HEAD --no-merges`). They carry no `FORK-PATCH` markers. Worth backfilling if any
of them ever survives a merge silently broken.

---

## Verifying the loot patches

Both are about a loop that only shows up with a **party bot** (a real-player master) whose bags are
nearly or entirely full. To reproduce or confirm a fix, put a gathering bot at >80% capacity next to
a node whose drops will not merge into a stack it already holds. Symptom: the bot re-casts the
gathering spell every few seconds on the same object, the node never despawns, and its inventory
does not change between casts.
