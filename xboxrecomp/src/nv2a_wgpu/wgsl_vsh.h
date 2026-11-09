/* wgsl_vsh.h -- NV2A vertex side as WGSL (see wgsl_vsh.c). The module
 * emitters themselves (wgsl_vsh_fixed, wgsl_vsh_program) are declared in
 * wgpu_int.h, the back end's shared header. */
#ifndef WGSL_VSH_H
#define WGSL_VSH_H

#include "wgpu_int.h"

/* The text every vertex module starts with: WGSL_COMMON, the arena loads,
 * nv2a_attr() (attribute fetch through WgVFetch, every NV097 type),
 * nv2a_clip() and the fog equation. The program path (M3) puts its body and
 * main after it, as the fixed path does. */
const char *wgsl_vsh_prelude(void);

#endif /* WGSL_VSH_H */
