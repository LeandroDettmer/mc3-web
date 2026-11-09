/* win32_host.c -- the host thread helpers on Windows (XBOXRECOMP_WIN_SDL_GL).
 *
 * On POSIX hosts these live in win32_compat.c, which Windows does not build
 * (it has the real Win32 API). They place threads on cores and feed the
 * Switch's CPU report; on Windows, as on macOS, the scheduler places threads
 * itself, so each is a no-op.
 */
#include <stdio.h>

void xbox_guest_pin(int interrupt)          { (void)interrupt; }
void xbox_nx_raise_host_thread(void)        { }
void xbox_nx_spread_thread(void)            { }
void xbox_nx_track_thread(void *entry)      { (void)entry; }
void xbox_nx_retag_thread(void *entry)      { (void)entry; }
