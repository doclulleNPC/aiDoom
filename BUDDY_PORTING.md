# Buddy-AI Verhalten & Portierungs-Anleitung

Zusammenfassung wie der aidoom-Co-op-Buddy aktuell funktioniert und wie man
ihn auf andere Projekte portiert.

## Was ist der Buddy?

Ein deterministischer, regelbasierter KI-Begleiter der in DOOM den Spieler
als zweite Spielfigur (`players[1]`) begleitet. Aktiviert via `-coop` Flag
(regelbasiert) oder `-aicoop` Flag (Stub für zukünftige LLM-Schicht).

## Aktuelles Verhalten — Top-Down

### 1. Spawn (Datei: `files/p_ai_coop.c`)

`P_AICoop_Init()` (Z. 71):
- Liest `-coop` und `-aicoop` Flags
- **Mutual-Exclusive**: beide zusammen → Fehlermeldung + Buddy aus
- Single-Player only (`netgame` blockt)
- Setzt `playeringame[1] = true` → `P_SpawnPlayer` in `P_LoadThings` spawnt Player 2 an `Player_2_Start`-Position

`P_AICoop_VerifySpawn()` (Z. 130):
- Läuft **nach** `P_LoadThings` in `P_SetupLevel`
- Checkt ob `players[coop_slot].mo != NULL` UND `playerstarts[coop_slot].type == 2`
- Wenn fehlt → WARNING + `companion_active = 0` für die Session

`P_AICoop_ResetSlot()` (Z. 117):
- Läuft **vor** `P_LoadThings` in `P_SetupLevel`
- Nullt `players[coop_slot].mo` (clear stale dangling pointer)
- Resettet `playerstarts[coop_slot].type = 0` (sentinel: "kein P2-Start in dieser Map")
- Resettet auch `x, y, angle, options` zu 0

### 2. Tick-Loop (jeden 35-tic = einmal pro Sekunde × 35)

`P_AICoop_BuildCmd()` in `p_ai_coop.c`:
1. **Sicht-Check**: Welche Monster sind in `COOP_SIGHT` (1280u = ~78m)?
2. **State-Machine** entscheidet aktuellen Modus:

| State | Code | Trigger | Verhalten |
|-------|------|---------|-----------|
| `FOLLOW` | 0 | Default | Folge Spieler 1, halte `COOP_NEAR` (256u) Distanz |
| `FIGHT` | 1 | Monster in Sicht | Greife nearest an, halte `COOP_KEEP` (192u) Combat-Distanz |
| `HEAL` | 2 | HP < `COOP_HEAL_HP` (50) | Suche nearest Med-Pack in `COOP_HEAL_RANGE` (1024u) |
| `HOLD` | 3 | User `wait`/`stay` cmd | Bleib an Position, defensiv |
| `COME` | 4 | User `come` cmd | Renne zu Spieler 1 für `COOP_SUMMON_TICS` (7s) |
| `GRAB` | 5 | Idle + Item in `COOP_GRAB_NEAR` (512u) | Pickup nearest Item |

3. **Pathfinding** (Z. 810-1000, `pf.c`-artige Logik):
   - BSP-Sub-Sector-Dijkstra (max 8000 Nodes, max 32 Edges pro Node)
   - A* wäre besser aber Dijkstra ist was yapb's `navigate.cpp` MIT-lizensiert
     als Alternative bietet
4. **Ticcmd-Generierung**: `forwardmove`/`sidemove`/`angleturn` basierend auf
   State + Path-Current-Waypoint

### 3. Konsole-Commands (Datei: `files/c_console.c`)

User-zu-Buddy-Kommunikation:

| Command | Funktion | Verhalten |
|---------|-----------|-----------|
| `buddy_come` / `come` | `P_AICoop_Summon()` | Setzt `summon = COOP_SUMMON_TICS` → State=COME |
| `buddy_wait` / `stay` / `wait` | `P_AICoop_Wait()` | Toggle `hold` Flag → State=HOLD |
| `buddy_attack` / `attack` | `P_AICoop_Attack()` | Setzt `forceaggro = COOP_ATTACK_TICS` (10s) + `forcetarget` → State=FIGHT |
| `buddy_report` / `report` | `P_AICoop_StatusReport()` | Print HP/Armor/Weapon/Ammo |
| `buddy_where` / `where` | `P_AICoop_Report()` | Print Position, Distance, HP, Direction |

### 4. Voice-Ausgabe (Datei: `files/i_voice.c`, `files/p_ai_coop.c`)

156 vorgebackene Sprachzeilen (OGG/Vorbis in `run/aidoom.wad`, Joker-HL-Stimme),
abgespielt über einen eigenen SDL3-Audio-Stream. `P_AICoop_VoiceTag(tag)` /
`AICoop_Callout(prefix, n)` lösen einen Tag (z.B. `state:fighting`, `kill:0`)
über `VOICE_MAP` in `i_voice.c` zum 8-Zeichen-Lumpnamen auf.

**Prioritäts-System** (gegen "Buddy redet zu viel", v.a. über *stuck*): Zeilen
liegen in 4 Tiers; ein höheres Tier **unterbricht** ein laufendes niedrigeres
(`I_Voice_Stop()` flusht den Stream) und wird seltener rate-limitiert:

| Tier | Was | Min-Abstand |
|-----:|-----|-------------|
| 3 COMMAND | Antworten auf `come`/`wait`/`attack`/`where` — immer, preemptet alles | 0 |
| 2 WEAPON  | "out of ammo" / "down to fists" | 4 s |
| 1 KILL    | Monster-Kill / Gib / Spree / "nice shot" | 4 s |
| 0 AMBIENT | alles andere (stuck/lost/idle/contact/hurt/door/…) | 8 s |

Kernstück: `AICoop_VoiceGate(prio)` (entscheidet + reserviert den Slot),
`AICoop_CalloutP(prefix, n, prio)`, `I_Voice_Busy()`/`I_Voice_Stop()`.
`"stuck:"` ist **edge-getriggert** (einmal pro Klemm-Episode, nicht pro Tic).
**Vollständiger Zeilen-Katalog + Tier-Zuordnung: `BUDDY_VOICE.md`.**

Spatial Audio: 3D-positioniert via `AICoop_VoicePan()` (links/rechts-Panning
basierend auf Spieler-zu-Buddy-Winkel, Distance-Attenuation).

### 5. HUD-Overlay (Datei: `files/hu_buddy.c`)

Top-Bar mit Buddy-Stats in der Mitte (BASE-Koords X=80..240, Y=0..15):
- HP-Armor-Waffe-Ammo-Distanz-State in einer Zeile
- Patch-basiert (STBAR/STTNUM/STTPRCNT) + TTF für Labels
- Config-Toggle `show_buddy_hud` in `m_misc.c` (Default 1)

## Determinismus-Eigenschaften

Der Buddy ist **deterministisch reproduzierbar**:
- Gleicher Seed (levelstarttic) → gleiche Buddy-Aktionen über die Zeit
- A*-frei (Dijkstra mit fester Distanzberechnung)
- Kein Random (P_Random() wird im Buddy-State-Machine nicht aufgerufen)

**Implikation für Netplay**: Buddy kann theoretisch über mehrere Knoten
repliziert werden (gleicher Snapshot → gleiche Aktion). Aktuell
single-player-only.

## Datenstrukturen

```c
// p_ai_coop.c
static int  coop_state;        // 0-5, current FSM state
static int  summon;             // "come" timer (in tics)
static int  hold;               // "wait" toggle
static int  forceaggro;          // "attack" timer
static mobj_t* forcetarget;     // "attack" target
static int  companion_active;   // 1 if buddy is in this game
static int  aicoop_layer;      // 1 if -aicoop flag was given

#define COOP_SIGHT    (1280*FRACUNIT)   // 1280 u = ~78m view range
#define COOP_TURN     1300               // max angleturn per tic (~7 deg)
#define COOP_FACING   1500               // open fire if |remaining turn| < this
#define COOP_LOOKMAX  56                 // vertical aim clamp
#define COOP_NEAR     (256*FRACUNIT)     // follow distance target
#define YIELD_DIST    (48*FRACUNIT)      // step-aside if human this close
#define COOP_KEEP     (192*FRACUNIT)     // advance-toward-monster-until-this-close
#define COOP_HEAL_HP  50                 // seek med-pack below this HP
#define COOP_HEAL_RANGE (1024*FRACUNIT)  // max heal-search distance
#define COOP_ITEM_RANGE (128*FRACUNIT)   // idle pickup search radius
#define COOP_GRAB_NEAR (512*FRACUNIT)    // grab only when still near human
#define COOP_SUMMON_TICS (7*TICRATE)     // 7s "come" duration
#define COOP_ATTACK_TICS (10*TICRATE)    // 10s "attack" duration
```

## Konfigurations-Tunables

In `files/m_misc.c` `defaults[]`:
```c
{"show_buddy_hud", &show_buddy_hud, 1},   // 0 = off, 1 = on
// (keine andere buddy-relevante config aktuell)
```

Die meisten Tunables sind als `#define COOP_*` hardcoded. Eine Auslagerung
in die Config wäre ein naheliegender erster Refactor (low risk, hoher UX-Wert).

## Wie portiert man das auf ein anderes Projekt?

### Szenario 1: Auf einen anderen DOOM-Source-Port (z.B. Crispy Doom, Doom Retro)

**Schritte:**
1. `p_ai_coop.c` (~1500 Z.) + `p_ai_coop.h` (~50 Z.) kopieren
2. In `p_setup.c` `P_SetupLevel`:
   - ResetHook (`P_AICoop_ResetSlot`) vor `P_LoadThings` einbauen
   - VerifyHook (`P_AICoop_VerifySpawn`) nach `P_AI_Reset` einbauen
3. In `p_mobj.c` `P_SpawnPlayer`: keine Änderung (Player-Spawn-Mechanik ist portabel)
4. In `g_game.c` `G_InitNew` Z. 1521: `players[i].playerstate = PST_REBORN` ist portabel
5. In `p_tick.c` `P_Ticker`: `P_AICoop_BuildCmd()` aufrufen
6. In `d_main.c` `D_DoomMain`: `P_AICoop_Init()` + Konsolen-Command-Parser einbauen

**Portabilitäts-Risiken:**
- `playeringame[]` ist überall gleich
- `playerstarts[]` ist static global in `p_setup.c` → **muss extern exposed werden**
  (in `doomstat.h` ist es schon extern deklariert in modernen Source-Ports)
- `MOBJ`-API ist vanilla DOOM

**Aufwand: ~2 Tage**, größte Hürde ist die Hook-Integration im Build-System
(CMake-glob, Makefile).

### Szenario 2: Auf eine andere Engine (z.B. GZDoom in ZScript, QuakeC für FTEQW)

**Vollständige Neuentwicklung** — die FSM-Logik ist portierbar, die
DOOM-spezifischen Calls (mobj, sector, ticcmd) sind es nicht.

**Was zu übernehmen ist (Konzept-Ebene):**
- State-Machine mit 6 States
- Distanz-basierte Transitions
- Pathfinding-Wrapper (egal welche Engine)
- Voice-System (enginespezifisch: GZDoom hat SNDINFO, QuakeC hat sound())

**Aufwand: ~3-4 Wochen** für sauberen Port auf z.B. QuakeC. Das meiste ist
Wrapper-Code für die jeweilige Engine-API.

### Szenario 3: Auf eine Custom-Game-Engine (z.B. dein eigenes Spiel)

**Wenig ratsam** — der Buddy ist explizit auf DOOM zugeschnitten:
- BSP-Linedef-Sector-System (für Pathfinding)
- DoomScript/thinker-Architektur
- Powerup-Mobj-System

Besser wäre: in einer Custom-Engine den Buddy von Grund auf neu zu
implementieren, aber die **FSM-Logik** (6 States, Distanz-Threshold,
Pathfinding-Anbindung) 1:1 zu übernehmen.

**Aufwand: ~1 Monat** (Engine-Schnittstellen + FSM-Implementation).

## Was man bei der Portierung leicht falsch macht

1. **`playerstarts[]` ist static** in `p_setup.c` — vergisst man den
   `extern` in der neuen Engine, gibt's Linker-Errors.
2. **`players[]` retain Werte zwischen Map-Loads** — der `P_AICoop_ResetSlot`
   vor `P_LoadThings` ist nicht optional, sonst zeigt der Verify-Hook
   false-positive "Buddy spawned" wegen dangling pointer aus voriger Map.
3. **`playeringame[]` muss in `P_AICoop_Init` gesetzt werden** (auf `true`),
   nicht erst in `P_SetupLevel` — sonst skippt `P_SpawnPlayer` weil der
   Flag noch nicht gesetzt ist wenn `P_LoadThings` läuft.
4. **`P_SpawnMapThing` hat einen `break`-statt-`continue`-Bug** für
   non-commercial cool-monsters (siehe `p_setup.c` Z. 337-338). Wenn deine
   Port-Engine commercial-Mode hat, irrelevant; wenn nicht, fix mit.
5. **Voice-System (`i_voice.c`)** ist sehr DOOM-spezifisch (Spatial-Audio
   mit `S_AdjustSoundParams`). Für anderen Port entweder eigenes
   Voice-System schreiben oder Buddy-Voice weglassen.

## Test-Checkliste für Portierung

Nach dem Port folgende Szenarien manuell testen:

- [ ] `./aidoom -coop` startet vanilla DOOM1 E1M1, Buddy ist bei Player 1
- [ ] `./aidoom -aicoop` (alleine) startet Buddy mit Stub-Warning
- [ ] `./aidoom -coop -aicoop` zeigt Mutual-Exclusive-Warning, kein Buddy
- [ ] Buddy folgt Spieler, greift Monster in Sicht an
- [ ] Buddy bleibt stehen bei `hold` Konsolen-Cmd
- [ ] Buddy kommt zu Spieler bei `come` Konsolen-Cmd
- [ ] Custom-WAD ohne `Player_2_Start` zeigt `P_AICoop_VerifySpawn`-WARNING
- [ ] Buddy-Pathfinding funktioniert über BSP-Sub-Sector-Edges
- [ ] HUD-Overlay zeigt Stats korrekt (HP, Armor, State, Distanz)
- [ ] Voice-Tags werden richtig gemappt (`state:fighting` etc.)
- [ ] Bei Buddy-Tod: WARNING + `companion_active = 0` bis Quit

## Lizenz-Hinweis

Der gesamte Buddy-Code ist im Repo-Lizenz-Header (DOOM Source License) und
damit portabel mit Copyright-Vermerk. Keine zusätzlichen Restriktionen
wie bei yapb (MIT) oder ReGameDLL (GPL).

## Verwandte Docs im Repo

- `AI_IMPROVEMENTS.md` — Plan für AI-Layer-Erweiterung (Punkt 1: LLM-Hook)
- `VISIBILITY_CACHE.md` — Geplanter Performance-Optimization
- `YAPB_ARCHITECTURE.md` — yapb-Code-Referenz für Inspiration
- `AIDOOM_PARAMETERS.md` — Vollständige CLI-Flag-Liste
- `BUDDY_HUD.md` — HUD-Implementation-Details
- `BUDDY_VOICE.md` — Voice-Zeilen-Katalog (156 Zeilen) + Prioritäts-System
- `AGENT_CONTROL.md` §1-13 — Director-Protokoll (LLM-Layer-Plan)
- `PATHFINDING.md` — BSP-Sub-Sector-Dijkstra-Algorithmus

## Code-Dateien die du für die Portierung brauchst

Alle in `files/`:
- `p_ai_coop.c` (~1500 Z.) — Kern (State-Machine, Pathfinding, BuildCmd)
- `p_ai_coop.h` (~50 Z.) — Public API
- `hu_buddy.c` (~470 Z.) — HUD-Overlay
- `hu_buddy.h` (~40 Z.) — HUD API
- `i_voice.c` (~500 Z.) — Voice-System
- `i_voice.h` — Voice API
- `c_console.c` — Konsolen-Command-Parser (Buddy-Cmds hinzufügen)

Modifikationen in existierenden Files:
- `p_setup.c` — ResetSlot/VerifySpawn Hooks hinzufügen
- `p_tick.c` — BuildCmd im Ticker aufrufen
- `d_main.c` — AICoop_Init im D_DoomMain aufrufen
- `m_misc.c` — `show_buddy_hud` config-key
- `run/start_buddy.sh` + `.bat` — Launcher (Beispiel für End-User-Aufruf)