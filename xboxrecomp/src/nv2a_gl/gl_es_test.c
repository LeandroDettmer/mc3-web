/*
 * gl_es_test.c -- checks for gl_es.c's GLSL 3.30 -> GLSL ES 3.00 rewrite.
 *
 * Not part of any build. Writes generated shaders (both dialects) for
 * glslangValidator, which tells whether WebGL2 would take them:
 *
 *   cc -DNV2A_GL_ES_TEST -I. gl_es_test.c gl_es.c gl_vsh.c gl_psh.c -lm -o gl_es_test
 *   ./gl_es_test <dir> [count]          # nv2a programs: random vertex programs
 *                                       # and combiner keys, plus fixed function
 *   ./gl_es_test -r <in> <out>          # rewrite one GLSL 3.30 file
 *   glslangValidator <dir>/es_*.vert ...; glslangValidator -l vs.vert fs.frag
 *
 * Also asserts the rewrite's shape on a small vertex shader.
 */
#include "gl_es.h"
#include "gl_psh.h"
#include "gl_vsh.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_rng = 12345;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

static void put(const char *dir, const char *name, int i, const char *ext, const char *text)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s%03d.%s", dir, name, i, ext);
    if (!(f = fopen(path, "w"))) { perror(path); exit(1); }
    fputs(text, f);
    fclose(f);
}

static void pair(const char *dir, int i, const char *const *vs, int nvs, const char *fs)
{
    char *v330, *ves, *fes;
    size_t len = 0;
    int k;
    for (k = 0; k < nvs; k++) len += strlen(vs[k]);
    v330 = malloc(len + 1); v330[0] = 0;
    for (k = 0; k < nvs; k++) strcat(v330, vs[k]);
    ves = nv2a_gl_es_source(vs, nvs);
    fes = nv2a_gl_es_source(&fs, 1);
    assert(ves && fes);
    put(dir, "gl_", i, "vert", v330);
    put(dir, "gl_", i, "frag", fs);
    put(dir, "es_", i, "vert", ves);
    put(dir, "es_", i, "frag", fes);
    free(v330); free(ves); free(fes);
}

static void self_check(void)
{
    const char *src[2] = {
        "#version 330 core\n"
        "layout(location = 0) in vec4 v0;\n"
        "layout(location = 1) in vec4 v1;\n"
        "layout(location = 10) in vec4 v10;\n",
        "void main() {\n    gl_Position = v0 + v10;\n}\n" };
    const char *other = "#version 410\nvoid main() {}\n";
    char *t = nv2a_gl_es_source(src, 2);
    assert(t && !strncmp(t, "#version 300 es\n", 16));
    assert(strstr(t, "in vec4 nv2a_a0;") && strstr(t, "in vec4 nv2a_a10;"));
    assert(strstr(t, "in vec4 v1;"));                     /* unread: untouched */
    assert(strstr(t, "v10 = (u_es_bgra & 1024) != 0 ? nv2a_a10.bgra : nv2a_a10;"));
    assert(strstr(t, "void main() {\n    nv2a_es_in();\n"));
    free(t);
    assert(nv2a_gl_es_source(&other, 1) == NULL);
}

int main(int argc, char **argv)
{
    static char body[256 * 1024], fsrc[64 * 1024];
    int i, n;
    self_check();
    if (argc == 4 && !strcmp(argv[1], "-r")) {
        static char in[256 * 1024];
        const char *p = in;
        FILE *f = fopen(argv[2], "r");
        char *t;
        size_t got;
        if (!f) { perror(argv[2]); return 1; }
        got = fread(in, 1, sizeof in - 1, f);
        in[got] = 0;
        fclose(f);
        if (!(t = nv2a_gl_es_source(&p, 1))) { fprintf(stderr, "%s: not GLSL 3.30 core\n", argv[2]); return 1; }
        if (!(f = fopen(argv[3], "w"))) { perror(argv[3]); return 1; }
        fputs(t, f);
        fclose(f);
        free(t);
        return 0;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: %s <dir> [count] | -r <in> <out>\n", argv[0]);
        return 2;
    }
    n = argc > 2 ? atoi(argv[2]) : 100;
    for (i = 0; i < n; i++) {
        uint32_t prog[136][4];
        Nv2aPshKey k;
        uint32_t len = 1 + rnd() % 24, j;
        const char *vs[3];
        int nvs = 0;
        memset(prog, 0, sizeof prog);
        for (j = 0; j < len; j++) {
            prog[j][0] = rnd(); prog[j][1] = rnd(); prog[j][2] = rnd();
            prog[j][3] = rnd() & ~1u;
        }
        prog[len - 1][3] |= 1;                               /* FINAL */
        memset(&k, 0, sizeof k);
        for (j = 0; j < 8; j++) {
            k.color_icw[j] = rnd(); k.color_ocw[j] = rnd();
            k.alpha_icw[j] = rnd(); k.alpha_ocw[j] = rnd();
        }
        k.control = (rnd() % 9) | (rnd() & 0xFFFFFF00u);
        k.final0 = rnd(); k.final1 = rnd();
        k.shader_program = rnd() & 0xFFFFF;
        k.other_input = rnd();
        vs[nvs++] = nv2a_gl_vsh_prelude();
        if (i % 4 == 3) {
            vs[nvs++] = nv2a_gl_vsh_fixed();
        } else {
            if (nv2a_gl_vsh_program((const uint32_t (*)[4])prog, 136, 0, body, sizeof body) < 0)
                continue;
            vs[nvs++] = body;
            vs[nvs++] = nv2a_gl_vsh_main_program();
        }
        if (nv2a_gl_psh(&k, fsrc, sizeof fsrc) < 0)
            continue;
        pair(argv[1], i, vs, nvs, fsrc);
    }
    printf("self-check ok, %d pairs in %s\n", n, argv[1]);
    return 0;
}
