# Buddy-AI: Verhalten & Portierung

Vollständige Beschreibung, wie der aiDoom-Co-op-Buddy aktuell funktioniert
(`files/p_ai_coop.c`), plus Portierungs-Hinweise. Stand: nach dem Revert auf das
„weniger-Kamikaze-wenn-verletzt"-Verhalten (+ Tür-Liste, Fass-Fix).

## Was ist der Buddy?

Ein **echter Spieler 2** (`players[1]`): eigene Waffen, Schaden, Pickups (außer
Schlüssel), eigenes „Down/Revive" statt Game-Over. Gesteuert von einem
**deterministischen, regelbasierten Bot**, der **jeden Tic** (35 Hz) `players[1].cmd`
baut — `P_AICoop_BuildCmd()`. Deterministisch (nur Geometrie/`P_AproxDistance`, kein
`P_Random` im State-Flow) → demo-/netz-sicher.

- Aktiviert mit **`-aicoop`** (der Bot). `-coop` = stiller Co-op-Slot ohne Bot.
- Ein **LLM-Director** kann optional High-Level-Taktiken **oben drauf** setzen
  (`-aidirector` + `tools/director.c`); ohne ihn läuft der Bot autonom.

### Lebenszyklus (in `P_SetupLevel`)
- `P_AICoop_ResetSlot()` **vor** `P_LoadThings`: nullt den stale `players[1].mo`-Pointer
  und den P2-Start-Sentinel (sonst false-positive „Buddy gespawnt" aus der Vormap).
- `P_AICoop_VerifySpawn()` **nach** `P_LoadThings`: hat die Map kein `Player_2_Start`,
  → WARNING + Buddy für die Session aus.

---

## Entscheidungsbaum (`BuildCmd`, Priorität oben → unten)

Wird einmal pro Tic durchlaufen; der erste passende Zweig setzt `coop_state`, das
Bewegungsziel `(tx,ty)`, `movethresh` (Stopp-Distanz) und ob gefeuert wird.

| # | Zweig | Bedingung | Verhalten |
|--:|-------|-----------|-----------|
| 0 | **Down** | HP ≤ 0 (incapacitated) | Bleibt liegen, ruft `help:`, **kein** Reborn; wartet auf Revive per USE des Menschen. |
| 1 | **`goto`** | LLM `BUD_GOTO` (`ai_goto>0`) | Navigiert zum Punkt, ignoriert Kämpfe/Items. |
| 2 | **`wait`/`stay`** | `user_hold` (Mensch) oder `hold` (Director `BUD_HOLD`) | Hält Position; feuert aber auf ein Monster in Reichweite. |
| 3 | **`come`** | `summon>0` (7 s, `COOP_SUMMON_TICS`) | Läuft zum Spieler (Pathfinder), ignoriert Kämpfe/Items. |
| 4 | **Come-Leash** | come-stay aktiv UND (>`COOP_LEASH` 640u weg ODER keine LoS zum Spieler) | Zurück zum Spieler — überschreibt Kampf/Item. |
| 5 | **Heal** | HP < `COOP_HEAL_HP` (50) UND Medkit < `COOP_HEAL_RANGE` (1024u) | Bricht ab, holt das nächste Medkit. |
| 6 | **Fight** | Sichtbares Monster `tgt` | Anvisieren + feuern. **Wenn `stayclose`** (HP<50 oder come-leash): nur Bedrohungen < `COOP_ENGAGE_NEAR` (448u) vom Buddy oder < `COOP_LEASH` (640u) vom Spieler, und mehr Abstand (`COOP_KEEP*2`=384u statt 192u). |
| 7 | **Grab** | Idle UND Spieler < `COOP_GRAB_NEAR` (512u) UND Item < `COOP_ITEM_RANGE` (128u) UND nicht „grab-stuck" | Sammelt das Item ein. |
| 8 | **Follow** | Default (Spieler existiert) | Folgt dem Spieler auf `COOP_NEAR` (256u; verletzt halb so weit), `avoiddamage=1`. |

`stayclose` = `(HP < COOP_CAUTION_HP 50) || come-stay`. Das ist der „weniger
Kamikaze"-Schalter: Buddy bleibt näher am Spieler und hält im Kampf mehr Abstand.

---

## Zielerfassung (`AICoop_FindTarget`)

Nächstes Monster, das **alle** erfüllt:
- `MF_COUNTKILL` + `MF_SHOOTABLE`, lebt, kein `MF_CORPSE`;
- in `COOP_SIGHT` (1280u);
- **direkte Sichtlinie** (`P_CheckSight`);
- **nicht geblacklistet** (s. u.).

Ein **Schaden-Watchdog** merkt sich die HP des Ziels: feuert der Buddy ~2 s ohne dass
die HP fallen (Schüsse verfehlen / unerreichbar), wird das Ziel für `COOP_BL_TICS`
(5 s) **geblacklistet** (max `COOP_BL_MAX`=8) → er wechselt das Ziel statt einzufrieren.

---

## Feuer-Logik

Im Fight-Zweig faltet sich pro Tic ab:
1. **Reaktionszeit** (`-buddyreact`): kurze Verzögerung bei *frisch* gesichtetem Ziel
   (nicht frame-perfekt).
2. **Vertikal-Aim**: berechnet die Steigung zum Ziel-Zentrum → `bot->lookdir`
   (geklammert auf `COOP_LOOKMAX` 56), damit Schüsse zu hohen/tiefen Gegnern elevieren
   statt in die Wand/Kiste davor.
3. **Klar-Schuss-Probe**: `P_AimLineAttack` entlang der Peilung. Auswertung:
   - **Friendly-Fire-Schutz**: `linetarget` ist ein **Spieler** (Mensch in der Linie)
     → **nicht** feuern, seitwärts straferen bis der Winkel frei ist.
   - **Feuer**, wenn `linetarget` ein Monster ist **und** er es anvisiert
     (`|rest-Drehung| < COOP_FACING` ~7°) **und** Reaktionszeit abgelaufen **und** kein
     Splash-Risiko **und** kein Fass im Weg.
   - **Fass-Schutz** (`AICoop_BarrelNear`): hält nur, wenn ein Fass *tatsächlich
     getroffen* werden könnte — in Schussrichtung (Vorwärts-Bogen) **und** in Sicht.
     Fass hinter einer Wand zählt nicht (sonst feuert er nie).
   - **Splash-Schutz**: Rakete/BFG auf < `COOP_BLAST_SAFE` (176u) → wechselt auf eine
     Nicht-Splash-Waffe (`AICoop_BestRanged`) und hält bis der Wechsel durch ist.
   - **Kein Lock + nah (<768u)**: zurückweichen, um den Winkel zu öffnen (Facing bleibt
     auf dem Ziel → feuert sobald frei).
4. **Melee-Hochziehen**: steht der Buddy mit Faust/Kettensäge da, aber das Ziel ist
   außer Reichweite, schaltet er auf eine Fernwaffe.
5. **Raketen-Ausweichen** (`COOP_DODGE_RANGE` 256u): seitliches Dodging einkommender
   Projektile.

---

## Bewegung & Navigation

### Pathfinder (`PF_*`, BSP-Sub-Sektor-Graph, `Pathfinding.md`)
- Graph aus Sub-Sektor-Zentroiden, Kanten über begehbare zweiseitige Linien
  (Kantengewicht = Distanz + Strafen: geschlossene Tür `PF_DOOR_PEN`, Schaden-Boden).
- **`PF_AStar`** (A* mit Distanz-Heuristik) findet die Route; `PF_NextWaypoint(mo,gx,gy)`
  liefert den **nächsten** Wegpunkt → der Buddy rundet Ecken statt an der Wand zu
  schaben. (Dieselbe API nutzt der Monster-Director, `P_AICoop_NextWaypoint`.)
- **Ecken-Rundung** (`AICoop_ChaseDir`, Doom-Monster-Stil): probiert die 8
  Kompass-Richtungen (No-Move-Reachability-Probe) und bleibt bei einer Richtung, statt
  an konkaven Ecken zu zappeln.

### Breadcrumb-Trail (`crumbx/crumby`)
Der Buddy zeichnet die jüngsten begehbaren Positionen des Menschen auf (alle ~48u,
`CRUMB_GAP`). Kommt er beim Folgen nicht durch (Pathfinder findet keine Route — z. B.
Teleporter/Secret, die der Graph nicht modelliert), **spielt er die Spur zurück**:
steuert gerade auf die *neueste direkt erreichbare* Krume zu — jeder Schritt ist ein
verifiziert begehbarer Sprung Richtung Spieler.

### Türen
- `AICoop_FindDoorAhead` lenkt ihn auf die **Mitte** einer geschlossenen DR-Tür auf der
  Route (sonst zielt die BSP-Wegpunkt-Logik an der Wand neben der Öffnung vorbei).
- Beim Verkeilen (`triedmove && stuck`) tippt er **USE** — aber nur, wenn `AICoop_DoorInFront`
  eine **echte** öffenbare Tür (Spezial 1/31/117/118) im 96u-Vorwärts-Bogen meldet
  (kein USE-Spam an Wänden). 45-Tic-Cooldown, damit keine DR-Tür mitten im Hub umkehrt.

### Gefahren & Kanten
- **Schaden-Boden** (`AICoop_DamagingFloor`): steht er in Nukage/Lava → raus zum
  nächsten Menschen; Folge-Bewegungen meiden Schaden-Boden voraus.
- **Kanten** (`AICoop_FallAhead`): kriecht nahe Abgründen/Pits, um nicht runterzurutschen.
- **Springen** (`AICoop_JumpableStep`, nicht im Netgame): niedrige Stufen, die er nicht
  hochsteigen kann, überspringt er per `BT_JUMP`.
- **Safe-Route** (`pf_safemode`): unter `COOP_SAFE_HP` (40) gewichtet `PF_AStar` Kanten
  zusätzlich nach einer Gefahren-Heatmap (`P_AICoop_NoteDamage` füttert sie) → der Buddy
  nimmt den ruhigeren Weg nach Hause.

---

## Down & Revive (L4D-artig)

- Bei HP ≤ 0 stirbt der Buddy **nicht** (kein Game-Over): er geht „down", bleibt liegen
  (kein USE → kein Reborn), ruft `help:`.
- Der Mensch revived ihn, indem er **nah** (`COOP_REVIVE_RANGE` 96u) **steht und USE
  drückt** — `P_AICoop_RevivePress` überträgt 10 HP vom Menschen (geht nicht, wenn der
  Mensch ≤ 10 HP hätte) und stellt den Buddy in-place wieder auf.

---

## Director-Direktiven (LLM-Schicht, optional)

`P_AICoop_SetDirective(tactic, focus, x, y, tics)` setzt eine **zeitlich begrenzte**
High-Level-Taktik, die den Bot überschreibt; läuft sie ab, fällt er auf das
Regelverhalten zurück.

| Taktik | Wirkung |
|--------|---------|
| `BUD_ENGAGE` | Bestimmtes Monster (oder nächstes) bekämpfen |
| `BUD_DEFEND` | Nah am Spieler bleiben, nur nahe Bedrohungen |
| `BUD_HOLD` | Position halten (`hold`) |
| `BUD_REGROUP`/`BUD_RETREAT` | Zum Spieler zurück |
| `BUD_GOTO` | Zu Punkt (x,y) (`ai_goto`) |
| `BUD_GRAB` | Items sammeln |

---

## Konsole & Tasten (`c_console.c`)

| Befehl | Funktion |
|--------|----------|
| `come` | zum Spieler laufen (`summon`) |
| `wait`/`stay` | Position halten (Toggle `user_hold`) |
| `attack` | nächstes Monster für `COOP_ATTACK_TICS` (10 s) anstürmen |
| `where` | Distanz/Richtung/HP/Tätigkeit melden |
| `report` | HP/Rüstung/Waffe/Munition |
| `buddygod` | Buddy-Gott-Modus |
| `buddyarm` | alle Waffen + Munition + Rüstung |
| `buddyhome` | Buddy zum Map-Spawn teleportieren |

Tasten-Binds (`m_misc.c`): `key_buddy_come` (`,`), `key_buddy_attack` (`.`),
`key_buddy_stay` (`-`) — rebindbar.

---

## Stimme & HUD

- **Voice** (`i_voice.c`, vorgebackene Clips in `run/aidoom.wad`, positional): getaggte
  Zeilen (`contact/clear/hurt/stuck/door/revive/help/kill/…`), 4-Tier-Prioritäts-System,
  rate-limitiert; höheres Tier preemptet niedrigeres. `stuck:` ist **flanken-getriggert**
  (einmal pro Klemm-Episode) und feuert **nur beim Navigieren** (nicht im Kampf-Jockeying).
  Voller Katalog: **`BUDDY_VOICE.md`**.
- **HUD** (`hu_buddy.c`): obere Leiste mittig — HP (eingefärbt), Rüstung, Waffe,
  Munition, Distanz, State; bei „down" ein 8-Wege-Kompass im Gesichts-Slot. Config
  `show_buddy_hud` (Default 1). Details: **`BUDDY_HUD.md`**.

---

## Konstanten (`#define COOP_*` in `p_ai_coop.c`)

| Konstante | Wert | Bedeutung |
|-----------|------|-----------|
| `COOP_SIGHT` | 1280u | Monster-Erfassungs-Reichweite |
| `COOP_NEAR` | 256u | Folge-Distanz zum Menschen |
| `COOP_KEEP` | 192u | Kampf-Annäherung (×2 wenn verletzt) |
| `COOP_LEASH` | 640u | come-stay / cautious: max. Abstand |
| `COOP_ENGAGE_NEAR` | 448u | im stayclose nur so-nahe Bedrohungen |
| `COOP_CAUTION_HP` | 50 | darunter „spielt sicher" |
| `COOP_HEAL_HP` / `_RANGE` | 50 / 1024u | Medkit suchen |
| `COOP_SAFE_HP` | 40 | darunter Safe-Route |
| `COOP_REVIVE_RANGE` | 96u | Revive-Reichweite (10 HP) |
| `COOP_BLAST_SAFE` | 176u | Splash-Sicherheit Rakete/BFG |
| `COOP_DODGE_RANGE` | 256u | Projektil-Ausweichen |
| `COOP_FACING` / `COOP_TURN` | 1500 / 1300 BAM | Feuer-Winkel / max. Drehung/Tic |
| `COOP_SUMMON_TICS` / `COOP_ATTACK_TICS` | 7 s / 10 s | `come` / `attack` Dauer |

---

## Determinismus

Geometrie-basiert, kein `P_Random` im State-Flow → gleicher Snapshot = gleiche Aktion
(demo-/netz-sicher; der LLM-Pfad ist nicht-deterministisch und liegt außerhalb des
Tic-Locks). Aktuell Single-Player.

---

## Portierung auf einen anderen DOOM-Source-Port

1. `p_ai_coop.c` + `p_ai_coop.h` kopieren (zieht `i_voice.*`, `hu_buddy.*` nach,
   sowie den Pathfinder, der intern ist).
2. Hooks: `P_AICoop_ResetSlot()` **vor** und `P_AICoop_VerifySpawn()` **nach**
   `P_LoadThings` (`p_setup.c`); `P_AICoop_BuildCmd()` im `P_Ticker`; `P_AICoop_Init()`
   in `D_DoomMain`; Buddy-Befehle im Konsolen-Parser.
3. `playeringame[1]=true` muss **in `P_AICoop_Init`** gesetzt sein, bevor `P_LoadThings`
   läuft, sonst spawnt `P_SpawnPlayer` Spieler 2 nicht.
4. `playerstarts[]` ggf. `extern` exposen (in modernen Ports schon in `doomstat.h`).

**Stolperfallen**: `players[]`/`playerstarts[]` behalten Werte über Map-Loads → der
ResetSlot-Hook ist Pflicht. Das Voice-System ist DOOM-spezifisch (positional über die
SDL-Streams in `i_voice.c`) — für andere Engines neu schreiben oder weglassen.

## Verwandte Docs
`BUDDY_HUD.md` (HUD), `BUDDY_VOICE.md` (Voice-Katalog + Tiers), `Pathfinding.md`
(BSP-Pathfinder), `AGENT_CONTROL.md` (Director-Protokoll), `DIRECTOR_MODES.md`
(LLM-vs-Regel-Director), `AIDOOM_PARAMETERS.md` (CLI-Flags).
