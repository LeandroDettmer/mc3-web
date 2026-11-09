/* gl_es.h -- the renderer on WebGL2 / OpenGL ES 3.0 (Emscripten builds).
 *
 * nv2a_gl is written against desktop GL 3.3 core. On the web it runs on
 * WebGL2 unchanged: gl_api.c resolves the entry points through
 * nv2a_gl_es_getproc and then nv2a_gl_es_install puts a thin layer between
 * the renderer and the browser's GL (gl_es.c), the way gl_check.c wraps for
 * error checks. Desktop GL and the Switch never compile any of it. */
#ifndef NV2A_GL_ES_H
#define NV2A_GL_ES_H

/* GLSL 3.30 core source -> GLSL ES 3.00: the version line and default
 * precisions; in the NV2A vertex shaders the inputs v0..v15 also become
 * copies swizzled per draw (D3DCOLOR arrays, which WebGL cannot fetch as
 * BGRA). NULL if the text is not GLSL 3.30 core (left as it is). The
 * result is malloc'd. */
char *nv2a_gl_es_source(const char *const *src, int n);

#if defined(__EMSCRIPTEN__)
/* getproc for nv2a_gl_load: the GLES names where WebGL2 has only those
 * (glClearDepthf, glDepthRangef), the rest as asked. */
void *nv2a_gl_es_getproc(const char *name);
void nv2a_gl_es_set_getproc(void *(*getproc)(const char *name));
/* After nv2a_gl_load has filled the table: the wrappers. */
void nv2a_gl_es_install(void);
/* Calls that reached WebGL (with RECOMP_GL_CALLSTATS=1) and calls the
 * state cache dropped, since the last call; returns 0 if not counting. */
int nv2a_gl_es_callstats(unsigned *calls, unsigned *skipped);
#endif

#endif /* NV2A_GL_ES_H */
