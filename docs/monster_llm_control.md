# Wie baue ich in Doom eine Steuerung der Monster durch LLM ein?

**TL;DR:** Das ist die "AI Director"-Variante. Das Repo hat bereits die
detaillierte Antwort: siehe **`AGENT_CONTROL.md` §12 "Letting an LLM control
the monsters"** — die hier ist die Kurzfassung, mit einer anderen
Betonung und einem paar eigenen Akzenten.

---

## 0. Die ehrliche Empfehlung in einem Satz

Hör auf, **jeden** Monster jeden Tic vom LLM denken zu lassen. Lass die
existierende C-KI weiter ihre 35-Hz-Reflexe machen, und lass das LLM in
Sekunden-Abständen **Squad-Orders** an die Monster schicken. Die Reflexe
sind umsonst, das LLM macht nur die Taktik, die der Original-AI
fehlt.

---

## 1. Was man vom Original-Doom behält — und was nicht

Vanilla-Doom-Monster-KI (`p_enemy.c` + `info.c`) ist *berüchtigt* primitiv:

  * **Greedy chase**: Monster laufen stur auf den Spieler zu.
  * **Infighting**: Monster, die sich gegenseitig sehen, ballern aufeinander,
    oft genug aus Versehen.
  * **Keine Flanken, kein Rückzug, kein Regrouping, keine Türen-Taktik**.

Was schon funktioniert und **nicht** neu gebaut werden muss:

  * `A_Chase` — Pathing via `P_NewChaseDir` (8 `movedir`-States)
  * `A_FaceTarget` — aiming
  * `A_*Attack` — fire/transitions in der State-Machine
  * `P_CheckSight` — line-of-sight check (kostet fast nix)
  * mobj-Thinker-List — Iteration über alle lebenden Monster

**Plan:** LLM ersetzt die *Taktik*, nicht die Reflexe. Du hookst einmal in
`A_Chase` und überschreibst nur die Anweisung, nicht die Ausführung.

---

## 2. Drei Granularitäten — eine ist richtig

| Variante                           | LLM-Calls | Skaliert?  | Wenn ja, dann...                  |
|------------------------------------|-----------|------------|------------------------------------|
| (a) Jedes Monster, jeder Tic       | pro Tick  | **Nein**   | Nur für EINEN Boss                |
| (b) Director / Squad-Commander     | pro Sek.  | **Ja**     | Standard-Empfehlung                |
| (c) Selektiv: 1 Boss smart, Rest Vanilla | pro Sek. | **Ja, billig** | Bester Impact/Effort |

**(b) ist die Antwort für 95% der Fälle.** LLM sieht pro Encounter einen
kompakten JSON, gibt Gruppen-Orders aus ("Imps: links flanken", "Gruppe B:
zurückfallen"), die C-Schicht führt sie aus.

---

## 3. Wo genau der Hook sitzt (auf aiDoom zugeschnitten)

In `p_enemy.c`, in `A_Chase`:

```c
void A_Chase (mobj_t* actor)
{
    if (P_AI_Active (actor)) {   // <-- neu (side-table lookup, kein mobj_t-Feld)
        A_LLMChase(actor);
        return;
    }
    /* ... existing vanilla logic ... */
}
```

`A_LLMChase` (neues Modul `p_ai_llm.c`) liest die Direktive und führt sie
mit den existierenden Primitives aus — `P_NewChaseDir`, `A_FaceTarget`,
Attack-State-Transition.

**Wichtig — und das ist der Grund warum das Repo-Doc es richtig macht:**

  * **Direktiven NICHT in `mobj_t` speichern.** `p_saveg.c` macht
    `memcpy` auf der ganzen Struct beim Speichern/Laden; ein neues
    Feld drinnen bricht Savegames. Lösung: **Side-Table** mit
    `mobj_t*` als Key.
  * **Modul:** neues `p_ai_llm.c` in `doom_SOURCES` aufnehmen, in
    `Makefile`. (Keine Header-Dependencies im Makefile — `make clean`
    nach Header-Änderungen, siehe `CLAUDE.md`.)

---

## 4. Observation — was das LLM sieht

Ein *Encounter*, nicht jeder Tic. Update **event-driven**: Spieler betritt
Raum, Monster wird getroffen, Sichtlinie ändert sich, Spieler schießt.

```json
{
  "encounter_id": "room3_imps",
  "player": {
    "pos": [1024, -256, 0],
    "health": 78, "armor": 50, "weapon": "shotgun"
  },
  "monsters": [
    {"id": "imp_1", "pos": [1100, -200, 0], "hp": 60,
     "state": "chase", "can_see_player": true},
    {"id": "imp_2", "pos": [1180, -240, 0], "hp": 60,
     "state": "chase", "can_see_player": false}
  ],
  "geometry": {
    "room": "room3",
    "exits": ["door_n", "door_w"],
    "player_can_reach": ["door_n"]
  },
  "player_dmg_last_3s": 18
}
```

Drei Quellen, allesamt free oder fast free:

  * **Player-State** aus `players[consoleplayer]` (C, instant)
  * **Monster-Liste** durch `thinkercap.next`-Iteration, gefiltert auf
    `mobj_t` mit `function.acp1 == P_MobjThinker` und in Encounter-Range
  * **Geometrie** aus BSP/Blockmap — einmalig pro Map-Lookup,
    cached in der Direktive

Kein Pixel. Kein CV. Kein Halluzinieren. LLM bekommt Wahrheit.

---

## 5. Action — was das LLM zurückgibt

Nicht "Monster 1: turn 15°". Sondern:

```json
{
  "directives": [
    {"ids": ["imp_1"],  "order": "flank_left",  "for_tics": 70},
    {"ids": ["imp_2"],  "order": "fallback",    "for_tics": 70},
    {"ids": ["imp_1","imp_2"], "order": "focus_fire", "after_tics": 70}
  ],
  "tactical_notes": "Imps split: one baits, one flanks from door_w"
}
```

`for_tics` / `after_tics` sind die Latency-Versicherung: Monster führen
den letzten Order aus, während das LLM über den nächsten nachdenkt. Der
Loop wartet **nie** auf das Modell.

Order-Typen, die die C-Schicht kennen muss (klein halten!):

  * `chase` / `hold` / `fallback` / `flank_left` / `flank_right`
  * `ambush` (Position via `x=`/`y=` übergeben)
  * `focus_fire` (auf wen, via `focus=`)
  * `use_door`

Mehr nicht. Mehr ist micromanagement und der LLM wird schlecht drin.

---

## 6. LLM-Modellwahl — die unausgesprochene Wahrheit

Nimm nicht das stärkste Modell. Nimm das **schnellste** das noch mitdenkt.

  * **Claude Haiku / GPT-4o-mini / Gemini Flash** für Encounter-Taktik:
    unter 500 ms Antwortzeit, gut genug für "flank" / "fallback".
  * Größere Modelle nur für **Boss-Design** oder für den einen (c)-Boss.

Latenz ist wichtiger als Intelligenz. Ein 3-Sekunden-LLM, das den
perfekten Plan ausheckt, ist 3 Sekunden in denen die Monster in der
Wand hängen.

---

## 7. Was man dabei gewinnt

| Original-Doom                          | LLM-Director                    |
|----------------------------------------|---------------------------------|
| Greedy chase                          | Flanking, Bait-and-Switch       |
| Infighting (zufällig)                  | Gezielt — Monster koordinieren  |
| Kein Rückzug                           | "Fall back, regroup"            |
| Türen = Hindernis                      | Monster können tactical Türen nutzen |
| Kein Target-Prio                       | "Schwächster Spieler" / "Schwerer Bewaffneter" |
| Alle Monster gleich hart               | Encounter-Tuning: leichter Raum vs. Boss |

Im Effekt: ein **tactical AI Director** (cf. Left 4 Dead) — pro Encounter
LLM-gesteuert, statt per Difficulty-Slider.

---

## 8. Wo diese Antwort im Repo weitergeht

Das hier ist die Kurzfassung. Für die echte Implementierung siehe:

  * **`AGENT_CONTROL.md` §12** "Letting an LLM control the *monsters*" — die
    ausführliche Version mit p_ai_llm.c-Skelett, A_LLMChase-Verhalten,
    Savegame-Konflikt-Warnung, Determinismus-Caveat.
  * **`AGENT_CONTROL.md` §12** "A nice hybrid" — der LLM-vs-LLM-Modus:
    Spieler auf ViZDoom (oder §2 Player-Hook), Monster auf In-Engine
    Director. **Genau der Modus, der "LLM spielt Doom als Dungeon-Master"
    wahr macht.**

---

## 9. Was diese Antwort *nicht* abdeckt (bewusst)

  * **Wo LLM-State persistiert wird** (RAM vs. Disk): in der Side-Table,
    Schlüssel `mobj_t*`, ist RAM — pro Map-Reset weg. Das ist Absicht
    (Taktik ist encounter-spezifisch), nicht übersehen.
  * **Netcode-/Demo-Kompatibilität**: Monster-AI nutzt `P_Random()` hart
    (move-dir, pain-chance). LLM-Direktiven brechen das. Singleplayer ja,
    Demo/Netcode nein. Steht im Repo-Doc §12 "Same two caveats".
  * **Cost-Budget**: bei 5 Encounter/s × Haiku ist man bei ein paar
    Cent/Stunde. Kein Thema. Wollte es nur erwähnt haben.
