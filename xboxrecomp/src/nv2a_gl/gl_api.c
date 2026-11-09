/* gl_api.c -- fill the entry-point table declared in gl_api.h. */
#include "gl_api.h"
#include "gl_es.h"

#include <stdio.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
/* Emscripten has buffer mapping only with -sFULL_ES3, which also puts its
 * client-side-array emulation into every draw call and buffer bind; the
 * browser link leaves it out. Nothing is mapped on WebGL2 (gl_es.c answers
 * NULL), so stand-ins satisfy the required list. */
static void *GLAPIENTRY web_no_map(GLenum t, GLintptr o, GLsizeiptr l, GLbitfield a)
{
    (void)t; (void)o; (void)l; (void)a;
    return NULL;
}
static GLboolean GLAPIENTRY web_no_unmap(GLenum t)
{
    (void)t;
    return GL_TRUE;
}
static void *web_getproc(const char *name)
{
    void *p = nv2a_gl_es_getproc(name);
    if (!p && !strcmp(name, "glMapBufferRange"))
        p = (void *)web_no_map;
    else if (!p && !strcmp(name, "glUnmapBuffer"))
        p = (void *)web_no_unmap;
    return p;
}
#endif

#define NV2A_GL_DEFINE(ret, name, args) PFN_##name p_##name;
NV2A_GL_FUNCS(NV2A_GL_DEFINE)
NV2A_GL_OPT_FUNCS(NV2A_GL_DEFINE)
#undef NV2A_GL_DEFINE

int nv2a_gl_load(void *(*getproc)(const char *name))
{
    int missing = 0;

#if defined(__EMSCRIPTEN__)
    nv2a_gl_es_set_getproc(getproc);    /* WebGL2: GLES names (gl_es.c) */
    getproc = web_getproc;
#endif
#define NV2A_GL_RESOLVE(ret, name, args) \
    p_##name = (PFN_##name)getproc(#name); \
    if (!p_##name) { \
        fprintf(stderr, "  [GL] missing entry point %s\n", #name); \
        missing++; \
    }
    NV2A_GL_FUNCS(NV2A_GL_RESOLVE)
#undef NV2A_GL_RESOLVE
#define NV2A_GL_RESOLVE_OPT(ret, name, args) p_##name = (PFN_##name)getproc(#name);
    NV2A_GL_OPT_FUNCS(NV2A_GL_RESOLVE_OPT)
#undef NV2A_GL_RESOLVE_OPT
#if defined(__EMSCRIPTEN__)
    if (!missing)
        nv2a_gl_es_install();
#endif
    return missing;
}
