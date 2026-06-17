# aiDoom — Monster Control Guide for an Agent

You are an **AI Director**: you command the *monsters* in aiDoom. You do **not**
move each monster every frame — you issue infrequent, high-level **squad orders**,
and the engine's own AI executes them every tic (pathing, aiming, firing). Think
tactics, not micro.

This is the operator's manual. The engine-side design lives in
`AGENT_CONTROL.md` §12–13; you don't need it to play.

---

## 1. Connect

Launch the game with the director socket enabled:

```sh
./aidoom -aidirector 31666 -warp 1 1     # port optional, default 31666
```

Connect a TCP client to **127.0.0.1:31666**. The protocol is **newline-delimited
text**, one command per line, one client at a time. The engine never blocks on
you — if you go quiet, monsters keep executing their last orders.

Commands: `observe`, `act`, `reset`, `wake`.

---

## 2. Perceive — `observe`

Send `observe\n`. You get back **one JSON line**:

```json
{"tic":58,
 "player":{"pos":[1056,-3616,0],"angle":90,"health":100,"armor":0,"weapon":1},
 "monsters":[
   {"id":1,"type":"imp","pos":[3440,-3472],"hp":60,"see_player":false,"order":"none"},
   {"id":2,"type":"zombieman","pos":[2912,-2816],"hp":20,"see_player":true,"order":"chase"}
 ],
 "count":6}
```

Field reference:

- `tic` — game time in 1/35 s ticks.
- `player.pos` — `[x, y, z]` in **map units**.
- `player.angle` — **degrees**, `0` = east, `90` = north, `180` = west, `270` = south.
- `player.health`, `armor`, `weapon` (weapon slot index).
- `monsters[]`:
  - `id` — **stable handle** you pass to `act`. Valid until the monster dies or
    the roster changes; ids are re-issued on level change. Re-`observe` to refresh.
  - `type` — `imp`, `zombieman`, `shotgunguy`, `pinky`, … (`monster` if unmapped).
  - `pos` — `[x, y]` map units.
  - `hp` — current health.
  - `see_player` — true if this monster currently has line-of-sight to the player.
  - `order` — the directive it's currently executing (`none` = vanilla AI).
- `count` — number of monsters in the roster.

Distance to the player = `hypot(player.x − mon.x, player.y − mon.y)`.
Bearing = `degrees(atan2(mon.y − player.y, mon.x − player.x))`.

**Observe on events, not on a timer:** the player enters a room, a monster is
hit or killed, line-of-sight flips, the player fires. A few times per second is
plenty.

---

## 3. Act — `act`

One directive per line:

```
act order=<name> ids=<csv> [for=<tics>] [after=<tics>] [x=<n> y=<n>] [focus=<id>]
```

Examples:

```
act order=flank_left ids=1,2 for=140
act order=fallback ids=3 for=200 after=35
act order=ambush ids=4,5 x=2912 y=-2816 for=300
act order=focus_fire ids=1,2,3 focus=0 for=140
```

The engine replies `ok\n`.

### Order vocabulary (keep it small — that's the point)

| order | meaning |
|---|---|
| `chase` | vanilla: walk straight at the target (the engine default) |
| `hold` | stand ground, face the target, fire when able |
| `fallback` | retreat — move away from the target |
| `flank_left` | circle: move ~90° to the target's left |
| `flank_right` | circle: move ~90° to the target's right |
| `ambush` | move toward point `x,y` and wait there (pass `x=`/`y=`) |
| `focus_fire` | retarget onto `focus=<id>` (`focus=0` = the player) and engage |
| `use_door` | head toward the target through doors (approximate) |

Monsters under any order **still aim and fire** when in range — you're changing
*where they move*, not disabling combat.

### Scheduling — always set `for`, optionally `after`

- `for=<tics>` — how long the order stays valid (35 tics ≈ 1 s). **Always set it.**
  When it expires the monster reverts to vanilla `chase`. Default if omitted: 70.
- `after=<tics>` — delay before the order activates. Lets you queue a follow-up,
  e.g. "flank for 70 tics, *then* focus-fire":
  ```
  act order=flank_left ids=1 for=70
  act order=focus_fire ids=1 focus=0 for=140 after=70
  ```

Because orders are **latched**, you never need to keep up with 35 Hz: issue an
order, walk away, the monster executes it until `for` runs out or you replace it.

---

## 4. The control loop you should run

```
loop:
    state = observe()
    if nothing tactically changed since last decision and orders still valid:
        continue            # don't spam; let the latched orders run
    plan = decide(state)    # your tactics (LLM call, rules, whatever)
    for directive in plan:
        act(directive)
```

`decide()` is where you earn your keep. The engine gives primitive chase +
infighting; you add coordination.

---

## 5. Tactical playbook (suggested)

- **Flank, don't bunch.** Send half the squad `flank_left`, half `flank_right`,
  so the player can't hold one corridor. Bunched monsters block each other.
- **Bait and switch.** One monster `chase` (the bait), the rest `ambush` at a
  chokepoint on the player's likely path; flip them to `focus_fire focus=0` once
  the player commits.
- **Focus the player when grouped.** `focus_fire ids=all focus=0` concentrates
  fire; far deadlier than the default scattered aggro.
- **Retreat the weak.** Low-`hp` monsters → `fallback` to stay alive and keep
  pressure; don't feed them to the player.
- **Respect line-of-sight.** Only monsters with `see_player:true` can shoot;
  use `flank`/`ambush` to *create* sightlines from new angles.
- **Use the hp signal.** Watch the player's `health` drop across observes
  (damage taken) — press the attack when they're hurt, regroup when they heal.

---

## 6. Constraints & gotchas

- **A monster obeys only once it's awake.** Orders on a sleeping monster are
  stored but do nothing until it wakes (sees or hears the player). Check
  `see_player`/`order` in the next `observe` to confirm it's acting.
- **ids are ephemeral.** They track live monsters between observes; after a death
  or level change, re-`observe` and rebuild your plan. Don't cache ids long-term.
- **No map/topology yet.** You get positions, not room graph. `ambush x,y` takes
  raw coordinates; plan paths from the `pos` fields you see.
- **Single-player only.** Directed monsters bypass the engine's RNG-driven AI, so
  this breaks demo/netgame sync. Fine for solo play.
- **`use_door`/`use_teleporter`** are approximate (treated as "move toward
  target"); don't rely on precise door routing yet.

---

## 7. `wake` (testing aid)

`wake\n` wakes every monster (targets the player, drops them into chase) — handy
to exercise your tactics without first luring the player into their sight. Reply
`ok\n`. Use it in tests; in real play, monsters wake naturally.

`reset\n` clears all directives (everyone back to vanilla).

---

## 8. Worked example (verified)

Player standing still; monster id 5 awake.

```
wake                              -> ok
observe  ... id 5 dist 1681 order none      # vanilla: it charges
observe  ... id 5 dist 1486 order none       (closing in)
act order=fallback ids=5 for=800  -> ok
observe  ... id 5 dist 1479 order fallback  # reverses
observe  ... id 5 dist 1759 order fallback   (now fleeing the player)
```

That reversal — charge under `none`, flee under `fallback` — is the whole
mechanism. Build your tactics on top of it.
