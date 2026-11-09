/* roam_plat.h -- free-roam multiplayer: the platform layer under the core.
 *
 * After nfsu2-web's runtime/roam_plat.h (MIT License, Copyright (c) 2026
 * th0mk), cut down to what roam_api.c needs.  Web: roam_web.c (a virtual UDP
 * endpoint on the page's /lan switch link).  Elsewhere: roam_null.c (no
 * platform yet: roam_plat_start fails).
 *
 * One endpoint per process.  A network thread receives, feeds the session and
 * runs rc_tick every 10 ms; the game thread pushes the local state and reads
 * the remote poses.  Both hold roam_plat_lock() around every session call.
 */
#ifndef ROAM_PLAT_H
#define ROAM_PLAT_H
#include "roam_core.h"

/* installs s->io.send / io.broadcast and starts the network thread on port;
 * call with the lock free.  0 on success, -1: no platform or no link */
int      roam_plat_start(rc_session *s, uint16_t port);
/* joins the network thread; call with the lock free, after rc_shutdown */
void     roam_plat_stop(void);
void     roam_plat_lock(void);
void     roam_plat_unlock(void);
int64_t  roam_plat_now_us(void);   /* monotonic */
uint32_t roam_plat_random(void);   /* nonzero */

#endif
