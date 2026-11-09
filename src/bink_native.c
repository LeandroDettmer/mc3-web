/*
 * bink_native.c -- Bink 1 movie frames decoded by FFmpeg instead of the
 * lifted Bink decoder.
 *
 * Why: the lifted decoder is x86 translated to C, without its SIMD; on the VM
 * the movies ran at ~22 of their 29.97 fps, and the audio stream starved (9%
 * silent 10 ms blocks) because one game thread decodes video and audio.
 * FFmpeg's binkvideo is bit-exact with Bink's own decoder: the planes the
 * game decoded for mc3intro.bik frame 151 equal FFmpeg's, byte for byte
 * (2026-10-15, make difftest), once V and U are swapped -- Bink stores Y, V, U.
 *
 * How: only the decode step is replaced (src/recomp_manual.c, sub_0037CCD0).
 * Bink keeps doing everything else -- file reading, timing, audio, the YUV to
 * RGB copy, frame bookkeeping. FFmpeg opens the same .bik itself (libavformat)
 * when the game calls BinkOpen, and decodes the frame Bink is on; a jump in
 * frame number (a loop, a skip) is a seek.
 *
 * Movie sound, likewise (2026-10-15): Bink decodes its audio correctly, but
 * the way to the speakers -- Bink's DirectSound ring, the emulated APU voice,
 * the output -- repeated, skipped and slowed it (tools/audio_diff.py: in
 * mc3intro 1567 repeats of 256..768 samples, 304 skips, 1.2 s of holes, 84%
 * speed). So FFmpeg decodes the soundtrack too and plays it on a voice of
 * the runtime's software mixer (apu_mixer_*, mixed at the output's clock),
 * kept in step with the video frame Bink is on; Bink's own decoded audio is
 * zeroed before it reaches DirectSound, so its timing machinery keeps
 * running on silence. MC3_NATIVE_BINK_AUDIO=0 leaves the sound to Bink;
 * MC3_MOVIE_VOLUME=0..100 (default 100).
 *
 * Host-side only: the wrappers in recomp_manual.c turn guest arguments into
 * host values. Built when CMake finds FFmpeg (MC3_NATIVE_BINK); otherwise the
 * stubs at the bottom keep the lifted decoder in charge. MC3_NATIVE_BINK=0 at
 * run time does the same.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int  mc3_bink_native_enabled(void);
void mc3_bink_native_open(uint32_t hbink, const char *xbox_name);
int  mc3_bink_native_frame(uint32_t hbink, int frame, uint8_t *dest, int w, int h);
void mc3_bink_native_close(uint32_t hbink);
void mc3_bink_native_audio(uint32_t hbink, int frame);
int  mc3_bink_native_audio_active(void);

#if defined(MC3_NATIVE_BINK)

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>

#include "apu/apu.h"            /* the runtime's software mixer */

/* The runtime's Xbox -> host path translation (xboxrecomp kernel_path.c). */
#ifdef _WIN32
#include <windows.h>
/* The runtime's host paths are UTF-16 on Windows (kernel.h, xbox_host_char). */
extern int xbox_translate_path(const char *xbox_path, wchar_t *host, unsigned int size);
#else
extern int xbox_translate_path(const char *xbox_path, char *host, unsigned int size);
#endif

#define MAX_MOVIES 4

typedef struct {
    uint32_t         hbink;     /* guest HBINK, 0 = free slot */
    AVFormatContext *fmt;
    AVCodecContext  *dec;
    AVPacket        *pkt;
    AVFrame         *frame;
    int              stream;
    int              next;      /* frame index FFmpeg will produce next, -1 unknown */
    long             served, fallbacks;
    struct MovieAudio *audio;   /* NULL: Bink plays its own sound */
    int              nofill;    /* holds the renderer's 4:3 fill off (fill_skip) */
    int              frame_ok;  /* m->frame is the frame Bink is showing */
    int              cal;       /* colour matrix: 0 not yet, 1 fitted, -1 unusable */
    float            mat[12];   /* R, G, B rows over (Y, U, V, 1), all 0..1 */
    float            alpha;     /* the constant alpha the player writes, 0..1 */
    uint32_t         dest;      /* the title's texture (BinkCopyToBuffer) */
    uint8_t         *dest_host; /* ... in host memory, and its pitch */
    uint32_t         dest_pitch;
    int              skipped;   /* the player's copy was skipped since it last ran */
    int              fit_tries; /* frames looked at for the colour fit */
} Movie;

/* ── Movies shown as they are, not filled to 16:9 ──
 *
 * The renderer fills screens the game pillarboxed to 4:3 out to the width
 * (nv2a_gl RECOMP_GL_FILL). For the boot movies -- the Rockstar and Rockstar
 * San Diego logos and the intro -- the original framing looked better
 * (2026-10-18), so while one of them plays the fill is held off.
 * MC3_FILL_SKIP: comma-separated name stems, e.g. "rockstar,sdlogo,mc3intro"
 * (default empty: everything filled to 16:9, as preferred 2026-10-20);
 * "legal" holds the legal screen too, recomp_manual.c sub_0001B500, not
 * reliable (other loading screens switch shape halfway). Weak, so a build
 * without the GL renderer still links. */
extern void nv2a_gl_fill_hold(int delta) __attribute__((weak));
extern void nv2a_gl_movie_frame(uint32_t va, uint32_t w, uint32_t h,
                                const uint8_t *y, int ys, const uint8_t *u, int us,
                                const uint8_t *v, int vs, const float m[12],
                                float alpha) __attribute__((weak));
extern void nv2a_gl_movie_end(uint32_t va) __attribute__((weak));
extern int  nv2a_gl_movie_shown(uint32_t va) __attribute__((weak));

static int fill_skip(const char *xbox_name)
{
    const char *e = getenv("MC3_FILL_SKIP"), *list = e ? e : "";
    const char *base = xbox_name, *p;
    size_t n;
    for (p = xbox_name; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            base = p + 1;
    while (*list) {
        const char *end = strchr(list, ',');
        n = end ? (size_t)(end - list) : strlen(list);
        if (n && strncasecmp(base, list, n) == 0 && (base[n] == '.' || !base[n]))
            return 1;
        list += n + (end ? 1 : 0);
    }
    return 0;
}

static Movie s_movies[MAX_MOVIES];

static Movie *find(uint32_t hbink);

/* ── Movie sound ─────────────────────────────────────────────── */

#define RING_SECS   1           /* the voice loops over a ring this long */
#define LEAD_MS     250         /* decoded sound kept ahead of the play position */
#define RESYNC_MS   120         /* off by more than this from the video: correct */

typedef struct MovieAudio {
    AVFormatContext *fmt;       /* its own reader: independent of the video's */
    AVCodecContext  *dec;
    AVPacket        *pkt;
    AVFrame         *frame;
    int              stream, eof;
    int              ch, rate;
    double           fps;       /* video frames per second */
    int16_t         *ring;      /* ring_frames x ch, interleaved s16 */
    uint32_t         ring_frames;
    int              voice, started;
    int              counted;   /* in s_audio_users: Bink's own sound muted */
    uint64_t         written;   /* frames written into the ring (sound or silence) */
    uint64_t         played;    /* frames the mixer has consumed */
    uint32_t         last_pos;
    int64_t          src;       /* soundtrack frames taken from FFmpeg (played or dropped) */
    int16_t         *pend;      /* decoded frames not yet in the ring */
    int              pend_n, pend_off, pend_cap;
    long             dropped, inserted, underruns;
} MovieAudio;

static int s_audio_users;      /* movies whose sound is ours */

int mc3_bink_native_audio_active(void)
{
    return s_audio_users > 0;
}

static int audio_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_NATIVE_BINK_AUDIO");
        on = !(e && e[0] == '0');
    }
    return on;
}

static void audio_free(MovieAudio *a)
{
    if (!a) return;
    if (a->voice >= 0) {
        apu_mixer_stop(a->voice);
        apu_mixer_free_voice(a->voice);
    }
    if (a->counted) s_audio_users--;
    if (a->frame) av_frame_free(&a->frame);
    if (a->pkt) av_packet_free(&a->pkt);
    if (a->dec) avcodec_free_context(&a->dec);
    if (a->fmt) avformat_close_input(&a->fmt);
    free(a->ring);
    free(a->pend);
    free(a);
}

static MovieAudio *audio_open(const char *path, double fps)
{
    MovieAudio *a = calloc(1, sizeof *a);
    const AVCodec *codec;
    const char *e;

    if (!a) return NULL;
    a->voice = -1;
    if (avformat_open_input(&a->fmt, path, NULL, NULL) < 0
            || avformat_find_stream_info(a->fmt, NULL) < 0)
        goto fail;
    a->stream = av_find_best_stream(a->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (a->stream < 0)
        goto fail;              /* a silent movie */
    codec = avcodec_find_decoder(a->fmt->streams[a->stream]->codecpar->codec_id);
    if (!codec || !(a->dec = avcodec_alloc_context3(codec)))
        goto fail;
    avcodec_parameters_to_context(a->dec, a->fmt->streams[a->stream]->codecpar);
    if (avcodec_open2(a->dec, codec, NULL) < 0)
        goto fail;
#if LIBAVCODEC_VERSION_MAJOR >= 60
    a->ch = a->dec->ch_layout.nb_channels;
#else
    a->ch = a->dec->channels;
#endif
    a->rate = a->dec->sample_rate;
    if (a->ch < 1 || a->ch > 2 || a->rate <= 0)
        goto fail;
    a->fps = fps > 0 ? fps : 30000.0 / 1001.0;
    a->ring_frames = (uint32_t)(a->rate * RING_SECS);
    a->ring = calloc(a->ring_frames, (size_t)a->ch * sizeof(int16_t));
    a->pkt = av_packet_alloc();
    a->frame = av_frame_alloc();
    a->voice = apu_mixer_alloc_voice();
    if (!a->ring || !a->pkt || !a->frame || a->voice < 0)
        goto fail;
    {
        APUMixerVoice *v = apu_mixer_get_voice(a->voice);
        v->pcm_data = a->ring;
        v->pcm_bytes = a->ring_frames * (uint32_t)a->ch * (uint32_t)sizeof(int16_t);
        v->num_channels = (uint32_t)a->ch;
        v->sample_rate = (uint32_t)a->rate;
        e = getenv("MC3_MOVIE_VOLUME");
        v->volume = e ? (float)atoi(e) / 100.0f : 1.0f;
    }
    /* Bink's own sound is muted from here on: frame 0's audio is decoded
     * before its video, so waiting for the first video frame is too late. */
    a->counted = 1;
    s_audio_users++;
    return a;
fail:
    audio_free(a);
    return NULL;
}

static int16_t to_s16(float f)
{
    f *= 32768.0f;
    return (int16_t)(f > 32767.0f ? 32767 : f < -32768.0f ? -32768 : (int)f);
}

/* Decode the next audio frame into a->pend (interleaved s16). 0 at the end. */
static int audio_decode(MovieAudio *a)
{
    for (;;) {
        int r = avcodec_receive_frame(a->dec, a->frame);
        if (r == 0) {
            int n = a->frame->nb_samples, ch = a->ch, i, c;
            if (n * ch > a->pend_cap) {
                free(a->pend);
                a->pend_cap = n * ch;
                a->pend = malloc((size_t)a->pend_cap * sizeof(int16_t));
                if (!a->pend) { a->pend_cap = 0; return 0; }
            }
            for (i = 0; i < n; i++)
                for (c = 0; c < ch; c++) {
                    int16_t v = 0;
                    switch (a->frame->format) {
                    case AV_SAMPLE_FMT_FLTP: v = to_s16(((const float *)a->frame->data[c])[i]); break;
                    case AV_SAMPLE_FMT_FLT:  v = to_s16(((const float *)a->frame->data[0])[i * ch + c]); break;
                    case AV_SAMPLE_FMT_S16P: v = ((const int16_t *)a->frame->data[c])[i]; break;
                    case AV_SAMPLE_FMT_S16:  v = ((const int16_t *)a->frame->data[0])[i * ch + c]; break;
                    default: break;
                    }
                    a->pend[i * ch + c] = v;
                }
            a->pend_n = n;
            a->pend_off = 0;
            return 1;
        }
        if (r != AVERROR(EAGAIN) || a->eof)
            return 0;
        if (av_read_frame(a->fmt, a->pkt) < 0) {
            a->eof = 1;
            avcodec_send_packet(a->dec, NULL);
            continue;
        }
        if (a->pkt->stream_index == a->stream)
            avcodec_send_packet(a->dec, a->pkt);
        av_packet_unref(a->pkt);
    }
}

/* Move n frames: the soundtrack's next frames into the ring (src, ring),
 * silence into the ring (!src, ring), or soundtrack frames dropped
 * (src, !ring). Past the soundtrack's end, silence. */
static void audio_take(MovieAudio *a, int64_t n, int src, int ring)
{
    while (n > 0) {
        int64_t k = n;
        const int16_t *from = NULL;

        if (ring) {
            uint32_t room = a->ring_frames - (uint32_t)(a->written % a->ring_frames);
            if (k > room) k = room;
        }
        if (src) {
            if (a->pend_off >= a->pend_n && !audio_decode(a)) {
                src = 0;                        /* the soundtrack ended */
                if (!ring) return;
                continue;
            }
            if (k > a->pend_n - a->pend_off) k = a->pend_n - a->pend_off;
            from = a->pend + (size_t)a->pend_off * a->ch;
            a->pend_off += (int)k;
            a->src += k;
        }
        if (ring) {
            int16_t *to = a->ring + (size_t)(a->written % a->ring_frames) * a->ch;
            if (from)
                memcpy(to, from, (size_t)k * a->ch * sizeof(int16_t));
            else
                memset(to, 0, (size_t)k * a->ch * sizeof(int16_t));
            a->written += (uint64_t)k;
        }
        n -= k;
    }
}

/* Zero the stretch after the write position, so a stall (a loading pause,
 * the movie's end) plays silence rather than the ring's previous lap. */
static void audio_zero_ahead(MovieAudio *a, uint32_t frames)
{
    uint64_t w = a->written;
    while (frames) {
        uint32_t at = (uint32_t)(w % a->ring_frames), k = a->ring_frames - at;
        if (k > frames) k = frames;
        memset(a->ring + (size_t)at * a->ch, 0, (size_t)k * a->ch * sizeof(int16_t));
        w += k;
        frames -= k;
    }
}

/* Once per video frame, from the game thread: account for what the mixer has
 * played, keep the soundtrack in step with video frame `frame`, top up. */
void mc3_bink_native_audio(uint32_t hbink, int frame)
{
    Movie *m = find(hbink);
    MovieAudio *a = m ? m->audio : NULL;
    uint32_t pos, frame_bytes;
    int64_t lead_target, drift, want;

    if (!a) return;
    frame_bytes = (uint32_t)a->ch * (uint32_t)sizeof(int16_t);
    lead_target = (int64_t)a->rate * LEAD_MS / 1000;

    if (a->started) {
        int active = 0;
        apu_mixer_get_state(a->voice, &pos, &active, NULL);
        pos /= frame_bytes;
        a->played += (pos + a->ring_frames - a->last_pos) % a->ring_frames;
        a->last_pos = pos;
        if (a->played > a->written) {           /* ran dry: it played silence */
            a->underruns++;
            a->written = a->played + (uint64_t)(a->rate / 100);
        }
        /* Which soundtrack frame plays now, against the one the video is on. */
        want = (int64_t)((double)frame * a->rate / a->fps);
        drift = (a->src - (int64_t)(a->written - a->played)) - want;
        if (drift < -(int64_t)a->rate * RESYNC_MS / 1000) {
            audio_take(a, -drift, 1, 0);        /* behind: skip ahead */
            a->dropped += -drift;
        } else if (drift > (int64_t)a->rate * RESYNC_MS / 1000) {
            int64_t n = drift < lead_target ? drift : lead_target;
            audio_take(a, n, 0, 1);             /* ahead: hold with silence */
            a->inserted += n;
        }
    } else if (frame > 0) {
        /* Joined late (a seek, or the native path started mid-movie). */
        audio_take(a, (int64_t)((double)frame * a->rate / a->fps), 1, 0);
    }

    if ((int64_t)(a->written - a->played) < lead_target)
        audio_take(a, lead_target - (int64_t)(a->written - a->played), 1, 1);
    audio_zero_ahead(a, a->ring_frames / 4);

    if (!a->started) {
        apu_mixer_set_position(a->voice, 0);
        apu_mixer_play(a->voice, 1);
        a->started = 1;
    }
}

int mc3_bink_native_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_NATIVE_BINK");
        on = !(e && e[0] == '0');
        fprintf(stderr, "[BINK] native (FFmpeg) video decode %s\n", on ? "on" : "off (MC3_NATIVE_BINK=0)");
    }
    return on;
}

static Movie *find(uint32_t hbink)
{
    int i;
    for (i = 0; i < MAX_MOVIES; i++)
        if (s_movies[i].hbink == hbink && hbink)
            return &s_movies[i];
    return NULL;
}

static void movie_flush(Movie *m);

static void release(Movie *m)
{
    if (m->skipped)
        movie_flush(m);
    if (m->nofill && nv2a_gl_fill_hold)
        nv2a_gl_fill_hold(-1);
    if (m->dest && nv2a_gl_movie_end)
        nv2a_gl_movie_end(m->dest);
    audio_free(m->audio);
    if (m->frame) av_frame_free(&m->frame);
    if (m->pkt) av_packet_free(&m->pkt);
    if (m->dec) avcodec_free_context(&m->dec);
    if (m->fmt) avformat_close_input(&m->fmt);
    memset(m, 0, sizeof *m);
}

/* The game passes Xbox paths ("d:\video\x.bik", or relative to the title's
 * directory). Ask the runtime first, then fall back to the game folder. */
static int host_path(const char *xbox_name, char *out, size_t n)
{
    const char *dir;
    size_t i;

#ifdef _WIN32
    /* Read as a narrow string, the UTF-16 path was just its first letter:
     * "FFmpeg cannot open Z" and every movie black (2026-10-23). FFmpeg
     * takes UTF-8 on Windows. */
    wchar_t wide[1024];
    int ok = xbox_translate_path(xbox_name, wide, (unsigned int)(sizeof wide / sizeof wide[0]))
             && WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)n, NULL, NULL) > 0;
    if (!ok) {
#else
    if (!xbox_translate_path(xbox_name, out, (unsigned int)n)) {
#endif
        dir = getenv("MC3_GAME_DIR");
        if (!dir || !*dir)
            return 0;
        if (xbox_name[0] && xbox_name[1] == ':')
            xbox_name += 2;
        while (*xbox_name == '\\' || *xbox_name == '/')
            xbox_name++;
        snprintf(out, n, "%s/%s", dir, xbox_name);
        for (i = strlen(dir); out[i]; i++)
            if (out[i] == '\\') out[i] = '/';
    }
    /* "sdmc:/switch/..." reads to FFmpeg as a URL with a protocol named
     * "sdmc", so every movie on the Switch fell back to the lifted decoder
     * (2026-10-17). "file:" names the protocol; it hands the rest to open(),
     * which knows sdmc:. A path starting with '/' has no scheme to misread. */
    if (out[0] != '/' && strchr(out, ':') && strncmp(out, "file:", 5)) {
        size_t len = strlen(out);
        if (len + 5 < n) {
            memmove(out + 5, out, len + 1);
            memcpy(out, "file:", 5);
        }
    }
    return 1;
}

void mc3_bink_native_open(uint32_t hbink, const char *xbox_name)
{
    char path[1024];
    const AVCodec *codec;
    Movie *m = NULL;
    int i;

    if (!hbink || !mc3_bink_native_enabled())
        return;
    for (i = 0; i < MAX_MOVIES && !m; i++)
        if (!s_movies[i].hbink) m = &s_movies[i];
    if (!m) {
        fprintf(stderr, "[BINK] native: no free slot for %s; lifted decoder\n", xbox_name);
        return;
    }
    if (!host_path(xbox_name, path, sizeof path)) {
        fprintf(stderr, "[BINK] native: cannot locate %s; lifted decoder\n", xbox_name);
        return;
    }
    if (avformat_open_input(&m->fmt, path, NULL, NULL) < 0
            || avformat_find_stream_info(m->fmt, NULL) < 0) {
        fprintf(stderr, "[BINK] native: FFmpeg cannot open %s; lifted decoder\n", path);
        release(m);
        return;
    }
    m->stream = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    codec = m->stream < 0 ? NULL
          : avcodec_find_decoder(m->fmt->streams[m->stream]->codecpar->codec_id);
    if (!codec) {
        release(m);
        return;
    }
    m->dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m->dec, m->fmt->streams[m->stream]->codecpar);
    m->dec->thread_count = 1;     /* the game thread waits on this anyway */
    if (avcodec_open2(m->dec, codec, NULL) < 0) {
        release(m);
        return;
    }
    m->pkt = av_packet_alloc();
    m->frame = av_frame_alloc();
    m->hbink = hbink;
    m->next = 0;
    if (fill_skip(xbox_name) && nv2a_gl_fill_hold) {
        m->nofill = 1;
        nv2a_gl_fill_hold(1);
    }
    if (audio_enabled()) {
        AVRational fr = m->fmt->streams[m->stream]->avg_frame_rate;
        m->audio = audio_open(path, fr.den ? av_q2d(fr) : 0);
        if (m->audio)
            fprintf(stderr, "[BINK] native sound: %d Hz, %d ch, mixer voice %d\n",
                    m->audio->rate, m->audio->ch, m->audio->voice);
    }
    fprintf(stderr, "[BINK] native: %s (%dx%d, %s) for HBINK %08X\n", path,
            m->dec->width, m->dec->height, av_get_pix_fmt_name(m->dec->pix_fmt), hbink);
}

void mc3_bink_native_close(uint32_t hbink)
{
    Movie *m = find(hbink);
    if (!m)
        return;
    fprintf(stderr, "[BINK] native: closed HBINK %08X: %ld frames decoded natively, %ld fell back\n",
            hbink, m->served, m->fallbacks);
    if (m->audio)
        fprintf(stderr, "[BINK] native sound: %.1f s played, resyncs dropped %.0f ms / inserted %.0f ms,"
                " %ld underruns\n", (double)m->audio->played / m->audio->rate,
                m->audio->dropped * 1000.0 / m->audio->rate,
                m->audio->inserted * 1000.0 / m->audio->rate, m->audio->underruns);
    release(m);
}

/* Decode until FFmpeg yields frame `want` (0-based); seek if Bink jumped. */
static int decode_to(Movie *m, int want)
{
    if (m->next < 0 || want < m->next || want > m->next + 30) {
        if (av_seek_frame(m->fmt, m->stream, want, AVSEEK_FLAG_BACKWARD) < 0
                && av_seek_frame(m->fmt, m->stream, 0, AVSEEK_FLAG_BACKWARD) < 0)
            return 0;
        avcodec_flush_buffers(m->dec);
        m->next = -1;
    }
    for (;;) {
        int r = av_read_frame(m->fmt, m->pkt);
        if (r < 0)
            return 0;
        if (m->pkt->stream_index != m->stream) {
            av_packet_unref(m->pkt);
            continue;
        }
        r = avcodec_send_packet(m->dec, m->pkt);
        av_packet_unref(m->pkt);
        if (r < 0)
            return 0;
        while ((r = avcodec_receive_frame(m->dec, m->frame)) == 0) {
            int idx = (int)(m->frame->pts != AV_NOPTS_VALUE ? m->frame->pts : m->next);
            m->next = idx + 1;
            if (idx == want)
                return 1;
            if (idx > want)
                return 0;
        }
    }
}

int mc3_bink_native_frame(uint32_t hbink, int want, uint8_t *dest, int w, int h)
{
    Movie *m = find(hbink);
    int cw = (w + 1) / 2, ch = (h + 1) / 2, y;
    const uint8_t *src;

    if (!m)
        return 0;
    m->frame_ok = 0;
    if (m->dec->width != w || m->dec->height != h || m->dec->pix_fmt != AV_PIX_FMT_YUV420P
            || !decode_to(m, want)) {
        m->fallbacks++;
        return 0;
    }
    m->frame_ok = 1;
    /* Bink's frame buffer: Y (w x h), then V, then U (each cw x ch), packed. */
    for (y = 0; y < h; y++)
        memcpy(dest + (size_t)y * w, m->frame->data[0] + (size_t)y * m->frame->linesize[0], (size_t)w);
    dest += (size_t)w * h;
    for (y = 0, src = m->frame->data[2]; y < ch; y++)
        memcpy(dest + (size_t)y * cw, src + (size_t)y * m->frame->linesize[2], (size_t)cw);
    dest += (size_t)cw * ch;
    for (y = 0, src = m->frame->data[1]; y < ch; y++)
        memcpy(dest + (size_t)y * cw, src + (size_t)y * m->frame->linesize[1], (size_t)cw);
    m->served++;
    return 1;
}

/* ── Movie frames upscaled by the renderer ──
 *
 * The title's player converts each frame to RGB on the CPU
 * (BinkCopyToBuffer, 4-7 ms) into a 640x480 texture that is then stretched.
 * Instead the frame's planes go to the renderer (nv2a_gl_movie_frame), which
 * converts and upscales them on the GPU when the texture is drawn. The
 * colour matrix is fitted, on a movie's first frame, to what the player's
 * own conversion wrote (least squares over a grid of pixels), so colours
 * match whatever matrix and range Bink uses. Once the renderer draws the
 * upscaled frame, the player's copy is skipped. MC3_MOVIE_HD=0 off;
 * MC3_MOVIE_SKIP_COPY=0 keeps the player's copy. */
static int movie_hd(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_MOVIE_HD");
        on = !(e && *e == '0') && nv2a_gl_movie_frame != NULL;
    }
    return on;
}

/* Solve the 4x4 system a x = b (Gaussian elimination); 0 if singular. */
static int solve4(double a[4][4], double b[4], double x[4])
{
    int i, j, k;
    for (i = 0; i < 4; i++) {
        int p = i;
        for (k = i + 1; k < 4; k++)
            if (fabs(a[k][i]) > fabs(a[p][i])) p = k;
        if (fabs(a[p][i]) < 1e-12) return 0;
        if (p != i) {
            double t;
            for (j = 0; j < 4; j++) { t = a[i][j]; a[i][j] = a[p][j]; a[p][j] = t; }
            t = b[i]; b[i] = b[p]; b[p] = t;
        }
        for (k = i + 1; k < 4; k++) {
            double f = a[k][i] / a[i][i];
            for (j = i; j < 4; j++) a[k][j] -= f * a[i][j];
            b[k] -= f * b[i];
        }
    }
    for (i = 3; i >= 0; i--) {
        double t = b[i];
        for (j = i + 1; j < 4; j++) t -= a[i][j] * x[j];
        x[i] = t / a[i][i];
    }
    return 1;
}

/* Fit m->mat to the player's output in dest (A8R8G8B8, pitch). */
static void movie_fit(Movie *m, const uint8_t *dest, uint32_t pitch)
{
    double ata[3][4][4], atb[3][4], x[4], err = 0.0;
    int c, gx, gy, n = 0, i, j, w = m->dec->width, h = m->dec->height, bad_alpha = 0, a0 = -1;
    memset(ata, 0, sizeof ata);
    memset(atb, 0, sizeof atb);
    for (gy = 0; gy < 60; gy++)
        for (gx = 0; gx < 80; gx++) {
            int px = (gx * w) / 80 + w / 160, py = (gy * h) / 60 + h / 120;
            const uint8_t *d = dest + (size_t)py * pitch + (size_t)px * 4;   /* B G R A */
            double in[4], out[3];
            in[0] = m->frame->data[0][(size_t)py * m->frame->linesize[0] + px] / 255.0;
            in[1] = m->frame->data[1][(size_t)(py / 2) * m->frame->linesize[1] + px / 2] / 255.0;
            in[2] = m->frame->data[2][(size_t)(py / 2) * m->frame->linesize[2] + px / 2] / 255.0;
            in[3] = 1.0;
            if (a0 < 0) a0 = d[3];             /* alpha must be one value throughout */
            else if (d[3] != a0) bad_alpha++;
            out[0] = d[2] / 255.0; out[1] = d[1] / 255.0; out[2] = d[0] / 255.0;
            for (c = 0; c < 3; c++) {
                if (d[2 - c] == 0 || d[2 - c] == 255)   /* clipped: says nothing */
                    continue;
                for (i = 0; i < 4; i++) {
                    for (j = 0; j < 4; j++) ata[c][i][j] += in[i] * in[j];
                    atb[c][i] += in[i] * out[c];
                }
            }
            n++;
        }
    /* A frame with too little in it (movies open on black) says nothing
     * about the matrix: stay unfitted and try the next frame, for up to
     * ~10 s of movie; Bink keeps copying meanwhile. */
    for (c = 0; c < 3; c++) {
        double cnt = ata[c][3][3], my = cnt > 0 ? ata[c][0][3] / cnt : 0.0;
        double vy = cnt > 0 ? ata[c][0][0] / cnt - my * my : 0.0;   /* luma variance */
        if (cnt >= 800.0 && vy >= (12.0 / 255.0) * (12.0 / 255.0))
            continue;
        if (++m->fit_tries >= 300) {
            m->cal = -1;
            fprintf(stderr, "[BINK] movie colours: no frame to fit in %d tries -> player's own copy kept\n",
                    m->fit_tries);
        }
        return;
    }
    for (c = 0; c < 3; c++) {
        if (!solve4(ata[c], atb[c], x)) { m->cal = -1; break; }
        for (i = 0; i < 4; i++) m->mat[c * 4 + i] = (float)x[i];
    }
    if (m->cal != -1) {
        /* how well it reproduces the player's pixels */
        for (gy = 0; gy < 60; gy++)
            for (gx = 0; gx < 80; gx++) {
                int px = (gx * w) / 80 + w / 160, py = (gy * h) / 60 + h / 120;
                const uint8_t *d = dest + (size_t)py * pitch + (size_t)px * 4;
                double yv = m->frame->data[0][(size_t)py * m->frame->linesize[0] + px] / 255.0;
                double uv = m->frame->data[1][(size_t)(py / 2) * m->frame->linesize[1] + px / 2] / 255.0;
                double vv = m->frame->data[2][(size_t)(py / 2) * m->frame->linesize[2] + px / 2] / 255.0;
                for (c = 0; c < 3; c++) {
                    double v = m->mat[c * 4] * yv + m->mat[c * 4 + 1] * uv + m->mat[c * 4 + 2] * vv
                             + m->mat[c * 4 + 3];
                    double o = d[2 - c] / 255.0, e;
                    v = v < 0 ? 0 : v > 1 ? 1 : v;
                    e = (v - o) * 255.0;
                    err += e * e;
                }
            }
        err = sqrt(err / (n * 3.0));
        m->cal = (err < 4.0 && bad_alpha == 0) ? 1 : -1;
        m->alpha = (a0 < 0 ? 255 : a0) / 255.0f;
    }
    fprintf(stderr, "[BINK] movie colours fitted: rms %.2f/255, alpha %s%d -> %s\n"
            "       R %.3f %.3f %.3f %+.3f | G %.3f %.3f %.3f %+.3f | B %.3f %.3f %.3f %+.3f\n",
            err, bad_alpha ? "varies, first " : "constant ", a0,
            m->cal == 1 ? "upscaled by the renderer" : "player's own copy kept",
            m->mat[0], m->mat[1], m->mat[2], m->mat[3], m->mat[4], m->mat[5], m->mat[6], m->mat[7],
            m->mat[8], m->mat[9], m->mat[10], m->mat[11]);
}

/* The movie closes with the player's copy skipped: the title's texture
 * still holds an old frame, and the title may draw it after the movie --
 * MC3's legal screen shows the intro's last frame (the MC3 logo) under its
 * text, and showed a stale one, dim and behind the text (2026-10-19). Write
 * the last frame there once, as the player would have, with the fitted
 * matrix and alpha. */
static void movie_flush(Movie *m)
{
    int w, h, x, y, c;
    if (!m->dest_host || m->cal != 1 || !m->frame || !m->frame->data[0])
        return;
    w = m->dec->width; h = m->dec->height;
    for (y = 0; y < h; y++) {
        const uint8_t *py = m->frame->data[0] + (size_t)y * m->frame->linesize[0];
        const uint8_t *pu = m->frame->data[1] + (size_t)(y / 2) * m->frame->linesize[1];
        const uint8_t *pv = m->frame->data[2] + (size_t)(y / 2) * m->frame->linesize[2];
        uint8_t *d = m->dest_host + (size_t)y * m->dest_pitch;
        for (x = 0; x < w; x++) {
            float in[3] = { py[x] / 255.0f, pu[x / 2] / 255.0f, pv[x / 2] / 255.0f };
            for (c = 0; c < 3; c++) {
                const float *r = &m->mat[c * 4];
                float v = r[0] * in[0] + r[1] * in[1] + r[2] * in[2] + r[3];
                int b = (int)(v * 255.0f + 0.5f);
                d[x * 4 + 2 - c] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);   /* B G R */
            }
            d[x * 4 + 3] = (uint8_t)(m->alpha * 255.0f + 0.5f);
        }
    }
    m->skipped = 0;
}

/* Around BinkCopyToBuffer(hbink, dest, pitch, height, x, y, flags).
 * after = 0, before the player's copy: 1 = skip it (the renderer shows the
 * upscaled frame). after = 1, once it ran: fit the colours on the first. */
int mc3_bink_native_copy(uint32_t hbink, uint32_t dest_va, const uint8_t *dest,
                         uint32_t pitch, uint32_t height, uint32_t x, uint32_t y, int after)
{
    Movie *m = find(hbink);
    int w, h;
    if (!m || !movie_hd() || !m->frame_ok || m->cal < 0)
        return 0;
    w = m->dec->width; h = m->dec->height;
    if (x || y || pitch < (uint32_t)w * 4 || height < (uint32_t)h) {
        m->cal = -1;                             /* not a plain 32-bit full-frame copy */
        return 0;
    }
    if (after) {
        if (m->cal == 0)
            movie_fit(m, dest, pitch);
        if (m->cal != 1)
            return 0;
    } else if (m->cal != 1) {
        return 0;
    }
    m->dest = dest_va;
    m->dest_host = (uint8_t *)dest;
    m->dest_pitch = pitch;
    if (after)
        m->skipped = 0;                          /* the player's copy just ran */
    nv2a_gl_movie_frame(dest_va, (uint32_t)w, (uint32_t)h,
                        m->frame->data[0], m->frame->linesize[0],
                        m->frame->data[1], m->frame->linesize[1],
                        m->frame->data[2], m->frame->linesize[2], m->mat, m->alpha);
    if (!after) {
        static int skip = -1;
        if (skip < 0) {
            const char *e = getenv("MC3_MOVIE_SKIP_COPY");
            skip = !(e && *e == '0');
        }
        if (skip && nv2a_gl_movie_shown && nv2a_gl_movie_shown(dest_va)) {
            m->skipped = 1;
            return 1;
        }
    }
    return 0;
}

#else  /* built without FFmpeg: the lifted decoder stays in charge */

int  mc3_bink_native_enabled(void) { return 0; }
void mc3_bink_native_open(uint32_t hbink, const char *xbox_name) { (void)hbink; (void)xbox_name; }
int  mc3_bink_native_frame(uint32_t hbink, int frame, uint8_t *dest, int w, int h)
{ (void)hbink; (void)frame; (void)dest; (void)w; (void)h; return 0; }
void mc3_bink_native_close(uint32_t hbink) { (void)hbink; }
int  mc3_bink_native_copy(uint32_t hbink, uint32_t dest_va, const uint8_t *dest,
                          uint32_t pitch, uint32_t height, uint32_t x, uint32_t y, int after)
{ (void)hbink; (void)dest_va; (void)dest; (void)pitch; (void)height; (void)x; (void)y; (void)after; return 0; }
void mc3_bink_native_audio(uint32_t hbink, int frame) { (void)hbink; (void)frame; }
int  mc3_bink_native_audio_active(void) { return 0; }

#endif
