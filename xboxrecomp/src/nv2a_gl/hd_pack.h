/* hd_pack.h -- replacement textures beside the game (see hd_pack.c). */
#ifndef NV2A_GL_HD_PACK_H
#define NV2A_GL_HD_PACK_H

#include <stdint.h>

int            hd_pack_init(void);       /* 1 when replacing or dumping */
int            hd_pack_has(uint64_t h);
unsigned char *hd_pack_load(uint64_t h, int *w, int *hgt);   /* RGBA8 */
void           hd_pack_free(unsigned char *px);
int            hd_pack_dumping(void);
void           hd_pack_dump(uint64_t h, const uint32_t *argb, int w, int hgt);

/* Background loading: a replacement decoded, mipmapped and DXT-compressed
 * on a worker thread. Request it when the title first uses the texture;
 * poll from the renderer for finished ones, upload, free. */
#define HD_MAX_LEVELS 13
typedef struct HdTex {
    uint64_t       key;
    int            w, h, levels, alpha;     /* alpha: DXT5, else DXT1 */
    int            lw[HD_MAX_LEVELS], lh[HD_MAX_LEVELS];
    unsigned char *data[HD_MAX_LEVELS];
    uint32_t       bytes[HD_MAX_LEVELS];
    struct HdTex  *next;
} HdTex;
void   hd_pack_request(uint64_t key);
HdTex *hd_pack_poll(void);                  /* NULL when none is ready */
void   hd_pack_tex_free(HdTex *t);

#endif
