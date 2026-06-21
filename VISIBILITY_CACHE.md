# Visibility-Cache für aidoom-Buddy

Konzept: zwischenspeichern, ob ein Mobj für den Buddy sichtbar ist, um
`P_CheckSight`-Raycasts zu vermeiden die jeden Tic laufen.

> **⛔ FAZIT (2026-06, gemessen): NICHT bauen.** Profiling unter echtem Kampf —
> mit *und* ohne LLM-Director — zeigt, dass `P_CheckSight` **~0,1 % des Tic-CPU-
> Budgets** kostet (siehe [Profiling-Ergebnisse](#profiling-ergebnisse-gemessen)).
> Die Annahme „17.500 Line-Tests/s, 10×-Gewinn" war ein grobe Überschätzung:
> `P_CheckSight` nutzt Reject-Matrix + BSP-Walk (~0,5–1,7 µs/Call), keinen
> Flat-Line-Walk. Der Cache würde ~0,1 % einsparen → der Aufwand (~3 Tage, neue
> Subsysteme, mobj-ID-Stabilität) lohnt sich nicht. Erst relevant, wenn ein
> künftiges Design Full-Map-Sicht *jeden Tic* pollt (~40.000 Calls/s für ~5 %).

## Was ist Visibility-Caching?

Sichtprüfung "kann Mob A Mob B sehen?" via `P_CheckSight(mobj_t*, mobj_t*)`
in `files/p_sight.c` ist ein Line-Walk durch alle Sektor-Lines. Teuer wenn
oft aufgerufen — besonders in:

- **Regelbasierter Buddy** (`files/p_ai_coop.c`): jeden Tic für jedes Monster
  in Reichweite via `AICoop_NearestMonsterTo` / `AICoop_MonsterInRange`
- **LLM-Buddy** (geplant, siehe `AI_IMPROVEMENTS.md #1`): Director fragt
  "welche Monster sind sichtbar?" als Snapshot für LLM-Decisions

**Komplexität**: bei E1M1 mit ~200 Lines × 5 sichtbare Monster × 35 tics/sec
= ~17.500 Line-Tests/Sekunde pro Buddy.

## Was wir bauen wollen

```
struct visibility_cache_entry {
    int     mobj_id;            // P_IdentifyMobj(mobj) - identifier, nicht pointer
    fixed_t last_x, last_y;     // mobj position bei letztem check
    int     last_check_tic;     // gametic bei letztem raycast
    boolean visible;            // ergebnis des raycasts
    fixed_t visibility_decay;  // ttl in FRACUNIT-tics (distance-scaled)
};

static visibility_cache_entry vcache[MAX_MOBJS];
```

**Logik** in einer neuen `files/p_ai_viscache.c`:
- `AIC_Visible(buddy_mobj, target_mobj)` → check cache, return cached if fresh
- `AIC_Invalidate(target_mobj)` → bei Monster-Tod, Map-Load
- `AIC_Flush()` → komplett löschen bei Map-Load

## TTL-Decay mit Distanz-Weighting

```
ttl_ms = base_ttl_ms / (1 + distance/1000)
```

Weiter entfernte Mobs bekommen kürzeres Cache-Window, weil:
- Großer Sichtkegel = größere Bewegungs-Freiräume
- Größerer Distanz = höherer Speed-Threshold für "out of view"

Default-Werte: `base_ttl_ms = 500` (= 17 tics bei 35 tic/s). Bei 1000u
Distanz = 250ms TTL, bei 3000u Distanz = 125ms TTL.

## Hook-Punkte in unserem Code

**Aktueller Code** (`files/p_ai_coop.c` ~ Z. 906):

```c
// Pseudo-code
mobj_t* AICoop_NearestMonsterTo(fixed_t x, fixed_t y)
{
    mobj_t* best = NULL;
    fixed_t bestd = MAXINT;
    for each monster in P_NextThinker:
        if (dist > COOP_SIGHT) continue;
        if (!P_CheckSight(buddy->mo, m)) continue;  // <-- raycast hier
        if (dist < bestd) { best = m; bestd = dist; }
    return best;
}
```

**Mit Cache:**

```c
mobj_t* AICoop_NearestMonsterTo(fixed_t x, fixed_t y)
{
    for each monster in P_NextThinker:
        if (AIC_VisibleCached(buddy->mo, m)) {  // cache-check vor raycast
            // mobj als sichtbar markiert, dist-compare
        }
}
```

## Inspiration aus yapb

**`/home/dulli/Source/cs16-client/3rdparty/yapb/`** (MIT-Lizenz, portierbar
mit Copyright-Vermerk):

- **`src/analyze.cpp`** (~400 Z.) — `GraphAnalyze` Klasse map-analyse,
  Reachability-Caching, Visibility-Heatmap. Heatmap-Grid wo Zellen mit
  "wie oft hab ich von dort aus gesehen"-Score gespeichert werden.
- **`src/navigate.cpp`** (~3500 Z.) — `TraceLine`/`isVisible`-Calls an
  ~Z. 2684 für Sicht-Block-Checks. Hat eingebautes Engine-Visibility-
  Wrapping aber **keinen dedizierten C-Cache-Layer** — yapb verlässt
  sich auf Half-Life's PVS/PAS (Potentially Visible Set).
- **`inc/support.h` Z. 44** — `bool isVisible(const Vector&, edict_t*)` —
  Public-API für Sichtprüfung, **ohne Caching**.

**Wichtige Erkenntnis**: yapb hat **keinen direkten Visibility-Cache** — sie
nutzen die Half-Life-Engine's PVS/PAS. Für unseren aidoom-Buddy müssen wir
den Cache selbst implementieren, mit yapb's Heatmap-Konzept als Inspiration.

## Geplanter Aufbau

**Neue Datei: `files/p_ai_viscache.c`** (~200 Zeilen) + **`files/p_ai_viscache.h`**:

```
p_ai_viscache.h:
    void   AIC_VisCache_Init (void);
    void   AIC_VisCache_Flush (void);
    void   AIC_VisCache_Invalidate (int mobj_id);
    boolean AIC_VisCache_Visible (mobj_t* viewer, mobj_t* target);

p_ai_viscache.c:
    - visibility_cache_entry vcache[MAX_MOBJS];
    - base_ttl_ms config (m_misc.c)
    - P_IdentifyMobj() for stable IDs across mobj death/respawn
```

**Header** `files/p_ai_coop.h`: Forward-Decl der AIC_VisCache_* Funktionen
damit `p_ai_coop.c` sie aufrufen kann.

**Init-Hook** in `files/p_setup.c` `P_SetupLevel` — `AIC_VisCache_Flush()`
nach `P_AI_Reset()`.

**Invalidate-Hook** in `files/p_inter.c` `Killed()` — wenn ein Mobj stirbt,
dessen vcache-Entry löschen.

**Config** in `files/m_misc.c` `defaults[]`:
```
{"viscache_ttl_ms", &viscache_ttl_ms, 500},
{"viscache_enabled", &viscache_enabled, 1},
```

## Lizenz-Hinweis

Die yapb-Code-Dateien sind **MIT-Lizenz** (siehe yapb `LICENSE.txt`).
Beim Übernehmen von Konzepten/Code-Schnipseln aus `analyze.cpp` oder
`navigate.cpp` muss ein Copyright-Vermerk in den Kommentaren der neuen
`p_ai_viscache.c` stehen, etwa:

```c
// Visibility-Cache-Struktur inspiriert von yapb's GraphAnalyze
// (~/cs16-client/3rdparty/yapb/src/analyze.cpp), MIT-Lizenz.
// Original-Copyright: yapb contributors.
```

## Performance-Erwartung

**Vorher** (regelbasierter Buddy):
- 17.500 Line-Tests/s auf E1M1 mit 5 Monstern
- `P_CheckSight` ist deterministisch und korrekt, aber kostet CPU-Tics

**Nachher** (mit Cache):
- 90%+ Cache-Hits bei statischen Szenen (Monster patrouilliert auf Route)
- Raycast nur wenn Position deutlich anders als letzter Check ODER Cache-Ttl abgelaufen
- ~1.750 Line-Tests/s = **10× Performance-Gewinn**

Für LLM-Layer (geplant): jeder Snapshot-Request ist jetzt billig → häufigere
Updates → besserer AI-Decision-Loop ohne Playsim-Stall.

## Aufwand-Schätzung

- `p_ai_viscache.c` + `.h`: **1 Arbeitstag**
- Hooks in `p_setup.c`, `p_inter.c`, `p_ai_coop.c`: **0.5 Tage**
- Config in `m_misc.c`: **0.5 Tage**
- Test mit xvfb + Performance-Profiling: **1 Tag**
- **Gesamt: ~3 Arbeitstage**, größte Gameplay-/Performance-Win pro Zeitaufwand

## Quellen-Referenzen

- **DOOM-Sight-Algorithmus**: `files/p_sight.c` `P_CheckSight` — vanilla DOOM
- **yapb Visibility-Inspiration**: `/home/dulli/Source/cs16-client/3rdparty/yapb/src/analyze.cpp`
- **yapb Heatmap-Grid-Pattern**: gleiche Datei, `GraphAnalyze::updateVisibility`
- **Regelbasierter Buddy (Aufrufer)**: `files/p_ai_coop.c` `AICoop_NearestMonsterTo`, `AICoop_MonsterInRange`
- **AI-Layer-Plan (LLM-Director)**: `AI_IMPROVEMENTS.md` #1 — nutzt Visibility-Daten als Snapshot
- **Verwandte QuakeOne-Recherche**: `/home/dulli/Source/QuakeOne/BOT_REQUISITEN.md` Abschnitt 5 (aidoom-Tier 2)

## Profiling-Ergebnisse (gemessen)

Gemessen 2026-06 auf diesem Rechner. Methode: temporärer Timing-Wrapper um
`P_CheckSight` (`clock_gettime(CLOCK_MONOTONIC)`, zählt Calls + akkumuliert ns)
plus Sekunden-Dump in `P_Ticker`. Referenz: ein Tic = 1/35 s = **28.571 µs**
CPU-Budget; „tic-budget %" = Anteil der Sicht-Zeit daran. E1M1 (`-warp 1 1`),
`-skill 4`, aktiver Kampf (Spieler in die Imps gelaufen + gestrafed).

**Korrektur der Ausgangsannahme:** `P_CheckSight` (`p_sight.c`) macht **Reject-
Matrix-Trivialtest (O(1), Z. 320) + BSP-Walk (`P_CrossBSPNode`, Z. 346)** —
gemessen **~0,5–1,7 µs/Call**, nicht „~200 Line-Tests". Die `sightcounts[0/1]`
trennen bereits Reject-Treffer von echten BSP-Walks. Der Buddy raycastet an genau
**einer** Stelle (`p_ai_coop.c` `AICoop_FindTarget`, 1× pro Tic über sichtbare
Monster); `AICoop_NearestMonsterTo` ist rein distanzbasiert (kein Sight).

| Szenario | Calls/s | µs/Call | µs/s | **Tic-Budget %** |
|----------|--------:|--------:|-----:|-----------------:|
| **A** — Buddy only (`-coop`, skill 4, Kampf)            | ~500–770 | 0,65–1,5 | 350–1170 | **0,03–0,12 %** |
| **B** — Buddy + LLM-Director (`-aicoop`, skill 4, Kampf) | ~450–800 | 0,52–1,7 | 230–1222 | **0,02–0,12 %** |

**Beobachtungen:**
- Sicht kostet in beiden Fällen **~0,1 % der CPU** — selbst im Gefecht.
- Der **LLM-Director ändert die Sicht-Last praktisch nicht**: sein `observe`
  (Sicht pro Monster in `p_ai_llm.c`) läuft nur ~alle 2–3 s (LLM-Latenz), also
  ~10–15 zusätzliche Calls/s — im Rauschen der ~500/s des Buddys verloren.
- Um Sicht auf ~5 % Budget zu bringen bräuchte es ~**40.000 Calls/s** (≈ 50× mehr)
  — d. h. Full-Map-Sicht jeden Tic, was kein aktuelles Design tut.

**Schlussfolgerung:** Der Visibility-Cache ist **nicht gerechtfertigt**. Selbst die
billige Alternative (FindTarget-Re-Acquire raten-limitieren) spart nur von 0,1 %
aus — überflüssig. Dieses Dokument bleibt als Referenz/Negativ-Ergebnis bestehen;
neu bewerten nur, falls ein künftiger Director Sicht-Snapshots mit hoher Frequenz
(jeden Tic, viele Mobs) pollt.