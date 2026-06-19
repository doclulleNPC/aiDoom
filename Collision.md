# Doom Collision: Blockmap vs BSP-Tree

> John Carmack sagt in dem Doc zum Release vom Code von Classic Doom etwas über BSP statt Blockmaps zu benutzen für die Collision Detection? Erkläre.

Ja, deine Erinnerung ist absolut richtig! Wenn John Carmack über das Design von Classic Doom (und später Quake) spricht, reflektiert er oft über eine der Designentscheidungen, die er im Nachhinein anders gelöst hätte: die Kollisionsabfrage (Collision Detection) über die Blockmap statt über den BSP-Tree (Binary Space Partitioning).

Hier ist die genaue Erklärung, was er damit meint, warum er es damals so gemacht hat und warum er es später bereute.

## 1. Die beiden Systeme in Doom

**Der BSP-Tree (für die Grafik):** Doom nutzt einen BSP-Tree, um das Level in voneinander abgetrennte Bereiche (Subsektoren) zu unterteilen. Dadurch weiß die Engine blitzschnell, welche Wände am nächsten zum Spieler stehen und in welcher Reihenfolge sie gezeichnet werden müssen. Der BSP-Tree teilt den Raum geometrisch perfekt auf.

**Die Blockmap (für die Kollision):** Für die Bewegung von Monstern, Spielern und Projektilen nutzte Carmack stattdessen ein simples, separates Gittermodell – die Blockmap. Das gesamte Level wird dabei in Planquadrate von 128×128 Einheiten unterteilt. Jedes Quadrat speichert eine Liste der Wände (Linedefs) und Objekte (Mobjs), die sich darin befinden. Bewegt sich etwas, muss das Spiel nur die Objekte in den direkt umliegenden Quadraten prüfen.

## 2. Warum wurde die Blockmap genutzt?

Anfang der 90er-Jahre (auf 386er- und 486er-PCs) war Rechenleistung extrem knapp. Carmack wählte die Blockmap, weil sie eine extrem billige und schnelle mathematische Abfrage erlaubte. Es ist im Code viel einfacher und performanter zu sagen: "Der Spieler ist in Planquadrat X/Y, prüfe nur die fünf Objekte dort", als sich jedes Mal durch die verschachtelte Baumstruktur des BSP-Trees zu hangeln.

## 3. Carmacks Kritik: Warum die Blockmap ein Fehler war

Als John Carmack den Quellcode von Doom Ende 1997 freigab (und in späteren Texten/Interviews), wies er darauf hin, dass die Trennung von Grafik (BSP) und Kollision (Blockmap) zu massiven Problemen führte:

- **Redundanz und Speicherverschwendung:** Das Spiel musste zwei komplett unterschiedliche Raumstrukturen für dasselbe Level im Speicher halten und berechnen.
- **Berühmte Gameplay-Bugs (Der "All-Ghosts"- und "Blockmap"-Bug):** Da das Gitter starr ist, kam es zu Rundungsfehlern und Logiklücken. Wenn ein Objekt beispielsweise genau auf der Grenze zweier Blöcke stand, passierte es regelmäßig, dass die Kollisionsabfrage versagte. Projektile (wie Schrotflinten-Pellets oder Raketen) flogen dann einfach spurlos durch Gegner oder Wände hindurch (bekannt als Hitscan-Spannweiten-Fehler). In extremen Fällen konnte die Kollision für das ganze Level versagen und alle Monster wurden zu "Geistern" (der All-Ghosts-Effekt).

## Wie er es später gelöst hat (Quake)

Carmack erkannte diesen Fehler recht schnell. Schon bei der Entwicklung von Quake verwarf er das Konzept starrer Blockmaps komplett. In Quake (und der darauffolgenden id-Tech-Engines) wird der BSP-Tree nicht nur für das Rendering der Grafik genutzt, sondern auch direkt für die Kollisionsabfrage verwendet (mithilfe sogenannter Clip Nodes).

Wenn Carmack also sagt, er hätte lieber den BSP-Tree für die Kollision in Doom genutzt, meint er: Hätte er die mathematische Präzision des BSP-Baums von Anfang an auch auf die Bewegung angewendet, wäre Doom vollkommen frei von den typischen "Durch-die-Wand-schießen"- oder "Durch-Gegner-hindurchgleiten"-Bugs gewesen.