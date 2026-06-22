# Buddy voice — line catalogue & priority system

Canonical reference for the AI co-op buddy's spoken voice lines: what the buddy
can say, when, and **which line wins when several want to play at once**.

The audio is pre-baked (ElevenLabs, Joker-HL voice) into `run/aidoom.wad` and
played offline through a dedicated SDL3 audio stream — see
`tools/bake_buddy_voice.py` (bake), `files/i_voice.c` (playback + tag→lump map),
`files/p_ai_coop.c` (when each line fires). Keep this doc in sync when lines or
the priority tiers change.

## What's in `aidoom.wad`

`run/aidoom.wad` is a PWAD with **248 lumps**:

| Kind | Count | Notes |
|------|------:|-------|
| `DS*` buddy voice clips (OGG/Vorbis) | 156 | the spoken lines below — ElevenLabs **Joker-HL** |
| `DD*` AI-Director voice clips (OGG/Vorbis) | 49 | the game-master persona — ElevenLabs **UT** (see below) |
| `BUF*` HUD face graphics | 42 | the buddy's status-bar face (see `BUDDY_HUD.md`) |
| `VOICEMAP` | 1 | text lump: lump↔phrase map (also `run/aidoom_voice_manifest.txt`; columns lump·persona·voice·phrase·src·bytes) |

All voice clips are **wired** — every `DS*`/`DD*` lump has a tag entry in
`i_voice.c`'s `VOICE_MAP`, and every map tag resolves to a real lump (verified by
joining the map against the WAD directory). So there is no dead audio and no
silent tag.

### Two personas, two streams

`bake_buddy_voice.py` bakes **both** voices in one pass (`PHRASES` = buddy/Joker via
`DEFAULT_VOICE`, `DIRECTOR` = UT via `DIRECTOR_VOICE`/`--director-voice`). At runtime
`i_voice.c` opens **two independent SDL streams** so the buddy and the Director can
speak at the same time without cutting each other off:

- **Buddy** — `DS*` lumps, `I_Voice_Say(tag)`, positional (panned from the buddy's
  spot in the world), gated by the four-tier priority system below (`p_ai_coop.c`).
- **AI Director** — `DD*` lumps, `I_Director_Say(tag)` / tags `dir:*`, non-positional
  "voice of god", driven by `p_ai_director.c::P_Director_Voice` (6 s ambient cooldown;
  `force=1` for phase changes barges in). Events: level **start**, **build**-up,
  single **spawn**, **horde**, **peak**, **big**-monster lean-in, **relax**, item
  **gift**, emergency **heal**/**ammo**, LLM tactics (**flank/ambush/focus/fallback**,
  via `P_Director_Say` from `p_ai_llm.c`), player **spree**/**down**, level **clear**
  (`G_ExitLevel`), and **idle** banter. Pressure-aware: no specials/hordes are
  announced/spawned when the player is critically low on HP/ammo.

> **Lump-name gotcha (CLAUDE.md):** WAD lump names are max 8 bytes. `summon_ok`
> and `state:following` historically used 9-char literals (`DSSUMONOK` /
> `DSWFOLLOW`) that only matched by truncation to `DSSUMONO` / `DSWFOLLO`. Those
> map literals are now tightened to the real 8-char names. If you add a line,
> keep the lump name ≤ 8 chars and check it against the on-disk WAD.

## Priority system

Lines fall into **four tiers**. A higher tier **preempts** a lower-tier line that
is still playing (the queued audio is flushed via `I_Voice_Stop`) and is
rate-limited less. This keeps low-value chatter ("I'm stuck!") from burying the
things the player actually needs to hear.

| Tier | Name | Min gap between lines | Behaviour |
|-----:|------|----------------------|-----------|
| 3 | **COMMAND** | none (`0`) | Answers to the player's orders — always plays, preempts everything below |
| 2 | **WEAPON** | 4 s | Weapon-state the player should act on ("out of ammo", "down to fists") |
| 1 | **KILL** | 4 s | Monster kill / gib / spree / "nice shot" quips |
| 0 | **AMBIENT** | 8 s | Everything else (stuck / lost / idle / contact / hurt / door / …) |

**Mechanics** (`files/p_ai_coop.c`):
- `AICoop_VoiceGate(prio)` is the single decision point. It checks
  `I_Voice_Busy()`; if the buddy is still talking, only a **strictly higher**
  tier may cut in (and it calls `I_Voice_Stop()` to barge in). Then it applies
  the per-tier minimum gap (`VP_GAP[]`) before reserving the slot.
- `AICoop_CalloutP(prefix, n, prio)` — rotated auto-callouts (`"<prefix>0".."<prefix>(n-1)"`).
  `AICoop_Callout(prefix, n)` is the back-compat wrapper at tier **AMBIENT**.
- Command acks go through `P_AICoop_VoiceTag()` → tier **COMMAND**, so the buddy
  always answers `come` / `wait` / `attack` / `where` and cuts off chatter.
- **`"stuck:"` is edge-triggered** (`if (!wasstuck) …`): it announces once per
  wedge episode, not every tic. (It used to fire every tic and starve every
  other line — the original "talks too much about being stuck" bug.)

**Audio layer** (`files/i_voice.c`): lines play by appending PCM to a dedicated
SDL3 stream (`SDL_PutAudioStreamData`). `I_Voice_Busy()` =
`SDL_GetAudioStreamQueued() > 0`; `I_Voice_Stop()` = `SDL_ClearAudioStream()`.

## Line catalogue (156 lines)

Grouped by tier; each row is a tag prefix, its rotation count, and the spoken
text. The tag→lump→text mapping lives in `i_voice.c` + the bake manifest.

### Tier 3 — COMMAND (27 lines) · *always answers, preempts*
| Tag | n | Lines |
|-----|--:|-------|
| `state:` | 6 | "Following you." / "Fighting." / "Getting health." / "Holding position." / "Coming to you." / "Grabbing an item." |
| `summon_ok` | 1 | "On my way!" *(screaming)* |
| `wait_hold` | 1 | "Holding position." |
| `wait_move` | 1 | "Moving out." |
| `attack_ok` | 1 | "Attacking!" |
| `attack_none` | 1 | "No targets around." |
| `status:` | 16 | weapon readout for the `report` command (Fists / Pistol[. Loaded.] / Shotgun / Chaingun / Rocket launcher / Plasma rifle / B.F.G. / Chainsaw / Super shotgun, each with a "Loaded." ammo variant) |

### Tier 2 — WEAPON (5 lines)
| Tag | n | Lines |
|-----|--:|-------|
| `dry:` | 3 | "I'm dry!" / "Out of ammo!" / "Need a reload!" |
| `fists:` | 2 | "Just my fists now." / "Knuckle up." |

### Tier 1 — KILL (34 lines) · *buddy got the kill; rare per-type quip*
| Tag | n | Lines |
|-----|--:|-------|
| `kill:` | 4 | "Got him!" / "Down!" / "Scratch one." / "Stay down." |
| `killimp:` | 3 | "I pimped that imp!" / "Imp? More like wimp." / "Hot-footed that imp." |
| `killzm:` | 1 | "Zombie down." |
| `killsg:` | 1 | "Thanks for the shells!" |
| `killcg:` | 1 | "Quit hoggin' the chaingun." |
| `killpk:` | 1 | "Bad dog!" |
| `killsc:` | 1 | "I see you, fuzzy." |
| `killsl:` | 1 | "Lost soul, meet floor." |
| `killcd:` | 1 | "Eat dirt, meatball!" |
| `killpe:` | 1 | "No more skull spam." |
| `killhk:` | 1 | "Knight, night!" |
| `killbn:` | 1 | "Baron? Barely." |
| `killrv:` | 1 | "Rest in pieces, bonehead!" |
| `killmc:` | 1 | "Lay off the donuts." |
| `killar:` | 1 | "Squashed that bug." |
| `killmm:` | 1 | "Big spider, big splat." |
| `killcy:` | 1 | "Tim-ber!" |
| `killav:` | 1 | "Stay dead this time!" |
| `killns:` | 1 | "Wrong game, pal." |
| `killkn:` | 1 | "Door's open!" |
| `gib:` | 3 | "Chunky." / "Boom!" / "Cleanup on aisle hell." |
| `spree:` | 4 | "On a roll!" / "Can't stop me!" / "They keep coming!" / "Damn, I'm good." |
| `nice:` | 2 | "Nice shot!" / "Good kill." *(the human scored near the buddy)* |

Trigger gates (`P_AICoop_NoteKill`): only when `killer == buddy` and the victim
is `MF_COUNTKILL`; overkill → `gib:` (~96/256), else a per-type quip (~64/256);
4 kills in 5 s → `spree:`. The tier-1 4-second gap then throttles repeats.

### Tier 0 — AMBIENT (90 lines) · *lowest; yields to everything above, 8 s gap*
| Tag | n | Lines |
|-----|--:|-------|
| `contact:` | 4 | "Contact!" / "Tango, engaging!" / "I see one!" / "Got movement!" |
| `hurt:` | 3 | "I'm hit!" / "Taking fire!" / "I need health!" |
| `clear:` | 3 | "Area clear." / "All quiet." / "Watch our six." |
| `dodge:` | 3 | "Incoming!" / "Whoa!" / "Not today." |
| `barrel:` | 3 | "Not near that barrel!" / "Barrel -- hold up." / "That thing'll blow." |
| `crit:` | 3 | "I'm dying here!" / "Critical -- cover me!" / "Patch me up, now!" |
| `taunt:` | 4 | "Come get some!" / "Is that all?" / "I do this for fun." / "Rest in pieces!" |
| `bigmon:` | 3 | "Big one!" / "Oh, that's a Cyberdemon..." / "We're gonna need more ammo." |
| `flank:` | 3 | "Behind you!" / "They're flanking!" / "On your six!" |
| `infight:` | 2 | "Let 'em fight." / "They're killing each other!" |
| `edge:` | 3 | "Careful -- nukage." / "Watch the edge." / "Easy, long drop." |
| `jump:` | 2 | "Hup!" / "Up we go." |
| `door:` | 2 | "Door!" / "Opening up." |
| `stuck:` | 3 | "I'm stuck!" / "Gimme a sec." / "Snagged on something." *(edge-triggered)* |
| `lost:` | 3 | "Where'd you go?" / "Wait up!" / "I lost you!" |
| `locked:` | 2 | "Locked. Need a key." / "Can't open this one." |
| `crush:` | 2 | "Crusher!" / "Don't get squished." |
| `plhurt:` | 3 | "You okay?!" / "I got you covered!" / "Fall back, I'll hold!" |
| `pldown:` | 2 | "No! ...I'll avenge you." / "Stay down, I got this." |
| `ff:` | 6 | "Hey! Watch it!" / "Friendly fire!" / "That's MY blood, pal." / "Friendly fire, jackass!" / "I'm on YOUR side, genius!" / "Shoot them, not me!" |
| `pickup:` | 3 | "Nice, ammo!" / "Health -- sweet." / "Don't mind if I do." |
| `healed:` | 2 | "Patched up." / "Better. Let's go." |
| `berserk:` | 2 | "Berserk! Get over here!" / "Now I'm untouchable!" |
| `god:` | 2 | "Invincible!" / "Can't touch me." |
| `arm:` | 2 | "Loaded for bear!" / "Now we're talking." |
| `lvlstart:` | 3 | "Fresh hell." / "Here we go again." / "Let's clear it." |
| `lvlclear:` | 3 | "All dead. Nice." / "Find the exit." / "Hail to the king." |
| `secret:` | 2 | "Secret!" / "Ooh, hidden stash." |
| `idle:` | 4 | "Quiet... too quiet." / "Anything?" / "Still with me?" / "I hate the waiting." |
| `help:` | 5 | "Man down! Help!" / "I'm hit bad -- get over here!" / "Don't leave me!" / "Buddy down! Medic!" / "Help me up, damn it!" |
| `revived:` | 3 | "Back in the fight!" / "Thanks -- I owe you one." / "Let's finish this." |

> **Note on the AMBIENT bucket:** by the chosen tiering, the urgent-sounding
> `crit:`, `pldown:` and `help:` (incapacitation scream) currently sit at the
> lowest tier with the rest. If those should out-rank kill quips, promote them
> in `P_AICoop_NoteKill`/`BuildCmd`'s call sites — it's a one-arg change per
> call (`AICoop_Callout(...)` → `AICoop_CalloutP(..., VP_…)`).

## Adding a new line

1. Add the phrase to `tools/bake_buddy_voice.py` (`PHRASES`/`EVENTS`), lump name
   ≤ 8 chars, and re-bake `run/aidoom.wad`.
2. Add the `{ "tag", "LUMP" }` row to `VOICE_MAP` in `files/i_voice.c`.
3. Call it from `files/p_ai_coop.c` at the right tier:
   `AICoop_CalloutP("mytag:", n, VP_…)` (rotated) or `AICoop_SayTagP("mytag", VP_…)`.
4. Update this catalogue.
