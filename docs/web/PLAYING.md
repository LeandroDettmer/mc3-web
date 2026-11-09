# Playing Midnight Club 3 in the browser

This guide is for playing the browser build. It assumes someone has already built it and is serving it with
`make web-serve` (see [README.md](../../README.md#build-the-browser-version)). The game runs in **Google Chrome**, the
tested browser.

## 1. Start

Open the address the server prints, **http://localhost:3000/** on the machine that serves it.

The start page's left panel is the launcher:

1. Pick the game under **Game**: **DUB Edition** or **Remix**. A disc picker appears when the server has more than one
   disc for that game (for example a USA and a European one).
2. Read the checks under the button. They say what the game needs: the disc on the server, the build, and the
   browser's storage. A check that fails says what to do.
3. Press **Play** (or **Enter**). The game starts in the same tab.

If the status says **No disc**, the server has no readable disc for that game. Stop it, then start it again with the
game's folder: `make web-serve` for the DUB Edition, `make web-serve GAME=remix` for the Remix.

The start page's **About**, **Credits** and **Legal** tabs say what the project is and where its code comes from.

## 2. Options

The right panel has four tabs: **Settings**, **Controls**, **Saves** and **Online**. Settings are saved in this browser
and apply the next time you press **Play**. **Default** leaves the game's own choice.

| Setting | What it does |
| --- | --- |
| Screen shape | 16:9 (the default) or 4:3, the way the game lays out its picture |
| Resolution | The render size, as a multiple of the Xbox's 720×480. 1× is the default; 1.5× gives 1280×720 with square pixels |
| Square pixels in 16:9 | Widens 16:9 frames so the pixels are square, not stretched |
| Fill 4:3 screens | Menus and videos laid out in the middle 4:3 are shown full width (not the boot logos) |
| Anti-aliasing (FXAA) | Smooths edges in the final pass |
| Sharpening | Contrast-adaptive sharpening, from off to 1 (default 0.5) |
| Mipmaps | The game's own mip levels with trilinear filtering |
| Anisotropic filtering | Sharper textures at a slant: 16× (the default) down to off |
| Texture detail bias | Lower is sharper, with more shimmer. The default is −0.25 |
| Skip intro movies | Presses through the Rockstar, Rockstar San Diego and intro movies. Off by default |
| Movie upscale, Movie upscale factor | Upscaling of the movies on the GPU. No effect in the browser yet |
| Language | English (default), Français, Deutsch, Español, Italiano, where the disc has them. "Press Start" stays English. The European discs have all five |
| Real-time races | Below 30 fps a race advances by the time passed, so it keeps its pace on a slow machine |
| Keyboard as pad 1 | Off leaves only controllers |
| Up and Down move the stick | Menus move with the stick. Auto (the default): while driving, when the game says so, the arrows are gas and brake alone. Always or Never force it |
| Per-draw matrices | Per game by default (for the Remix, Off is its default). On or Off |
| Frame rate in the log, Performance report in the log | Diagnostics for the log: the frame rate every 10 seconds, or lock waits and audio pacing every 5 or 10 seconds |

**Advanced: more switches** takes `KEY=VALUE` lines for any `RECOMP_*` or `MC3_*` setting that is not in the table.
These win over the rows above. **Reset all settings** puts everything back to the defaults.

The line **Passed to the game** shows the settings the page will send.

## 3. Controls

The keyboard is pad 1, and the game's own controls sit where the Xbox pad has them. Click the game first so it gets the
keys. Keyboard as pad 1 (Settings) turns it off.

| Keys | Xbox pad |
| --- | --- |
| ← → | Steer (left stick) |
| ↑ | Gas (menus: up) |
| ↓ | Brake, reverse (menus: down) |
| A, B, X, Y | Buttons A, B, X, Y |
| Space | Handbrake (A) |
| Enter | Start |
| Backspace, Tab | Back |
| I, J, K, L | D-pad: songs, map |
| Q, E (or Shift, Ctrl) | White, Black |
| T, F, G, H (or Num 8, Num 4, Num 2, Num 6) | Right stick |
| Z, C | Left and right stick click |

Controllers work through the browser's Gamepad API, as pads 1 to 4 in the order they were connected. A controller
connected after the page opens is picked up within a second. Press a button on it once the game is running, so the
browser shows it to the page. The keyboard stays on top of pad 1.

On a phone or tablet, an on-screen controller shows over the game (Settings, **Touch controls**: Auto, On, Off, and
its opacity; the **Touch** button on the bar turns it on and off). The left half of the picture is a thumb stick
where your thumb lands; Gas and Brake are bottom right with A, B, X and Y (Y looks back) above them; the d-pad is top
left; White, Back, Start and Black along the top. A thumb can slide from Gas to Brake, or between buttons, without
letting go. On a phone, Play goes full screen in landscape (not on an iPhone's Safari: add the page to the Home
Screen instead), and the game runs at 1× resolution and 30 fps unless you pick otherwise. The game needs 2 GB of
memory: older and 32-bit phones can't start it.

To change the keys in the game, use its own **Options**, **Controls**.

## 4. Saves

Saves are kept in this browser's private storage (OPFS), under `UDATA/54540079/`. They stay between visits to the same
address in the same browser. They are not shared between browsers or devices, and clearing the site's data in the
browser removes them.

- **Profiles in this browser** lists the profiles, with **Refresh**.
- **Import Xbox save** takes a profile's `.mc3` from an Xbox and signs it for the port. Memory-unit images (`.gsx`) are
  not imported in the browser yet: use `make import-save FILE=…` on your computer for those.
- PS2 saves can't be used.

If the browser gives the page no private storage (a private window, or an address that is neither `https://` nor
`localhost`), the saves last only until the page closes. The page says so.

## 5. Online

The **Online** tab holds the game's System Link: everyone who plays on the same server would meet as if on one local
network, and find the others' games in the System Link menu.

This is not finished. The browser's link to the server is still in progress. For now the **System Link** switch (the
Remix) only opens the game's System Link menu. **Your name** is kept for the server link; in the game, your profile's
name is used.

Free roam for everyone on one server is planned, without a player limit.

To play with friends from other PCs, the page has to be served over `https://`: the browser runs the game only on a
secure page or on `localhost`.

## 6. Leaving

The page's **Quit** button asks "Quit the game? Anything not saved in the game is lost." and, on confirmation, reloads the
page. Save in the game first.
