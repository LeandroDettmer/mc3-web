/*
 * gl_es.c -- the GL 3.3 core renderer on WebGL2 (OpenGL ES 3.0), see gl_es.h.
 *
 * What WebGL2 lacks and how it is made up here, so that nv2a_gl.c keeps a
 * single code path:
 *   - GLSL 3.30 core: glShaderSource rewrites to GLSL ES 3.00
 *     (nv2a_gl_es_source).
 *   - BGRA: texture data given as GL_BGRA is swizzled to RGBA on the CPU
 *     at upload; GL_BGRA read-backs are read as RGBA and swizzled; vertex
 *     arrays with size GL_BGRA (D3DCOLOR) go up as RGBA and the vertex
 *     shader swaps them back (uniform u_es_bgra, a bit per attribute, set
 *     before each draw from what this layer saw of the attribute pointers).
 *   - unsynchronised buffer maps: glMapBufferRange answers NULL, which
 *     every streaming caller already handles with glBufferSubData.
 *   - GL_DEPTH_CLAMP (the vertex shaders clamp z themselves), border clamp
 *     (clamp to edge), GL_TEXTURE_LOD_BIAS (dropped), program binaries
 *     (none in WebGL; the browser caches compiled shaders itself).
 *   - glClearDepth / glDepthRange: the float versions (gl_api.h).
 *   - feedback loops (a draw sampling a texture attached to the draw
 *     framebuffer, a blit within one framebuffer): WebGL2 drops them; the
 *     draw samples a copy, the blit goes through a scratch texture.
 * glDrawElementsBaseVertex, ARB_buffer_storage and 64-bit timer queries
 * resolve to NULL, and the renderer already does without them.
 */
#include "gl_es.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int nv2a_gl_es_unit;            /* not an empty unit off the web */

#if defined(__EMSCRIPTEN__) || defined(NV2A_GL_ES_TEST)   /* the test: gl_es_test.c */

/* ── Shader text ──────────────────────────────────────────────────── */

static const char s_es_head[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp sampler2D;\n"
    "precision highp samplerCube;\n"
    "precision highp sampler3D;\n"
    "precision highp sampler2DShadow;\n";

static int is_ident(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/* Is the identifier tok used anywhere in s other than at skip? */
static int uses(const char *s, const char *tok, const char *skip)
{
    size_t n = strlen(tok);
    const char *p = s;
    while ((p = strstr(p, tok)) != NULL) {
        if (p != skip && (p == s || !is_ident(p[-1])) && !is_ident(p[n]))
            return 1;
        p += n;
    }
    return 0;
}

char *nv2a_gl_es_source(const char *const *src, int n)
{
    static const char v330[] = "#version 330 core\n";
    static const char vmain[] = "void main() {\n";
    const char *var[16] = { 0 }, *body, *after = NULL, *mainp = NULL, *p;
    size_t len = 0, cap;
    char *all, *out, *o;
    unsigned used = 0;
    int i, k;

    for (i = 0; i < n; i++)
        len += strlen(src[i]);
    all = (char *)malloc(len + 1);
    if (!all)
        return NULL;
    for (o = all, i = 0; i < n; i++) {
        size_t l = strlen(src[i]);
        memcpy(o, src[i], l);
        o += l;
    }
    *o = 0;
    if (strncmp(all, v330, sizeof v330 - 1) != 0) {
        free(all);
        return NULL;
    }
    body = all + sizeof v330 - 1;

    /* NV2A vertex shaders: inputs read by the program become copies. */
    for (k = 0; k < 16; k++) {
        char d[48], t[8];
        const char *at;
        snprintf(d, sizeof d, "layout(location = %d) in vec4 v%d;", k, k);
        snprintf(t, sizeof t, "v%d", k);
        if ((at = strstr(body, d)) == NULL)
            continue;
        var[k] = at + strlen(d) - strlen(t) - 1;   /* the name in the declaration */
        if (!uses(body, t, var[k])) {
            var[k] = NULL;                          /* unread: stays as declared */
            continue;
        }
        used |= 1u << k;
        if (!after || at > after)
            after = at;
    }
    if (used) {
        after = strchr(after, '\n');
        mainp = strstr(body, vmain);
        if (!after || !mainp || mainp < after) {
            used = 0;                               /* not the shape expected */
            memset(var, 0, sizeof var);
        } else {
            after++;
            mainp += sizeof vmain - 1;
        }
    }

    cap = sizeof s_es_head + len + 160 + 120 * 16;
    out = (char *)malloc(cap);
    if (!out) {
        free(all);
        return NULL;
    }
    o = out;
    memcpy(o, s_es_head, sizeof s_es_head - 1);
    o += sizeof s_es_head - 1;
    for (p = body; *p; ) {
        int hit = -1;
        if (used) {
            for (k = 0; k < 16; k++)
                if (var[k] == p) { hit = k; break; }
            if (p == after) {
                o += sprintf(o, "uniform int u_es_bgra;   /* D3DCOLOR arrays: bit per input */\n");
                for (k = 0; k < 16; k++)
                    if (used & (1u << k))
                        o += sprintf(o, "vec4 v%d;\n", k);
                o += sprintf(o, "void nv2a_es_in() {\n");
                for (k = 0; k < 16; k++)
                    if (used & (1u << k))
                        o += sprintf(o, "    v%d = (u_es_bgra & %d) != 0 ? nv2a_a%d.bgra : nv2a_a%d;\n",
                                     k, 1 << k, k, k);
                o += sprintf(o, "}\n");
            }
            if (p == mainp)
                o += sprintf(o, "    nv2a_es_in();\n");
        }
        if (hit >= 0) {
            o += sprintf(o, "nv2a_a%d", hit);
            p += hit < 10 ? 2 : 3;                  /* past "vN" */
        } else {
            *o++ = *p++;
        }
    }
    *o = 0;
    free(all);
    return out;
}

#endif /* __EMSCRIPTEN__ || NV2A_GL_ES_TEST */

/* ── WebGL2 layer ─────────────────────────────────────────────────── */
#if defined(__EMSCRIPTEN__)
#include "gl_api.h"

static void *(*s_getproc)(const char *name);

void nv2a_gl_es_set_getproc(void *(*getproc)(const char *name))
{
    s_getproc = getproc;
}

void *nv2a_gl_es_getproc(const char *name)
{
    if (!strcmp(name, "glClearDepth"))
        name = "glClearDepthf";
    else if (!strcmp(name, "glDepthRange"))
        name = "glDepthRangef";
    return s_getproc(name);
}

static PFN_glShaderSource          r_ShaderSource;
static PFN_glPixelStorei           r_PixelStorei;
static PFN_glTexImage2D            r_TexImage2D;
static PFN_glTexSubImage2D         r_TexSubImage2D;
static PFN_glReadPixels            r_ReadPixels;
static PFN_glTexParameteri         r_TexParameteri;
static PFN_glTexParameterf         r_TexParameterf;
static PFN_glEnable                r_Enable;
static PFN_glDisable               r_Disable;
static PFN_glBindVertexArray       r_BindVertexArray;
static PFN_glVertexAttribPointer   r_VertexAttribPointer;
static PFN_glEnableVertexAttribArray  r_EnableVertexAttribArray;
static PFN_glDisableVertexAttribArray r_DisableVertexAttribArray;
static PFN_glUseProgram            r_UseProgram;
static PFN_glLinkProgram           r_LinkProgram;
static PFN_glDeleteProgram         r_DeleteProgram;
static PFN_glGetUniformLocation    r_GetUniformLocation;
static PFN_glUniform1i             r_Uniform1i;
static PFN_glDrawElements          r_DrawElements;
static PFN_glDrawArrays            r_DrawArrays;
static PFN_glActiveTexture         r_ActiveTexture;
static PFN_glBindTexture           r_BindTexture;
static PFN_glDeleteTextures        r_DeleteTextures;
static PFN_glBindFramebuffer       r_BindFramebuffer;
static PFN_glDeleteFramebuffers    r_DeleteFramebuffers;
static PFN_glFramebufferTexture2D  r_FramebufferTexture2D;
static PFN_glFramebufferRenderbuffer r_FramebufferRenderbuffer;
static PFN_glBlitFramebuffer       r_BlitFramebuffer;
static PFN_glClear                 r_Clear;
static PFN_glGenTextures           r_GenTextures;
static PFN_glGenFramebuffers       r_GenFramebuffers;

/* ── Redundant state calls ──
 * Every WebGL call is a trip through JavaScript (validation, then the GPU
 * process's command buffer). nv2a_gl skips framebuffer, vertex array,
 * buffer and viewport binds that change nothing (gl_merge.h), but forgets
 * its texture-unit and render-state caches at every clear, flip, blit and
 * texture eviction (tex_bind_forget, state_dirty) and then sets all of it
 * again. This layer sees every GL call there is (one context, all through
 * the p_gl* table), so its shadow of texture units and enables is always
 * exact: a call that would set what is already set is dropped here.
 * RECOMP_GL_STATECACHE=0 off. */
static int      s_cache = 1;
static uint32_t s_skips;                /* calls dropped since the last callstats */
static uint32_t s_calls;                /* calls into WebGL (RECOMP_GL_CALLSTATS=1) */
static int      s_counting;

#define GL_UNPACK_ROW_LENGTH_ 0x0CF2
#define GL_DEPTH_CLAMP_       0x864F
#define GL_DEPTH_ATTACHMENT_  0x8D00
#define GL_STENCIL_ATTACHMENT_ 0x8D20

static void es_tex_written(GLuint tex);
static void es_tex_specified(GLuint tex, GLsizei w, GLsizei h, GLint ifmt, GLenum fmt, GLenum type);
static GLuint es_bound_tex(void);
static void es_tex_param(GLenum t, GLenum pname, GLint v);

static void GLAPIENTRY es_ShaderSource(GLuint sh, GLsizei n, const GLchar *const *s, const GLint *len)
{
    char *t = len ? NULL : nv2a_gl_es_source(s, n);
    if (!t) {
        r_ShaderSource(sh, n, s, len);
        return;
    }
    r_ShaderSource(sh, 1, (const GLchar *const *)&t, NULL);
    free(t);
}

/* ── BGRA pixels ── */
static GLint s_unpack_row, s_unpack_align = 4;

static void GLAPIENTRY es_PixelStorei(GLenum pname, GLint v)
{
    if (pname == GL_UNPACK_ROW_LENGTH_)
        s_unpack_row = v;
    else if (pname == GL_UNPACK_ALIGNMENT)
        s_unpack_align = v;
    r_PixelStorei(pname, v);
}

static void swap_rb(uint8_t *d, const uint8_t *s, size_t px)
{
    size_t i;
    for (i = 0; i < px; i++, d += 4, s += 4) {
        uint8_t b = s[0];
        d[0] = s[2]; d[1] = s[1]; d[2] = b; d[3] = s[3];
    }
}

/* w x h BGRA texels as the unpack state reads them -> tight RGBA. */
static const void *bgra_to_rgba(const void *px, GLsizei w, GLsizei h)
{
    static uint8_t *buf;
    static size_t cap;
    size_t row = (size_t)(s_unpack_row > 0 ? s_unpack_row : w) * 4, need = (size_t)w * h * 4;
    size_t al = s_unpack_align > 0 ? (size_t)s_unpack_align : 4;
    GLsizei y;
    row = (row + al - 1) / al * al;
    if (need > cap) {
        uint8_t *n = (uint8_t *)realloc(buf, need);
        if (!n)
            return NULL;
        buf = n;
        cap = need;
    }
    for (y = 0; y < h; y++)
        swap_rb(buf + (size_t)y * w * 4, (const uint8_t *)px + (size_t)y * row, (size_t)w);
    return buf;
}

/* Upload tight RGBA with the unpack state set for it, then put it back. */
static void tight_unpack(int on)
{
    if (s_unpack_row)
        r_PixelStorei(GL_UNPACK_ROW_LENGTH_, on ? 0 : s_unpack_row);
    if (s_unpack_align != 4)
        r_PixelStorei(GL_UNPACK_ALIGNMENT, on ? 4 : s_unpack_align);
}

static void GLAPIENTRY es_TexImage2D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b,
                                     GLenum fmt, GLenum type, const void *px)
{
    if (t == GL_TEXTURE_2D && l == 0)
        es_tex_specified(es_bound_tex(), w, h, ifmt == GL_BGRA ? GL_RGBA8 : ifmt,
                         fmt == GL_BGRA ? GL_RGBA : fmt, type);
    if (fmt != GL_BGRA) {
        r_TexImage2D(t, l, ifmt, w, h, b, fmt, type, px);
        return;
    }
    if (ifmt == GL_BGRA)
        ifmt = GL_RGBA8;
    if (!px) {
        r_TexImage2D(t, l, ifmt, w, h, b, GL_RGBA, type, NULL);
        return;
    }
    if (!(px = bgra_to_rgba(px, w, h)))
        return;
    tight_unpack(1);
    r_TexImage2D(t, l, ifmt, w, h, b, GL_RGBA, type, px);
    tight_unpack(0);
}

static void GLAPIENTRY es_TexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h,
                                        GLenum fmt, GLenum type, const void *px)
{
    if (t == GL_TEXTURE_2D)
        es_tex_written(es_bound_tex());
    if (fmt != GL_BGRA || !px) {
        r_TexSubImage2D(t, l, x, y, w, h, fmt == GL_BGRA ? GL_RGBA : fmt, type, px);
        return;
    }
    if (!(px = bgra_to_rgba(px, w, h)))
        return;
    tight_unpack(1);
    r_TexSubImage2D(t, l, x, y, w, h, GL_RGBA, type, px);
    tight_unpack(0);
}

/* The renderer reads back 4-byte texels with GL_PACK_ALIGNMENT 4 and no
 * pack row length: rows are tight. */
static void GLAPIENTRY es_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type,
                                     void *px)
{
    if (fmt != GL_BGRA) {
        r_ReadPixels(x, y, w, h, fmt, type, px);
        return;
    }
    r_ReadPixels(x, y, w, h, GL_RGBA, type, px);
    if (px)
        swap_rb((uint8_t *)px, (const uint8_t *)px, (size_t)w * h);
}

/* ── Texture and fixed state WebGL2 does not have ── */
static void GLAPIENTRY es_TexParameteri(GLenum t, GLenum pname, GLint v)
{
    if (v == GL_CLAMP_TO_BORDER)
        v = GL_CLAMP_TO_EDGE;           /* shortcut: border colour (0,0,0,0) lost; shader border if a title needs it */
    es_tex_param(t, pname, v);
    r_TexParameteri(t, pname, v);
}

static void GLAPIENTRY es_TexParameterf(GLenum t, GLenum pname, GLfloat v)
{
    es_tex_param(t, pname, (GLint)v);
    if (pname == GL_TEXTURE_LOD_BIAS)
        return;                         /* shortcut: RECOMP_GL_LOD_BIAS has no effect on WebGL2 */
    r_TexParameterf(t, pname, v);
}

static int s_scissor;                   /* GL_SCISSOR_TEST, for the copies' blits */
static uint32_t s_cap_known, s_cap_on;  /* bit per cap_bit */

static uint32_t cap_bit(GLenum cap)
{
    switch (cap) {
    case GL_BLEND:        return 1;
    case GL_CULL_FACE:    return 2;
    case GL_DEPTH_TEST:   return 4;
    case GL_STENCIL_TEST: return 8;
    case GL_SCISSOR_TEST: return 16;
    default:              return 0;
    }
}

/* 1: cap already in state `on` (the call is dropped); else noted as set. */
static int cap_same(GLenum cap, int on)
{
    uint32_t b = cap_bit(cap);
    if (!b)
        return 0;
    if (s_cache && (s_cap_known & b) && !!(s_cap_on & b) == on) {
        s_skips++;
        return 1;
    }
    s_cap_known |= b;
    s_cap_on = on ? s_cap_on | b : s_cap_on & ~b;
    return 0;
}

static void GLAPIENTRY es_Enable(GLenum cap)
{
    if (cap == GL_SCISSOR_TEST)
        s_scissor = 1;
    if (cap != GL_DEPTH_CLAMP_ && !cap_same(cap, 1))
        r_Enable(cap);
}

static void GLAPIENTRY es_Disable(GLenum cap)
{
    if (cap == GL_SCISSOR_TEST)
        s_scissor = 0;
    if (cap != GL_DEPTH_CLAMP_ && !cap_same(cap, 0))
        r_Disable(cap);
}

static void *GLAPIENTRY es_MapBufferRange(GLenum t, GLintptr off, GLsizeiptr len, GLbitfield access)
{
    (void)t; (void)off; (void)len; (void)access;
    return NULL;                        /* callers fall back to glBufferSubData */
}

static GLboolean GLAPIENTRY es_UnmapBuffer(GLenum t)
{
    (void)t;
    return GL_TRUE;
}

/* ── D3DCOLOR vertex arrays ──
 * Per vertex array object: which attributes point at BGRA data and which
 * are enabled. Per program: where u_es_bgra is and what it holds. */
typedef struct { uint32_t bgra, on; } EsVao;
typedef struct { GLint loc; uint32_t val; } EsProg;   /* loc -2: not looked up */
static EsVao  *s_vaos;
static EsProg *s_progs;
static GLuint  s_nvaos, s_nprogs, s_cur_vao, s_cur_prog;

static void *grow(void *a, GLuint *n, GLuint want, size_t elem, int fill)
{
    GLuint nn = *n ? *n : 64;
    uint8_t *b;
    while (nn <= want)
        nn *= 2;
    b = (uint8_t *)realloc(a, (size_t)nn * elem);
    if (!b)
        return NULL;
    memset(b + (size_t)*n * elem, fill, (size_t)(nn - *n) * elem);
    *n = nn;
    return b;
}

static EsVao *vao_cur(void)
{
    static EsVao none;
    if (s_cur_vao >= s_nvaos) {
        EsVao *v = (EsVao *)grow(s_vaos, &s_nvaos, s_cur_vao, sizeof *v, 0);
        if (!v)
            return &none;
        s_vaos = v;
    }
    return &s_vaos[s_cur_vao];
}

static EsProg *prog_get(GLuint p)
{
    if (p >= s_nprogs) {
        GLuint old = s_nprogs, i;
        EsProg *v = (EsProg *)grow(s_progs, &s_nprogs, p, sizeof *v, 0);
        if (!v)
            return NULL;
        for (i = old; i < s_nprogs; i++)
            v[i].loc = -2;
        s_progs = v;
    }
    return &s_progs[p];
}

static void GLAPIENTRY es_BindVertexArray(GLuint v)
{
    s_cur_vao = v;
    r_BindVertexArray(v);
}

static void GLAPIENTRY es_VertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm,
                                              GLsizei stride, const void *ptr)
{
    EsVao *v = vao_cur();
    if (size == GL_BGRA) {
        size = 4;
        v->bgra |= 1u << i;
    } else {
        v->bgra &= ~(1u << i);
    }
    r_VertexAttribPointer(i, size, type, norm, stride, ptr);
}

static void GLAPIENTRY es_EnableVertexAttribArray(GLuint i)
{
    vao_cur()->on |= 1u << i;
    r_EnableVertexAttribArray(i);
}

static void GLAPIENTRY es_DisableVertexAttribArray(GLuint i)
{
    vao_cur()->on &= ~(1u << i);
    r_DisableVertexAttribArray(i);
}

static void GLAPIENTRY es_UseProgram(GLuint p)
{
    s_cur_prog = p;
    r_UseProgram(p);
}

static void GLAPIENTRY es_LinkProgram(GLuint p)
{
    EsProg *e = prog_get(p);
    if (e)
        e->loc = -2;                    /* relinked: uniforms anew */
    r_LinkProgram(p);
}

static void GLAPIENTRY es_DeleteProgram(GLuint p)
{
    EsProg *e = prog_get(p);
    if (e)
        e->loc = -2;
    if (p == s_cur_prog)
        s_cur_prog = 0;
    r_DeleteProgram(p);
}

static void bgra_sync(void)
{
    EsProg *p;
    EsVao *v;
    uint32_t m;
    if (!s_cur_prog || !(p = prog_get(s_cur_prog)))
        return;
    if (p->loc == -2) {
        p->loc = r_GetUniformLocation(s_cur_prog, "u_es_bgra");
        p->val = 0;                     /* 0 after linking */
    }
    if (p->loc < 0)
        return;
    v = vao_cur();
    m = v->bgra & v->on;
    if (m != p->val) {
        r_Uniform1i(p->loc, (GLint)m);
        p->val = m;
    }
}

/* ── Feedback loops ──
 * Desktop GL lets a draw sample a texture that is attached to the bound
 * framebuffer, and lets a blit read and write one framebuffer; WebGL2
 * refuses both (INVALID_OPERATION, the draw or blit is dropped). The Xbox
 * titles do both: a surface sampled while drawn into (blur, reflections),
 * the edge fill and same-surface 2D copies. So this layer tracks texture
 * bindings, framebuffer attachments and texture sizes; a draw that samples
 * an attachment of the draw framebuffer samples a copy (refreshed only when
 * the texture was written since), and a blit within one framebuffer goes
 * through a scratch texture. */
#define ES_UNITS 16
static GLuint s_unit_tex[ES_UNITS];     /* GL_TEXTURE_2D binding per unit */
static GLuint s_unit;                   /* active unit */
static GLuint s_draw_fbo, s_read_fbo;
static uint32_t s_wseq;

#define ES_ONCE(...) do { static int said_; if (!said_) { said_ = 1; fprintf(stderr, __VA_ARGS__); } } while (0)

static GLuint es_bound_tex(void)
{
    return s_unit < ES_UNITS ? s_unit_tex[s_unit] : 0;
}

/* Texture level-0 sizes and last write, by name (open addressing). */
/* par: MIN/MAG filter, WRAP_S/T as last set (0: never set, GL's default);
 * pgen counts their changes. Kept here because reading them back
 * (glGetTexParameteriv) is a synchronous round trip to the GPU process in
 * WebGL: 4 of them per feedback-loop draw were 71% of the GL thread's time
 * at the title (2026-11-06). */
typedef struct { GLuint name; GLsizei w, h; GLint ifmt; GLenum fmt, type; uint32_t wseq;
                 GLint par[4]; uint32_t pgen; } EsTex;
#define ES_TOMB 0xFFFFFFFFu
static EsTex   *s_tex;
static uint32_t s_tex_cap, s_tex_fill, s_tex_live;   /* fill: live + tombstones */

static EsTex *tex_find(GLuint n)
{
    uint32_t i;
    if (!n || n == ES_TOMB || !s_tex_cap)
        return NULL;
    for (i = (n * 2654435761u) & (s_tex_cap - 1);; i = (i + 1) & (s_tex_cap - 1)) {
        if (s_tex[i].name == n)
            return &s_tex[i];
        if (!s_tex[i].name)
            return NULL;
    }
}

static EsTex *tex_slot(EsTex *t, uint32_t cap, GLuint n)
{
    uint32_t i = (n * 2654435761u) & (cap - 1);
    while (t[i].name && t[i].name != ES_TOMB)
        i = (i + 1) & (cap - 1);
    return &t[i];
}

static EsTex *tex_add(GLuint n)
{
    EsTex *e = tex_find(n);
    if (e || !n || n == ES_TOMB)
        return e;
    if ((s_tex_fill + 1) * 2 > s_tex_cap) {         /* rehash, dropping tombstones */
        uint32_t cap = 256, i;
        EsTex *t;
        while (cap < (s_tex_live + 1) * 4)
            cap *= 2;
        if (!(t = (EsTex *)calloc(cap, sizeof *t)))
            return NULL;
        for (i = 0; i < s_tex_cap; i++)
            if (s_tex[i].name && s_tex[i].name != ES_TOMB)
                *tex_slot(t, cap, s_tex[i].name) = s_tex[i];
        free(s_tex);
        s_tex = t;
        s_tex_cap = cap;
        s_tex_fill = s_tex_live;
    }
    e = tex_slot(s_tex, s_tex_cap, n);
    if (!e->name)
        s_tex_fill++;
    s_tex_live++;
    memset(e, 0, sizeof *e);
    e->name = n;
    return e;
}

static const GLenum k_copy_pn[4] = { GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER,
                                     GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T };

/* A glTexParameter on the 2D texture bound on the active unit. */
static void es_tex_param(GLenum t, GLenum pname, GLint v)
{
    EsTex *e;
    int i;
    if (t != GL_TEXTURE_2D)
        return;
    for (i = 0; i < 4 && k_copy_pn[i] != pname; i++) { }
    if (i == 4 || !(e = tex_add(es_bound_tex())) || e->par[i] == v)
        return;
    e->par[i] = v;
    e->pgen++;
}

static void es_tex_specified(GLuint tex, GLsizei w, GLsizei h, GLint ifmt, GLenum fmt, GLenum type)
{
    EsTex *e = tex_add(tex);
    if (!e)
        return;
    e->w = w; e->h = h; e->ifmt = ifmt; e->fmt = fmt; e->type = type;
    e->wseq = ++s_wseq;
}

static void es_tex_written(GLuint tex)
{
    EsTex *e = tex_find(tex);
    if (e)
        e->wseq = ++s_wseq;
}

/* Texture attachments per application framebuffer. */
typedef struct { GLuint fbo, color, depth; GLenum datt; } EsFb;
static EsFb  *s_fbs;
static GLuint s_nfbs, s_capfbs;

static EsFb *fb_find(GLuint f, int add)
{
    GLuint i;
    if (!f)
        return NULL;
    for (i = 0; i < s_nfbs; i++)
        if (s_fbs[i].fbo == f)
            return &s_fbs[i];
    if (!add)
        return NULL;
    if (s_nfbs == s_capfbs) {
        GLuint cap = s_capfbs ? s_capfbs * 2 : 32;
        EsFb *n = (EsFb *)realloc(s_fbs, cap * sizeof *n);
        if (!n)
            return NULL;
        s_fbs = n;
        s_capfbs = cap;
    }
    memset(&s_fbs[s_nfbs], 0, sizeof *s_fbs);
    s_fbs[s_nfbs].fbo = f;
    return &s_fbs[s_nfbs++];
}

/* The draw framebuffer was written (draw, clear, blit). */
static void fb_written(void)
{
    EsFb *f = fb_find(s_draw_fbo, 0);
    if (f) {
        es_tex_written(f->color);
        es_tex_written(f->depth);
    }
}

static void scissor_off(void)
{
    if (s_scissor)
        r_Disable(GL_SCISSOR_TEST);
}

static void scissor_back(void)
{
    if (s_scissor)
        r_Enable(GL_SCISSOR_TEST);
}

/* Copies of attachments sampled by draws into their own framebuffer. */
typedef struct {
    GLuint src, tex, fbo;
    GLsizei w, h;
    GLint ifmt;
    GLenum fmt, type, att;
    uint32_t seq, pgen;
    int valid;
} EsCopy;
static EsCopy s_copy[8];
static unsigned s_copy_next;

static GLint base_filter(GLint f)         /* the copy has one level */
{
    if (f == 0x2700 || f == 0x2702)     /* NEAREST_MIPMAP_NEAREST / _LINEAR */
        return GL_NEAREST;
    if (f == 0x2701 || f == GL_LINEAR_MIPMAP_LINEAR)
        return GL_LINEAR;
    return f;
}

/* A current copy of `t` (bound on the active unit, attached to the draw
 * framebuffer at `att`), left bound on the active unit; NULL: none. */
static EsCopy *copy_bind(GLuint t, GLenum att)
{
    static const GLint dflt[4] = { 0x2702 /* NEAREST_MIPMAP_LINEAR */, GL_LINEAR, GL_REPEAT, GL_REPEAT };
    const GLenum *pn = k_copy_pn;
    EsTex *e = tex_find(t);
    EsCopy *c = NULL;
    GLint pv[4];
    int i, params;
    if (!e || !e->w || !e->h) {
        ES_ONCE("  [GL] WebGL2: texture %u sampled while attached to framebuffer %u, size unknown: draw left as is\n",
                t, s_draw_fbo);
        return NULL;
    }
    for (i = 0; i < 8; i++)
        if (s_copy[i].src == t)
            c = &s_copy[i];
    if (!c) {
        c = &s_copy[s_copy_next++ & 7];
        c->src = t;
        c->valid = 0;
    }
    params = !c->valid || c->pgen != e->pgen;
    for (i = 0; i < 4; i++)             /* the original's sampling (es_tex_param) */
        pv[i] = e->par[i] ? e->par[i] : dflt[i];
    if (!c->tex)
        r_GenTextures(1, &c->tex);
    if (!c->fbo)
        r_GenFramebuffers(1, &c->fbo);
    r_BindTexture(GL_TEXTURE_2D, c->tex);
    if (!c->valid || c->w != e->w || c->h != e->h || c->ifmt != e->ifmt || c->fmt != e->fmt
        || c->type != e->type || c->att != att) {
        r_TexImage2D(GL_TEXTURE_2D, 0, e->ifmt, e->w, e->h, 0, e->fmt, e->type, NULL);
        r_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        r_BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->fbo);
        if (c->att && c->att != att)    /* a slot reused across attachment kinds */
            r_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, c->att, GL_TEXTURE_2D, 0, 0);
        r_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, att, GL_TEXTURE_2D, c->tex, 0);
        c->w = e->w; c->h = e->h; c->ifmt = e->ifmt; c->fmt = e->fmt; c->type = e->type;
        c->att = att;
        c->valid = 1;
        c->seq = e->wseq - 1;           /* stale */
        params = 1;
    }
    if (params) {
        pv[0] = base_filter(pv[0]);
        for (i = 0; i < 4; i++)
            r_TexParameteri(GL_TEXTURE_2D, pn[i], pv[i]);
        c->pgen = e->pgen;
    }
    if (c->seq != e->wseq) {
        GLbitfield m = att == GL_COLOR_ATTACHMENT0 ? GL_COLOR_BUFFER_BIT
                     : att == GL_DEPTH_STENCIL_ATTACHMENT ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT
                     : GL_DEPTH_BUFFER_BIT;
        r_BindFramebuffer(GL_READ_FRAMEBUFFER, s_draw_fbo);
        r_BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->fbo);
        scissor_off();
        r_BlitFramebuffer(0, 0, e->w, e->h, 0, 0, e->w, e->h, m, GL_NEAREST);
        scissor_back();
        r_BindFramebuffer(GL_READ_FRAMEBUFFER, s_read_fbo);
        c->seq = e->wseq;
    }
    r_BindFramebuffer(GL_DRAW_FRAMEBUFFER, s_draw_fbo);
    return c;
}

/* Before a draw: units whose texture is attached to the draw framebuffer
 * get a copy bound instead; returns them as a mask for loop_end. */
static uint32_t loop_begin(void)
{
    EsFb *f = fb_find(s_draw_fbo, 0);
    uint32_t mask = 0;
    int moved = 0;
    GLuint u;
    if (!f || (!f->color && !f->depth))
        return 0;
    for (u = 0; u < ES_UNITS; u++) {
        GLuint t = s_unit_tex[u];
        if (!t || (t != f->color && t != f->depth))
            continue;
        ES_ONCE("  [GL] WebGL2: draw samples texture %u attached to its framebuffer %u: a copy is sampled\n",
                t, s_draw_fbo);
        r_ActiveTexture(GL_TEXTURE0 + u);
        moved = 1;
        if (copy_bind(t, t == f->color ? GL_COLOR_ATTACHMENT0 : f->datt))
            mask |= 1u << u;
    }
    if (moved)
        r_ActiveTexture(GL_TEXTURE0 + s_unit);
    return mask;
}

static void loop_end(uint32_t mask)
{
    GLuint u;
    for (u = 0; mask; u++, mask >>= 1)
        if (mask & 1) {
            r_ActiveTexture(GL_TEXTURE0 + u);
            r_BindTexture(GL_TEXTURE_2D, s_unit_tex[u]);
        }
    r_ActiveTexture(GL_TEXTURE0 + s_unit);
}

static void GLAPIENTRY es_DrawElements(GLenum mode, GLsizei n, GLenum type, const void *at)
{
    uint32_t m;
    bgra_sync();
    m = loop_begin();
    r_DrawElements(mode, n, type, at);
    if (m)
        loop_end(m);
    fb_written();
}

static void GLAPIENTRY es_DrawArrays(GLenum mode, GLint first, GLsizei n)
{
    uint32_t m;
    bgra_sync();
    m = loop_begin();
    r_DrawArrays(mode, first, n);
    if (m)
        loop_end(m);
    fb_written();
}

static void GLAPIENTRY es_Clear(GLbitfield m)
{
    r_Clear(m);
    fb_written();
}

/* s_unit / s_unit_tex are exact between calls: the copies (loop_begin,
 * copy_bind, the blit scratch) put back what they bind before returning. */
static void GLAPIENTRY es_ActiveTexture(GLenum u)
{
    if (s_cache && u - GL_TEXTURE0 == s_unit) {
        s_skips++;
        return;
    }
    s_unit = u - GL_TEXTURE0;
    r_ActiveTexture(u);
}

static void GLAPIENTRY es_BindTexture(GLenum t, GLuint tex)
{
    if (t == GL_TEXTURE_2D && s_unit < ES_UNITS) {
        if (s_cache && s_unit_tex[s_unit] == tex) {
            s_skips++;
            return;
        }
        s_unit_tex[s_unit] = tex;
    }
    r_BindTexture(t, tex);
}

static void GLAPIENTRY es_DeleteTextures(GLsizei n, const GLuint *ids)
{
    GLsizei k;
    GLuint i;
    for (k = 0; k < n; k++) {
        GLuint t = ids[k];
        EsTex *e = tex_find(t);
        if (!t)
            continue;
        if (e) {
            e->name = ES_TOMB;
            s_tex_live--;
        }
        for (i = 0; i < ES_UNITS; i++)
            if (s_unit_tex[i] == t)
                s_unit_tex[i] = 0;
        for (i = 0; i < s_nfbs; i++) {  /* a deleted texture can no longer be sampled */
            if (s_fbs[i].color == t) s_fbs[i].color = 0;
            if (s_fbs[i].depth == t) s_fbs[i].depth = 0;
        }
        for (i = 0; i < 8; i++)
            if (s_copy[i].src == t)
                s_copy[i].src = 0;      /* its texture and FBO are reused */
    }
    r_DeleteTextures(n, ids);
}

static void GLAPIENTRY es_BindFramebuffer(GLenum t, GLuint f)
{
    if (t != GL_READ_FRAMEBUFFER)
        s_draw_fbo = f;
    if (t != GL_DRAW_FRAMEBUFFER)
        s_read_fbo = f;
    r_BindFramebuffer(t, f);
}

static void GLAPIENTRY es_DeleteFramebuffers(GLsizei n, const GLuint *ids)
{
    GLsizei k;
    for (k = 0; k < n; k++) {
        EsFb *f = fb_find(ids[k], 0);
        if (f)
            *f = s_fbs[--s_nfbs];
        if (ids[k] && ids[k] == s_draw_fbo)
            s_draw_fbo = 0;
        if (ids[k] && ids[k] == s_read_fbo)
            s_read_fbo = 0;
    }
    r_DeleteFramebuffers(n, ids);
}

static void fb_attach(GLenum t, GLenum att, GLuint tex)
{
    EsFb *f = fb_find(t == GL_READ_FRAMEBUFFER ? s_read_fbo : s_draw_fbo, 1);
    if (!f)
        return;
    if (att == GL_COLOR_ATTACHMENT0)
        f->color = tex;
    else if (att == GL_DEPTH_ATTACHMENT_ || att == GL_DEPTH_STENCIL_ATTACHMENT) {
        f->depth = tex;
        f->datt = att;
    } else if (att == GL_STENCIL_ATTACHMENT_ && f->datt == GL_DEPTH_STENCIL_ATTACHMENT)
        f->depth = 0;                   /* the stencil half replaced */
}

static void GLAPIENTRY es_FramebufferTexture2D(GLenum t, GLenum att, GLenum tt, GLuint tex, GLint l)
{
    fb_attach(t, att, tt == GL_TEXTURE_2D && l == 0 ? tex : 0);
    r_FramebufferTexture2D(t, att, tt, tex, l);
}

static void GLAPIENTRY es_FramebufferRenderbuffer(GLenum t, GLenum att, GLenum rt, GLuint rb)
{
    fb_attach(t, att, 0);
    r_FramebufferRenderbuffer(t, att, rt, rb);
}

/* A blit within one framebuffer: source to a scratch texture, then to the
 * destination. Colour only (no caller blits depth within a surface). */
static void GLAPIENTRY es_BlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                          GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                                          GLbitfield m, GLenum filter)
{
    static GLuint tex, fbo;
    static GLsizei tw, th;
    GLsizei w = sx1 > sx0 ? sx1 - sx0 : sx0 - sx1, h = sy1 > sy0 ? sy1 - sy0 : sy0 - sy1;
    if (s_read_fbo != s_draw_fbo) {
        r_BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, m, filter);
        fb_written();
        return;
    }
    ES_ONCE("  [GL] WebGL2: blit within framebuffer %u goes through a copy\n", s_draw_fbo);
    if (m & ~(GLbitfield)GL_COLOR_BUFFER_BIT)
        ES_ONCE("  [GL] WebGL2: depth/stencil blit within framebuffer %u dropped\n", s_draw_fbo);
    if (!(m & GL_COLOR_BUFFER_BIT) || !w || !h)
        return;
    if (!fbo) {
        r_GenTextures(1, &tex);
        r_GenFramebuffers(1, &fbo);
    }
    if (w > tw || h > th) {
        tw = w > tw ? w : tw;
        th = h > th ? h : th;
        r_BindTexture(GL_TEXTURE_2D, tex);
        r_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        r_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        r_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        r_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        r_BindTexture(GL_TEXTURE_2D, es_bound_tex());
        r_BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        r_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    }
    r_BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    scissor_off();                      /* the scissor applies to the destination only */
    r_BlitFramebuffer(sx0, sy0, sx1, sy1, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    scissor_back();
    r_BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    r_BindFramebuffer(GL_DRAW_FRAMEBUFFER, s_draw_fbo);
    r_BlitFramebuffer(0, 0, w, h, dx0, dy0, dx1, dy1, GL_COLOR_BUFFER_BIT, filter);
    r_BindFramebuffer(GL_READ_FRAMEBUFFER, s_read_fbo);
    fb_written();
}

/* ── RECOMP_GL_CALLSTATS=1: every call that reaches WebGL, counted ──
 * A thunk under each entry point (below this layer's wrappers, so the
 * calls they make themselves count and the ones they drop do not). */
#define ES_COUNTED(V, R) \
    R(const GLubyte *, glGetString, (GLenum a), (a)) \
    R(GLenum, glGetError, (void), ()) \
    V(glViewport, (GLint a, GLint b, GLsizei c, GLsizei d), (a, b, c, d)) \
    V(glScissor, (GLint a, GLint b, GLsizei c, GLsizei d), (a, b, c, d)) \
    V(glClearColor, (GLfloat a, GLfloat b, GLfloat c, GLfloat d), (a, b, c, d)) \
    V(glGetIntegerv, (GLenum a, GLint *b), (a, b)) \
    V(glClearDepth, (NV2A_GLdepth a), (a)) \
    V(glClearStencil, (GLint a), (a)) \
    V(glClear, (GLbitfield a), (a)) \
    V(glEnable, (GLenum a), (a)) \
    V(glDisable, (GLenum a), (a)) \
    V(glBlendFunc, (GLenum a, GLenum b), (a, b)) \
    V(glBlendEquation, (GLenum a), (a)) \
    V(glBlendColor, (GLfloat a, GLfloat b, GLfloat c, GLfloat d), (a, b, c, d)) \
    V(glDepthFunc, (GLenum a), (a)) \
    V(glDepthMask, (GLboolean a), (a)) \
    V(glDepthRange, (NV2A_GLdepth a, NV2A_GLdepth b), (a, b)) \
    V(glColorMask, (GLboolean a, GLboolean b, GLboolean c, GLboolean d), (a, b, c, d)) \
    V(glStencilFunc, (GLenum a, GLint b, GLuint c), (a, b, c)) \
    V(glStencilOp, (GLenum a, GLenum b, GLenum c), (a, b, c)) \
    V(glStencilMask, (GLuint a), (a)) \
    V(glCullFace, (GLenum a), (a)) \
    V(glFrontFace, (GLenum a), (a)) \
    V(glPixelStorei, (GLenum a, GLint b), (a, b)) \
    V(glReadPixels, (GLint a, GLint b, GLsizei c, GLsizei d, GLenum e, GLenum f, void *g), (a, b, c, d, e, f, g)) \
    V(glFinish, (void), ()) \
    V(glGenTextures, (GLsizei a, GLuint *b), (a, b)) \
    V(glDeleteTextures, (GLsizei a, const GLuint *b), (a, b)) \
    V(glBindTexture, (GLenum a, GLuint b), (a, b)) \
    V(glActiveTexture, (GLenum a), (a)) \
    V(glTexImage2D, (GLenum a, GLint b, GLint c, GLsizei d, GLsizei e, GLint f, GLenum g, GLenum h, const void *i), (a, b, c, d, e, f, g, h, i)) \
    V(glTexSubImage2D, (GLenum a, GLint b, GLint c, GLint d, GLsizei e, GLsizei f, GLenum g, GLenum h, const void *i), (a, b, c, d, e, f, g, h, i)) \
    V(glCompressedTexImage2D, (GLenum a, GLint b, GLenum c, GLsizei d, GLsizei e, GLint f, GLsizei g, const void *h), (a, b, c, d, e, f, g, h)) \
    V(glTexParameteri, (GLenum a, GLenum b, GLint c), (a, b, c)) \
    V(glGenFramebuffers, (GLsizei a, GLuint *b), (a, b)) \
    V(glDeleteFramebuffers, (GLsizei a, const GLuint *b), (a, b)) \
    V(glBindFramebuffer, (GLenum a, GLuint b), (a, b)) \
    V(glFramebufferTexture2D, (GLenum a, GLenum b, GLenum c, GLuint d, GLint e), (a, b, c, d, e)) \
    V(glFramebufferRenderbuffer, (GLenum a, GLenum b, GLenum c, GLuint d), (a, b, c, d)) \
    R(GLenum, glCheckFramebufferStatus, (GLenum a), (a)) \
    V(glBlitFramebuffer, (GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield i, GLenum j), (a, b, c, d, e, f, g, h, i, j)) \
    V(glGenRenderbuffers, (GLsizei a, GLuint *b), (a, b)) \
    V(glDeleteRenderbuffers, (GLsizei a, const GLuint *b), (a, b)) \
    V(glBindRenderbuffer, (GLenum a, GLuint b), (a, b)) \
    V(glRenderbufferStorage, (GLenum a, GLenum b, GLsizei c, GLsizei d), (a, b, c, d)) \
    R(GLuint, glCreateShader, (GLenum a), (a)) \
    V(glDeleteShader, (GLuint a), (a)) \
    V(glDeleteProgram, (GLuint a), (a)) \
    V(glShaderSource, (GLuint a, GLsizei b, const GLchar *const *c, const GLint *d), (a, b, c, d)) \
    V(glCompileShader, (GLuint a), (a)) \
    V(glGetShaderiv, (GLuint a, GLenum b, GLint *c), (a, b, c)) \
    V(glGetShaderInfoLog, (GLuint a, GLsizei b, GLsizei *c, GLchar *d), (a, b, c, d)) \
    R(GLuint, glCreateProgram, (void), ()) \
    V(glAttachShader, (GLuint a, GLuint b), (a, b)) \
    V(glBindAttribLocation, (GLuint a, GLuint b, const GLchar *c), (a, b, c)) \
    V(glLinkProgram, (GLuint a), (a)) \
    V(glGetProgramiv, (GLuint a, GLenum b, GLint *c), (a, b, c)) \
    V(glGetProgramInfoLog, (GLuint a, GLsizei b, GLsizei *c, GLchar *d), (a, b, c, d)) \
    V(glUseProgram, (GLuint a), (a)) \
    R(GLint, glGetUniformLocation, (GLuint a, const GLchar *b), (a, b)) \
    V(glUniform1i, (GLint a, GLint b), (a, b)) \
    V(glUniform1iv, (GLint a, GLsizei b, const GLint *c), (a, b, c)) \
    V(glUniform1f, (GLint a, GLfloat b), (a, b)) \
    V(glUniform2fv, (GLint a, GLsizei b, const GLfloat *c), (a, b, c)) \
    V(glUniform4fv, (GLint a, GLsizei b, const GLfloat *c), (a, b, c)) \
    V(glGenVertexArrays, (GLsizei a, GLuint *b), (a, b)) \
    V(glBindVertexArray, (GLuint a), (a)) \
    V(glGenBuffers, (GLsizei a, GLuint *b), (a, b)) \
    V(glBindBuffer, (GLenum a, GLuint b), (a, b)) \
    V(glBufferData, (GLenum a, GLsizeiptr b, const void *c, GLenum d), (a, b, c, d)) \
    V(glBufferSubData, (GLenum a, GLintptr b, GLsizeiptr c, const void *d), (a, b, c, d)) \
    V(glVertexAttribPointer, (GLuint a, GLint b, GLenum c, GLboolean d, GLsizei e, const void *f), (a, b, c, d, e, f)) \
    V(glEnableVertexAttribArray, (GLuint a), (a)) \
    V(glDisableVertexAttribArray, (GLuint a), (a)) \
    V(glVertexAttrib4f, (GLuint a, GLfloat b, GLfloat c, GLfloat d, GLfloat e), (a, b, c, d, e)) \
    V(glDrawElements, (GLenum a, GLsizei b, GLenum c, const void *d), (a, b, c, d)) \
    V(glDrawArrays, (GLenum a, GLint b, GLsizei c), (a, b, c)) \
    V(glTexParameterf, (GLenum a, GLenum b, GLfloat c), (a, b, c)) \
    V(glGenerateMipmap, (GLenum a), (a)) \
    R(GLsync, glFenceSync, (GLenum a, GLbitfield b), (a, b)) \
    R(GLenum, glClientWaitSync, (GLsync a, GLbitfield b, GLuint64 c), (a, b, c)) \
    V(glDeleteSync, (GLsync a), (a)) \
    V(glGenQueries, (GLsizei a, GLuint *b), (a, b)) \
    R(GLuint, glGetUniformBlockIndex, (GLuint a, const GLchar *b), (a, b)) \
    V(glUniformBlockBinding, (GLuint a, GLuint b, GLuint c), (a, b, c)) \
    V(glBindBufferRange, (GLenum a, GLuint b, GLuint c, GLintptr d, GLsizeiptr e), (a, b, c, d, e)) \
    V(glBeginQuery, (GLenum a, GLuint b), (a, b)) \
    V(glEndQuery, (GLenum a), (a)) \
    V(glGetQueryObjectiv, (GLuint a, GLenum b, GLint *c), (a, b, c))

#define ES_CT_V(name, P, A) static PFN_##name c_##name; \
    static void GLAPIENTRY ct_##name P { s_calls++; c_##name A; }
#define ES_CT_R(ret, name, P, A) static PFN_##name c_##name; \
    static ret GLAPIENTRY ct_##name P { s_calls++; return c_##name A; }
ES_COUNTED(ES_CT_V, ES_CT_R)
#define ES_CI_V(name, P, A) if (p_##name) { c_##name = p_##name; p_##name = ct_##name; }
#define ES_CI_R(ret, name, P, A) if (p_##name) { c_##name = p_##name; p_##name = ct_##name; }

int nv2a_gl_es_callstats(unsigned *calls, unsigned *skipped)
{
    *calls = s_calls;
    *skipped = s_skips;
    s_calls = s_skips = 0;
    return s_counting;
}

#define ES_WRAP(w) do { r_##w = p_gl##w; if (p_gl##w) p_gl##w = es_##w; } while (0)

void nv2a_gl_es_install(void)
{
    const char *e = getenv("RECOMP_GL_CALLSTATS");
    if (e && e[0] == '1') {
        ES_COUNTED(ES_CI_V, ES_CI_R)
        s_counting = 1;
        fprintf(stderr, "  [GL] WebGL calls counted per frame, on the [hitch] lines (RECOMP_GL_CALLSTATS)\n");
    }
    e = getenv("RECOMP_GL_STATECACHE");
    s_cache = !(e && e[0] == '0');
    fprintf(stderr, "  [GL] WebGL2: redundant bindings dropped %s (RECOMP_GL_STATECACHE)\n",
            s_cache ? "on" : "off");
    ES_WRAP(ShaderSource);
    ES_WRAP(PixelStorei);
    ES_WRAP(TexImage2D);
    ES_WRAP(TexSubImage2D);
    ES_WRAP(ReadPixels);
    ES_WRAP(TexParameteri);
    ES_WRAP(TexParameterf);
    ES_WRAP(Enable);
    ES_WRAP(Disable);
    ES_WRAP(BindVertexArray);
    ES_WRAP(VertexAttribPointer);
    ES_WRAP(EnableVertexAttribArray);
    ES_WRAP(DisableVertexAttribArray);
    ES_WRAP(UseProgram);
    ES_WRAP(LinkProgram);
    ES_WRAP(DeleteProgram);
    ES_WRAP(DrawElements);
    ES_WRAP(DrawArrays);
    ES_WRAP(Clear);
    ES_WRAP(ActiveTexture);
    ES_WRAP(BindTexture);
    ES_WRAP(DeleteTextures);
    ES_WRAP(BindFramebuffer);
    ES_WRAP(DeleteFramebuffers);
    ES_WRAP(FramebufferTexture2D);
    ES_WRAP(FramebufferRenderbuffer);
    ES_WRAP(BlitFramebuffer);
    r_GenTextures = p_glGenTextures;
    r_GenFramebuffers = p_glGenFramebuffers;
    r_GetUniformLocation = p_glGetUniformLocation;
    r_Uniform1i = p_glUniform1i;
    p_glMapBufferRange = es_MapBufferRange;
    p_glUnmapBuffer = es_UnmapBuffer;
    /* Not in WebGL2 (Emscripten may hand out stubs that only set errors). */
    p_glGetProgramBinary = NULL;
    p_glProgramBinary = NULL;
    p_glProgramParameteri = NULL;
    p_glBufferStorage = NULL;
    p_glDrawElementsBaseVertex = NULL;
    p_glGetQueryObjectui64v = NULL;
    fprintf(stderr, "  [GL] WebGL2: GLSL ES 3.00, BGRA swizzled on upload/read-back and in the vertex shader\n");
}
#endif /* __EMSCRIPTEN__ */
