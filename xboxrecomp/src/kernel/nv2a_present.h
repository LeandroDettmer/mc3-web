/* nv2a_present.h -- where the title's picture goes on the host screen.
 *
 * Shared by nv2a_gl and nv2a_vk so both present alike.
 *
 * An Xbox title renders into a 4:3-sized buffer (640 or 720 x 480) whether
 * the TV is 4:3 or 16:9; told the TV is widescreen (kernel_xbox.c, on unless
 * RECOMP_WIDESCREEN=0) it renders that buffer anamorphic and the TV
 * stretches it. So: widescreen and narrower than 16:9 -> shown at 16:9. The
 * old test, "narrower than 3:2", missed 720x480 (exactly 3:2) and a surface
 * whose pitch is wider than what is drawn (Midnight Club 3: 768-pixel pitch,
 * 720 drawn), so MC3 came out at 1.6:1 with 48 unused columns on the right.
 * lw is therefore the drawn width (GlSurf/VkSurf vw), not the pitch.
 *
 * RECOMP_GL_ASPECT=fill stretches to the whole window instead (any shape).
 */
#ifndef NV2A_PRESENT_H
#define NV2A_PRESENT_H

#include <stdlib.h>
#include <string.h>

int xbox_video_widescreen(void);

/* The picture's rectangle on a ww x wh screen, for a logical (title-pixel,
 * anti-aliasing removed) picture of lw x lh: x, y, width, height. */
static inline void nv2a_present_rect(float lw, float lh, int ww, int wh, int r[4])
{
    static int fill = -1;
    float scale;

    if (fill < 0) {
        const char *e = getenv("RECOMP_GL_ASPECT");
        fill = e && !strcmp(e, "fill");
    }
    if (fill || lw <= 0.0f || lh <= 0.0f) {
        r[0] = 0; r[1] = 0; r[2] = ww; r[3] = wh;
        return;
    }
    if (xbox_video_widescreen() && lw * 9.0f < lh * 16.0f - 1.0f)
        lw = lh * 16.0f / 9.0f;
    scale = (float)ww / lw < (float)wh / lh ? (float)ww / lw : (float)wh / lh;
    r[2] = (int)(lw * scale);
    r[3] = (int)(lh * scale);
    r[0] = (ww - r[2]) / 2;
    r[1] = (wh - r[3]) / 2;
}

#endif
