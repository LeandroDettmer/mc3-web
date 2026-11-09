/*
 * autostart.c -- MC3_AUTOSTART=cruise: from boot straight into Arcade, Cruise.
 *
 * Input like a player's on pad 1, the way MC3_SKIP_MOVIES presses through
 * the intro movies: one press at a time, each once its screen is up, so
 * the game's own menus do the work. The screens are told apart by the
 * Flash files the front end opens (the kernel's path hook):
 *
 *   flash\bg_savegame.xbck      title (PRESS START) and the profile choice
 *   UDATA\<title>\ listed       the profile screen filling its list
 *   *.mc3* read                 a profile exists (read at boot already)
 *   flash\bg_main.xbck          main menu
 *   flash\bg_arcade.xbck        Arcade
 *   flash\loadscreen_arcade*    the cruise loading
 *
 * Any other bg_* / loadscreen_* means a wrong turn: it stops. The title's
 * frame hook calls mc3_autostart_frame() once a game frame, with whether
 * the state machine is in a race or cruise (which ends it) and how many
 * Downs the main menu needs to reach Arcade. Input of the player's own
 * (host pad, keyboard) ends it at once.
 *
 * The way (Remix, a keyboard run in the browser, 2026-11-05): title Start;
 * profile A (the first profile is highlighted); main menu Career, Tokyo
 * Challenge, Arcade: Down x2, A; Arcade (San Diego, Cruise, 1 player,
 * stock car: the defaults) A; time of day / weather / traffic A; Start A.
 * With no profile the game asks to make one: left to the player (it stops
 * and says so); from the next boot it goes through.
 *
 * Presses are held 3 game frames and at least 150 ms (the menus lost
 * shorter ones, sdl_keyboard.c), and wait for the front end to go quiet
 * (no path asked for 1 s) and the screen's own transition (3 s / 30
 * frames after its file opened). The menus move with the d-pad.
 *
 * MC3_AUTOSTART=freeroam (Remix, a test hook for free roam): the same way,
 * but one Down more on the main menu (Online, the item after Arcade), A on
 * its screen (flash\bg_network.xbck: Free Roam is the first item), then
 * Arcade's setup screens as above. MC3_AUTOSTART=freeroam:<city> (atlanta,
 * detroit, tokyo; sandiego the default) presses Right that many times on
 * Arcade's first screen (its city, San Diego first) before its A; each
 * city is its own free-roam room ('[ROAM] room <city>').
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

extern void (*xbox_path_hook)(const char *xbox_path);
extern void usb_gamepad_set_held(int dev, uint8_t digital, uint8_t analog);
extern int  usb_gamepad_host_input(int dev);

#define PAD_DOWN  0x02u     /* digital: d-pad down */
#define PAD_RIGHT 0x08u     /* digital: d-pad right */
#define PAD_START 0x10u     /* digital */
#define PAD_A     0x01u     /* analog: A */

enum {
    SEEN_TITLE    = 1 << 0,
    SEEN_PROFILES = 1 << 1,
    SEEN_PROFILE  = 1 << 2,
    SEEN_MAIN     = 1 << 3,
    SEEN_ARCADE   = 1 << 4,
    SEEN_LOADING  = 1 << 5,
    SEEN_WRONG    = 1 << 6,
    SEEN_NETWORK  = 1 << 7,
};
enum { ST_BOOT, ST_TITLE, ST_PROFILE, ST_MAIN, ST_NETWORK, ST_ARCADE, ST_LOADING, ST_DONE };
static const char *const k_stage[] = { "boot", "title", "profile", "main menu", "Online", "Arcade", "loading", "done" };

static int s_seen;                  /* SEEN_*, set from any thread */
static int s_freeroam;              /* MC3_AUTOSTART=freeroam: through Online, Free Roam */
static int s_city;                  /* freeroam:<city>: Rights on Arcade's city (0 San Diego .. 3 Tokyo) */
static uint32_t s_path_ms;          /* when the last path was asked for */
static char s_wrong[48];

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void on_path(const char *p)
{
    const char *base = p, *q;
    size_t n = strlen(p);
    int bit = 0;

    for (q = p; *q; q++)
        if (*q == '\\' || *q == '/')
            base = q + 1;
    if (!strcasecmp(base, "bg_savegame.xbck"))
        bit = SEEN_TITLE;
    else if (!strcasecmp(base, "bg_main.xbck"))
        bit = SEEN_MAIN;
    else if (!strcasecmp(base, "bg_arcade.xbck"))
        bit = SEEN_ARCADE;
    else if (!strncasecmp(base, "loadscreen_arcade", 17))
        bit = SEEN_LOADING;
    else if (s_freeroam && !strcasecmp(base, "bg_network.xbck"))
        bit = SEEN_NETWORK;
    else if (!strncasecmp(base, "bg_", 3) || !strncasecmp(base, "loadscreen_", 11)) {
        bit = SEEN_WRONG;
        snprintf(s_wrong, sizeof s_wrong, "%s", base);
    } else if (strstr(p, "\\UDATA\\") && n && p[n - 1] == '\\')
        bit = SEEN_PROFILES;
    else if (strstr(base, ".mc3"))
        bit = SEEN_PROFILE;
    __atomic_store_n(&s_path_ms, now_ms(), __ATOMIC_RELAXED);
    if (bit)
        __atomic_fetch_or(&s_seen, bit, __ATOMIC_RELAXED);
}

int mc3_autostart_on(void)
{
    static int on = -1;
    if (on < 0) {
        static const char *const cities[] = { "sandiego", "atlanta", "detroit", "tokyo" };
        const char *e = getenv("MC3_AUTOSTART");
        s_freeroam = e && !strncasecmp(e, "freeroam", 8) && (!e[8] || e[8] == ':');
        if (s_freeroam && e[8]) {
            s_city = -1;
            for (int i = 0; i < 4; i++)
                if (!strcasecmp(e + 9, cities[i]))
                    s_city = i;
            if (s_city < 0) {
                fprintf(stderr, "[AUTOSTART] MC3_AUTOSTART=%s: city unknown (sandiego, atlanta, detroit, tokyo)\n", e);
                s_freeroam = 0;
            }
        }
        on = e && (!strcasecmp(e, "cruise") || s_freeroam);
        if (e && *e && strcmp(e, "0") && !on)
            fprintf(stderr, "[AUTOSTART] MC3_AUTOSTART=%s: unknown, only \"cruise\" or \"freeroam\"\n", e);
        if (on) {
            xbox_path_hook = on_path;
            fprintf(stderr, "[AUTOSTART] pressing the way to %s on pad 1 (MC3_AUTOSTART);"
                    " any input of yours stops it\n", s_freeroam ? "Online, Free Roam" : "Arcade, Cruise");
        }
    }
    return on;
}

static int s_stage = ST_BOOT;
static uint32_t s_stage_ms, s_press_ms, s_release_ms;
static unsigned s_stage_frames, s_press_frames, s_release_frames;
static int s_holding, s_presses, s_downs;

static void finish(const char *why)
{
    if (s_holding)
        usb_gamepad_set_held(0, 0, 0);
    s_holding = 0;
    s_stage = ST_DONE;
    xbox_path_hook = NULL;
    fprintf(stderr, "[AUTOSTART] %s\n", why);
}

static void press(uint8_t digital, uint8_t analog, uint32_t now)
{
    usb_gamepad_set_held(0, digital, analog);
    s_holding = 1;
    s_press_ms = now;
    s_press_frames = 0;
    s_presses++;
}

/* The screen has had its transition, the front end is quiet and the last
 * press is long enough ago. */
static int ready(uint32_t now, uint32_t settle_ms, uint32_t again_ms)
{
    return !s_holding
        && now - s_stage_ms >= settle_ms && s_stage_frames >= 30
        && now - __atomic_load_n(&s_path_ms, __ATOMIC_RELAXED) >= 1000
        && s_release_frames >= 5 && now - s_release_ms >= again_ms;
}

void mc3_autostart_frame(int in_game, int downs_to_arcade)
{
    uint32_t now;
    int seen, to;

    if (s_stage == ST_DONE || !mc3_autostart_on())
        return;
    now = now_ms();
    if (usb_gamepad_host_input(0)) {
        finish("stopped: input from you");
        return;
    }
    if (in_game) {
        finish(s_stage == ST_LOADING ? "in Cruise" : "in game: stopped");
        return;
    }

    s_stage_frames++;
    s_press_frames++;
    s_release_frames++;
    if (s_holding && s_press_frames >= 3 && now - s_press_ms >= 150) {
        usb_gamepad_set_held(0, 0, 0);
        s_holding = 0;
        s_release_ms = now;
        s_release_frames = 0;
    }

    seen = __atomic_load_n(&s_seen, __ATOMIC_RELAXED);
    if (seen & SEEN_WRONG) {
        char msg[96];
        snprintf(msg, sizeof msg, "stopped: %s opened, not the way to Cruise", s_wrong);
        finish(msg);
        return;
    }
    if (s_freeroam)
        downs_to_arcade++;         /* Online: the item after Arcade */
    to = seen & SEEN_LOADING ? ST_LOADING : seen & SEEN_ARCADE ? ST_ARCADE
       : seen & SEEN_NETWORK ? ST_NETWORK : seen & SEEN_MAIN ? ST_MAIN : s_stage;
    if (to == s_stage && s_stage == ST_BOOT && (seen & SEEN_TITLE))
        to = ST_TITLE;
    if (to == s_stage && s_stage == ST_TITLE && s_presses
        && ((seen & SEEN_PROFILES) || s_presses >= 2))
        to = ST_PROFILE;
    if (to != s_stage) {
        s_stage = to;
        s_stage_ms = now;
        s_stage_frames = 0;
        s_presses = 0;
        s_downs = 0;
        fprintf(stderr, "[AUTOSTART] %s\n", k_stage[to]);
    }

    switch (s_stage) {
    case ST_TITLE:                  /* Start; the profile list follows */
        if (ready(now, 3000, 5000)) {
            if (s_presses >= 3) {
                finish("stopped: the title did not take Start");
                return;
            }
            __atomic_fetch_and(&s_seen, ~SEEN_PROFILES, __ATOMIC_RELAXED);
            press(PAD_START, 0, now);
        }
        break;
    case ST_PROFILE:                /* A on the first profile; bg_main follows */
        if (!(seen & SEEN_PROFILE)) {
            finish("stopped: no profile yet -- make one in the game; from the next boot it starts by itself");
            return;
        }
        if (ready(now, 2000, 6000)) {
            if (s_presses >= 4) {
                finish("stopped: the profile screen did not open the main menu");
                return;
            }
            press(0, PAD_A, now);
        }
        break;
    case ST_MAIN:                   /* Down to Arcade, A; bg_arcade follows */
        if (s_downs < downs_to_arcade) {
            if (ready(now, 3000, 400)) {
                press(PAD_DOWN, 0, now);
                s_downs++;
            }
        } else if (s_downs == downs_to_arcade) {
            if (ready(now, 3000, 600)) {
                press(0, PAD_A, now);
                s_downs++;          /* past the count: A is pressed once */
            }
        } else if (now - s_press_ms > 20000) {
            finish("stopped: Arcade did not open");
            return;
        }
        break;
    case ST_NETWORK:                /* A on Free Roam: Arcade's setup, no new Flash file */
        if (s_presses && ready(now, 3000, 5000)) {
            __atomic_fetch_or(&s_seen, SEEN_ARCADE, __ATOMIC_RELAXED);
            break;
        }
        if (ready(now, 3000, 6000)) {
            if (s_presses >= 3) {
                finish("stopped: Online did not open Free Roam");
                return;
            }
            press(0, PAD_A, now);
        }
        break;
    case ST_ARCADE:                 /* the city (freeroam:<city>), then A through the setup screens */
        if (!s_presses && s_downs < s_city) {
            if (ready(now, 3000, 400)) {
                press(PAD_RIGHT, 0, now);
                s_downs++;
                s_presses = 0;      /* the Rights do not count as setup presses */
            }
            break;
        }
        if (ready(now, 3000, 3000)) {
            if (s_presses >= 6) {
                finish("stopped: Arcade did not start the cruise");
                return;
            }
            press(0, PAD_A, now);
        }
        break;
    case ST_LOADING:                /* nothing to press: wait for the cruise */
        if (now - s_stage_ms > 300000)
            finish("stopped: the cruise did not start");
        break;
    }
}
