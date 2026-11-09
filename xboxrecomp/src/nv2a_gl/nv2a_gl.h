/*
 * nv2a_gl.h -- OpenGL renderer for the NV2A pushbuffer executor.
 *
 * Call nv2a_gl_install() before the title starts rendering, with the
 * executor enabled (RECOMP_PB_EXEC). The window and GL context are created
 * on the executor's thread on first use.
 */
#ifndef NV2A_GL_H
#define NV2A_GL_H

#include <stdint.h>

void nv2a_gl_install(void);

/* A static screen is up (the title is loading behind it): compile the
 * remembered shader programs now, a few a frame, when they were not
 * compiled at start-up (no program binaries). Any thread. */
void nv2a_gl_prewarm_start(void);

/* Hold the 4:3-to-16:9 fill (RECOMP_GL_FILL) off while > 0: +1 when a movie
 * that should keep its original framing starts, -1 when it ends. */
void nv2a_gl_fill_hold(int delta);

/* Movie frames upscaled on the GPU: the planes of the frame about to be
 * copied into the title's A8R8G8B8 texture at va (4:2:0, w x h), with the
 * colour matrix (R, G, B rows over Y, U, V, 1; all 0..1) and the constant
 * alpha the title's own copy writes. Any thread. */
void nv2a_gl_movie_frame(uint32_t va, uint32_t w, uint32_t h,
                         const uint8_t *y, int ys, const uint8_t *u, int us,
                         const uint8_t *v, int vs, const float m[12], float alpha);
void nv2a_gl_movie_end(uint32_t va);
int  nv2a_gl_movie_shown(uint32_t va);

#endif
