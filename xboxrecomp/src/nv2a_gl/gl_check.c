/* gl_check.c -- RECOMP_GL_ERRORS=2: the GL call that raised an error.
 *
 * RECOMP_GL_ERRORS=1 checks once per step (upload, draw, present), which
 * names the step but not the call, and an error from set-up shows up at the
 * first step after it. With 2, the calls that set state, textures,
 * framebuffers and draws are wrapped: each is followed by glGetError and the
 * first errors are logged with the call and its arguments. Slow (a round
 * trip per call on a forwarding driver): a diagnostic. Written for the first
 * Windows runs on Parallels' GL, which drew black (2026-10-23).
 */
#include "gl_api.h"

#include <stdio.h>
#include <stdlib.h>

static PFN_glGetError real_glGetError;
static unsigned s_reported;

static void chk(const char *call, const char *fmt, long long a, long long b, long long c,
                long long d, long long e, long long f)
{
    GLenum err;
    while ((err = real_glGetError()) != GL_NO_ERROR) {
        if (s_reported++ >= 60)
            continue;
        fprintf(stderr, "  [GL] error 0x%04X in %s(", err, call);
        fprintf(stderr, fmt, a, b, c, d, e, f);
        fprintf(stderr, ")\n");
    }
}

#define H "0x%llX"
#define D "%lld"

static PFN_glEnable real_glEnable;
static void GLAPIENTRY w_glEnable(GLenum cap)
{ real_glEnable(cap); chk("glEnable", H, cap, 0, 0, 0, 0, 0); }
static PFN_glDisable real_glDisable;
static void GLAPIENTRY w_glDisable(GLenum cap)
{ real_glDisable(cap); chk("glDisable", H, cap, 0, 0, 0, 0, 0); }
static PFN_glClear real_glClear;
static void GLAPIENTRY w_glClear(GLbitfield m)
{ real_glClear(m); chk("glClear", H, m, 0, 0, 0, 0, 0); }
static PFN_glPixelStorei real_glPixelStorei;
static void GLAPIENTRY w_glPixelStorei(GLenum p, GLint v)
{ real_glPixelStorei(p, v); chk("glPixelStorei", H ", " D, p, v, 0, 0, 0, 0); }
static PFN_glBindTexture real_glBindTexture;
static void GLAPIENTRY w_glBindTexture(GLenum t, GLuint id)
{ real_glBindTexture(t, id); chk("glBindTexture", H ", " D, t, id, 0, 0, 0, 0); }
static PFN_glActiveTexture real_glActiveTexture;
static void GLAPIENTRY w_glActiveTexture(GLenum u)
{ real_glActiveTexture(u); chk("glActiveTexture", H, u, 0, 0, 0, 0, 0); }
static PFN_glTexImage2D real_glTexImage2D;
static void GLAPIENTRY w_glTexImage2D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h,
                                      GLint b, GLenum fmt, GLenum type, const void *p)
{
    real_glTexImage2D(t, l, ifmt, w, h, b, fmt, type, p);
    (void)b; (void)p;
    chk("glTexImage2D", "level " D ", internal " H ", " D "x" D ", format " H ", type " H,
        l, ifmt, w, h, fmt, type);
}
static PFN_glTexSubImage2D real_glTexSubImage2D;
static void GLAPIENTRY w_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w,
                                         GLsizei h, GLenum fmt, GLenum type, const void *p)
{
    real_glTexSubImage2D(t, l, x, y, w, h, fmt, type, p);
    (void)t; (void)x; (void)y; (void)p;
    chk("glTexSubImage2D", "level " D ", " D "x" D ", format " H ", type " H "%.0lld",
        l, w, h, fmt, type, 0);
}
static PFN_glCompressedTexImage2D real_glCompressedTexImage2D;
static void GLAPIENTRY w_glCompressedTexImage2D(GLenum t, GLint l, GLenum ifmt, GLsizei w,
                                                GLsizei h, GLint b, GLsizei n, const void *p)
{
    real_glCompressedTexImage2D(t, l, ifmt, w, h, b, n, p);
    chk("glCompressedTexImage2D", "level " D ", internal " H ", " D "x" D ", " D " bytes%.0lld",
        l, ifmt, w, h, n, 0);
}
static PFN_glTexParameteri real_glTexParameteri;
static void GLAPIENTRY w_glTexParameteri(GLenum t, GLenum p, GLint v)
{ real_glTexParameteri(t, p, v); chk("glTexParameteri", H ", " H ", " H, t, p, v, 0, 0, 0); }
static PFN_glTexParameterf real_glTexParameterf;
static void GLAPIENTRY w_glTexParameterf(GLenum t, GLenum p, GLfloat v)
{ real_glTexParameterf(t, p, v); chk("glTexParameterf", H ", " H ", " D, t, p, (long long)v, 0, 0, 0); }
static PFN_glGenerateMipmap real_glGenerateMipmap;
static void GLAPIENTRY w_glGenerateMipmap(GLenum t)
{ real_glGenerateMipmap(t); chk("glGenerateMipmap", H, t, 0, 0, 0, 0, 0); }
static PFN_glBindFramebuffer real_glBindFramebuffer;
static void GLAPIENTRY w_glBindFramebuffer(GLenum t, GLuint f)
{ real_glBindFramebuffer(t, f); chk("glBindFramebuffer", H ", " D, t, f, 0, 0, 0, 0); }
static PFN_glFramebufferTexture2D real_glFramebufferTexture2D;
static void GLAPIENTRY w_glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l)
{ real_glFramebufferTexture2D(t, a, tt, tex, l); chk("glFramebufferTexture2D", H ", " H ", " H ", " D ", " D, t, a, tt, tex, l, 0); }
static PFN_glFramebufferRenderbuffer real_glFramebufferRenderbuffer;
static void GLAPIENTRY w_glFramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint rb)
{ real_glFramebufferRenderbuffer(t, a, rt, rb); chk("glFramebufferRenderbuffer", H ", " H ", " H ", " D, t, a, rt, rb, 0, 0); }
static PFN_glRenderbufferStorage real_glRenderbufferStorage;
static void GLAPIENTRY w_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h)
{ real_glRenderbufferStorage(t, f, w, h); chk("glRenderbufferStorage", H ", " H ", " D "x" D, t, f, w, h, 0, 0); }
static PFN_glBlitFramebuffer real_glBlitFramebuffer;
static void GLAPIENTRY w_glBlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f,
                                           GLint g, GLint h, GLbitfield m, GLenum flt)
{
    real_glBlitFramebuffer(a, b, c, d, e, f, g, h, m, flt);
    chk("glBlitFramebuffer", D "," D " " D "x" D " mask " H " filter " H, a, b, c, d, m, flt);
    (void)e; (void)f; (void)g; (void)h;
}
static PFN_glReadPixels real_glReadPixels;
static void GLAPIENTRY w_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *p)
{ real_glReadPixels(x, y, w, h, f, t, p); chk("glReadPixels", D "x" D " format " H " type " H "%.0lld%.0lld", w, h, f, t, x, y); }
static PFN_glUseProgram real_glUseProgram;
static void GLAPIENTRY w_glUseProgram(GLuint p)
{ real_glUseProgram(p); chk("glUseProgram", D, p, 0, 0, 0, 0, 0); }
static PFN_glUniform1i real_glUniform1i;
static void GLAPIENTRY w_glUniform1i(GLint l, GLint v)
{ real_glUniform1i(l, v); chk("glUniform1i", D ", " D, l, v, 0, 0, 0, 0); }
static PFN_glDrawElements real_glDrawElements;
static void GLAPIENTRY w_glDrawElements(GLenum m, GLsizei n, GLenum t, const void *i)
{ real_glDrawElements(m, n, t, i); chk("glDrawElements", H ", " D ", " H, m, n, t, 0, 0, 0); }
static PFN_glDrawArrays real_glDrawArrays;
static void GLAPIENTRY w_glDrawArrays(GLenum m, GLint f, GLsizei n)
{ real_glDrawArrays(m, f, n); chk("glDrawArrays", H ", " D ", " D, m, f, n, 0, 0, 0); }
static PFN_glBlendFunc real_glBlendFunc;
static void GLAPIENTRY w_glBlendFunc(GLenum s, GLenum d)
{ real_glBlendFunc(s, d); chk("glBlendFunc", H ", " H, s, d, 0, 0, 0, 0); }
static PFN_glBlendEquation real_glBlendEquation;
static void GLAPIENTRY w_glBlendEquation(GLenum m)
{ real_glBlendEquation(m); chk("glBlendEquation", H, m, 0, 0, 0, 0, 0); }
static PFN_glDepthFunc real_glDepthFunc;
static void GLAPIENTRY w_glDepthFunc(GLenum f)
{ real_glDepthFunc(f); chk("glDepthFunc", H, f, 0, 0, 0, 0, 0); }
static PFN_glStencilFunc real_glStencilFunc;
static void GLAPIENTRY w_glStencilFunc(GLenum f, GLint r, GLuint m)
{ real_glStencilFunc(f, r, m); chk("glStencilFunc", H ", " D ", " H, f, r, m, 0, 0, 0); }
static PFN_glStencilOp real_glStencilOp;
static void GLAPIENTRY w_glStencilOp(GLenum a, GLenum b, GLenum c)
{ real_glStencilOp(a, b, c); chk("glStencilOp", H ", " H ", " H, a, b, c, 0, 0, 0); }

static PFN_glUniform1iv real_glUniform1iv;
static void GLAPIENTRY w_glUniform1iv(GLint l, GLsizei n, const GLint *v)
{ real_glUniform1iv(l, n, v); chk("glUniform1iv", D ", count " D ", first " D, l, n, v ? v[0] : 0, 0, 0, 0); }
static PFN_glUniform1f real_glUniform1f;
static void GLAPIENTRY w_glUniform1f(GLint l, GLfloat v)
{ real_glUniform1f(l, v); chk("glUniform1f", D ", %.0lld~" D, l, 0, (long long)(v * 1000), 0, 0, 0); }
static PFN_glUniform2fv real_glUniform2fv;
static void GLAPIENTRY w_glUniform2fv(GLint l, GLsizei n, const GLfloat *v)
{ real_glUniform2fv(l, n, v); chk("glUniform2fv", D ", count " D, l, n, 0, 0, 0, 0); }
static PFN_glUniform4fv real_glUniform4fv;
static void GLAPIENTRY w_glUniform4fv(GLint l, GLsizei n, const GLfloat *v)
{ real_glUniform4fv(l, n, v); chk("glUniform4fv", D ", count " D, l, n, 0, 0, 0, 0); }

#define WRAP(name) do { if (p_##name) { real_##name = p_##name; p_##name = w_##name; } } while (0)

void nv2a_gl_check_install(void)
{
    const char *e = getenv("RECOMP_GL_ERRORS");
    if (!e || *e != '2' || !p_glGetError)
        return;
    real_glGetError = p_glGetError;
    WRAP(glEnable); WRAP(glDisable); WRAP(glClear); WRAP(glPixelStorei);
    WRAP(glBindTexture); WRAP(glActiveTexture); WRAP(glTexImage2D); WRAP(glTexSubImage2D);
    WRAP(glCompressedTexImage2D); WRAP(glTexParameteri); WRAP(glTexParameterf);
    WRAP(glGenerateMipmap); WRAP(glBindFramebuffer); WRAP(glFramebufferTexture2D);
    WRAP(glFramebufferRenderbuffer); WRAP(glRenderbufferStorage); WRAP(glBlitFramebuffer);
    WRAP(glReadPixels); WRAP(glUseProgram); WRAP(glUniform1i); WRAP(glDrawElements);
    WRAP(glDrawArrays); WRAP(glBlendFunc); WRAP(glBlendEquation); WRAP(glDepthFunc);
    WRAP(glStencilFunc); WRAP(glStencilOp);
    WRAP(glUniform1iv); WRAP(glUniform1f); WRAP(glUniform2fv); WRAP(glUniform4fv);
    fprintf(stderr, "  [GL] every state, texture, framebuffer and draw call checked (RECOMP_GL_ERRORS=2)\n");
}
