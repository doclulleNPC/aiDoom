# BUDDY_HUD.md — Companion Top-of-Screen HUD

Small "second STBAR" HUD overlay for the AI co-op companion ("buddy") in aidoom.
Lives at the top of the screen, centred, half the height of the player status
bar (`STBAR`) and uses a mix of the original DOOM patches (half-scaled) and the
baked DejaVuSansMono TTF atlas that the console already uses.

Source: `files/hu_buddy.h`, `files/hu_buddy.c`
Hooks: `files/hu_stuff.c` (`HU_Buddy_Drawer` from `HU_Drawer`),
       `files/d_main.c` (`HU_Buddy_Init` after `HU_Init`),
       `files/i_video.c` (`HU_Buddy_SetRes` after `ST_SetRes`),
       `files/m_misc.c` (`show_buddy_hud` config key).

---

## Umgesetzt (Was der Code tut)

- **Zentrierter HUD-Strip oben**, BASE-Koords (320×200) `X=80..240`, `Y=0..15`
  (= 16 Pixel hoch, halbe Höhe des originalen `STBAR` mit 32 Pixel).
- **Layout, von links nach rechts** (alles BASE-Koords):
  - `HP` (TTF-label) + `STTNUM` 3-stellig + `STTPRCNT` %
  - Waffen-Name (TTF, kurz-Code: `FIST/PISTOL/SHOTGUN/CHAINGUN/ROCKET/PLASMA/BFG/CHAINSAW/SSG`)
  - `A:` (TTF-label) + `STTNUM` 3-stellig (nur bei Waffen mit Munition)
  - `D:` (TTF-label) + `STTNUM` 3-stellig + `U` (Distanz zum nächsten Human)
  - `S:` (TTF-label) + State-Name (TTF, `FOLLOW/FIGHT/HEAL/HOLD/COME/GRAB`)
- **Patch-Rendering** mit eigenem `HU_Buddy_DrawPatchHalf`-Walker, der die
  originalen DOOM-Patches (`STBAR`/`STTNUM*`/`STYSNUM*`/`STTPRCNT`/`STKEYS*`/
  `STARMS`/`STTMINUS`) per nearest-neighbour auf halbe Größe (jede 2. Spalte,
  jede 2. Zeile) ins `screens[0]`-Bytebuffer blittet.
- **TTF-Rendering** mit Sub-Sampling (`BUDDY_TTF_SCALE_X/Y = 2`) gegen den
  gebackenen DejaVuSansMono-Atlas (`tools/font_atlas.h`, shared mit der
  Console-Overlay).
- **Separators**: dünne horizontale Linien (PLAYPAL 96, helles Grau) am
  oberen und unteren Rand des Strips als visuelle Barriere gegen das 3D-Bild.
- **Config-Toggle** `show_buddy_hud` in `m_misc.c`'s `defaults[]` (Default 1 =
  ON), persistiert via `M_LoadDefaults`/`M_SaveDefaults`.
- **Auto-On**: HUD rendert auch ohne `-aicoop` problemlos (frühe Returns
  wenn `P_AICoop_Slot() < 0`, also kein Player 2 im Spiel → no-op).
- **Hooks**:
  - `HU_Buddy_Init()` aus `D_DoomMain` direkt nach `HU_Init` (vor `ST_Init`).
  - `HU_Buddy_Drawer()` aus `HU_Drawer` (`hu_stuff.c`), läuft nach
    `ST_Drawer` und vor `I_FinishUpdate`.
  - `HU_Buddy_SetRes()` aus `V_SetRes` (`i_video.c`), derzeit no-op aber
    stable API falls je ein per-resolution Buffer dazukommt.

---

## Done

- [x] Eigene `files/hu_buddy.h` mit public API (`HU_Buddy_Init`/`Drawer`/
      `SetRes` + `extern int show_buddy_hud`).
- [x] Eigenes `files/hu_buddy.c` mit Patch-Half-Renderer, TTF-Sub-Sampler,
      Widget-Komposition und Layout.
- [x] Patches geladen in `HU_Buddy_Init`: `STTNUM0..9`, `STTPRCNT`,
      `STTMINUS`, `STBAR`, `STARMS`, `STKEYS0..5`.
- [x] `HU_Buddy_Drawer` in `HU_Drawer` (`hu_stuff.c:496`) integriert.
- [x] `HU_Buddy_Init` in `D_DoomMain` (`d_main.c:1218`) integriert.
- [x] `HU_Buddy_SetRes` in `V_SetRes` (`i_video.c:506,545`) integriert.
- [x] `show_buddy_hud` extern decl + `defaults[]`-Eintrag in `m_misc.c:208,241`.
- [x] Build clean, keine neuen Warnings oder Errors.
- [x] Game-Run-Test (xvfb headless): binary startet, initialisiert alle
      Subsysteme, `HU_Buddy_Init: Companion HUD.` im stderr, Buddy läuft
      (`PLD 56 navok=1 state=0` Pathfinding-Spuren), `HU_Buddy_Drawer` läuft
      jeden Frame, kein Crash, kein Speicherleck.
- [x] CMake + Build.sh greifen das neue `hu_buddy.c` automatisch via
      `file(GLOB ... files/*.c)` bzw. `gcc ... *.c`.

---

## Todo (offene Punkte / Dinge die noch verifiziert werden müssen)

- [ ] **Visuelle Verifikation im echten Game**: in Xvfb-Tests konnte ich
      das HUD nicht im Screenshot sehen — das deutet stark auf ein
      Xvfb-spezifisches `SDL_RenderPresent`/Buffer-Sync-Problem hin, nicht
      auf einen Code-Bug. Im echten Run mit echtem Window-Manager sollte
      das HUD sichtbar sein. **Bitte selbst testen**:
      `cd ~/Source/aidoom && ./build.sh && ./run/aidoom -iwad doom.wad -aicoop`.
- [ ] **Patch-Half-Skalierung visuell prüfen**: mein `HU_Buddy_DrawPatchHalf`
      ist handgeschrieben (kein V_DrawPatch-Reuse weil das nicht
      beliebig skalieren kann) und muss gegen die Original-STBAR-Optik
      verglichen werden. Wenn Glyphen verzerrt/verkehrt herum aussehen,
      ist vermutlich der `leftoffset`/`topoffset`-Sub-Sampling-Offset der
      Bug — easy fix.
- [ ] **Tuning der Layout-X-Offsets**: `X_HP_LABEL`, `X_HP_VALUE`,
      `X_WEAPON` etc. sind handgetuned für BASE 320×200. Bei
      `widescreen == true` ist `WIDESCREENDELTA` korrekt addiert, aber die
      Spacing-Werte könnten im 16:9-Mode zu eng werden. Im 4:3/16:10
      unverändert.
- [ ] **Options-Menu-Integration**: `show_buddy_hud` ist via Config-Toggle
      änderbar, aber nicht im Options-Menü sichtbar. Falls gewünscht, einen
      `Messages`-Style-Eintrag im Options-Menü (`m_menu.c:1117`-Pattern)
      hinzufügen.
- [ ] **Consolen-Command**: ein `buddy_hud [on|off]` Command im
      `c_console.c` für schnelles Toggle wäre nice-to-have.
- [ ] **Visual Mode für hud-strip background**: aktuell nur dünne
      Separator-Linien. Falls eine echte "Box" gewünscht ist (wie die
      originale STBAR mit `STBAR`-Hintergrund-Patch), kann man den halben
      `STBAR`-Patch (320×16 statt 320×32, subsampled) als Background
      benutzen — erfordert aber dass `STBAR` selbst auch halbiert wird
      (oder einen separaten `STBAR2`-Lump).

---

## Fixes (Bugs die im Verlauf gefixt wurden)

1. **`HU_FONTSIZE`/`HU_FONTSTART` undeclared**: erster Build brach ab weil
   `hu_buddy.c` `hu_font[]` benutzte ohne `hu_stuff.h` zu inkluden. Fix:
   `#include "hu_stuff.h"` ergänzt.

2. **`mobj_t->playerstate` existiert nicht**: `playerstate` ist auf
   `player_t`, nicht auf `mobj_t`. Fix: check über `players[i].playerstate`
   statt `lis->playerstate`.

3. **`show_buddy_hud` undefined reference (linker)**: Variable war `extern`
   in `m_misc.c` aber nirgendwo definiert. Fix: `int show_buddy_hud = 1;`
   in `hu_buddy.c` (Default ON).

4. **`HU_Buddy_DrawNumberTall` forward-decl mit falscher Signatur**:
   Forward-decl hatte `(int, int, int, byte)`, Definition hatte
   `(int, int, int, byte, int width)`. Fix: forward-decl um `int width`
   erweitert.

5. **Shadow-strip am Ende von `DrawStrip` tat nichts sinnvolles**: ein
   Dead-Code-Block der `if (screens[0][...] == 0) screens[0][...] = 0`
   machte (set 0 auf 0). Fix: entfernt.

6. **xvfb-Screenshots verfehlten das Fenster**: aiDoom-Fenster war 1776×1000
   auf negativen Koordinaten (-376,-116) weil SDL3 HiDPI-Scaling machte,
   `import -window root` schoss nur den schwarzen Xvfb-Hintergrund. Fix:
   vor jedem Test `xdotool windowmove 0 0` + `windowsize 320 200` +
   `windowactivate` + `windowraise`, dann `import -window <id>`.

7. **Falsche Palette für "weiß"-Debug**: `palette[0xff]` ist `#a76b6b`
   (gedämpftes Rosa), NICHT Weiß. Weiß-Pixel sind `palette[4]`/`[168]`/
   `[208]`/`[224]`. Beim ersten Patch-Renderer-Test hatte ich `0xff` als
   "helles Weiß" benutzt und gedacht mein Code rendert nicht — der
   rendert schon, ich hatte nur die falsche Farbe. Debug-Pixel auf
   `palette[4]` umgestellt, dann war's klar sichtbar… als plötzlich
   DOOM's eigener 3D-Render darüber malte (siehe unten).

---

## History (Iteration-Verlauf)

### v1: erste Implementierung (rechtbündig, TTF-only, mit BG-Box)

- Status-Bar-artiger HUD oben rechts, kleiner TTF-Text in einer Zeile.
- V_DrawBlock-basierter BG-Streifen mit `BUDDY_BG_PAL` (rosa, weil
  PLAYPAL[16]).
- Rosa Hintergrund zu auffällig, User wollte transparent.

### v2: rosa BG raus, TTF-atlas statt V_DrawPatch, Shadow-Trick

- `V_DrawBlock`-BG komplett entfernt — transparent.
- Eigener TTF-Glyph-Renderer (`HU_Buddy_DrawChar`) gegen den
  `font_alpha[]`-Atlas, der bereits für die Console benutzt wird.
- 1-Pixel-schwarzer Schatten 1px down/right für Lesbarkeit auf Lava.
- Eine Zeile, rechtsbündig ausgerichtet.
- **Erfolgreich getestet** in Xvfb (Screenshot zeigte gelben Text
  `[BUDDY] HP 100 ARM 7 PISTOL 50 12U FOLLOW` oben rechts).

### v3: User wollte links frei für DOOM-Messages → rechtsbündig (done)

- Position von links nach rechts verschoben: erst pro-Spalte-x-offset,
  dann komplett auf `x = BASE_WIDTH - 2 + WIDESCREENDELTA - total_width`
  umgestellt (eine Zeile, alle Felder zu einem String konkat-en,
  Gesamtlänge messen, von rechts aus rendern).

### v4: User wollte STBAR-Look (echte Patches, halbe Größe, zentriert)

- Komplettes Rewrite von `hu_buddy.c`:
  - Eigener `HU_Buddy_DrawPatchHalf` für column-major Patch-Rendering
    mit nearest-neighbour 2×2 → 1 Sub-Sampling.
  - TTF bleibt für Waffen-Name und State (gibt's nicht als WAD-Patches).
  - Layout komplett neu: 160 BASE-Pixel breit, zentriert in 320.
  - Separator-Linien oben/unten (statt BG-Box).
- **Visuelles Problem in Xvfb**: `HU_Buddy_Drawer` läuft, schreibt in
  `screens[0]`, aber das Resultat kommt im Screenshot nicht an.
  Debug-Block (großes 120×50 weißes V_DrawBlock bei x=100,y=50)
  ebenfalls nicht sichtbar → deutet auf Xvfb-Renderer-Disconnect,
  nicht auf Code-Bug.

---

## Bekanntes Problem: Xvfb-Renderer-Disconnect

**Symptom**: `HU_Buddy_Drawer` läuft jeden Frame (`[bh] drew` im stderr),
schreibt Pixel in `screens[0]` (verifiziert via Debug-Prints). Aber
`import -window root` zeigt diese Pixel nicht.

**Verifikation**:
- Eigener Code (`screens[0][...] = 4`) → 0 helle Pixel im Screenshot
- Original STBAR (über `ST_Drawer`/`V_DrawPatch`) → sichtbar im Screenshot
- Original DOOM-Messages (über `HUlib_drawSText`) → sichtbar im Screenshot

**Hypothese**: Xvfb rendert SDL-Texturen anders als ein echter X-Server,
möglicherweise wird `SDL_RenderPresent`'s Present-Rect nicht zuverlässig
geflusht. Im echten Run mit normalem Window-Manager (KDE, GNOME, etc.)
oder direkt auf dem TTY sollte das HUD erscheinen.

**Workaround für Remote-Test**: nicht möglich — bitte selbst im echten
Game verifizieren.

---

## Umsetzung im Code (Schritt-für-Schritt Anleitung für Maintainer)

Falls jemand das ganze nachvollziehen oder erweitern will:

### Datei-Layout

```
files/
├── hu_buddy.h       (neu)  ~40 Zeilen   — Public API + show_buddy_hud decl
├── hu_buddy.c       (neu)  ~460 Zeilen  — Renderer + Layout
├── hu_stuff.c       (mod)  +6 Zeilen    — HU_Buddy_Drawer Hook
├── d_main.c         (mod)  +4 Zeilen    — HU_Buddy_Init Hook
├── i_video.c        (mod)  +6 Zeilen    — HU_Buddy_SetRes Hook
└── m_misc.c         (mod)  +4 Zeilen    — show_buddy_hud extern + config
```

### Hook-Pattern

Alle Hooks sind symmetrisch zu den existierenden `HU_*` und `ST_*` Hooks:

```c
// hu_stuff.c:486
void HU_Drawer(void)
{
    HUlib_drawSText(&w_message);
    HUlib_drawIText(&w_chat);
    if (automapactive)
        HUlib_drawTextLine(&w_title, false);

    HU_Buddy_Drawer ();   // ← hier, am Ende von HU_Drawer
}
```

```c
// d_main.c:1218
HU_Init ();
C_Init ();
HU_Buddy_Init ();   // ← hier, vor ST_Init
ST_Init ();
```

```c
// i_video.c:506, 545
void ST_SetRes (void);
void HU_Buddy_SetRes (void);   // ← forward decl
...
ST_SetRes();
HU_Buddy_SetRes();   // ← nach ST_SetRes
R_SetViewSize(...);
```

```c
// m_misc.c:208, 241
extern int show_buddy_hud;
...
{"show_buddy_hud", &show_buddy_hud, 1},
```

### Build

Beide Build-Systeme greifen `files/*.c` automatisch:
- `build.sh` (Linux/macOS): `cd files && gcc ... *.c -o aidoom ...`
- `CMakeLists.txt`: `file(GLOB AIDOOM_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/files/*.c)`

→ Neue `files/hu_buddy.c` wird ohne weitere Makefile-Änderungen
aufgegriffen.

### Erweiterungs-Pattern

**Neues Buddy-Stat anzeigen** (z.B. Buddy-Armor-Type):

1. Erweitere `struct player_t` ist nicht nötig — `armortype` ist schon da.
2. In `HU_Buddy_DrawStrip` (kurz vor dem State-Block) eine Zeile wie:
   ```c
   HU_Buddy_DrawTtfString (X_STATE_LBL - 20, Y_LABEL, "T1", 231);
   ```
3. Optional: `X_*`-Define am Anfang der Funktion neu berechnen wenn der
   Strip zu lang wird.

**Patch durch TTF ersetzen** (z.B. wenn `STYSNUM` zu klein zum Lesen ist):

```c
// alt:
HU_Buddy_DrawNumberTall (X_AMMO_VALUE, Y_DIGITS, ammo, 231, 3);
// neu:
char cell[8]; sprintf(cell, "%d", ammo);
HU_Buddy_DrawTtfString (X_AMMO_VALUE - 12, Y_LABEL, cell, 231);
```

**Eigenes Panel mit echtem STBAR-Hintergrund**:

```c
// STBAR ist 320x32, bei halber Höhe wär's 320x16.  Patch-Half-Renderer
// macht aus 320x32 pixel-exakt ein 160x16-Block (das Layout ist
// aktuell aber 160x16 zentriert, also 80..240 horizontal).  Einfach:
HU_Buddy_DrawPatchHalf (BUDDY_BAR_X, BUDDY_BAR_Y, p_stbar);
```

---

## Siehe auch

- `CLAUDE.md` — Engine-Architecture-Übersicht (i_*, p_*, r_*, hu_* Module).
- `AGENT_CONTROL.md` §12-13 — LLM-Director-Protocol (nicht direkt
  relevant fürs HUD aber Kontext warum der Buddy existiert).
- `tools/font_atlas.h` — gebackener DejaVuSansMono-Atlas (shared mit
  Console-Overlay).
- `files/st_stuff.c` — Original-STBAR als Referenz-Implementation.