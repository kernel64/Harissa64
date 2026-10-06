# Compatibility matrix

One row per game. Until the console path exists (M4), the V2 column records the reference path in h64test (interpreter, RSP LLE, software RDP, M2); the V1 column records what V1 (master, 7eeedf6) does, from the user's console tests and Xenia, for comparison.

Columns (V2): boot, menu, gameplay, audio, textures, framebuffer effects, saves, save states, FPS, pause, reset, 10-minute stability, notes.

| Game | V1 (console unless noted) | V2 |
|---|---|---|
| Super Mario 64 | Plays | M2: title, file select, intro, gameplay in the castle grounds; sound OK (WAV) |
| Mario Kart 64 | Plays, full speed (PAL) since 0.1.6 | M2: menus and a race (Luigi Raceway) |
| The Legend of Zelda: Ocarina of Time | Plays; pause map: long GPU stalls, freeze when closing it (open issue) | M2 (Master Quest): title, file creation, intro cutscene; stops on a cutscene text box (open issue) |
| The Legend of Zelda: Majora's Mask | Plays, some graphic noise | — |
| Star Fox 64 | Boots and renders (Xenia) | — |
| Wave Race 64 | Boots and renders (Xenia) | — |
| F-Zero X | Boots and renders (Xenia) | — |
| 1080 Snowboarding | — | — |
| Mario Tennis | Boots and renders (Xenia) | — |
| Mario Golf | Boots and renders (Xenia) | — |
| Paper Mario | Crashed when control starts; fixed in 7eeedf6 (Xenia), console test pending | — |
| Pokémon Stadium | Boots (Xenia) | — |
| Pokémon Snap | — | — |
| Kirby 64 | Boots and renders (Xenia) | — |
| Yoshi's Story | Boots and renders (Xenia) | — |
| Donkey Kong 64 | Boots and renders (Xenia) | — |
| Banjo-Kazooie | Boots and renders (Xenia) | — |
| Banjo-Tooie | Boots (Xenia) | — |
| Perfect Dark | — | — |
| Jet Force Gemini | — | — |
| Diddy Kong Racing | Boots and renders (Xenia) | — |
| Conker's Bad Fur Day | — | — |
| Super Smash Bros. | Boots and renders (Xenia) | — |
| GoldenEye 007 | Boots and renders (Xenia) | — |
| Mario Party | Black screen after the intro (Rice limitation) | — |
| Rogue Squadron | — | — |
| Battle for Naboo | — | — |
| Resident Evil 2 | — | — |
| Beetle Adventure Racing | — | — |
| World Driver Championship | — | — |
| The World Is Not Enough | Plays; hands fixed; crash when quitting (open issue) | — |
| Mortal Kombat 4 | Slow in fights (11–12 FPS) | — |
| Pilotwings 64 | — | — |
