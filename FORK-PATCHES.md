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

**Why:** the `!IsRealPlayer(botAI->GetMaster())` half disables the bag-space check entirely for
any bot whose master is a real player — i.e. for every party bot. Such a bot queues
`CMSG_AUTOSTORE_LOOT_ITEM` regardless of free slots, the server rejects the store, and the loot
stays in the object. Because the object still holds loot it is never emptied and never despawns,
so `add gathering loot` re-adds it, `can loot` fires, and the bot re-loots the same node forever,
doing nothing else — no combat, no follow. Reproduced 2026-08-24/25 on herbalism *and* mining
nodes with bots at 86–100% bag capacity, and confirmed with instrumentation: 12 completed mining
casts in 70 seconds with the bot's inventory byte-identical before and after.

Aggravated by `mod-gathering-scale` at `Scale = 5.0` — Copper Ore rolls 5–20 instead of 1–4, so a
node needs far more free slots than its loot table implies.

`80` is not a new constant: `ShouldSellValue` (`src/Ai/Base/Value/MaintenanceValues.cpp`) uses the
same threshold against the same value, and `BagSpaceValue` returns percent-full.

**If upstream fixes this themselves,** drop this patch rather than merging both.

---

## Not catalogued here

Six local commits predate this file and are not documented above (`git log --oneline
upstream/master..HEAD --no-merges`). They carry no `FORK-PATCH` markers. Worth backfilling if any
of them ever survives a merge silently broken.
