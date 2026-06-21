# yapb Architecture für aidoom — Was wir lernen können

yapb ist ein Counter-Strike 1.6 Bot (Half-Life Engine) mit ~35.000 Zeilen
C++17, MIT-lizenziert. Quelle: `~/Source/cs16-client/3rdparty/yapb/`.
Diese MD erklärt yapb's Kernarchitektur und was davon für aidoom's Buddy
nutzbar ist.

## yapb's Layer-Architektur

```
+--------------------------------------------------+
|  hooks.cpp + linkage.cpp                         |  <- Game-Integration (HL-SDK)
+--------------------------------------------------+
                       v
+--------------------------------------------------+
|  botlib.cpp                                      |  <- Bot-Lifecycle (think, movement)
|   + combat.cpp (aim, dodge)                      |
|   + tasks.cpp (goal selection)                    |
|   + planner.cpp (multi-step planning)            |
|   + practice.cpp (learning)                      |
|   + vision.cpp (frustum, FOV checks)             |
|   + vistable.cpp (waypoint-graph vis cache)      |
+--------------------------------------------------+
                       v
+--------------------------------------------------+
|  navigate.cpp (A* pathfinding)                   |  <- Spatial
|  graph.cpp (waypoint graph, auto-generation)     |
|  analyze.cpp (heatmap, reachability cache)       |
|  entities.cpp (entity properties DB)             |
+--------------------------------------------------+
                       v
+--------------------------------------------------+
|  support.cpp (utility), storage.cpp (save/load)  |  <- Infrastructure
+--------------------------------------------------+
```

## Was die einzelnen Module machen

### `hooks.cpp` + `linkage.cpp` — HL-SDK-Integration
**~600 Zeilen** Halb-Life-SDK-Stubs. yapb implementiert die Bot-Callbacks
(`ClientCommand`, `StartFrame`, `Spawn` etc.). **Für uns irrelevant** —
wir haben kein HL-SDK, sondern DOOM's eigene Engine-API.

**Was wir lernen können:** wie yapb die Bot-Hooks als Singleton-Klassen
organisiert (`Singleton<BotManager>`, `Singleton<Graph>` etc.). Clean pattern
für global-state.

### `botlib.cpp` — Bot-Lifecycle (~4400 Zeilen)
Zentrale Bot-Klasse. Hat eine `think()` Methode die jeden Game-Tick
aufgerufen wird — darin:

```cpp
void Bot::think() {
    updateLookAngles();        // wohin schaut der Bot
    updateBodyAngles();        // body yaw matching look
    processTasks();            // was macht der Bot gerade
    executeAction();           // aktuelle Goal-Aktion
    applyMovement();           // tastatur/mouse input synthetisieren
}
```

**Für aidoom:** Wir haben `P_AICoop_BuildCmd()` in `files/p_ai_coop.c`
das jeden Tic einen `ticcmd_t` (analog zu yapb's `applyMovement()`)
produziert. Architektur-Mapping ist direkt — nur ohne HL-SDK-Wrapper.

### `combat.cpp` — Aim, Dodge, Weapons (~2700 Zeilen)
Hat drei Aim-Modi (`AimType::None | Headshot | Normal | Spray | Pre-aim`),
Lead-Berechnung für Moving-Targets, Recoil-Kompensation pro Waffe.

**Kern-Funktion** `Bot::fireWeapon()`:
```cpp
void Bot::fireWeapon() {
    // 1. Predicted target position (account for target velocity + ping)
    Vector aim = target.pos + target.vel * ping_sec;
    
    // 2. Spread-Compensation (recoil has piled up N shots)
    aim += spray_pattern[shots_fired] * weapon.spread_per_shot;
    
    // 3. Apply aim offset
    view.angle = (aim - eye_pos).angle();
    
    // 4. Fire if angle within accuracy cone
    if (angle_diff < m_accuracyCone) {
        sendInput(INPUT_ATTACK);
    }
}
```

**Für aidoom:** unser `COOP_FACING=1500` Schwellwert in `p_ai_coop.c` ist
grob — yapb's Lead-Berechnung würde den Buddy messbar besser machen
besonders bei sich bewegenden Monstern. **~150-200 Zeilen** Portierung
in eine neue `p_ai_combat.c`.

### `navigate.cpp` — A* Pathfinding (~3500 Zeilen)
Drei Path-Modi:

```
enum PathFind {
    Fast = 0,    // greedy, schnelle Wege, nicht optimal
    Optimal = 1, // classic A* mit Heuristik, balanced
    Safe = 2     // ausweicht auf offene Türen/Healthpacks
};
```

A* über yapb's Waypoint-Graph (siehe `graph.cpp`). Edges haben Gewichte
(`m_pathDistance`) für Türöffnungs-Strafen (`+200`), Hazard-Floor-Strafen
(`+1000`), etc.

**Für aidoom:** Wir haben **schon** A*-Pathfinding in `files/p_ai_coop.c`
via BSP-Sub-Sector-Dijkstra (~800 Zeilen handgeschrieben, ~Z. 810-1000).
yapb's A* ist konzeptionell gleich aber mit besserer Heuristik und
Gewichtssystem. **Portierung:** unser Dijkstra durch yapb's A*-Framework
ersetzen. Geschwindigkeitsvorteil bei großen Maps.

### `graph.cpp` — Waypoint-Graph + Auto-Generation (~2900 Zeilen)
yapb hat ein **Waypoint-Format** (`.pwf`-Dateien) die Editor-erstellte
oder automatisch generierte Knoten enthalten. `GraphAnalyze` baut den
Graph on-the-fly wenn keine `.pwf` da ist.

**Wichtige Erkenntnis:** yapb's Ansatz ist **hybrid** — Waypoint-File
als Optimierung, on-the-fly als Fallback. Wir haben **nur** on-the-fly
(BSP-Sub-Sector-Centroids) — was laut `BOT_REQUISITEN.md` Zeile 25 explizit
als Stärke gewertet wird ("waypoint-frei").

**Was wir trotzdem lernen können:** yapb's `GraphAnalyze` macht
Reachability-Analyse beim Graph-Build — wir machen das auch (in
`AICoop_BuildNavGraph` ~Z. 970) aber weniger ausgefeilt.

### `analyze.cpp` — Heatmap + Reachability (~400 Zeilen)
Baut eine 2D-Grid-Heatmap die zeigt "wie sichtbar ist jede Zelle des
Levels von meiner Position aus". Zellen die ich oft einsehe → hoher
Heatmap-Wert. **Präemptiver** Visibility-Cache.

**Für aidoom:** direkter Use-Case für unseren `p_ai_viscache.c` (siehe
`VISIBILITY_CACHE.md`). yapb's Heatmap-Pattern als Inspiration für den
Aufbau, nicht für 1:1-Übernahme — yapb's Heatmap ist 2D-Grid-basiert,
unser Visibility-Cache ist Mobj-basiert.

### `vision.cpp` + `vistable.cpp` — Visibility-System (~830 Zeilen zusammen)
**Zwei separate Systeme**:

**`vision.cpp` — Frustum-basiert** (für Echtzeit-Sichtprüfung):
```cpp
class Frustum : public Singleton<Frustum> {
    struct Plane { Vector normal, point; float result; };
    enum class PlaneSide { Top, Bottom, Left, Right, Near, Far, Num };
    static constexpr float kFov = 75.0f;
    
    // Update Frustum-Planes für aktuelle Blickrichtung
    void calculate(Planes &planes, const Vector &viewAngle, 
                   const Vector &viewOffset) const;
    
    // Pre-check: ist Entity im Frustum? (cheap, vor Raycast)
    bool check(const Planes &planes, edict_t *ent) const;
};
```

**`vistable.cpp` — Waypoint-Graph-basiert** (für Bot-Planung):
```cpp
class GraphVistable : public Singleton<GraphVistable> {
    SmallArray<VisStorage> m_vistable;  // [src_node * num_nodes + dst_node]
    
    // Pre-computed Visibility zwischen Waypoint-Nodes
    bool visible(int srcIndex, int destIndex, VisIndex vis) const;
    
    // Saved/loaded mit Bot-Setup
    void load(); void save() const; void rebuild();
};
```

`VisStorage` ist `uint8_t` — 1 Byte pro (src,dst) Paar → kann komplette
Visibility-Matrix als Bit-Packed-Array speichern. Für 1000 Nodes = 125KB
(wenn nicht bit-packed). `GraphVistable::rebuild()` läuft einmal beim
Bot-Setup, `visible()` ist O(1) Lookup.

**Für aidoom:** **Genau dieses Pattern** ist was ich für `p_ai_viscache.c`
geplant habe. Wir können yapb's Frustum-Check als Pre-Raycast-Filter
nehmen und für unseren Mobj-basierten Cache adaptieren (statt Waypoint-
basiert).

### `planner.cpp` — Task-Planer (~580 Zeilen)
yapb's Bot hat eine **Task-Queue** mit Prioritäten:

```
enum Task {
    Attack = 0, FollowUser = 1, PickupItem = 2, 
    Camp = 3, Hunt = 4, SeekCover = 5, DefuseBomb = 6, ...
};
```

`Planner` baut Sub-Task-Bäume: "Heal-Item holen" → "Finde Item in
Sichtweite" → "Path zu Item" → "Pickup ausführen".

**Für aidoom:** Wenn wir Punkt 1 aus `AI_IMPROVEMENTS.md` bauen (Buddy-
LLM-Hook), brauchen wir genau das. LLM gibt **Ziel** ("hol Heal"), der
Planner zerlegt in Sub-Steps für den ticcmd-Generator. **~300 Zeilen**
Portierung in `p_ai_planner.c`.

### `practice.cpp` — Learning/Memory (~600 Zeilen)
yapb lernt aus Erfahrung: wo Enemies spawnen, welche Wege sich lohnen,
welche Waffen gut sind an welcher Position. Speichert in `.ypb`-Files
pro Map.

**Für aidoom:** Wenn Punkt 7 aus `AI_IMPROVEMENTS.md` (Memory/Learning)
gebaut wird — yapb's Pattern ist die Vorlage. Datei `~/.aidoom/buddy_*.dat`
pro Map.

### `config.cpp` + `storage.cpp` — Config + Save/Load
yapb's Config ist CVar-System mit Hot-Reload. Storage ist binäre
serialization mit Versionierung.

**Für aidoom:** wir haben schon `m_misc.c` Config-System — kein Bedarf.

## High-Level: was wir von yapb lernen

| yapb-Modul | Zeilen | Für aidoom | Aufwand |
|-----------|--------|------------|---------|
| `botlib.cpp` (think-loop) | 4400 | Architektur-Mapping, kein Code-Reuse | 0 Tage (Pattern) |
| `navigate.cpp` (A*) | 3500 | **A* statt Dijkstra in unserem Path-Finder** | 2-3 Tage |
| `vision.cpp` (Frustum) | 640 | **Frustum-Pre-Check vor P_CheckSight** | 0.5 Tage |
| `vistable.cpp` (VisCache) | 190 | **Inspiration für p_ai_viscache.c** | 1 Tag (siehe VISIBILITY_CACHE.md) |
| `analyze.cpp` (Heatmap) | 400 | Heatmap-Pattern für zukünftige Erweiterungen | 1 Tag |
| `combat.cpp` (Aim) | 2700 | **Lead-Berechnung + Recoil-Kompensation** | 1-2 Tage |
| `planner.cpp` (Task-Queue) | 580 | **Für AI-Layer-Bau (Punkt 1 AI_IMPROVEMENTS.md)** | 2 Tage |
| `practice.cpp` (Memory) | 600 | **Für Memory-Feature (Punkt 7)** | 2 Tage |
| `graph.cpp` (Waypoints) | 2900 | **Nicht portieren** — wir sind waypoint-frei, das ist Stärke | 0 Tage |
| `hooks.cpp` (HL-SDK) | 600 | **Nicht portieren** — HL-spezifisch | 0 Tage |
| Rest (config, storage, support, etc.) | ~3000 | Haben wir schon oder nicht relevant | 0 Tage |

> **Status-Update (2026-06):** Mehreres davon ist inzwischen erledigt bzw. verworfen:
> - **A\* statt Dijkstra** — ✅ erledigt (`PF_AStar`, kein `PF_Dijkstra` mehr).
> - **Visibility-Cache + Frustum-Pre-Check** — ❌ verworfen, `P_CheckSight` kostet
>   gemessen ~0,1 % CPU (siehe `VISIBILITY_CACHE.md`).
> - **Danger-aware G-Cost (`gfunctionKillsDist`) + Safe-Route-Modus** — ✅ erledigt:
>   abklingende Damage-Heatmap pro Sub-Sektor (`pf_danger`, gefüttert aus
>   `P_DamageMobj`), die `PF_AStar` im `pf_safemode` als Kanten-Strafe nutzt; der
>   Buddy wählt bei Retreat/Regroup (`summon`) oder niedrigem HP den ruhigen Weg
>   nach Hause. Runtime-only/decaying, keine Savegame-Änderung.
> - **Combat-Lead-Berechnung** — offen (marginal, DOOM-Autoaim leitet schon vor);
>   **Recoil-Kompensation** — N/A (DOOM hat kein CS-Recoil-Pattern).

**Realistische Empfehlung:**

**Phase 1 (schnelle Wins, ~1 Woche):**
1. Visibility-Cache `p_ai_viscache.c` + Frustum-Pre-Check
2. A*-Pathfinding statt Dijkstra
3. Combat-Verbesserungen (Lead, Recoil)

**Phase 2 (für AI-Layer-Bau, ~1-2 Wochen):**
4. Task-Planner
5. Practice/Memory

**Phase 3 (nice-to-have, später):**
6. Heatmap-Visualisierung
7. Buddy-Behavior-Learner

**Nicht portieren:**
- HL-SDK-Integration (hooks, linkage, engine) — komplett Engine-spezifisch
- Waypoint-Editor — wir sind waypoint-frei, das ist unsere Stärke laut `BOT_REQUISITEN.md`
- Bot-Chat/Chatlib — aidoom hat `i_voice.c` für Buddy-Voice, andere Mechanismen
- Fakeping/Practice-Replay — nicht relevant für Single-Player-Coop-Bot

## Lizenz-Hinweis yapb

**MIT-Lizenz** — portierbar mit Copyright-Vermerk. Bei Code-Übernahme aus
yapb-Files MUSS der Original-Copyright-Header in der neuen aidoom-Datei
stehen:

```c
// Based on yapb (https://github.com/YaPB/yapb), MIT-licensed
// Original work Copyright (c) yapb project developers and PODBot contributors
// Adapted for aidoom's DOOM buddy (single-player coop companion)
```

Die MIT-Lizenz erlaubt:
- ✅ Kommerzielle Nutzung
- ✅ Modifikation
- ✅ Distribution
- ✅ Private Nutzung
- **Nur Bedingung**: Copyright-Vermerk + Lizenztext mitliefern

## Quellen-Referenzen

Alle Pfade relativ zu `~/Source/`:
- yapb Source: `cs16-client/3rdparty/yapb/src/`
- yapb Headers: `cs16-client/3rdparty/yapb/inc/`
- yapb Lizenz: `cs16-client/3rdparty/yapb/LICENSE.txt`
- yapb README: `cs16-client/3rdparty/yapb/README.md`
- aidoom Buddy: `aidoom/files/p_ai_coop.c` (1422 Z., regelbasiert)
- aidoom LLM-Layer (geplant): `aidoom/files/p_ai_llm.c` (725 Z.)
- AI-Improvements-Plan: `aidoom/AI_IMPROVEMENTS.md`
- Visibility-Cache-Plan: `aidoom/VISIBILITY_CACHE.md`
- QuakeOne-Bot-Recherche: `QuakeOne/BOT_REQUISITEN.md`