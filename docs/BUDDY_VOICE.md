# Buddy voice — line catalogue and gate behavior

## Asset layout

The voice system is pre-baked. `files/i_voice.c` maps short tags to WAD lumps and `files/p_ai_coop.c` / `files/p_ai_director.c` request those tags at runtime.

- Buddy persona: `DS*` lumps in the voice PWAD, played through the buddy/positional stream.
- Director persona: `DD*` lumps, played through the separate director stream.
- The source voice map currently contains **168 buddy entries**, **52 director entries**, **220 mapped entries total**. The total physical lump count in a particular `aidoom.wad` is an asset snapshot and should be regenerated/checked after a rebake; it is not a source invariant.

The WAD is configured with `aidoom_wad`; the legacy `buddy_wad` setting remains accepted. Default lookup uses the normal ID0 path. Lump names are limited to eight bytes; keep the source tag/lump spelling byte-compatible.

## Two independent streams

Buddy and Director audio can overlap because `i_voice.c` owns separate SDL audio streams. This is intentional: the buddy is positional/local, while the Director is a global game-master voice.

This does not mean the buddy can overlap with itself. The buddy gate refuses a new buddy line while `I_Voice_Busy()` reports a queued/playing line.

## Buddy gate (as implemented)

`AICoop_VoiceGate` in `files/p_ai_coop.c` is a busy gate, not a preemptive priority queue:

- if a buddy line is already busy, the new line is dropped;
- there is no buddy-side `I_Voice_Stop()` preemption when a higher tier arrives;
- `vp_cur` is tracked but is not used to preempt the current clip;
- ambient buddy chatter additionally defers while the Director stream is busy;
- command lines have no independent cooldown, but they can still be dropped if the buddy stream is occupied;
- kill/weapon/command lines may overlap Director audio because the streams are independent.

The practical result is “one buddy clip at a time, with source-side tier selection,” not “higher tier always interrupts lower tier.”

## Buddy tiers and gaps

The source uses four priority classes. The current minimum gaps are:

| Tier | Typical content | Gap |
|---|---|---:|
| ambient | contact, clear, hurt, stuck, door flavor | 5 s |
| kill | per-type kill quips | 3 s |
| weapon | weapon callouts | 3 s |
| command | explicit player command/revive/help | 0 s |

Those gaps are eligibility spacing; the busy gate can still reject a line. A command is therefore not an audio delivery guarantee.

## Director voice timing

Director voice uses separate timing in `files/p_ai_director.c`:

- ambient line gap: **16 s**;
- hard floor between any two Director lines, including forced events: **6 s**.

The Director's `force` flag can barge past the normal ambient-idle requirement, but not the six-second floor.

## Catalogue categories

The exact tag list is the `VOICE_MAP` array in `files/i_voice.c`. The catalogue includes the current families for:

- command acknowledgements;
- contact/clear/status;
- damage/hurt and stuck/door navigation;
- revive/help/downed state;
- weapon and ammo events;
- kill lines, including rotated per-monster variants;
- Director phase, spawn, item, relax, safety and objective announcements.

Do not infer a tag from prose. Add it to `VOICE_MAP`, ensure the WAD lump is eight-byte compatible, then call it from the correct source subsystem.

## Adding a line

1. Bake the clip into the correct persona namespace (`DS*` or `DD*`) with a valid eight-byte lump name.
2. Add the `{ "tag", "LUMP" }` row to `VOICE_MAP` in `files/i_voice.c`.
3. Call it from `files/p_ai_coop.c` or `files/p_ai_director.c` with the appropriate gate/force semantics.
4. Update the catalogue and, if the asset bundle changed, record the new asset snapshot separately.
5. Inspect the PWAD directory to confirm the on-disk name matches the source lookup exactly.

## Failure modes

- **No audio:** verify `aidoom_wad`, the ID0 search path and the physical lump name.
- **One line silently missing:** check `I_Voice_Busy()` and the tier/cooldown gate before blaming the WAD.
- **Buddy seems to talk over itself:** verify that the observation is not actually Director audio; they use different streams.
- **Seven/eight character mismatch:** inspect the WAD directory bytes; embedded-NUL padding matters to `W_CheckNumForName`.

## Source map

- Voice stream/load/map: `files/i_voice.c`, `files/i_voice.h`.
- Buddy call sites/gate: `files/p_ai_coop.c`.
- Director call sites/timing: `files/p_ai_director.c`.
- HUD/status integration: `files/hu_buddy.c`.
