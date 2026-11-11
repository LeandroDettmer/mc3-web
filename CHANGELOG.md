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
