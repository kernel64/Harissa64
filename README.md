# Harissa64

Harissa64 is a Nintendo 64 emulator for the Xbox 360. It runs on modded consoles (RGH or JTAG) and starts like any other homebrew, from Aurora, FreeStyle Dash or another dashboard. The name comes from harissa, the Tunisian chili paste.

It's a hobby project, written from scratch, and this is its first release. A lot of games already run at full speed, a few are still slow in busy scenes, and most of the library hasn't been tried yet. The [games list](docs/games.md) shows what we tested and how fast it runs.

Use it only with games you own. No ROMs are included, and none will be.

## What you need

- An Xbox 360 with RGH or JTAG and a dashboard such as Aurora.
- A controller. Up to four players are supported.
- Your N64 games as `.z64`, `.n64`, `.v64` or `.zip` files.

## Installing

1. Copy the Harissa64 folder (with `default.xex` inside) to your console, for example to `Hdd1:\Emulators\Harissa64\`.
2. Make a `roms` folder next to the `.xex` and put your games in it.
3. Scan the folder in your dashboard, then start Harissa64. Pick a game in the list and press A.

The emulator remembers the last game you picked. In Aurora it shows up as "Harissa64" with a pepper icon. A cover and a background are in the `art` folder: in Aurora, open the game's menu, go to the asset manager and load them from there.

## Controls

| Xbox 360 | N64 |
|---|---|
| Left stick | Analog stick |
| A / B | A / B |
| X, Y, LT or RT | Z |
| LB / RB | L / R |
| Right stick | C buttons |
| D-pad | D-pad |
| START | START |

While playing:

- **Short press on BACK**: pause and open the menu (save or load a state, settings, reset, back to the game list, quit).
- **BACK + RB**: save a state. **BACK + LB**: load it.
- **BACK + D-pad left/right**: change the state slot (1 to 9).
- **Hold BACK for 3 seconds**: back to the game list.

In the game list, X shows the About page.

## Saves

Your in-game saves are kept automatically, one folder per game, in `saves\<game name> <code>\` next to the `.xex`. Save states go in the same folder. Copy that folder somewhere if you want a backup.

## Settings

Open the menu in a game, then **Settings**. You can save the settings for all games or just for the game you're playing.

The **Graphics** page has four presets:

- **Original**: the plain N64 picture.
- **N64 Enhanced**: smoother textures and a little sharpening.
- **N64 Smooth**: the same with light scanlines.
- **N64 CRT**: looks like an old TV.

You can also pick the resolution (native, x2 or x3), widescreen 16:9 (the 3D view gets wider instead of being stretched; it works better in some games than others), texture filtering, edge smoothing and a few screen effects.

## Good to know

- Some games show a black screen for about 4 seconds when they start. That's the cartridge's own boot check, it's normal.
- The speed shown in the corner (turn it on in the settings) is the N64's frame rate: 60 is full speed, 50 for European games.
- If a game misbehaves, try the other CPU mode in the settings (the interpreter is much slower but useful to compare), and tell us about it.

## Building

You need Visual Studio 2010 and the Xbox 360 SDK. Open `platform/xbox360/harissa64v2.sln` and build the Release configuration for the Xbox 360 platform.

## Credits and licence

Harissa64 is free software under the GPL v2, see [LICENSE](LICENSE). Some parts come from or are based on other open-source projects (ares, ParaLLEl-RDP, mupen64plus, libdragon, GLideN64, zlib and others), and the interface uses the Inter font. [THIRD_PARTY.md](THIRD_PARTY.md) lists them with their licences. Thanks to all of them.

Nintendo 64 is a trademark of Nintendo. This project is not affiliated with or endorsed by Nintendo.
