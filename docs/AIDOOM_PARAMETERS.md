# aidoom CLI-Parameter Reference

Vollständige Liste aller CLI-Flags die `aidoom` akzeptiert, extrahiert via
`grep -rn 'M_CheckParm' files/*.c`. Kategorisiert nach Quelle und Verhalten.

## Vanilla DOOM (von 1996, weitergegeben)

| Flag | Parameter | Default | Wirkung |
|------|-----------|---------|---------|
| `-iwad` | `<file>` | auto-detect | Pfad zur IWAD-Datei (DOOM/DOOM2/Plutonia/TNT/Chex Quest 3). Identifikation via Filename (`doom.wad`, `doom2.wad`, `plutonia.wad`, `tnt.wad`, `freedoom*.wad`, `chex3.wad`). |
| `-file` | `<file>` | — | Lädt zusätzliche PWADs die das IWAD überschreiben. Mehrfach verwendbar. |
| `-warp` | `<ep> <map>` | `1 1` | Direktsprung zu Episode/Map. Im Commercial-Mode ist `ep` die Map-Nummer (`map01`..`map32`), im Registered-Mode `E?M?` (z.B. `-warp 2 1` → MAP02 oder E2M1 je nach IWAD). |
| `-skill` | `<1-5>` | `3` | Schwierigkeitsgrad: 1=I'm Too Young, 2=Hey Not Too Rough, 3=Hurt Me Plenty, 4=Ultra-Violence, 5=Nightmare. |
| `-episode` | `<n>` | `1` | Episode-Auswahl im Menu (nur Registered/Retail-Mode). |
| `-nomonsters` | — | off | Keine Monster spawnen. |
| `-respawn` | — | off | Monster respawnen nach Tod (Deathmatch-typisch). |
| `-fast` | — | off | Monster reagieren langsamer/greifen später an. |
| `-turbo` | `<%>` | `100` | Spieler-Geschwindigkeit in Prozent (z.B. `-turbo 200`). |
| `-altdeath` | — | off | Alternative Deathmatch-Modus (1 Kill = 1 Punkt, Items respawnen). |
| `-deathmatch` | — | off | Standard Deathmatch (20er-Frags). |
| `-timer` | `<min>` | off | Deathmatch mit Zeitlimit (z.B. `-timer 10`). |
| `-avg` | — | off | Austin Virtual Gaming: zeigt IP-Adresse für Matchmaking. |
| `-warp` | (s.o.) | `1 1` | |
| `-loadgame` | `<slot>` | — | Lädt gespeichertes Spiel. |
| `-record` | `<lmp>` | — | Nimmt Demo auf in `<lmp>.lmp`. |
| `-playdemo` | `<lmp>` | — | Spielt Demo ab. |
| `-timedemo` | `<lmp>` | — | Spielt Demo mit Benchmark-Output (fps nach Beenden). |
| `-maxdemo` | `<n>` | `8` | Maximale Demo-Größe in Sekunden. |
| `-fastdemo` | — | off | Schnelle Demo-Aufzeichnung (überspringt Frames). |

### Network-Modi (alle single-process in aidoom)

| Flag | Parameter | Default | Wirkung |
|------|-----------|---------|---------|
| `-net` | — | off | Hostet ein LAN-Spiel (server). |
| `-server` | — | off | Wie `-net`. |
| `-port` | `<n>` | `5029` | UDP-Port für Netplay. |
| `-connect` | `<ip>` | — | Client-Modus: verbindet zu IP als Slave-Knoten. |
| `-netclient` | `<ip>` | — | Wie `-connect`. |
| `-netplayers` | `<n>` | `4` | Maximale Anzahl Spieler. |
| `-dup` | `<n>` | `1` | Duplication-Faktor für `-net`-Modus. |
| `-extratic` | `<n>` | `1` | Extra-Tics zwischen Netzwerk-Updates. |
| `-wart` | `<lmp>` | — | "Warp To": Demo-Aufzeichnung die zum angegebenen Level springt. |
| `-cdrom` | — | off | CD-ROM-Modus (deaktiviert Datei-Save, schreibt in /cdrom/cdprocd). |

### Chocolate-/Crispy-DOOM-spezifisch (Netzwerk-Compatibility)

| Flag | Parameter | Default | Wirkung |
|------|-----------|---------|---------|
| `-querychoc` | — | off | Chocolate DOOM Server-Discovery. |
| `-chocsyn` | — | off | Chocolate DOOM Sync-Mode. |

### Vanilla-Development-Modi

| Flag | Wirkung |
|------|---------|
| `-shdev` | Development-Mode: Shareware-WAD-Pfade, kein Music-Copyright-Check. |
| `-regdev` | Development-Mode: Registered-WAD-Pfade. |
| `-comdev` | Development-Mode: Commercial-WAD-Pfade. |
| `-devparm` | Enable Developer-Mode (Cheat-Codes, fps-Anzeige). |
| `-debugfile` | Aktiviert Debug-Output in `debugfile` (Default: `debug.txt`). |
| `-statcopy` | `<file>` | Externe Stat-Copy für Statistik-Tools. |
| `-config` | `<file>` | Alternativer Pfad zur Config-Datei (statt `aidoom.cfg`). |

## aidoom-spezifisch

### Video/Auflösung

| Flag | Parameter | Default | Wirkung |
|------|-----------|---------|---------|
| `-1` | — | — | Resolution Scale = 1 (320x200). |
| `-2` | — | — | Resolution Scale = 2 (640x400). |
| `-3` | — | — | Resolution Scale = 3 (960x600). |
| `-4` | — | — | Resolution Scale = 4 (1280x800). |
| `-render` | `<n>` | aus cfg | Resolution Scale 1..7 (auf 1..7 geclampt). Überschreibt `-1..-4`. |
| `-fullscreen` | — | aus cfg | Vollbild-Modus erzwingen. |
| `-nomouse` | — | off | Maus-Input deaktivieren. |
| `-nograb` | — | off | Maus-Grab deaktivieren (Maus kann Fenster verlassen). |
| `-nodraw` | — | off | Kein Rendering (Benchmark/Headless-Modus). |
| `-noblit` | — | off | Kein Buffer-Blit zum Fenster (CPU-only-Test). |

### Audio

| Flag | Parameter | Default | Wirkung |
|------|-----------|---------|---------|
| `-oldmixer` | — | off | Fällt auf den alten Single-Stream-`SDL_OpenAudioDevice`-Mixer zurück. Default ist der neue SDL3-Multi-Stream-Mixer (`i_sound.c`). |

### Monster-AI / LLM-Director

| Flag | Parameter | Default | Wirkung |
|------|-----------|---------|---------|
| `-aidirector` | `<port>` | off | Aktiviert den LLM-Director. Öffnet TCP-Listener auf `<port>` (aidoom.cfg-Default: 31666), erwartet einen Director-Client (`run/director`, SDL3/C) der das Monster-Verhalten steuert. |
| `-aidemo` | — | off | Eingebauter AI-Director ohne Ollama (deterministisches Test-Mode für Director-Protokoll). |
| `-aiplayer` | `[port\|demo]` | off | Volle Agent/LLM-Steuerung des **menschlichen** Spielers. LLM-Modus: TCP-Listener auf `127.0.0.1:<port>` (Default 31700). `-aiplayer demo` = eingebautes Skript-Brain (kein LLM). Siehe `g_agent.c` / `AGENT_CONTROL.md`. |
| `-director` | — | off | **L4D-Style Spawn-Director (regelbasiert, offline, kein LLM).** Misst pro Tic ein Spieler-Stresslevel (0–100) aus: erlittenem Schaden (burst-gewichtet), Close-Quarters-Kills (Nahkampf/Schrotflinte zählt, Snipen kaum) und niedriger Munition. Fährt damit einen Build-up→Peak→Relax-Zyklus: spawnt Extra-Monster **außer Sicht** hinter den Survivors während des Aufbaus, droppt im Relax Items. Deterministisch (P_Random, tic-locked). Läuft *zusätzlich* zu den normalen Map-Monstern (am besten mit `-skill 4`). Die LLM-Variante liest dasselbe Stresslevel via `-aidirector`. |

### Co-op Companion (Buddy)

| Flag | Parameter | Default | Wirkung |
|------|-----------|---------|---------|
| `-coop` | — | off | Aktiviert den regelbasierten Co-op-Buddy (Player 2). Verlangt dass die Map ein `Player_2_Start`-Thing hat; sonst WARNING und Buddy disabled. Single-Player only. |
| `-aicoop` | — | off | Aktiviert den AI-gesteuerten Companion (Player 2): die regelbasierte Basis **plus** den LLM-Director. Öffnet den AI-Transport (TCP, `-aidirector`-Port oder Default 31666), exponiert den Buddy im `observe`-Stream und nimmt `buddy order=…`-Befehle an. Der Director (`run/director`) setzt damit pro Zyklus die Buddy-Taktik (engage/defend/hold/regroup/retreat/grab); ohne Director-Verbindung läuft der Buddy autonom (regelbasiert). Start am einfachsten via `start_aidoom.sh --aicoop`. |
| `-buddyreact` | `<tics>` | 0 | Reaktionszeit/Skill des Buddys: Verzögerung (in Tics, 35/s) zwischen dem Erblicken eines **neuen** Ziels und dem ersten Schuss. `0` = frame-perfekt (altes Verhalten), `~14` ≈ menschliche ~0,4 s, höher = träger. Beeinflusst nur das Eröffnen des Feuers, nicht das Zielen/Navigieren. |
| `-nofriendlyfire` (alias `-noff`) | — | off | Friendly-Fire-**Schutz**: Spieler und AI-Buddy können sich **nicht** gegenseitig verletzen. Default aus = Vanilla-Co-op (sie können). |

### Monster-Infight

| Flag | Parameter | Default | Wirkung |
|------|-----------|---------|---------|
| `-infight` | — | off | Monster-Projektile treffen auch die **eigene** Spezies → Same-Species-Infighting an. (Hieß früher `-friendlyfire`.) Default aus = Vanilla. |
| `-autoaim` | — | off | **Stellt das vertikale Vanilla-Autoaim wieder her.** Per Default ist Autoaim für den Menschen **aus**: Schüsse (Hitscan + Projektile) gehen exakt entlang des Freelook-Pitch ("shoot where you look") — so kann man gezielt platzieren / Headshots setzen. Mit `-autoaim` rastet der Schuss wie in Vanilla vertikal auf ein Ziel ein. Der AI-Buddy behält Autoaim immer. (`autoaim` in `p_pspr.c`/`p_mobj.c`.) |
| `-infinitetall` | — | off | **Zurück auf Vanilla "infinitely tall actors".** Per Default ist over/under-3D-Clipping **an** (`over_under`, `p_map.c`): man läuft unter fliegenden Monstern durch und kann auf Dingen stehen. `-infinitetall` schaltet das ab (jede x/y-Überlappung blockiert, egal wie hoch). |

### Hilfe

| Flag | Wirkung |
|------|---------|
| `-help` / `-h` / `-?` / `/?` | Gibt die komplette Flag-Übersicht aus und beendet sich. |

**`-coop` und `-aicoop` sind mutually exclusive.** Werden beide zusammen
gesetzt, gibt `P_AICoop_Init` eine Fehlermeldung aus und deaktiviert den
Buddy für die Session. Relaunch mit einem der zwei Flags.

### Co-op Buddy (Display)

Der Buddy hat einen HUD-Overlay der via `show_buddy_hud` Config-Toggle (Default 1)
in `m_misc.c` ein/ausgeschaltet wird. Es gibt **kein** dediziertes CLI-Flag dafür;
nur die Config-Option.

## Beispielaufrufe

```bash
# Vanilla DOOM1, Episode 1, Skill 4, Player 1
./aidoom -iwad doom.wad -warp 1 1 -skill 4

# DOOM2 mit Respawn-Monstern, Deathmatch-Mode
./aidoom -iwad doom2.wad -skill 5 -deathmatch -respawn -timer 10

# aidoom Co-op mit Buddy und LLM-Monster-Director
./aidoom -iwad doom2.wad -coop -aidirector 31666

# Custom WAD überlagert DOOM2, HiRes, VSync aus
./aidoom -iwad doom2.wad -file mymod.wad -3 -infight

# Demo-Aufzeichnung
./aidoom -iwad doom2.wad -record demo1 -warp 1 1 -skill 3

# Net-Client (Spieler 2 verbindet sich zum Host)
./aidoom -iwad doom2.wad -netclient 192.168.1.10
```

## aidoom-Config (aidoom.cfg)

Zusätzlich zu CLI-Flags gibt es noch Config-Keys in `run/aidoom.cfg`:

```
iwad                <file>   # Auto-Load wenn keine -iwad Flag
show_messages       0|1      # Pickup-Messages im HUD
show_buddy_hud      0|1      # Co-op-Buddy-HUD (Top-Bar)
show_inventory_hud  0|1      # (J) Artefakt-Inventar-Readout
screen_resolution   1..7     # Resolution Scale (Default 3)
fullscreen          0|1      # Vollbild-Modus
scale_mode          0|1      # Texture-Scale-Mode (0=NEAREST, 1=LINEAR)
vsync               0|1      # VSync (Default 1)
integer_scale       0|1      # Integer-Scaling zum Fenster
render_backend      <n>      # SDL-Render-Backend-Auswahl
statusbar_style     0|1|2    # 0=vanilla 1=small 2=alt HUD
light_dither        0|1      # Light-Banding weichzeichnen
aspect              0|1|2    # 0=4:3, 1=16:9, 2=16:10  (ersetzt das alte 'widescreen')
autorun             0|1      # Always-Run ohne Hold
sprite_shadows      0|1      # Weiche Objektschatten unter Sprites (Default 1)
key_buddy_come      <code>   # Co-op-Buddy "komm her"   (Default ',' = 44)
key_buddy_attack    <code>   # Co-op-Buddy "angreifen"  (Default '.' = 46)
key_buddy_stay      <code>   # Co-op-Buddy "bleib/halt" (Default '-' = 0x2d; übernimmt die Taste vom Screen-Size-Shortcut)
# + alle key_* für Tasten-Bindings
```

Config wird geladen via `M_LoadDefaults()` in `m_misc.c` (vor Init), gespeichert
via `M_SaveDefaults()` (typischerweise beim Quit).

Die `ollama_*` / `gpu_*` Keys werden **nicht** von der Engine verwaltet, sondern von
den SDL3-Tools (`tools/aidoom_config.c`, `tools/director.c`, `tools/gpumon_sdl.c`)
gelesen/geschrieben; die Engine bewahrt sie beim Speichern nur auf. Defaults der
Config-App: `ollama_host`=`localhost`, `ollama_port`=`11434`,
`ollama_model`=`ministral-3:8b`, `gpu_host`=`localhost`, `gpu_user`=leer.