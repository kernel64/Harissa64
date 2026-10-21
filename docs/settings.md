# Settings

There are two ways in:

- From the game list, press **Y**. What you change there applies to every game.
- During a game, press **BACK** briefly to pause, then pick **Settings**. Here you can save for all games, or only for the game you're playing. A game's own settings win over the general ones.

Most changes show up straight away. The two marked "next start" only apply the next time you launch a game.

Not sure where to begin? Leave everything as it is. The defaults are what we test with.

## Main page

**CPU (next start)**
How the N64's processor is run.
- *Recompiler*: fast. This is the one you want.
- *Interpreter*: much slower, most games won't be playable. It's only there to compare when a game behaves strangely.

**RSP (next start)**
How the N64's graphics and sound chip is handled.
- *HLE*: fast, recommended.
- *LLE*: closer to the real hardware but a lot slower. Useful if a picture or a sound looks wrong and you want to see whether it's the emulator's fault.

**Audio margin**
How much sound is kept ready in advance, from 50 to 500 ms. A bigger margin means fewer crackles and cuts, a smaller one means the sound follows the action a bit more closely. The default (500 ms) suits most games. If you hear crackling, don't lower it.

**Show FPS**
Shows the speed in a corner of the screen. 60 means full speed (50 for European games).
- *FPS*: the speed only.
- *FPS, CPU, RAM*: also how busy the emulator is (CPU, in %) and how much of the console's memory is in use.
- *FPS, CPU, RAM, resolution*: also the resolution the game is drawn at, and the N64's own picture size.

**Graphics** and **Controls** open their own pages, described below.

**Language**
The language of the menus: English, Spanish, German, French, Arabic, Portuguese (Brazil) or Russian. Left and right change it straight away. From the game list, A opens the list with the flags. The choice is kept for every game, without saving. Game names and game info stay as they are (mostly in English), and so does the About page.

These two are only there when you open the settings from the game list:

**Box art downloads**
*On* (the default): when you select a game that has no box art or info yet, Harissa64 fetches them from the internet, once, and keeps them in `art\games`. *Off*: it only shows what is already in that folder. If the console has no internet, nothing bad happens either way: the game just shows its initials.

**Get box art for all games**
Downloads the box art and info of every game in your list in one go. You can keep browsing while it works: the progress shows at the bottom right. At the end a message says how many games got a picture. Games it couldn't find before are tried again. Without internet, a message tells you so and nothing changes. On a PC, `tools/fetch_game_art.py` does the same thing (see the README).

**Save for all games / Save for this game only**
Keeps your changes for next time. If you leave without saving, they last until you quit the game.

## Graphics

**Preset**
A quick way to set the look in one go. It changes the texture filter, smoothing, sharpening, blur and screen effect, but leaves the resolution and brightness alone.
- *Original*: the plain N64 picture, with crisp pixels.
- *N64 Enhanced*: smoother textures and a touch of sharpening. A good everyday choice.
- *N64 Smooth*: the same, with light scanlines.
- *N64 CRT*: looks like an old TV, with scanlines and a soft glow.

If you change one of those settings yourself, the preset shows *Custom*.

**Resolution**
How many pixels the games are drawn with. More pixels give a sharper picture but ask more of the console. The resolution also sets the shape of the picture.
- *Native 320x240*: the size the N64 used. Blocky, but authentic. 4:3.
- *x2 640x480*: twice as sharp. 4:3.
- *x3 960x720*: the default, a good balance. 4:3.
- *HD 1280x720 (16:9)*: the sharpest, in widescreen. A very busy game may run slower with it. If so, go back to x3.

With the 4:3 resolutions you get the original shape, with black bars on the sides of a wide TV. With HD 1280x720 the picture fills a wide TV and shows more of the scene on the left and right, without stretching anything. The menus and the on-screen info stay in the middle, where the game expects them. Widescreen works better in some games than others: N64 games were made for 4:3, so some don't draw what's at the far edges, and you may see things pop in or empty strips there. If a game looks wrong in HD, pick x3, or turn on *Stretch to 16:9*.

**Stretch to 16:9**
Pulls the 4:3 picture wider to fill a wide TV, at any resolution. Everything looks a bit wider, but nothing is missing. With HD 1280x720 it replaces the widescreen view, which helps games that look wrong in widescreen.

**Picture size, Picture left/right, Picture up/down**
Fit the picture to your TV. *Picture size* makes it bigger or smaller (100% is the normal size): go a little above 100% if your TV shows black borders around the picture, or below if it cuts the edges. The two others move the picture, if it is not centred on your screen.

**V-Sync**
Off by default. When on, each picture waits for the TV to be ready, so the image doesn't tear (a line cutting through the picture when the camera moves). When a game is late on a picture, that picture is shown at once instead of waiting; you may see a tear on that picture only. Some busy games run slower with it on, so turn it on per game where tearing bothers you. If you never want a tear, even when a game slows down, put `vsync=2` in harissa64v2.ini.

**Anti-flicker**
Off by default. Some N64 games make an effect see-through by drawing it only every other picture (a shadow, a glow, the disc under the character you're looking at in Smash Remix). On an old TV this looked like a soft, see-through effect; on a modern screen it flickers. With this on, each picture is mixed with the one before, so those effects look as they did on an old TV. Fast motion gets a little softer.

**Brightness**
Some games are very dark on a modern TV (Doom 64, for example). This lifts the dark parts of the picture without washing out the bright ones.
- *Normal*: the picture as the console sends it.
- *Lighter*: a bit brighter in the shadows.
- *Much lighter*: for very dark games.

Like the other settings, you can keep it for one game only.

**Texture filter**
How textures look up close.
- *N64 3-point*: the N64's own smoothing, slightly soft. The most faithful.
- *Bilinear*: a bit smoother.
- *Nearest*: no smoothing at all, sharp square pixels.

**Edge smoothing (FXAA)**
Softens the jagged edges of 3D objects. On by default.

**Sharpen**
Brings back some detail after smoothing. *Low* is subtle, *High* is stronger.

**Blur**
Softens the whole picture, for a gentler, old-TV feel. *Soft* or *Strong*.

**Screen effect**
Imitates a type of screen.
- *None*: a clean picture.
- *Scanlines*: the dark lines of an old TV.
- *Light scanlines*: the same, more discreet.
- *CRT*: scanlines with a soft glow around bright areas.
- *CRT curved*: the same on a slightly curved screen.
- *LCD grid*: the fine grid of a handheld's screen.

The CRT effects look best with smoothing, sharpening and blur turned off. That's what the *N64 CRT* preset does.

## Controls

This page shows which Xbox 360 button does what on the N64 controller, for each player. A picture of the N64 controller shows the button you're on, and it blinks while it waits for you to press something.

- **LB / RB**: choose the player (1 to 4). Players 2 to 4 are used when those controllers are plugged in.
- **A** on an N64 button: press the Xbox button you want for it.
- **Y**: add another button for the same N64 button (for example, two ways to fire).
- **X**: clear it.
- **Analog stick**: pick what moves the N64 stick: the left stick, the right stick, or the D-pad, or turn it off. With the D-pad, its directions move the stick at full tilt and no longer press the N64 D-pad.
- **Invert**: flips the stick left/right (X), up/down (Y), or both.
- **Dead zone**: how far you need to push the stick before it reacts. Raise it if your character moves on their own.
- **Sensitivity**: how far the N64 stick goes when you push yours all the way.
- **Rumble Pak**: *On* (the default): in the games made for the N64's Rumble Pak, your controllers vibrate. *Off*: no vibration. In games that save on a Controller Pak, player 1 keeps the Controller Pak and the other players get the vibration.
- **Reset to default**: back to the usual layout.

The **BACK** button always stays reserved for the emulator's menu. Your layout is saved like the other settings, for all games or for one game.
