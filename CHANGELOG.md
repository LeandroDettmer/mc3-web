# Changelog

What changed for players, newest first: the start page's Changelog button shows this file (the deploy's stage script
copies it next to index.html).

Each day with a release gets one section: `## <date> (<commit>)`, the commit being the last one the release brings, so
the next section covers the commits after it. A second release on the same day adds its lines to that day's section
and moves its commit on. One line per change, in plain text, as `- **Area:** what changed`, written for players: new
things and noticeable improvements first, smaller fixes last. Left out: the server's insides, logs, tests, docs and
the recompilation toolkit's internals. The newest section's date is the release date shown in the page's header.

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
