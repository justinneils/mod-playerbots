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

## `selfbot-free-flight`

**Files:** `src/Bot/PlayerbotAI.cpp`, in `PlayerbotAI::UpdateAIInternal` (just before
`DoNextAction`); `src/Bot/PlayerbotAI.h` (the `flightSuspended` member).

**Added:**

```cpp
if (IsSelfBot(bot) && (bot->CanFly() || bot->IsFlying()))
{
    if (!flightSuspended)
    {
        flightSuspended = true;
        bot->InterruptNonMeleeSpells(false);
        bot->AttackStop();
        bot->GetMotionMaster()->Clear();
    }

    SetNextCheckDelay(sPlayerbotAIConfig.reactDelay);
    if (pmo)
        pmo->finish();
    return;
}

if (flightSuspended)
{
    flightSuspended = false;
    Reset();
}
```

**Why:** this server runs `mod-player-fly`, whose `.fly` command grants a normal player free flight
(server-side `SetCanFly`, not a mount) for as long as they leave it toggled on. A character flying
under its own steering must not also be driven by a selfbot. The case that forced it: a hunter's
`no pet` trigger queues `call pet` at 60 relevance, the cast is refused while airborne
(`PlayerbotAI::CastSpell` bails on `bot->IsFlying()`), and the trigger re-fires on the next tick
forever — starving every other action and spamming the owner's own client.

Scoped to `IsSelfBot` deliberately. Ordinary bots get `MOVEMENTFLAG_CAN_FLY` from playerbots itself
in `MovementActions` so they can follow a flying master, and gating them on `CanFly()` would ground
that. A selfbot's flags, by contrast, come from a real client, which reports them truthfully and
clears them when flight ends — so the suspension cannot latch.

Only the action engine is skipped: the chat replies, `HandleCommands`, the packet handlers and the
logout path above it all still run, so the character keeps answering commands while its owner flies,
and `Reset()` on the way back down makes it re-plan instead of resuming a stale target.

`bot->CanFly()` covers a flying mount too, which is the same situation and wants the same answer.
`StopMoving()` is deliberately *not* called: the client owns this character's movement, and a stop
packet would only fight it. Clearing the motion master is enough — that is where a bot-initiated
move lives.

---

## `hunter-arcane-shot`

**File:** `src/Ai/Class/Hunter/HunterActions.cpp`, in `CastArcaneShotAction::isUseful`

Two independent upstream bugs in the same six lines. Together they make Arcane Shot
**unreachable for every hunter on this realm** — the action is in all three hunter strategies
at 5.3 relevance, and `Engine::ExecuteAction` drops it as `ACTION_RESULT_USELESS` on every tick.

### 1. The Explosive Shot test is inverted

**Upstream:**

```cpp
if (!target || !botAI->HasSpell("explosive shot"))
    return false;
```

**Here:**

```cpp
if (!target)
    return false;

if (botAI->HasSpell("explosive shot"))
    return false;
```

**Why:** upstream `a76f2ca2` ("Refactor HasSpell/HasAura and convert spellIds to constants",
#2435) rewrote the original

```cpp
if (bot->HasSpell(53301) || bot->HasSpell(60051) ||
    bot->HasSpell(60052) || bot->HasSpell(60053))  // Explosive Shot spell IDs
    return false;
```

and dropped the negation on the way. The intent was and is *"Explosive Shot replaces Arcane Shot
as the filler, so skip Arcane Shot once you have it"* — a 51-point Survival talent at level 60+.
After the refactor Arcane Shot requires Explosive Shot, so a Beast Mastery or Marksmanship hunter
at any level, and **every** hunter below 60, never casts it.

### 2. The armor-penetration read is a health read

**Upstream:**

```cpp
bot->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1) + bot->GetUInt32Value(CR_ARMOR_PENETRATION)
```

**Here:**

```cpp
bot->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + CR_ARMOR_PENETRATION)
```

**Why:** `CR_ARMOR_PENETRATION` is a *rating index* (24, `Unit.h`), not a field index. It is meant
to be added to `PLAYER_FIELD_COMBAT_RATING_1` to pick the 25th entry of that array; upstream adds
the two **field reads** instead. Field 24 is `UNIT_FIELD_HEALTH` (`OBJECT_END + 0x12`), so the
second term is the bot's current health — and `armorPenRating > 435` is then true for anything past
roughly level 8. This one survives bug 1: fixing only the Explosive Shot test leaves Arcane Shot
still refused, which is why both changes are in this patch.

**Verifying:** any hunter without Explosive Shot, in combat with a target. Before, `arcane shot`
never appears in the action stream at all (`.abi` decision log, or `20_log_changes.lua`); after, it
lands in the rotation between `auto shot` and `raptor strike`. `env/dist/etc/abi-scripts/32_hunter.lua`
also asks for it by name every tick via `ai.do_action`, and that call goes through the same
`isUseful` gate, so a passing script and a silent rotation was the symptom that found this.

---

## `gathering-selfbot-master`

**File:** `src/Mgr/Item/LootObjectStack.cpp`, in `LootObject::IsLootPossible` (the
`canUseRequirement` lambda)

**Upstream** (#2638 `ddf54880`, reshaped by #2807 `81c0516b`):

```cpp
Player* master = botAI->GetMaster();
bool const hasActivePlayerMaster = master && !GET_PLAYERBOT_AI(master);
if (gatheringObject && !hasActivePlayerMaster)
    // refuse herb/ore/skin nodes above 80% bag space
```

**Here:**

```cpp
bool const hasActivePlayerMaster = botAI->HasGameClientMaster();
```

**Why:** upstream's gate exempts bots whose master is a real player, but "real player" there means
"has no PlayerbotAI". A selfbot master (`.playerbots bot self`) has one, so its party bots got the
80% gate anyway. Observed 2026-10-11: Thoronto (Mining 187, Mining Pick, 1 yd from a Gold Vein at
82% bags, master Montreale as selfbot) logged `add gathering loot - FAILED` 836 times in a row and
never mined. `HasGameClientMaster()` is upstream's own `IsRealPlayer || IsSelfBot` helper.

Exempting these bots from the 80% gate is safe here because `loot-bag-space` and
`gathering-no-room` already cover what the gate was for, the infinite re-gather loop.

**Diagnosed with** a throwaway abi-script logging `ai.value(guid, "bag space")` on
`add gathering loot` plus `bot.master.name == bot.name` for the master.

**If upstream switches this to `HasGameClientMaster()` or `IsSelfBot` itself,** drop this patch.

---

## `clean-quest-log-random-only`

**File:** `src/Ai/Base/Actions/DropQuestAction.cpp`, in `CleanQuestLogAction::Execute`

**Upstream:** after the `DropObsoleteQuests` check, every bot that runs the action drops each quest
more than 5/6/7/8 levels (by bot level band) below its own level, silently unless `debug rpg` is on.

**Here:** added straight after the `DropObsoleteQuests` check:

```cpp
if (!event.getOwner() && !sRandomPlayerbotMgr.IsRandomBot(bot))
    return false;
```

**Why:** the action is fired automatically by `"random"` in the `nc` strategy (and `"seldom"` in
`maintenance`), which every bot runs, selfbots included. Observed 2026-10-11: Montreale (level 36,
played as a selfbot) had 18 quests silently abandoned. All of them were level 28 or lower, which
matches the 30-39 threshold of 7 exactly, so you could not keep a low-level quest. The account alts
grouped with it had the same exposure. `DropObsoleteQuests` stays on because the 500 random bots
need it. Triggered events carry no owner, while a whispered `clean quest log` command does, so the
explicit command still works on alts.

**If upstream scopes this to random bots itself,** drop this patch.

---

## `no-auto-chest-loot`

**Files:** `src/Ai/Base/Actions/AddLootAction.cpp` (`IsChestLootBlocked`, called from
`AddLootAction::Execute` and `AddAllLootAction::AddLoot`); `src/PlayerbotAIConfig.h` / `.cpp`
(`AutoLootChests`); `conf/playerbots.conf.dist` (`Playerbots.AutoLootChests = 0`).

**Upstream:** any `GAMEOBJECT_TYPE_CHEST` object a bot can open goes on its `available loot` stack.
Only the per-entry `Playerbots.DisallowedGameObjects` list keeps one out.

**Here:** while `Playerbots.AutoLootChests` is 0 (the default), both add paths refuse a chest-type
object unless its lock has a Herbalism or Mining skill case:

```cpp
SkillType const skill = SkillByLockType(LockType(lockInfo->Index[i]));
if (skill == SKILL_HERBALISM || skill == SKILL_MINING)
    return false;
```

**Why:** Justin wanted no automatic chest looting at all (2026-10-18). A per-entry list cannot do
that: the world has hundreds of chest entries. The two gated functions cover every automatic way a
game object reaches the stack:

- `add all loot`, from the `often` trigger in the `loot` strategy (the nearby sweep);
- `add gathering loot`, from the `gather` timer, which routes through `AddAllLootAction::AddLoot`
  and would otherwise send a rogue to pick every locked chest it walks past;
- `add loot`, fired by `use game object` when the master opens an object (the bot copies the click).

What this blocks: treasure chests, locked chests, dungeon chests, and quest crates and sacks. Quest
pickups are the same object type. With `SyncQuestWithPlayer = 1`, a bot already leaves a quest item
for its master, so it gains nothing by running to one. Herb and ore nodes are chest-type too, which is
why the lock is checked. Fishing pools are `GAMEOBJECT_TYPE_FISHINGHOLE` and unaffected.

`u <object link>` (`UseItemAction`) does not use these paths, so an explicit order still opens a
chest. The whispered `add all loot` command does use them, so it now skips chests too.

**If this ever needs to go:** set `Playerbots.AutoLootChests = 1`. That restores upstream behaviour
without a rebuild.

---

## `loot-introspection`

**File:** `src/Mgr/Item/LootObjectStack.h` (header only)

**Upstream:** `LootObjectStack` keeps its pending loot, its release flag, its pending deadline and
its target list private, with no getters. `LootTarget` keeps its retry count and retry deadline
private too.

**Here:** const getters, with no behaviour change: `GetPendingLoot`, `IsAwaitingRelease`,
`GetPendingUntil` and `GetTargets` on the stack, and `GetRetryCount` and `GetRetryUntil` on
`LootTarget`.

**Why:** mod-altbotinspector's `ai.loot()` reads them, so a bot stuck on a loot target can be
diagnosed live from Lua (`env/dist/etc/abi-scripts/90_loot_diag.lua`). Before this, the only
way to look was a C++ log line and a rebuild, and the restart cleared the stuck state. Seen
2026-10-24: Thoronto stood next to a copper vein while `move to loot` failed every tick.
`IsLootPending()` cannot stand in for these getters, because it cancels and defers as a side effect.

**If upstream adds equivalent accessors,** point `AltBotInspectorWorld.cpp` at them and drop
this patch. If it renames the private members, only these one-line bodies need to follow.

---

## Not catalogued here

Six local commits predate this file and are not documented above (`git log --oneline
upstream/master..HEAD --no-merges`). They carry no `FORK-PATCH` markers. Worth backfilling if any
of them ever survives a merge silently broken.

**Retired 2026-10-11:** `9b3b060b` (score heirlooms by their scaled stats) is superseded by
upstream #2821 (`1e72d5f8`, "Have Heirloom Items scale with level"), which reads the same
ScalingStatDistribution / ScalingStatValue data for stats, weapon damage, armour and spell power.
The only difference was plumbing -- ours added a level parameter to `CollectItemStats`, upstream's
passes it to the `StatsCollector` constructor -- so the merge took upstream's `StatsCollector.cpp`,
`StatsCollector.h` and `StatsWeightCalculator.cpp` whole. No caller used our signature. Five
uncatalogued commits remain.

---

## Verifying the loot patches

Both are about a loop that only shows up with a **party bot** (a real-player master) whose bags are
nearly or entirely full. To reproduce or confirm a fix, put a gathering bot at >80% capacity next to
a node whose drops will not merge into a stack it already holds. Symptom: the bot re-casts the
gathering spell every few seconds on the same object, the node never despawns, and its inventory
does not change between casts.
