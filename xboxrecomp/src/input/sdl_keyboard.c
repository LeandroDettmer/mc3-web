/*
 * sdl_keyboard.c -- the keyboard as player 1 on Linux (SDL), as the Windows
 * build already has it (xinput_device.c, RECOMP_KEYBOARD=1).
 *
 * The SDL renderers drained the window's events and dropped them, and
 * xbox_FramebufferKeyDown is a stub returning 0 outside Windows, so on Linux
 * RECOMP_KEYBOARD did nothing: a title waiting on PRESS START could only be
 * passed with a real controller (Midnight Club 3, 2026-10-16). Now the
 * renderers hand key events here and xbox_InputGetState reads them as pad 1.
 *
 * Laid out so the buttons are where their names say and the arrows drive
 * (2026-10-21; the WASD layout of 2026-10-17 put A/B on J/K, which made
 * menus confusing, and left the Switch's L/R unmapped). The d-pad -- songs
 * and map in an MC3 race -- moves to I J K L so the arrows can steer. The
 * Windows build keeps its Z X A S scheme (xinput_device.c):
 *
 *   A B X Y       A B X Y                 Enter / Backspace  START / BACK
 *   left / right  left stick (steer, menus)
 *   up            right trigger (gas) + left stick up (menus; not while
 *                 driving where the title says so: xbox_keyboard_driving)
 *   down          left trigger (brake) + left stick down (likewise)
 *   I J K L       d-pad up / left / down / right
 *   Q / E         white / black (the Switch's L / R; also Shift / Ctrl)
 *   Z / C         left / right thumb click
 *   T F G H or numpad 8 4 2 6           right stick (camera)
 *   Space         A (MC3's handbrake: the Switch's A)
 *   Tab           BACK too (select, reachable while driving)
 *
 * Keys arrive only while the game window has the focus; losing it releases
 * everything, so a key held while switching away does not stay down.
 * Written by the window thread, read by the USB thread, one byte each.
 */
/* Set by a title's own code (see keyboard_stick_y); defined on every
 * platform so the title links where there is no SDL keyboard. */
int (*xbox_keyboard_driving)(void);

#if (!defined(_WIN32) || defined(XBOXRECOMP_SDL_HOST)) && !defined(__SWITCH__)

#include <SDL.h>
#include <stdlib.h>
#include <string.h>

#include "xinput_xbox.h"

static volatile unsigned char s_down[SDL_NUM_SCANCODES];
static volatile Uint32 s_press_at[SDL_NUM_SCANCODES];   /* SDL_GetTicks() | 1 */

/* A tap shorter than a frame or two must still reach the game: events come
 * in once per presented frame (the renderer's pump; in the browser once per
 * requestAnimationFrame of the GL thread), so at 13 fps a 60-150 ms tap's
 * down and up arrived together and the pad never saw it (Remix menus,
 * 2026-11-05). A press counts as held for at least this long. */
#define KEY_MIN_HOLD_MS 120

#ifdef __EMSCRIPTEN__
/* ...but only a tap whose down and up came in the same pump needs all of it.
 * One seen across pumps was already down for a pump (about a game frame);
 * stretching it to 120 ms made every tap longer than the user's, and the
 * Remix's lists (mcUiGroup, sub_0034B870) step again whenever their slide
 * ends with the direction still held: a tap then moved two items
 * (2026-11-14). Such a tap is held KEY_SEEN_MS at least. */
#define KEY_SAME_PUMP_MS 3
#define KEY_SEEN_MS 50
static volatile Uint32 s_hold_ms[SDL_NUM_SCANCODES];

/* Even 50 ms was too long: a human tap is 80-200 ms and a list's slide is
 * shorter, so left/right in the garage still moved two items (2026-11-14,
 * b33e71f). In menus (dir_held) the arrows and I J K L reach the pad as a
 * key repeat: a DIR_PULSE_MS pulse at the press (about two game frames,
 * however short or long the tap), then nothing until the key has been held
 * DIR_REPEAT_MS, then held (the lists scroll at their own pace). The pulse
 * ends only after the pad state was also read twice since the press
 * (s_reads, counted in xbox_sdl_keyboard_state, which the game's
 * XInputGetState merges): below ~15 fps in menus (phones) 70 ms can pass
 * between two reads and the pulse would never be seen. s_dir_at is the
 * up-to-down moment only: the browser's auto-repeat keydowns do not
 * restart it. */
#define DIR_PULSE_MS 70
#define DIR_REPEAT_MS 350
static volatile Uint32 s_dir_at[SDL_NUM_SCANCODES];    /* SDL_GetTicks() | 1 */
#define DIR_PULSE_READS 2
static volatile Uint32 s_reads;                        /* pad-state merges so far */
static volatile Uint32 s_dir_rd[SDL_NUM_SCANCODES];    /* s_reads at the press */
#endif

void xbox_sdl_keyboard_event(int scancode, int down)
{
    if (scancode < 0 || scancode >= SDL_NUM_SCANCODES)
        return;
#ifdef __EMSCRIPTEN__
    if (down && !s_down[scancode]) {
        s_dir_rd[scancode] = s_reads;
        s_dir_at[scancode] = SDL_GetTicks() | 1;
    }
    if (down)
        s_hold_ms[scancode] = KEY_MIN_HOLD_MS;
    else if (s_down[scancode] && SDL_GetTicks() - s_press_at[scancode] >= KEY_SAME_PUMP_MS)
        s_hold_ms[scancode] = KEY_SEEN_MS;     /* before s_down drops: a reader never sees a stale 120 shorten */
#endif
    s_down[scancode] = down ? 1 : 0;
    if (down)
        s_press_at[scancode] = SDL_GetTicks() | 1;
}

void xbox_sdl_keyboard_reset(void)
{
    memset((void *)s_down, 0, sizeof s_down);
    memset((void *)s_press_at, 0, sizeof s_press_at);
#ifdef __EMSCRIPTEN__
    memset((void *)s_dir_at, 0, sizeof s_dir_at);
#endif
}

static int held(int scancode)
{
    Uint32 t = s_press_at[scancode];
#ifdef __EMSCRIPTEN__
    return s_down[scancode] || (t && SDL_GetTicks() - t < s_hold_ms[scancode]);
#else
    return s_down[scancode] || (t && SDL_GetTicks() - t < KEY_MIN_HOLD_MS);
#endif
}

int xbox_sdl_keyboard_down(int scancode)
{
    return scancode >= 0 && scancode < SDL_NUM_SCANCODES && held(scancode);
}

/* One window event, from the renderer's event loop (nv2a_gl, nv2a_vk). */
void xbox_sdl_keyboard_handle(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        xbox_sdl_keyboard_event(e->key.keysym.scancode, e->type == SDL_KEYDOWN);
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            xbox_sdl_keyboard_reset();
        break;
    default:
        break;
    }
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* The page's in-game Settings (web/pre.js mc3SetLive): 0 = RECOMP_KEYBOARD
 * as at start, 1 off, 2 on. Read at every poll. */
static volatile int s_kb_live;
EMSCRIPTEN_KEEPALIVE int *xbox_keyboard_box(void) { return (int *)&s_kb_live; }
#endif

int xbox_sdl_keyboard_enabled(void)
{
    static int on = -1;
#ifdef __EMSCRIPTEN__
    int l = __atomic_load_n(&s_kb_live, __ATOMIC_RELAXED);
    if (l)
        return l == 2;
#endif
    if (on < 0) {
        const char *v = getenv("RECOMP_KEYBOARD");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on;
}

/* Up/down move the left stick as well as the triggers, for menus -- but a
 * title may give stick up/down a job of its own while driving: the MC3
 * Remix raises and pulls in its race camera (2026-10-25), which the gas key
 * then did too. A title can say when the player is driving
 * (xbox_keyboard_driving, set by its own code); then up/down are the
 * triggers alone. RECOMP_KEYBOARD_STICK_Y=0 never moves the stick, =1
 * always does. */
static int keyboard_stick_y(void)
{
    static int mode = -2;
    if (mode == -2) {
        const char *v = getenv("RECOMP_KEYBOARD_STICK_Y");
        mode = v && *v == '0' ? 0 : v && *v == '1' ? 1 : -1;
    }
    if (mode >= 0)
        return mode;
    return !(xbox_keyboard_driving && xbox_keyboard_driving());
}

static int keyboard_menu(void)
{
    return !getenv("RECOMP_KEYBOARD_STICK_Y") && xbox_keyboard_driving
           && !xbox_keyboard_driving();
}

/* A direction key (arrows, I J K L); menu = keyboard_menu(). Driving, and
 * every native build, keep held(). */
#ifdef __EMSCRIPTEN__
static int dir_held(int scancode, int menu)
{
    Uint32 t = s_dir_at[scancode], el;
    if (!menu)
        return held(scancode);
    if (!t)
        return 0;
    el = SDL_GetTicks() - t;
    return el < DIR_PULSE_MS || s_reads - s_dir_rd[scancode] < DIR_PULSE_READS
           || (el >= DIR_REPEAT_MS && s_down[scancode]);
}
#else
#define dir_held(scancode, menu) held(scancode)
#endif

static SHORT axis_of(int neg_held, int pos_held)
{
    return (SHORT)((pos_held ? 32767 : 0) - (neg_held ? 32767 : 0));
}

static SHORT axis(int negative, int positive)
{
    return axis_of(held(negative), held(positive));
}

/* One axis from two key pairs (numpad or letters), clamped to the stick. */
static SHORT axis2(int neg1, int pos1, int neg2, int pos2)
{
    int v = axis(neg1, pos1) + axis(neg2, pos2);
    return (SHORT)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}

/* Merge the keyboard into a state (a real pad's, or an empty one): buttons
 * OR'ed, analog buttons at full pressure, sticks where a key is held. */
void xbox_sdl_keyboard_state(XBOX_INPUT_STATE *s)
{
    WORD b = 0;
    BYTE *an = s->Gamepad.bAnalogButtons;
    SHORT x;
    int menu = keyboard_menu();
    (void)menu;   /* native: dir_held is held */
#ifdef __EMSCRIPTEN__
    s_reads++;
#endif

    if (dir_held(SDL_SCANCODE_I, menu)) b |= XBOX_GAMEPAD_DPAD_UP;
    if (dir_held(SDL_SCANCODE_K, menu)) b |= XBOX_GAMEPAD_DPAD_DOWN;
    if (dir_held(SDL_SCANCODE_J, menu)) b |= XBOX_GAMEPAD_DPAD_LEFT;
    if (dir_held(SDL_SCANCODE_L, menu)) b |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (held(SDL_SCANCODE_RETURN) || held(SDL_SCANCODE_KP_ENTER))
                                        b |= XBOX_GAMEPAD_START;
    if (held(SDL_SCANCODE_BACKSPACE) || held(SDL_SCANCODE_TAB))
                                        b |= XBOX_GAMEPAD_BACK;
    if (held(SDL_SCANCODE_Z))         b |= XBOX_GAMEPAD_LEFT_THUMB;
    if (held(SDL_SCANCODE_C))         b |= XBOX_GAMEPAD_RIGHT_THUMB;
    s->Gamepad.wButtons |= b;

    if (held(SDL_SCANCODE_A) || held(SDL_SCANCODE_SPACE)) an[XBOX_BUTTON_A] = 255;
    if (held(SDL_SCANCODE_B)) an[XBOX_BUTTON_B] = 255;
    if (held(SDL_SCANCODE_X)) an[XBOX_BUTTON_X] = 255;
    if (held(SDL_SCANCODE_Y)) an[XBOX_BUTTON_Y] = 255;
    if (held(SDL_SCANCODE_Q) || held(SDL_SCANCODE_LSHIFT) || held(SDL_SCANCODE_RSHIFT))
        an[XBOX_BUTTON_WHITE] = 255;
    if (held(SDL_SCANCODE_E) || held(SDL_SCANCODE_LCTRL) || held(SDL_SCANCODE_RCTRL))
        an[XBOX_BUTTON_BLACK] = 255;
    /* In menus (a title with a driving hook, not driving) up/down are the
     * stick alone: menus that read triggers too (garage) took two steps. */
    if (!menu) {
        if (held(SDL_SCANCODE_DOWN)) an[XBOX_BUTTON_LTRIGGER] = 255;
        if (held(SDL_SCANCODE_UP))   an[XBOX_BUTTON_RTRIGGER] = 255;
    }

    if ((x = axis_of(dir_held(SDL_SCANCODE_LEFT, menu), dir_held(SDL_SCANCODE_RIGHT, menu))) != 0)
        s->Gamepad.sThumbLX = x;
    if (keyboard_stick_y()
        && (x = axis_of(dir_held(SDL_SCANCODE_DOWN, menu), dir_held(SDL_SCANCODE_UP, menu))) != 0)
        s->Gamepad.sThumbLY = x;
    if ((x = axis2(SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6, SDL_SCANCODE_F, SDL_SCANCODE_H)) != 0)
        s->Gamepad.sThumbRX = x;
    if ((x = axis2(SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_8, SDL_SCANCODE_G, SDL_SCANCODE_T)) != 0)
        s->Gamepad.sThumbRY = x;
}

#endif
