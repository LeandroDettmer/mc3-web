# Changelog

What changed for players, oldest first: the start page's Changelog button shows this file (the deploy's stage script
copies it next to index.html).

Each day with a release gets one section: `## <date> (<commit>)`, the commit being the last one the release brings, so
the next section covers the commits after it. A second release on the same day adds its lines to that day's section
and moves its commit on. One line per change, in plain text, as `- **Area:** what changed`, written for players: new
things and noticeable improvements first, smaller fixes last. Left out: the server's insides, logs, tests, docs and
the recompilation toolkit's internals. The page header always shows the first release, 9 November 2026; each later day adds its section at the end, dated that day.

## 2026-11-09 (a749352)

- **Game:** Midnight Club 3: DUB Edition Remix runs in the browser, with sound, music, movies, saves and keyboard or controller.
- **Online:** Free Roam: cruise a city with other players, who show as cars with name tags and as blips on the map.
- **Online:** Online Races: make or join a race room from the page (invite link, optional password); the game's System Link sees only that room.
- **Online:** Online is the first item of the main menu. Online play keeps its own profile, starting with $2,000,000; your career save is not touched.
- **Online:** The page shows who is playing now and lists the players.
- **Online:** Nicknames are unique among the players online; the page asks for one before Play.
- **Free Roam:** Race from a start point on the map against the AI, then go back to your spot in the cruise.
- **Free Roam:** Challenge a nearby player to a street race: start grid, countdown and the game's own checkpoints.
- **Free Roam:** More cars shown at once (up to 32 with 128 MB of game memory), with your car's tyres and paint.
- **Free Roam:** Garages and race starts show on the maps; away players keep a dimmed blip at their last place.
- **Graphics:** WebGPU renderer, much faster than WebGL (the cruise runs at 30 fps); WebGL stays as the fallback.
- **Graphics:** Widescreen (Wide, 16:9, 21:9, 32:9) shows more to the sides; Stretch fills the screen.
- **Graphics:** Optional 60 fps races (experimental).
- **Phones and TVs:** On-screen touch controls, full screen and landscape on Play.
- **Phones and TVs:** A low-memory mode starts the game on TVs, iPhones and small devices, and a box tells you why when it cannot start.
- **Loading:** The game files are downloaded once and kept in the browser; the rest loads quietly in the background.
- **Page:** New look in the game's colours, a Settings window, a key list by action, a Skip intro option and a saves import for Xbox saves.
- **Page:** Release date and this changelog at the top.

## 2026-11-10 (379853a)

- **Crashes:** after a crash, Restart brings the game back quickly without the intro movies; when the graphics device is lost it restarts with WebGL.
- **Crashes:** a crash in the online Free Roam offers Back to Free Roam: the game goes straight back to the cruise, at the city and spot you were.
- **Crashes:** crashes, freezes and closed tabs now send their log by themselves, so they can be fixed.
- **Free Roam:** joining while another player sits on the start spot no longer puts your car inside theirs.
- **Free Roam:** other players' cars no longer show without tyres.
- **Online:** the players box says Career instead of Elsewhere.
- **Free Roam:** no more invisible cars at the start spot that you could crash into.
- **iPhone:** no more false "Out of memory" box at start.
- **Loading:** a slow first start no longer shows "No picture" too soon.
- **Free Roam:** another player falling into the sea no longer opens the Restart Cruise menu for everyone.
- **Online:** your own nick is never refused when you come back; another player still can't take it.
- **Online:** the player counts update with far fewer requests.
- **iPhone:** Restart waits for the memory of the last run instead of failing; a crash in the Online menu fixed.
- **Free Roam:** the city screen no longer shows the online count, so the game stops asking the server for it.
- **Free Roam:** A on the city screen starts the cruise; the time, weather, pedestrians and traffic screen is skipped.
- **Online:** opening the site in a second tab no longer drops the game tab out of Free Roam (players stopped seeing each other).
- **Free Roam:** no more crash ("Heap overrun") when the cruise loads with many players: a PC now gives the game 128 MB.
- **Free Roam:** no more nameless Eclipses parked at the start.
- **Free Roam:** a player driving near you gets a car even when players in the garage or the menus held the car slots.
- **Free Roam:** every player in your city shows on the map, and the full map (I) lists the players on its side.
- **Firefox:** the game shows a picture again instead of a black screen (it now draws with WebGL there).
- **Free Roam:** no more red Eclipses driving around the city: the cruise had AI racers since the options screen was skipped.
- **Free Roam:** the minimap and the full map show every player of your city, not just the 20 nearest, and their places update every second.
- **Free Roam:** no more freeze when another player's car changes while you cruise.
- **Free Roam:** cars of nearby players come straight from their browser (P2P, with Cloudflare's TURN behind strict routers): smoother, and the server only relays what cannot connect. Settings: "Direct connections (P2P) in Free Roam".
- **Free Roam:** every player in your city is on the map now, not only the 20 nearest; each city has a server of its own.
- **Free Roam:** San Diego only for now, so everyone meets in the same city.

## 2026-11-11 (5be444b)

- **Free Roam:** the cars your device can show now go to the players nearest you: a player driving close by takes the car of one far away (after 3 s, at least 50 m nearer), so phones no longer show far cars while the ones next to you are dim arrows.
- **Free Roam:** street races start with a headlight flash: flash (White, Q) near a player to challenge, flash back to accept; nothing pops up just for being near someone. On phones the White button is now Flash.
- **Free Roam:** street races start on the race's own start grid, side by side and facing the road, and run from its first checkpoint.
- **Free Roam:** players in the same city no longer miss each other after a server hiccup: the page keeps reconnecting to the city.
- **Free Roam:** a notice shows when the connection to the other players is lost, and offers a reload if it does not come back.
- **Saves:** your Remix career profiles now show in Saves with a Download button, and "Import save" takes a profile downloaded from this site (another browser or device); on phones, Download opens the share sheet (Save to Files, AirDrop, Mail).
- **Stability:** fixed two causes of the game freezing for good (a controller timing bug and a sound chip wait, e.g. when the music changes).
- **Stability:** iPhones and iPads start with less memory, which should stop many tabs from closing on their own.
- **Graphics:** phones whose GPU froze the game with WebGPU (Mali, older Adreno) draw with WebGL, and any device where WebGPU froze or was lost uses WebGL from its next start.
- **Loading:** the background download now also fills cars, the city map and the career's movies, so fewer stalls in career and Free Roam.
- **Page:** a "Report a bug" button opens a bug report on GitHub with the details filled in.
- **Online:** the main menu's Online item shows how many players are in Free Roam and loads the San Diego cruise at once, in your profile's car (hold White / Q while picking it for Online Races).
- **Free Roam:** other players' cars show their tyres, not just the rims.
- **Free Roam:** the map shows every player in the city, also while paused (some were missing).
- **Free Roam:** a player shown on the map with no car in your world (away, or past your phone's car limit) has a dimmed blip.
- **Page:** the in-game Settings window holds the important options in Graphics, Free Roam and Controls groups: renderer, resolution, screen shape, stretch, 60 fps, texture filtering, low-memory mode, FPS counter, Players shown, name tags, direct connections, game memory, touch controls. Each says whether it changes live or after a restart, and a Restart button brings the game back without the intro.
- **Free Roam:** players on phones no longer cut others out of view: everyone links to their 32 nearest players (a car 'appearing from nowhere' was a missing link).
- **Loading:** music track changes no longer freeze the game on a first visit: the background download fetches the start of every song first (kept in memory where the browser gives no room, e.g. an incognito tab), and the rest of a song is fetched ahead while it plays.
- **Loading:** the start page's 'Download the whole game now' button is back: the whole disc kept in the browser, no waits for the network in game.
- **Free Roam:** another player riding a motorbike no longer freezes your game for good when their bike appears mid-cruise: it shows as a car until your next cruise load.
- **Free Roam:** street races use the game's own checkpoints at last (the flares, the race arrow and the checkpoint sound, not the page's marker) and run the race's whole course, not just its first 2 km. Both racers need this version.
- **Loading:** with "Skip intro" on, the red legal screen at start is gone too: the game loads behind a "Loading the game…" spinner, a little sooner (the screen no longer takes time from the loading, nor plays its way out). On a first visit most of that wait is the game's data coming over the network.

## 2026-11-12 (9ce8569)

- **Page:** more of the in-game Settings change without a restart: resolution, anisotropic filtering and mipmaps (WebGPU), 60 fps (at once, mid-drive too), Players shown (lower hides the farthest cars at once), keyboard as pad 1, touch controls and their opacity. Renderer, game memory, low-memory mode and texture detail bias still need Restart.
- **Page:** the in-game Settings window is laid out anew: sections on the left (Game, Graphics, Free Roam, Controls, Info; tabs on top on a narrow phone), one short line per option (tap or focus it for the whole text), a quiet live / restart mark, and Restart and Close always at the bottom, Restart lit up once an option that needs it changed. Keyboard works inside it now (Tab, arrows, Enter, Esc).
- **Free Roam:** street races start with both cars facing the course (they stood turned around on the grid).
