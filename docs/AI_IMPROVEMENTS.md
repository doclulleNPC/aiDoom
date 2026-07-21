# AI/LLM-Verbesserungen für aidoom

Mögliche Richtungen, wie das LLM/AI-System in aidoom über die bestehende
Director-Implementierung hinaus verbessert werden könnte. Stand: aktuelle
Session.

## Was schon da ist

- `files/p_ai_llm.c`/`.h` (~725 Zeilen): LLM-Director für Monster via TCP-
  Line-Protocol oder eingebauter `-aidemo`-Modus. Off außer mit `-aidirector
  [port]` oder `-aidemo`. Hooks in `A_Chase` (`p_enemy.c`), `P_Ticker`,
  `P_SetupLevel`. Direktiven in Side-Table keyed by `mobj_t*`.
- `files/p_ai_coop.c`/`.h` (~1148 Zeilen): Regelbasierter Co-op-Buddy
  (`-aicoop`), deterministisch, BSP-Dijkstra-Pathfinding. Konsolen-Cmds
  `where`/`come`/`wait`/`attack`/`report`.
- `files/i_voice.c`: Offline-OGG-Sprachausgabe für den Buddy via `aidoom.wad`.
- `AGENT_CONTROL.md` §1-13: Director-Protokoll-Spec (Player §1-11, Monster
  §12-13). `MONSTER_AGENT_GUIDE.md`, `doom_agent_api_architecture.md`,
  `doom_agent_api_vizdoom.md`: weitere Director-Doku.

## 1. Buddy-Gehirn mit LLM kombinieren

Aktuell rein regelbasierte FSM mit starren Distanz-Threshold-Übergängen
(FOLLOW/FIGHT/HEAL/HOLD/COME/GRAB). LLM-Director-Aufsatz könnte:

- **Kontextbewusst entscheiden**: "Buddy steht im Nukage, sollte flüchten"
  vs "Buddy ist voll HP, kann offensiv pushen". Aktuell prüft `p_ai_coop.c`
  das nur grob (Escape-nukage seit 2026).
- **Predictive flanking** basierend auf Player-Position und Map-Geometrie:
  vorhersagen wo Monster spawnen, sich dahinter positionieren.
- **Trade-off-Decisions** statt harter Priorität: heilen vs item-grabben
  vs player-decken — aktuell deterministische Priority-Kette.

**Umsetzungsidee**: Zweite Director-Connection pro `players[coop_slot]`,
eigener kleiner Prompt-Loop ("Du bist Spieler 2, HP=X, Armor=Y, Player 1
ist Z units entfernt, Monster in Sichtweite:..."). Regelbasierter
`p_ai_coop.c` bleibt als Fallback wenn LLM nicht erreichbar oder Latency
zu hoch.

**Impact**: Höchster Gameplay-Impact, weil der Buddy aktuell der schwächste
AI-Teil ist. Konsolen-Kommandos `where`/`come`/`wait`/`attack` könnten
optional LLM-Antworten statt statischer Strings geben.

## 2. Schlaueres Monster-Verhalten (LLM-Attack-Patterns)

Aktuell hat jedes Monster nur Vanilla-AI (state machine in `p_enemy.c`).
LLM könnte pro Monster-Typ unterschiedliche Direktiven generieren:

- **Imp vs Sergeant**: "Sergeant gibt Vorwarn-Schuss ab, Imp charges frontal"
- **Cacodemon**: "fliegt zur Seite, dann Fireball aus off-angle"
- **Arch-Vile**: "resurrect-Pattern: erst defensive Position, dann Resurrect
  vom gefallenen Monster, nicht sofort offensiv"

`p_ai_llm.c` hat schon eine Tag-basierte Direktiven-Tabelle, aber die ist
**statisch** (vorgegebene Befehle). LLM könnte **dynamische Attack-Patterns**
basierend auf Situation generieren: "Monster X greift aus Schatten von
Position Y an" statt nur "Monster X, attackiere Spieler".

**Impact**: Mittlerer Aufwand, hoher Difficulty-Impact. Mehr Spieltiefe
ohne Monster-Stats zu ändern.

## 3. Director-Broadcast über Map-State

Aktuell sieht der Director hauptsächlich `player_t`-Daten. Erweiterung:

- **Monster-Positionen broadcasten** (alive, wo, type) → Director kann
  "Schicke die 3 Imps am Corridor in den Hinterhalt"
- **Sector-Information**: "Player ist in nukage-damaging floor", "Player
  hat gerade Keycard X aufgehoben" → Director kann gezielt Monster spawnen
  oder Türen schließen
- **Sound-basiert**: "Player hat Schuss abgegeben, alle Monster in Hörweite
  wissen wo er ist" — kombiniert mit DOOM's Sound-Propagation

**Umsetzung**: Director-Side bekommt erweiterten Snapshot in
`P_AI_Ticker` (siehe `p_ai_llm.c`). Bidirektional: Director kann dann auch
`spawn_extra_monster(type, x, y)`-Calls zurück ans Spiel senden.

## 4. Reinforcement-Learning-Hybrid

Regelbasiert für **harte Constraints** (nicht durch Türen gehen die closed
sind, nicht in nukage laufen wenn HP low — beides schon implementiert) +
LLM für **strategische Entscheidungen** (welches Monster zuerst, wann flüchten,
wann pushen).

Constraint-Layer ist deterministisch (= Demo-/Netplay-kompatibel), LLM-Layer
ist strategisch und kann Latency haben ohne dass die Playsim blockiert
(async queue wie schon im aktuellen Director).

## 5. Buddy-Mikrofon/Sprach-Coordination

`i_voice.c` existiert schon (offline OGG via `aidoom.wad`). Erweiterung:

- **Echtzeit-Sprach-Coordination**: Buddy sagt "flanking left", Player
  versteht (über Hotkey oder Text-Overlay)
- **Director spricht mit Buddy UND Player**: zwei Audio-Streams, Director
  koordiniert beide

## 6. Multi-Agent-Coordination

Wenn mehrere Buddies (z.B. `-aicoop 3` für 3 AI-Buddies): LLM koordiniert
die als Squad. Aktuell würde jeder Buddy unabhängig agieren (jeder hat
eigene `coop_state`-Variable, eigene Direktiven-Warteschlange).

Squad-Taktiken:
- "Buddy 1 zieht Aggro frontal, Buddy 2 umgeht links"
- "Buddies wechseln sich beim Heal-Item-Tragen ab"
- "Squad-Position Formation um Player 1"

## 7. Memory / Lernfähigkeit

Director schreibt State in `~/.aidoom/ai_memory.dat`:

- Welche Attack-Patterns haben gegen diesen Player-Typ funktioniert
- Welche Monster-Spawns sind gefährlich auf welcher Map
- Welche Item-Locations sind gut

Beim nächsten Start gleicher Map: Director nutzt Memory für bessere
Strategien. Über Zeit lernt das System pro Map und pro Spieler-Stil.

## 8. Tool-Use statt nur Text-Directives

Aktuell ist das LLM-Protokoll text-basiert ("attack:2" etc.). Mit
Function-Calling könnte das LLM:

- Map-Editor-Aktionen auslösen ("spawn extra ammo here")
- Sector-Tags ändern
- Script-Events triggern

Mächtig aber gefährlich — könnte Savegames zerstören. Eher was für
Creative-Mode oder Fork. Braucht striktes Sandboxing (LLM darf nur
genehmigte Tools aufrufen, nicht beliebiges Map-Manipulation).

## Priorisierung

| # | Idee                    | Aufwand | Impact | Empfehlung       |
|---|-------------------------|---------|--------|------------------|
| 1 | Buddy-LLM-Hook          | Mittel  | Hoch   | **Zuerst**       |
| 2 | Monster-Attack-Patterns | Mittel  | Hoch   | **Parallel**     |
| 3 | Map-State-Broadcast     | Klein   | Mittel | Mit #1+#2        |
| 4 | RL-Hybrid               | Groß    | Mittel | Nach #1+#2       |
| 5 | Voice-Coordination      | Mittel  | Niedrig| Polish           |
| 6 | Multi-Agent-Squad       | Groß    | Mittel | Bei Bedarf       |
| 7 | Memory/Learning         | Mittel  | Mittel | Mit #2           |
| 8 | Tool-Use                | Groß    | ?      | Creative-Mode    |

## Siehe auch

- `files/p_ai_llm.c`/`.h` — Director-Implementation
- `files/p_ai_coop.c`/`.h` — Buddy-Implementation
- `AGENT_CONTROL.md` §12-13 — Director-Protocol
- `MONSTER_AGENT_GUIDE.md` — Wie Monster als Agents funktionieren
- `doom_agent_api_architecture.md` — Architektur-Overview
- `doom_agent_api_vizdoom.md` — Vergleich mit ViZDoom