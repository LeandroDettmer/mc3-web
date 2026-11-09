/* roam_null.c -- free roam's platform outside the browser: none yet.
 * roam_start fails (roam_plat_start returns -1), so native builds keep their
 * behaviour.  A native UDP platform would follow nfsu2-web's roam_bsd.c. */
#ifndef __EMSCRIPTEN__
#include "roam_plat.h"
#include <time.h>

int  roam_plat_start(rc_session *s, uint16_t port){ (void)s; (void)port; return -1; }
void roam_plat_stop(void){}
void roam_plat_lock(void){}
void roam_plat_unlock(void){}
int64_t roam_plat_now_us(void){ return (int64_t)clock() * 1000000 / CLOCKS_PER_SEC; }
uint32_t roam_plat_random(void){ return 1; }
#endif
