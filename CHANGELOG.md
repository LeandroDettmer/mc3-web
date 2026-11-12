# Changelog

What changed for players, oldest first: the start page's Changelog button shows this file (the deploy's stage script
copies it next to index.html).

Each day with a release gets one section: `## <date> (<commit>)`, the commit being the last one the release brings.
A second release on the same day adds its lines to that day's section and moves its commit on. One short line per
player-visible change, as `- **Area:** what changed`, most interesting first. No logs, addresses, settings names or
internals. The page header shows the first release (9 November 2026); each later day adds its section at the end.

## 2026-11-09 (a749352)

- **Game:** Midnight Club 3: DUB Edition Remix runs in the browser, with sound, music, movies, saves and keyboard or controller.
- **Online:** Free Roam: cruise a city with other players, who show as cars with name tags and as blips on the map.
- **Online:** Online Races: make or join a race room from the page (invite link, optional password).
- **Online:** Online is the first item of the main menu, with its own profile starting at $2,000,000; your career save is not touched.
- **Online:** The page shows who is playing now. Nicknames are unique among online players.
- **Free Roam:** Race the AI from a start point on the map, then go back to your spot in the cruise.
- **Free Roam:** Challenge a nearby player to a street race, with a start grid, countdown and the game's own checkpoints.
- **Free Roam:** Up to 32 cars shown at once on a PC, with their tyres and paint; garages and race starts show on the maps.
- **Graphics:** WebGPU renderer, much faster than WebGL (the cruise runs at 30 fps); WebGL stays as the fallback.
- **Graphics:** Widescreen (16:9, 21:9, 32:9) shows more to the sides; Stretch fills the screen. Optional 60 fps races (experimental).
- **Phones and TVs:** On-screen touch controls, full screen and landscape on Play, and a low-memory mode for TVs, iPhones and small devices.
- **Loading:** The game files are downloaded once and kept in the browser.
- **Page:** New look, a Settings window, a key list by action, a Skip intro option and Xbox save import.

## 2026-11-10 (379853a)

- **Free Roam:** Cars of nearby players come straight from their browser (P2P): smoother, with the server only relaying what cannot connect. Each city has its own server.
- **Free Roam:** Every player in your city shows on the minimap and the full map (I), which also lists them, updated every second.
- **Free Roam:** A on the city screen starts the cruise at once, in San Diego, where everyone meets.
- **Free Roam:** Fixed crashes when the cruise loads with many players, and freezes when another player's car changes.
- **Free Roam:** Fixed cars inside yours at the start, cars without tyres, invisible cars, and stray AI Eclipses.
- **Free Roam:** Another player falling into the sea no longer opens the Restart Cruise menu for everyone.
- **Crashes:** Restart brings the game back quickly without the intro; a crash in Free Roam offers Back to Free Roam at your city and spot.
- **Firefox:** The game shows a picture again instead of a black screen.
- **iPhone:** No more false "Out of memory" box at start, and Restart works after a crash.
- **Online:** Opening the site in a second tab no longer drops your game out of Free Roam.

## 2026-11-11 (5be444b)

- **Free Roam:** Street races use the game's own checkpoints and run the race's whole course, starting side by side on the race's own grid. Challenge by flashing (White, Q; Flash on phones) near a player, flash back to accept.
- **Free Roam:** The nearest players get the cars your device can show, and everyone links to their 32 nearest, so cars no longer appear from nowhere.
- **Free Roam:** Players on the map with no car in your world have a dimmed blip, also while paused.
- **Free Roam:** A notice shows when the connection to other players is lost, and the page keeps reconnecting.
- **Free Roam:** Another player's motorbike no longer freezes your game; it shows as a car until your next cruise load.
- **Online:** The main menu's Online item shows how many players are in Free Roam and loads the San Diego cruise at once, in your profile's car (hold White / Q for Online Races).
- **Saves:** Your Remix career profiles show in Saves with Download, and Import save takes a profile from another browser or device.
- **Page:** The in-game Settings window holds the important options in Graphics, Free Roam and Controls groups, with a Restart button.
- **Page:** A Report a bug button opens a bug report with the details filled in.
- **Loading:** Music track changes no longer freeze the game on a first visit; the background download fetches cars, the city map and career movies too.
- **Loading:** The "Download the whole game now" button is back.
- **Loading:** With Skip intro on, the red legal screen is gone too, and the game loads behind a spinner.
- **Stability:** Fixed two causes of the game freezing for good; iPhones and iPads start with less memory so tabs stop closing.
- **Graphics:** Phones whose GPU froze with WebGPU (Mali, older Adreno) use WebGL, and any device where WebGPU failed uses WebGL from its next start.

## 2026-11-12 (f5b20be)

- **Game:** Drift mode (Settings: Off, On or Strong): tap the handbrake, or lift off the throttle with the wheel turned, and the back steps out; the throttle holds the angle and counter-steering catches it. Works on every car in Free Roam, races and career; other players see your drift. A Drift button in the top-right bar switches it.
- **Game:** Drift points, with Drift mode on or off: slide hard (20 degrees or more, 50 km/h or more) for at least 1.5 seconds to bank points (more for a wider angle and more speed), chain drifts for up to x5, and hitting a wall or spinning out loses the drift. In Free Roam Online every 10,000 points pays $10,000, kept in your Online profile.
- **Online:** Free Roam races pay off: 1st, 2nd and 3rd win $25,000, $12,000 and $6,000, and wins unlock Nitro, Slingshot, Burnout, Two-wheel driving, In-air control, Zone/Agro/Roar and the Pearlescent and Color-shift paints. Kept in your Online profile.
- **Free Roam:** Map races now have their AI opponents, and players at the same start point can race them together: press White to invite the others.
- **Free Roam:** Street races for up to 8 players: press White (Q, or Flash on touch) to invite every driving player within 200 m (the 7 nearest), and each joins with White within 10 seconds, even while cruising. Players who drive up during those 10 seconds get the invite too. The result lists every racer's place and time.
- **Free Roam:** An Online progress panel beside the players list on the full map shows races won, your latest unlock and the next one.
- **Page:** A Map button next to Settings and Drift opens and closes the full map, with or without touch controls.
- **Page:** More Settings change without a restart (resolution, filtering, 60 fps, Players shown, keyboard, touch controls), in a new layout with keyboard support.
- **Page:** After a new version is published, players get a dismissable notice; only updates that need it reload online players, after a "reloading in 60 s" box.
- **Free Roam:** Street race cars now face the course, and passed checkpoints fade as in the game's own races.
- **Free Roam:** Nearby players show as cars far more reliably: pausing or opening the full map no longer removes their cars, and dropped links no longer leave them as dimmed arrows.
- **Game:** Drift points now show on the left of the screen, and only a crash (wall, car or prop) loses a drift: hard slides and spins no longer do.
- **Online:** Online career (on by default; switch it off in Settings, even in game): your career's city shows the other players driving there, from Free Roam or their own careers, as cars and on the map, and they see you, nick tags over their cars included. Your career, money and saves are untouched; no street races or online money in the career.
- **Free Roam:** On phones, other players' cars now show as their own cars instead of copies of yours.
- **Free Roam:** Race start areas are twice as big (60 m) and win over the street race prompt: with players near, White invites them all into the same race against the AI, even if they are outside the start area.
- **Free Roam:** A busy city no longer freezes the game: past 16 cars on screen (players, slots and career cars together) every car still builds, in Free Roam and the Online career.
- **Free Roam:** Cars are built only for players within about 300 m of you; farther players show as normal blips on the map, so a full city no longer runs out of cars for the players around you.
- **Free Roam:** Players in an AI race, or in an Online career race, now show to the others as a car racing where they are (bright on the map) instead of vanishing; street race invites skip them.
- **Game:** Safari 17 on a Mac no longer crashes a few seconds after Play.
- **Free Roam:** In an AI race or an Online career race on a PC, the other players nearby show as ghost cars: you see them driving and their name tags, but they never get in your way.
- **Free Roam:** The full map's players list now splits your city into Free Roam and Online career players, each with a count (and the start page's players list does the same).
- **Game:** Settings has a Save button: while you drive in the city it saves your career now (as the game does after a race), or in Free Roam your Online money and garage.
