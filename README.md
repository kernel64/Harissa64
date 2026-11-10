# Harissa64

Harissa64 is a Nintendo 64 emulator for the Xbox 360. It runs on modded consoles (RGH or JTAG) and starts like any other homebrew, from Aurora, FreeStyle Dash or another dashboard. The name comes from harissa, the Tunisian chili paste.

It's a hobby project, written from scratch. A lot of games already run at full speed, a few are still slow in busy scenes, and most of the library hasn't been tried yet. The [games list](docs/games.md) shows how games run, with the version they were tested on. Add your own results there.

Use it only with games you own. No ROMs are included, and none will be.

## What you need

- An Xbox 360 with RGH or JTAG and a dashboard such as Aurora.
- A controller. Up to four players are supported.
- Your N64 games as `.z64`, `.n64`, `.v64` or `.zip` files.

## Installing

1. Copy the Harissa64 folder (with `default.xex` inside) to your console, for example to `Hdd1:\Emulators\Harissa64\`.
2. Make a `roms` folder next to the `.xex` and put your games in it.
3. Scan the folder in your dashboard, then start Harissa64. Pick a game in the list and press A.

The first time, Harissa64 asks for your language: English, Spanish, German, French, Arabic, Portuguese (Brazil) or Russian. You can change it later in **Settings > Language**.

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

You can change these in **Settings > Controls**: pick an N64 button, press A, then press the Xbox button or stick direction you want for it. Y adds a second button, X clears it. Each player (1 to 4) has their own controls, and you can save them for all games or for one game. BACK is kept for the emulator's menu. Games made for the Rumble Pak make your controllers vibrate; you can turn it off on the same page.

While playing:

- **Short press on BACK**: pause and open the menu (save or load a state, settings, reset, back to the game list, quit).
- **BACK + RB**: save a state. **BACK + LB**: load it.
- **BACK + D-pad left/right**: change the state slot (1 to 9).
- **Hold BACK for 3 seconds**: back to the game list.

In the game list:

- **D-pad left/right** jumps to the previous or next letter.
- **B** adds the game to your favourites, or takes it out. Favourites come first in the list, with a gold dot.
- **X** shows the About page.

### Games on a USB stick or in another folder

In the game list, press **START** to open **Files**. It lists the console's drives: `game:` (the Harissa64 folder), `Hdd1:` (the hard disk), `Usb0:` to `Usb2:` (USB sticks and drives) and `Dvd:`. Only folders and N64 games show up.

- **A** opens a folder, or starts the game.
- **B** goes back up one folder. From the list of drives, it goes back to the game list.
- **Y** makes the folder you're in your game folder: the game list then shows the games from there, every time you start Harissa64.

A USB stick plugged in while Harissa64 is running shows up the next time you open Files.

## Box art and game info

When you move through the game list, the right side shows the game's box art, its real title, year, developer, genre, number of players and a few lines about it.

If your console is online, Harissa64 downloads these by itself the first time you select a game, and keeps them in the `art\games` folder next to the `.xex`. After that they show up straight away, even offline. To get everything in one go, open **Settings** from the game list (Y) and pick **Get box art for all games**. You can turn the downloads off in the same place.

No internet on the console? Run the script on a PC instead (you need [Python 3](https://www.python.org)):

```
python tools/fetch_game_art.py "C:\path\to\your\roms"
```

It fills an `art\games` folder: copy it to the console, into the Harissa64 folder. Games it can't find just show their initials.

The pictures and the text come from [libretro-thumbnails](https://github.com/libretro-thumbnails/Nintendo_-_Nintendo_64), [libretro-database](https://github.com/libretro/libretro-database), [GameDB-N64](https://github.com/niemasd/GameDB-N64) and Wikipedia. They are not part of Harissa64.

## HD textures

Fans have redrawn the textures of some games in high resolution. Harissa64 can use these packs: the game looks sharper, and plays the same.

In the game list, select a game and press **RT**. The page lists the packs for that game, with their author and size, and shows where each one stands:

- **Active** (green): the game uses this pack. The line at the top says so too.
- **Installed**: on the console, not in use.
- **Not downloaded**: press **A** to install it. Harissa64 downloads it from its author's page, checks it and prepares it for the console. A big pack takes several minutes; you can play meanwhile, it waits and goes on afterwards.

On an installed pack, **A** turns it on for this game, or off if it is the active one. **X** removes it, **Y** stops an install.

A pack is downloaded once and serves every game it suits: the other versions of the game (USA, Europe, Japan) and hacks that keep its name or its code.

In a game, **HD textures** in **Settings > Effects** turns the pack on or off at once.

The packs are made by their authors and stay on their pages; Harissa64 only keeps a [list of links](docs/texture-packs.json). To add a pack to the list, open a pull request.

### Adding a pack by hand

For a pack that is not in the list (on Mega, Google Drive, a forum...), or a console without internet, convert the pack on a Windows PC. Everything needed is in the `tools` folder of the Harissa64 zip: `hd_textures.py` and `h64hdconv.exe`. You only need [Python 3](https://www.python.org).

1. Download the pack. A `.zip` can be used as it is; extract a `.7z` or `.rar` first.
2. Find the game's name in the pack: its pictures are named like `SUPER MARIO 64#0A1B2C3D#0#2_all.png`. The part before the first `#` is the name.
3. Open a command prompt in the `tools` folder and run:

   ```
   python hd_textures.py convert "C:\path\to\pack.zip" "SUPER MARIO 64"
   ```

   It makes `hd_textures\SUPER MARIO 64.h64hd`. A big pack takes a minute or two.
4. Copy that file to the console, into the `hd_textures` folder of Harissa64 (create the folder if it is not there): `Harissa64\hd_textures\SUPER MARIO 64.h64hd`.
5. Start the game, open the menu (short press on **BACK**), and turn on **HD textures** in **Settings > Effects**.

The file name must be the game's name as Harissa64 writes it in its `saves` folder, without the 4-letter code at the end: `saves\SUPER MARIO 64 NSME` gives `SUPER MARIO 64.h64hd`. The script names it that way already; rename it only if the game shows no HD textures. One file serves every version of the game (USA, Europe, Japan).

The same script also downloads and converts the packs of the list on a PC: `python hd_textures.py list` shows them, `python hd_textures.py get sm64-reloaded` gets one.

Options: `--out FOLDER` (where the `hd_textures` folder is made), `--max 2048` (keep pictures up to 2048 pixels wide; the default 1024 halves bigger ones, which saves the console's memory).

## Cheats

In a game, open the menu (short press on **BACK**) and pick **Cheats**. The list shows the codes known for that game: turn each one on or off with A or the D-pad. Some have several choices (a character, a level...): left and right go through them. What you turn on is remembered for that game.

- The codes come from `cheats\mupencheat.txt`, which is in the zip. It covers most games.
- Codes that say "Press GS" work while you click the right stick.
- A code can make a game misbehave or freeze. If that happens, turn it off; some need a reset of the game to go away.

To add your own GameShark codes, make a text file in the `cheats` folder, named like the game's save folder (for example `cheats\SUPER MARIO 64 NSME.txt`): a line with the cheat's name, then its codes, one per line.

```
Infinite lives
8033B21D 0064
```

## Saves

Your in-game saves are kept automatically, one folder per game, in `saves\<game name> <code>\` next to the `.xex`. Save states go in the same folder. Copy that folder somewhere if you want a backup.

## Settings

Open the menu in a game, then **Settings**. You can save the settings for all games or just for the game you're playing.

The **Effects** page has four presets:

- **Original**: the plain N64 picture.
- **N64 Enhanced**: smoother textures and a little sharpening.
- **N64 Smooth**: the same with light scanlines.
- **N64 CRT**: looks like an old TV.

You can also pick the resolution (native, x2 or x3 in 4:3, or HD 1280x720 in 16:9 widescreen, where the 3D view gets wider instead of being stretched; it works better in some games than others), texture filtering, edge smoothing and a few screen effects.

Every setting is explained in [docs/settings.md](docs/settings.md).

## Good to know

- Some games show a black screen for about 4 seconds when they start. That's the cartridge's own boot check, it's normal.
- The speed shown in the corner (turn it on in the settings) is the N64's frame rate: 60 is full speed, 50 for European games.
- If a game misbehaves, try the other CPU mode in the settings (the interpreter is much slower but useful to compare), and tell us about it.

## Building

You need Visual Studio 2010 and the Xbox 360 SDK. Open `platform/xbox360/harissa64v2.sln` and build the Release configuration for the Xbox 360 platform.

## Support the project

Harissa64 is free and always will be. It is built in my spare time: every game fixed means hours of tests on a real console. If it gives you good moments with your N64 games and you would like to help it go further, a donation lets me spend more time on it: more games working, better speed, new features.

Any amount helps, and thank you.

### Donate in cryptocurrency

| Currency | Network | Address |
|---|---|---|
| Tether (USDT) | TRON (TRC20) | `TMWW2XeV9kY1U9Cv4AeW9Z6oXnuxXyBut8` |
| Bitcoin (BTC) | Bitcoin | `16GjM9aBDiTEkMyzcMq4sMWpdacc4zWkty` |
| Ether (ETH) | Ethereum | `0xcb9b2f3563249c9508eae26af396f21926ec928c` |
| Solana (SOL) | Solana | `mFxWKAEYnV5oKwYwtzmGCNfvWLNyqfL2FBWH3qSLSHU` |

Send each currency only on the network shown on its line: coins sent on another network are lost. Check the address before you send: a transfer can't be cancelled.

### Other ways to help

- Star the repository on GitHub: it helps other players find it.
- Report a bug in the [issues](https://github.com/kernel64/Harissa64/issues), with a save state if you can: it is the fastest way to get a game fixed.
- Add your results to the [games list](docs/games.md).
- Tell other Xbox 360 players about Harissa64.

## Credits and licence

Harissa64 is free software under the GPL v2, see [LICENSE](LICENSE). Some parts come from or are based on other open-source projects (ares, ParaLLEl-RDP, mupen64plus, libdragon, GLideN64, zlib and others), and the interface uses the Inter and Noto Sans Arabic fonts. [THIRD_PARTY.md](THIRD_PARTY.md) lists them with their licences. Thanks to all of them.

Nintendo 64 is a trademark of Nintendo. This project is not affiliated with or endorsed by Nintendo.
