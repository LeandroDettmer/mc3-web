/* wgsl_psh.h -- NV2A register combiners as a WGSL fragment module (see
 * wgsl_psh.c). The same prototypes as wgpu_int.h's "wgsl_psh.c (A)" part,
 * for code that needs only these (wgsl_test.c). */
#ifndef WGSL_PSH_H
#define WGSL_PSH_H

#include <stddef.h>
#include <stdint.h>
#include "../nv2a_gl/gl_psh.h"          /* Nv2aPshKey */

void wgsl_psh_key(const uint32_t *regs, Nv2aPshKey *k);
int  wgsl_psh(const Nv2aPshKey *k, const uint8_t tex_dim[4], char *buf, size_t cap);

#endif
