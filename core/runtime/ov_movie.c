// From fe3h-pc-recomp runtime/ov_movie.c @ 47ef99126793dbd13e4a475feb910a13ef6639b8 (MIT, Copyright (c) 2026 Jeff Aigner), copied with fe3h renamed to mp4 only; a decoder the host cannot decode is taken over as a null decoder that drops its stream; decodes H.264 in texture mode with openh264 into the game's NVN textures, openh264 loaded at run time (Cisco's DLL).
// From totk-pc-recomp runtime/ov_movie.c @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), copied with totk renamed to fe3h only. Country-Box: openh264 through dlopen (plat_lib_open), plat threads, SDL message box (Linux); H.264 in buffer mode (NV12 output, Returns HD).
// Adapted from MacNativeGames games/totk/runtime/movie_host.c @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors).
// movie::Decoder for VP9 on the host with libvpx (BSD-3-Clause, linked statically).
// TotK's pre-rendered movies (Event/Movie/*.webm, 28 files: VP9 profile 0, 1920x1080, 30 fps, video only) play through
// the SDK movie library in subsdk0, Android's MediaCodec over NVIDIA NvMM. Its decoder opens NVDEC through NvRm, whose
// first NvOsDrvOpen (sdk+0x3f9050) aborts with "Use of nv::InitializeGraphics is required." because ov_gfx.c takes
// nv::InitializeGraphics over; nvdrv and mm:u are not served either. The game's own demuxer (movie::Extractor) runs as
// guest code; only the decoder is replaced.
// main imports the constructor and calls the decoder through the vtable for movie::Decoder, whose slots are ABS64
// relocations by name: the HOST_OVERRIDE entries below replace them. A VP9 buffer-mode decoder (type 3, mode 1) is a host
// object, +0 the library's vtable, +8 the host Movie (where the library keeps its DecoderImpl); any other decoder runs
// the original code. H.264 (type 1) is decoded on the host too, with openh264: in texture mode (mode 2) into the game's
// NVN textures, in buffer mode (mode 1, Returns HD's Unity player) through the same NV12 output as VP9. Behaviour follows
// DecoderImpl:
// - output format 4 is packed NV12 (Y plane, then interleaved UV; stride = width), copied into the client's Buffer;
// - input and output buffers are indices handed out FIFO, as MediaCodec does; input flag 4 / output flag 4 = end of stream;
// - client events {input available, output available, format changed, error} (DecoderEvents, 4 EventType pointers) are
//   signalled with nn::os::SignalEvent on a guest thread.
// Decoding runs on one host worker thread per decoder (libvpx adds its own tile threads), never on the game's threads:
// SendInputBufferForDecode queues the input and returns. Events the worker owes (input free, format changed, error) are
// signalled at the next entry call or present, whichever comes first.
// Pacing: the game shows one decoded frame per game frame, which plays a 30 fps movie at 30 fps only at 30 fps. So
// decoded frames are handed over when their presentation time is due, counted from the first frame (movie_tick, at every
// present on the presenting guest thread), at most one per present. When the game falls behind, a frame more than 1.5
// periods late is dropped if the next one is due too, so the picture keeps time with the movie's sound.
// MP4_MOVIE_PACE=0 hands every decoded frame over at the next present; MP4_MOVIE_HOST=0 leaves every decoder to the
// library (which aborts at the first movie).
// openh264 is loaded at run time, not linked: the installer downloads Cisco's binary (whose H.264 patent licence holds
// only for that binary, or a build of the system's package). The game cannot be played without it: a null
// decoder in its place starves the game's movie player (it stops feeding input about 27 s into the opening cinematic
// and waits for frames, a black screen with the music going on, issue #2), so movie_require_codec refuses to start
// without it. The library is OPENH264_LIB next to the executable, else the system's OPENH264_LIB or libopenh264.so
// (dlopen). Country-Box: the headers are the system's (2.6.0, soname 8); an older soname is another ABI and is not
// looked for.
#include <emmintrin.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <wels/codec_api.h>
#include <wels/codec_ver.h>

#include <SDL3/SDL_messagebox.h>

#include "cfg.h"
#include "platform/platform.h"
#include "gfx.h"
#include "host.h"
#include "movie.h"
#include "nvn_api.h"
#include "nvn_obj.h"

void kern_backtrace(const Cpu *c);   // kernel.h

enum { IN_BUFS = 4, IN_CAP = 2 << 20, OUT_BUFS = 8, MAX_MOVIES = 16, VPX_THREADS = 4 };
enum { ST_OK = 0, ST_ERROR = 0x1000, ST_NULL = 0x100b, ST_NO_OUTPUT = 0x100e, ST_NO_INPUT = 0x1011,
       ST_UNSUPPORTED = 0x2001, ST_NOT_CONFIGURED = 0x2002 };

const char *const movie_symbols[MOVIE_ENTRIES] = {
    "_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", "_ZN5movie7DecoderD2Ev",
    "_ZN5movie7DecoderD0Ev", "_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE",
    "_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", "_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE",
    "_ZN5movie7Decoder5StartEv", "_ZN5movie7Decoder4StopEv", "_ZN5movie7Decoder5ResetEv", "_ZN5movie7Decoder5FlushEv",
    "_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", "_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE",
    "_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj", "_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_",
    "_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE", "_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_",
    "_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", "_ZN5movie7Decoder12GetLastErrorEv",
    "_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE",
    "_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE"};

typedef struct { int index; unsigned flags; int64_t time; } Ready;
typedef struct { int index, offset, size; int64_t time; unsigned flags; } Pending;
typedef struct {
    uint64_t guest;                          // the movie::Decoder object
    int format, configured, started, w, h, eos_in, format_sent, last_error;
    int unsupported;   // a decoder type or mode the host cannot decode: a null decoder that takes the stream and drops it,
                       // so the player sees its end and the game goes on
    int silent;        // Country-Box: AAC taken over as silence of the stream's length (MP4_MOVIE_AUDIO=silent)
    int ch, rate;      // silent: channels and sample rate from Configure
    int h264;          // H.264: openh264 decodes
    int texture;       // H.264 in texture mode: the frames go into the game's NVN textures as RGBA (get_output), so the
                       // output buffers hold RGBA frames; in buffer mode they hold NV12, as for VP9
    ISVCDecoder *dec;  // openh264, live while h264_live
    int h264_live;
    uint8_t *csd;      // the SPS and PPS from the media data, Annex B, fed to the decoder before the stream
    size_t csd_len;
    uint64_t events;                         // guest DecoderEvents *: EventType * per MOVIE_EV_*
    vpx_codec_ctx_t vpx;
    int vpx_live;
    uint8_t *in[IN_BUFS];                    // kernel region: the game writes the compressed frames here
    int in_free[IN_BUFS], nin_free;
    Pending pending[IN_BUFS];                // sent, not decoded yet (the worker takes them in order)
    int npending;
    uint8_t *out[OUT_BUFS];                  // NV12 frames
    int out_bytes[OUT_BUFS], out_held[OUT_BUFS];
    Ready decoded[OUT_BUFS];                 // decoded, not due yet
    int ndecoded;
    Ready ready[OUT_BUFS];                   // handed over: AcquireOutputBufferIndex returns these
    int nready;
    unsigned owed;                           // events to signal on the next guest thread (bit per MOVIE_EV_*)
    uint64_t t0;                             // host time of the first frame handed over (pacing)
    int64_t pts0;                            // its presentation time (us)
    MovieStats st;
    PlatMutex mu;
    PlatCond work, idle;
    PlatThread *thread;
    int busy, quit;
    uint64_t gen;                            // bumped by Flush, Stop, Reset, destruction: a decode in flight is dropped
} Movie;

static MovieGuest G;
static volatile int32_t g_ready;
static PlatMutex g_init = PLAT_MUTEX_INIT;
static Movie *g_movies[MAX_MOVIES];
static volatile int32_t g_count;                // live host decoders: movie_tick returns at once when 0
static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;   // shared for lookups; lock order: g_lock, then Movie.mu
static uint64_t (*g_clock)(void) = kern_now_ns;
static int g_pace = -1;

void movie_set_clock(uint64_t (*clock)(void)) { g_clock = clock ? clock : kern_now_ns; }
void movie_set_pace(int on) { g_pace = on; }
static int skip_movies(void) {
    static int on = -1;
    if (on < 0) on = getenv("CB_SKIP_MOVIES") && !strcmp(getenv("CB_SKIP_MOVIES"), "1");
    return on;
}

static int pace_on(void) {
    if (g_pace < 0) {
        const char *e = cfg_get("MP4_MOVIE_PACE");
        // Country-Box: a game whose player times frames itself (boot.c movie_self_paced: Tropical Freeze shows a frame
        // when its own clock passes the frame's time, main+0x5ec90c) gets every decoded frame at once; pacing them on
        // the host clock as well dropped 315 of 4471 frames of its opening whenever the two clocks drifted (run-62).
        g_pace = e && *e ? strcmp(e, "0") != 0 : !rt_game_movie_self_paced();
    }
    return g_pace;
}
#define OPENH264_LIB "libopenh264.so.8"   // Country-Box: openh264 2.6 (was .so.7, 2.4)
static long (*p_create)(ISVCDecoder **);
static void (*p_destroy)(ISVCDecoder *);

// 1 when openh264 is loaded (once, at the first H.264 decoder); "OpenH264 Video Codec provided by Cisco Systems, Inc."
// goes to the log, as Cisco's binary licence asks of software that uses it.
static int h264_lib(void) {
    static int state = -1;
    if (state >= 0) return state;
    state = 0;
    char path[PATH_MAX + 64];
    void *h = NULL;
    const char *where = "";
    if (plat_exe_path(path, PATH_MAX)) {   // a copy next to the executable first (the AppImage's own)
        char *slash = strrchr(path, '/');
        if (slash) {
            snprintf(slash + 1, 64, "%s", OPENH264_LIB);
            if (plat_file_exists(path) && (h = plat_lib_open(path))) where = "next to the executable";
        }
    }
    static const char *const names[] = {OPENH264_LIB, "libopenh264.so"};
    for (size_t i = 0; i < sizeof names / sizeof *names && !h; i++)
        if ((h = plat_lib_open(names[i]))) snprintf(path, sizeof path, "%s", names[i]), where = "the system";
    if (!h) {
        rt_log("movie: openh264 (%s) not found next to the executable or in the system", OPENH264_LIB);
        return 0;
    }
    rt_log("movie: openh264 from %s (%s); OpenH264 Video Codec provided by Cisco Systems, Inc.", path, where);
    p_create = (long (*)(ISVCDecoder **))(void (*)(void))plat_lib_sym(h, "WelsCreateDecoder");
    p_destroy = (void (*)(ISVCDecoder *))(void (*)(void))plat_lib_sym(h, "WelsDestroyDecoder");
    if (!p_create || !p_destroy) {
        rt_log("movie: openh264 lacks WelsCreateDecoder/WelsDestroyDecoder");
        return 0;
    }
    return state = 1;
}

static int host_off(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = cfg_get("MP4_MOVIE_HOST");
        v = e && !strcmp(e, "0");
    }
    return v;
}

// Before the game starts (runtime/boot.c rt_boot): without openh264 the opening cinematic never ends (see the top of
// this file), so the game does not start. An interactive start says why in a message box, as the log is not where a
// player looks; a scripted run (--seconds) only logs and exits.
void movie_require_codec(int interactive) {
    if (host_off() || h264_lib()) return;
    if (interactive)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Country-Box",
                                 "The OpenH264 video codec (" OPENH264_LIB ") is missing, and the game's cinematics "
                                 "cannot play without it.\n\n"
                                 "Install your system's openh264 package, or put " OPENH264_LIB " next to the "
                                 "executable.\n\n"
                                 "OpenH264 Video Codec provided by Cisco Systems, Inc.", NULL);
    rt_fatal("movie: openh264 (%s) is needed for the game's cinematics: install the system's openh264 package or put "
             "%s next to the executable", OPENH264_LIB, OPENH264_LIB);
}

static uint64_t need(const char *name) {
    uint64_t a = ldr_symbol(name);
    if (!a) rt_fatal("movie: guest symbol %s is not loaded", name);
    return a;
}

// The guest functions, resolved the first time any entry runs (the modules are loaded and relocated by then).
static void guest_init(void) {
    if (g_ready) return;
    plat_mutex_lock(&g_init);
    if (!g_ready) {
        G.signal_event = need("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
        G.buf_set_range = need("_ZN5movie6Buffer8SetRangeEii");
        G.buf_set_data = need("_ZN5movie6Buffer18SetDataAndCapacityEPvm");
        G.buf_base = need("_ZNK5movie6Buffer4BaseEv");
        G.buf_capacity = need("_ZNK5movie6Buffer8CapacityEv");
        G.md_find_int32 = need("_ZNK5movie9MediaData9FindInt32EPKcPi");
        G.md_find_string = need("_ZNK5movie9MediaData10FindStringEPKcPS2_");
        G.md_set_int32 = need("_ZN5movie9MediaData8SetInt32EPKci");
        G.md_set_rect = need("_ZN5movie9MediaData7SetRectEPKciiii");
        G.md_set_string = need("_ZN5movie9MediaData9SetStringEPKcS2_");
        G.md_find_buffer = need("_ZNK5movie9MediaData10FindBufferEPKcPPNS_6BufferE");
        G.buf_size = need("_ZNK5movie6Buffer4SizeEv");
        G.vptr = need("_ZTVN5movie7DecoderE") + 0x10;
        for (int i = 0; i < MOVIE_ENTRIES; i++) G.orig[i] = need(movie_symbols[i]);
        plat_atomic_xchg(&g_ready, 1);
    }
    plat_mutex_unlock(&g_init);
}

void movie_set_guest(const MovieGuest *g) {
    plat_mutex_lock(&g_init);
    G = *g;
    plat_atomic_xchg(&g_ready, 1);
    plat_mutex_unlock(&g_init);
}

#define U(x) ((uint64_t)(uintptr_t)(x))
static void buf_set_range(Cpu *c, uint64_t b, int off, int n) { rt_call(c, G.buf_set_range, 3, b, (uint64_t)off, (uint64_t)n); }
static void buf_set_data(Cpu *c, uint64_t b, uint64_t p, uint64_t n) { rt_call(c, G.buf_set_data, 3, b, p, n); }
static int md_find_int32(Cpu *c, uint64_t md, const char *k, int *v) {
    return (uint8_t)rt_call(c, G.md_find_int32, 3, md, U(k), U(v));
}
static void md_set_int32(Cpu *c, uint64_t md, const char *k, int v) { rt_call(c, G.md_set_int32, 3, md, U(k), (uint64_t)(uint32_t)v); }

// Country-Box: SDK 4.4.0 (Tropical Freeze) constructs decoders with the three-argument constructor (no trailing int);
// subsdk0 exports both forms. ov_ctor serves both; a decoder left to the library runs the form the game called.
#define CTOR3 "_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeE"
static _Thread_local int t_ctor3;

// The original code of an entry with the caller's arguments; its result stays in x0.
static void pass(Cpu *c, int which) {
    uint64_t a[8];
    memcpy(a, c->x, sizeof a);
    uint64_t orig = G.orig[which];
    if (which == MOVIE_CTOR && t_ctor3) {   // Country-Box: the three-argument constructor's own original
        static uint64_t ctor3;
        if (!ctor3) ctor3 = need(CTOR3);
        orig = ctor3;
    }
    if (!orig) rt_fatal("movie: no original code for %s", movie_symbols[which]);
    rt_call(c, orig, 8, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}

static Movie *find(uint64_t guest) {
    Movie *m = NULL;
    pthread_rwlock_rdlock(&g_lock);
    for (int i = 0; i < MAX_MOVIES && !m; i++)
        if (g_movies[i] && g_movies[i]->guest == guest) m = g_movies[i];
    pthread_rwlock_unlock(&g_lock);
    return m;
}

// Events are signalled after the locks are released: the woken game thread calls straight back in.
typedef struct { uint64_t ev[4 * MAX_MOVIES]; int n; } Signals;
static void collect(Movie *m, Signals *s) {   // m->mu held
    for (int k = 0; k < 4; k++)
        if (m->owed & (1u << k) && m->events && s->n < (int)(sizeof s->ev / sizeof *s->ev)) {
            uint64_t ev = LD64(m->events + 8 * (uint64_t)k);
            if (ev) s->ev[s->n++] = ev;
        }
    m->owed = 0;
}
static void fire(Cpu *c, const Signals *s) {
    for (int i = 0; i < s->n; i++) rt_call(c, G.signal_event, 1, s->ev[i]);
}

static int frame_bytes(const Movie *m) { return m->silent ? 1024 * m->ch * 2 : m->texture ? m->w * m->h * 4 : m->w * m->h + (m->w / 2) * (m->h / 2) * 2; }

// I420 (libvpx, openh264) -> packed NV12: Y rows, then rows of interleaved U, V.
static void to_nv12(const uint8_t *py, const uint8_t *pu, const uint8_t *pv, int ys, int us, int vs, uint8_t *dst, int w,
                    int h) {
    for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * w, py + (size_t)y * ys, (size_t)w);
    uint8_t *uv = dst + (size_t)w * h;
    int cw = w / 2;
    for (int y = 0; y < h / 2; y++) {
        const uint8_t *u = pu + (size_t)y * us;
        const uint8_t *v = pv + (size_t)y * vs;
        uint8_t *o = uv + (size_t)y * w;
        int x = 0;
        for (; x + 16 <= cw; x += 16) {
            __m128i a = _mm_loadu_si128((const __m128i *)(u + x)), b = _mm_loadu_si128((const __m128i *)(v + x));
            _mm_storeu_si128((__m128i *)(o + 2 * x), _mm_unpacklo_epi8(a, b));
            _mm_storeu_si128((__m128i *)(o + 2 * x + 16), _mm_unpackhi_epi8(a, b));
        }
        for (; x < cw; x++) {
            o[2 * x] = u[x];
            o[2 * x + 1] = v[x];
        }
    }
}

static int free_out(const Movie *m) {
    for (int i = 0; i < OUT_BUFS; i++)
        if (!m->out_held[i]) return i;
    return -1;
}
static int free_outs(const Movie *m) {
    int n = 0;
    for (int i = 0; i < OUT_BUFS; i++) n += !m->out_held[i];
    return n;
}
static int can_take(const Movie *m, const Pending *p) {
    int want = (p->size > 0) + ((p->flags & MOVIE_FLAG_EOS) && !m->eos_in);
    return free_outs(m) >= want;
}
static void push_decoded(Movie *m, int slot, unsigned flags, int64_t time, int bytes) {
    m->out_held[slot] = 1;
    m->out_bytes[slot] = bytes;
    m->decoded[m->ndecoded++] = (Ready){slot, flags, time};
}

// I420 (openh264) -> RGBA8 for the game's RGBA8_SRGB texture: BT.709 limited range, the HD video default. Eight
// pixels at a time in 16-bit SSE2 arithmetic with the coefficients scaled by 32 (1.164 -> 37, 1.793 -> 57, 0.213 -> 7,
// 0.533 -> 17, 2.112 -> 68), which keeps every term inside 16 bits and is within two levels of the exact value; the
// scalar tail handles a width that is not a multiple of eight. Two rows and two columns share a chroma sample.
static void rgba_pixel(int y, int d, int e, uint8_t *o) {
    int c = 37 * (y - 16);
    int r = (c + 57 * e + 16) >> 5, g = (c - 7 * d - 17 * e + 16) >> 5, b = (c + 68 * d + 16) >> 5;
    o[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
    o[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
    o[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    o[3] = 255;
}
static void to_rgba(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ys, int uvs, uint8_t *dst, int w, int h) {
    const __m128i zero = _mm_setzero_si128(), c16 = _mm_set1_epi16(16), c128 = _mm_set1_epi16(128), round = _mm_set1_epi16(16);
    const __m128i ky = _mm_set1_epi16(37), kr = _mm_set1_epi16(57), kgd = _mm_set1_epi16(7), kge = _mm_set1_epi16(17), kb = _mm_set1_epi16(68);
    const __m128i alpha = _mm_set1_epi16(255);
    for (int j = 0; j < h; j++) {
        const uint8_t *yr = y + (size_t)j * ys, *ur = u + (size_t)(j / 2) * uvs, *vr = v + (size_t)(j / 2) * uvs;
        uint8_t *o = dst + (size_t)j * w * 4;
        int i = 0;
        for (; i + 8 <= w; i += 8) {
            __m128i Y = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(yr + i)), zero), c16);
            __m128i U4 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(*(const int32_t *)(ur + i / 2)), zero);   // 4 chroma samples
            __m128i V4 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(*(const int32_t *)(vr + i / 2)), zero);
            __m128i D = _mm_sub_epi16(_mm_unpacklo_epi16(U4, U4), c128);   // each doubled: two columns share it
            __m128i E = _mm_sub_epi16(_mm_unpacklo_epi16(V4, V4), c128);
            __m128i C = _mm_add_epi16(_mm_mullo_epi16(Y, ky), round);
            __m128i R = _mm_srai_epi16(_mm_add_epi16(C, _mm_mullo_epi16(E, kr)), 5);
            __m128i G = _mm_srai_epi16(_mm_sub_epi16(_mm_sub_epi16(C, _mm_mullo_epi16(D, kgd)), _mm_mullo_epi16(E, kge)), 5);
            __m128i B = _mm_srai_epi16(_mm_add_epi16(C, _mm_mullo_epi16(D, kb)), 5);
            // interleave to RGBA: R,G pairs and B,A pairs as 16-bit lanes, then bytes
            __m128i rg = _mm_or_si128(_mm_packus_epi16(R, zero), _mm_slli_si128(_mm_packus_epi16(G, zero), 8));   // 8 R then 8 G bytes
            __m128i ba = _mm_or_si128(_mm_packus_epi16(B, zero), _mm_slli_si128(_mm_packus_epi16(alpha, zero), 8));
            __m128i rg16 = _mm_unpacklo_epi8(rg, _mm_srli_si128(rg, 8));   // R0 G0 R1 G1 ...
            __m128i ba16 = _mm_unpacklo_epi8(ba, _mm_srli_si128(ba, 8));   // B0 A0 B1 A1 ...
            _mm_storeu_si128((__m128i *)(o + 4 * i), _mm_unpacklo_epi16(rg16, ba16));
            _mm_storeu_si128((__m128i *)(o + 4 * i + 16), _mm_unpackhi_epi16(rg16, ba16));
        }
        for (; i < w; i++) rgba_pixel(yr[i], ur[i / 2] - 128, vr[i / 2] - 128, o + 4 * i);
    }
}

static int h264_open(Movie *m) {
    if (p_create(&m->dec) || !m->dec) return 0;
    SDecodingParam param;
    memset(&param, 0, sizeof param);
    param.uiTargetDqLayer = 255;
    param.eEcActiveIdc = ERROR_CON_SLICE_COPY;
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_DEFAULT;
    if ((*m->dec)->Initialize(m->dec, &param)) {
        p_destroy(m->dec);
        m->dec = NULL;
        return 0;
    }
    // One decoding thread: openh264's thread pool faulted inside the game's Configure call (a write to a low address).
    m->h264_live = 1;
    if (m->csd_len) {   // the parameter sets first; they produce no picture
        unsigned char *planes[3] = {0};
        SBufferInfo info;
        memset(&info, 0, sizeof info);
        (*m->dec)->DecodeFrameNoDelay(m->dec, m->csd, (int)m->csd_len, planes, &info);
    }
    return 1;
}

static void h264_close(Movie *m) {
    if (m->h264_live) {
        (*m->dec)->Uninitialize(m->dec);
        p_destroy(m->dec);
        m->dec = NULL;
        m->h264_live = 0;
    }
}

static int vpx_open(Movie *m) {
    vpx_codec_dec_cfg_t cfg = {.threads = VPX_THREADS, .w = (unsigned)m->w, .h = (unsigned)m->h};
    if (vpx_codec_dec_init(&m->vpx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) return 0;
    vpx_codec_control(&m->vpx, VP9D_SET_ROW_MT, 1);
    m->vpx_live = 1;
    return 1;
}

// The worker: takes inputs in order, decodes them outside the lock into a reserved output slot, publishes the frame.
static void worker(void *arg) {
    Movie *m = arg;
    plat_mutex_lock(&m->mu);
    for (;;) {
        while (!m->quit && !(m->started && (m->vpx_live || m->h264_live || m->unsupported || m->silent) && m->npending && can_take(m, &m->pending[0])))
            plat_cond_wait(&m->work, &m->mu);
        if (m->quit) break;
        Pending p = m->pending[0];
        memmove(m->pending, m->pending + 1, (size_t)--m->npending * sizeof *m->pending);
        // Country-Box: CB_SKIP_MOVIES=1 (sweeps): the first input ends the stream, no frame is decoded, so the player
        // reaches the movie's end at once and the game goes on as after any movie.
        if (skip_movies() && !m->eos_in) {
            m->in_free[m->nin_free++] = p.index;
            m->owed |= 1u << MOVIE_EV_INPUT;
            m->eos_in = 1;
            push_decoded(m, free_out(m), MOVIE_FLAG_EOS, p.time, 0);
            rt_log("movie: skipped (CB_SKIP_MOVIES): end of stream at the first input");
            continue;
        }
        if (skip_movies()) {   // inputs after the skip are given back unread
            m->in_free[m->nin_free++] = p.index;
            m->owed |= 1u << MOVIE_EV_INPUT;
            continue;
        }
        int slot = -1;
        if (p.size > 0) {
            slot = free_out(m);
            m->out_held[slot] = 1;   // reserved: in no queue, so the game cannot release it
        }
        uint64_t gen = m->gen, dt = 0;
        int64_t out_time = p.time;   // Country-Box: the picture's own time (H.264 with B-frames comes out reordered)
        int got = 0, w = m->w, h = m->h;
        uint8_t *dst = slot >= 0 ? m->out[slot] : NULL;
        vpx_codec_err_t e = VPX_CODEC_OK;
        const char *bad = NULL;
        m->busy = 1;
        plat_mutex_unlock(&m->mu);
        if (p.size > 0 && m->h264_live) {
            uint64_t t0 = kern_now_ns();
            unsigned char *planes[3] = {0};
            SBufferInfo info;
            memset(&info, 0, sizeof info);
            info.uiInBsTimeStamp = (unsigned long long)p.time;
            DECODING_STATE ds = (*m->dec)->DecodeFrameNoDelay(m->dec, m->in[p.index] + p.offset, p.size, planes, &info);
            if (info.iBufferStatus == 1) out_time = (int64_t)info.uiOutYuvTimeStamp;
            if (ds != dsErrorFree && ds != dsFramePending) e = VPX_CODEC_ERROR;   // reported below as a decode error
            if (info.iBufferStatus == 1 && planes[0]) {
                int pw = info.UsrData.sSystemBuffer.iWidth, ph = info.UsrData.sSystemBuffer.iHeight;
                if (pw != w || ph != h) bad = "layout";
                else if (m->texture) to_rgba(planes[0], planes[1], planes[2], info.UsrData.sSystemBuffer.iStride[0],
                                             info.UsrData.sSystemBuffer.iStride[1], dst, w, h);
                else to_nv12(planes[0], planes[1], planes[2], info.UsrData.sSystemBuffer.iStride[0],
                             info.UsrData.sSystemBuffer.iStride[1], info.UsrData.sSystemBuffer.iStride[1], dst, w, h);
                got = 1;
            }
            dt = kern_now_ns() - t0;
        } else if (p.size > 0 && m->silent) {   // one AAC access unit: 1024 samples per channel of silence
            memset(dst, 0, (size_t)frame_bytes(m));
            got = 1;
        } else if (p.size > 0 && !m->unsupported) {   // VP9; the null decoder drops the input: no frame, no error
            uint64_t t0 = kern_now_ns();
            e = vpx_codec_decode(&m->vpx, m->in[p.index] + p.offset, (unsigned)p.size, NULL, 0);
            if (e == VPX_CODEC_OK) {
                vpx_codec_iter_t it = NULL;
                const vpx_image_t *img;
                while ((img = vpx_codec_get_frame(&m->vpx, &it)) != NULL) {
                    if (img->fmt != VPX_IMG_FMT_I420 || (int)img->d_w != w || (int)img->d_h != h) bad = "layout";
                    else if (!got) to_nv12(img->planes[VPX_PLANE_Y], img->planes[VPX_PLANE_U], img->planes[VPX_PLANE_V],
                                           img->stride[VPX_PLANE_Y], img->stride[VPX_PLANE_U], img->stride[VPX_PLANE_V],
                                           dst, w, h);
                    got++;
                }
            }
            dt = kern_now_ns() - t0;
        }
        plat_mutex_lock(&m->mu);
        m->busy = 0;
        plat_cond_broadcast(&m->idle);
        if (m->gen != gen) continue;   // flushed, stopped or reset meanwhile: the queues were reset after this
        m->in_free[m->nin_free++] = p.index;
        m->owed |= 1u << MOVIE_EV_INPUT;
        if (p.size > 0) {
            m->st.decode_ns += dt;
            if (dt > m->st.max_ns) m->st.max_ns = dt;
            if (bad) rt_fatal("movie: a decoded frame's layout differs from the configured %dx%d", w, h);
            if (e != VPX_CODEC_OK || !got) {
                m->out_held[slot] = 0;
                // Country-Box: Tropical Freeze's player sends end of stream on an input of the whole buffer's capacity
                // at time 0 (measured: 2 MiB, garbage after the last unit). Its failure is not the stream's error.
                if (e != VPX_CODEC_OK && !(p.flags & MOVIE_FLAG_EOS)) {
                    m->st.errors++;
                    m->last_error = ST_ERROR;
                    m->owed |= 1u << MOVIE_EV_ERROR;
                    if (m->st.errors <= 4) {
                        const char *d = m->vpx_live ? vpx_codec_error_detail(&m->vpx) : "openh264 refused the access unit";
                        const uint8_t *b = m->in[p.index] + p.offset;   // Country-Box: the start says Annex B or AVCC
                        rt_log("movie: decode error on a %d-byte frame at %lld us: %s (starts %02x %02x %02x %02x %02x %02x)",
                               p.size, (long long)p.time, d ? d : vpx_codec_err_to_string(e), b[0], b[1], b[2], b[3], b[4],
                               b[5]);
                    }
                }
            } else {
                if (got > 1) rt_log("movie: %d frames from one input at %lld us, the first kept", got, (long long)p.time);
                if (!m->format_sent) {
                    m->format_sent = 1;
                    m->owed |= 1u << MOVIE_EV_FORMAT;
                }
                // Tropical Freeze's player shows frames in queue order and only when their time is past the last shown
                // (main+0x5ec90c, measured): a frame with its input's decode-order time stalled it for good.
                push_decoded(m, slot, 0, out_time, frame_bytes(m));
                m->st.frames++;
                if (m->h264 && (m->st.frames <= 20 || m->st.frames % 60 == 0))
                    rt_log("movie: H.264 frame %llu at %lld us, decode %.1f ms (max %.1f), %llu dropped, %llu handed over",
                           (unsigned long long)m->st.frames, (long long)p.time, dt / 1e6, m->st.max_ns / 1e6,
                           (unsigned long long)m->st.dropped, (unsigned long long)m->st.handed);
                if (m->st.frames % 600 == 0)
                    rt_log("movie: %llu frames, decode %.2f ms per frame (max %.2f)", (unsigned long long)m->st.frames,
                           m->st.decode_ns / 1e6 / (double)m->st.frames, m->st.max_ns / 1e6);
            }
        }
        if (p.flags & MOVIE_FLAG_EOS && !m->eos_in) {
            m->eos_in = 1;
            push_decoded(m, free_out(m), MOVIE_FLAG_EOS, p.time, 0);
            rt_log("movie: end of stream after %llu frames (decode %.2f ms per frame, max %.2f, %llu errors)",
                   (unsigned long long)m->st.frames, m->st.frames ? m->st.decode_ns / 1e6 / (double)m->st.frames : 0.0,
                   m->st.max_ns / 1e6, (unsigned long long)m->st.errors);
        }
    }
    plat_mutex_unlock(&m->mu);
}

// With m->mu held: invalidate a decode in flight and wait for the worker to leave it. Returns whether the calling guest
// thread gave its core up; the caller calls kern_core_back after releasing m->mu.
static int quiesce(Movie *m) {
    int away = 0;
    m->gen++;
    while (m->busy) {
        if (!away) away = kern_core_away();
        plat_cond_wait(&m->idle, &m->mu);
    }
    return away;
}
static void unlock_back(Movie *m, int away) {
    plat_mutex_unlock(&m->mu);
    kern_core_back(&away);
}

static void reset_queues(Movie *m) {
    m->nin_free = 0;
    for (int i = 0; i < IN_BUFS; i++) m->in_free[m->nin_free++] = i;
    memset(m->out_held, 0, sizeof m->out_held);
    m->npending = m->nready = m->ndecoded = 0;
    m->eos_in = 0;
    m->t0 = 0;
}
static void release_codec(Movie *m) {   // m->mu held, worker quiesced
    if (m->vpx_live) {
        vpx_codec_destroy(&m->vpx);
        m->vpx_live = 0;
    }
    h264_close(m);
    for (int i = 0; i < IN_BUFS; i++) {
        if (m->in[i]) mem_kernel_free(m->in[i], IN_CAP);
        m->in[i] = NULL;
    }
    for (int i = 0; i < OUT_BUFS; i++) {
        free(m->out[i]);
        m->out[i] = NULL;
    }
    m->configured = m->started = 0;
    reset_queues(m);
}

// Host time a decoded frame is due (ns); end of stream is due as soon as it is first in line.
static uint64_t due_at(const Movie *m, const Ready *d) {
    int64_t ahead = d->time - m->pts0;
    if (ahead < 0) ahead = 0;
    return m->t0 + (uint64_t)ahead * 1000;
}
// One present: drop leading frames more than 1.5 periods late while the next one is due too, then hand over at most one
// due frame (end of stream right after the last frame).
static void release_due(Movie *m, uint64_t now) {
    if (!m->ndecoded) return;
    if (!pace_on()) {
        for (int i = 0; i < m->ndecoded; i++) m->ready[m->nready++] = m->decoded[i];
        m->st.handed += m->ndecoded;
        m->ndecoded = 0;
        m->owed |= 1u << MOVIE_EV_OUTPUT;
        return;
    }
    while (m->ndecoded > 1 && m->t0 && !(m->decoded[0].flags & MOVIE_FLAG_EOS) &&
           !(m->decoded[1].flags & MOVIE_FLAG_EOS)) {
        uint64_t due = due_at(m, &m->decoded[0]), next = due_at(m, &m->decoded[1]);
        if (now < next || now - due <= (next - due) * 3 / 2) break;
        m->out_held[m->decoded[0].index] = 0;
        m->st.dropped++;
        memmove(m->decoded, m->decoded + 1, (size_t)--m->ndecoded * sizeof *m->decoded);
        plat_cond_signal(&m->work);
    }
    Ready d = m->decoded[0];
    if (!(d.flags & MOVIE_FLAG_EOS)) {
        if (!m->t0) {
            m->t0 = now;
            m->pts0 = d.time;
        }
        if (now < due_at(m, &d)) return;
    }
    memmove(m->decoded, m->decoded + 1, (size_t)--m->ndecoded * sizeof *m->decoded);
    m->ready[m->nready++] = d;
    m->st.handed++;
    m->owed |= 1u << MOVIE_EV_OUTPUT;
}

void movie_tick(Cpu *c) {
    if (!g_count) return;
    Signals s = {0};
    pthread_rwlock_rdlock(&g_lock);
    for (int i = 0; i < MAX_MOVIES; i++) {
        Movie *m = g_movies[i];
        if (!m) continue;
        plat_mutex_lock(&m->mu);
        if (m->started) release_due(m, g_clock());
        collect(m, &s);
        plat_mutex_unlock(&m->mu);
    }
    pthread_rwlock_unlock(&g_lock);
    fire(c, &s);
}

int movie_stats(uint64_t obj, MovieStats *out) {
    Movie *m = find(obj);
    if (!m) return 0;
    plat_mutex_lock(&m->mu);
    *out = m->st;
    plat_mutex_unlock(&m->mu);
    return 1;
}

int movie_quiet(uint64_t obj) {
    Movie *m = find(obj);
    if (!m) return 1;
    plat_mutex_lock(&m->mu);
    int q = !m->busy && !(m->started && (m->vpx_live || m->h264_live) && m->npending && can_take(m, &m->pending[0]));
    plat_mutex_unlock(&m->mu);
    return q;
}

// ---- entries
// Decoder(DecoderType type, DecoderOutputFormat format, DecoderMode mode, int priority)
static void ov_ctor(Cpu *c) {
    uint64_t obj = arg_x(c, 0);
    int type = (int)arg_x(c, 1), format = (int)arg_x(c, 2), mode = (int)arg_x(c, 3);
    guest_init();
    if (host_off()) {
        rt_log("movie: decoder type %d mode %d format %d left to the library (MP4_MOVIE_HOST=0)", type, mode, format);
        pass(c, MOVIE_CTOR);
        return;
    }
    int h264 = type == MOVIE_TYPE_H264 && (mode == MOVIE_MODE_TEXTURE || mode == MOVIE_MODE_BUFFER);
    if (h264 && !h264_lib()) rt_fatal("movie: an H.264 decoder without openh264");   // movie_require_codec let it by
    // Audio (type 4, "audio/mp4a-latm" in this game's cinematics) is left to the library: its decoders for AAC and
    // Opus are software (FDK AAC, libopus inside subsdk0) and run lifted; only the video decoders need the NVIDIA
    // driver. MP4_MOVIE_AUDIO=null takes it over as a null decoder instead (the game then shows no video either: its
    // player waits on the audio clock).
    // Country-Box: MP4_MOVIE_AUDIO=silent takes AAC over as silence of the stream's length (a diagnosis aid: the player
    // gets audio buffers on time whatever the library's decoder does).
    int silent = type == MOVIE_TYPE_AAC && (!strcmp(cfg_value("MP4_MOVIE_AUDIO"), "silent") || skip_movies());
    if (type == MOVIE_TYPE_AAC && !silent && strcmp(cfg_value("MP4_MOVIE_AUDIO"), "null")) {
        rt_log("movie: audio decoder type %d mode %d format %d left to the library", type, mode, format);
        pass(c, MOVIE_CTOR);
        return;
    }
    int unsupported = !h264 && !silent && (type != MOVIE_TYPE_VP9 || mode != MOVIE_MODE_BUFFER);
    // The library's decoder would run lifted into the NVIDIA driver (NvRm), which the host does not serve. VP9 in
    // buffer mode (TotK), H.264 in texture mode (MP4: the decoder writes into the game's own NVN textures) and H.264 in
    // buffer mode (Returns HD: NV12 frames the game uploads itself) are decoded on the host; any other decoder is
    // taken over as a null decoder that takes the stream and shows nothing, so the player runs through to the end of
    // the movie.
    if (unsupported)
        rt_log("movie: decoder type %d mode %d format %d: the host decodes VP9 in buffer mode and H.264 in buffer and "
               "texture mode only; a null decoder stands in", type, mode, format);
    Movie *m = calloc(1, sizeof *m);
    if (!m) rt_fatal("movie: decoder allocation");
    m->guest = obj;
    m->format = format;
    m->unsupported = unsupported;
    m->silent = silent;
    m->h264 = h264;
    m->texture = h264 && mode == MOVIE_MODE_TEXTURE;
    plat_mutex_init(&m->mu);
    plat_cond_init(&m->work);
    plat_cond_init(&m->idle);
    m->thread = plat_thread_new(worker, m, "movie decode");
    if (!m->thread) rt_fatal("movie: worker thread");
    pthread_rwlock_wrlock(&g_lock);
    int i = 0;
    while (i < MAX_MOVIES && g_movies[i] && g_movies[i]->guest != obj) i++;
    if (i == MAX_MOVIES) rt_fatal("movie: more than %d live decoders", MAX_MOVIES);
    if (g_movies[i]) rt_fatal("movie: decoder object %#llx constructed twice", (unsigned long long)obj);
    g_movies[i] = m;
    plat_atomic_inc(&g_count);
    pthread_rwlock_unlock(&g_lock);
    ST64(obj, G.vptr);
    ST64(obj + 8, U(m));
    rt_log("movie: %s decoder %#llx on the host (libvpx %s, openh264 %d.%d), output format %d",
           m->texture ? "H.264 texture-mode" : h264 ? "H.264 buffer-mode" : unsupported ? "null" : "VP9", (unsigned long long)obj,
           vpx_codec_version_str(), OPENH264_MAJOR, OPENH264_MINOR, format);
}

static void destroy(Cpu *c, uint64_t obj) {
    (void)c;
    Movie *m = NULL;
    pthread_rwlock_wrlock(&g_lock);
    for (int i = 0; i < MAX_MOVIES; i++)
        if (g_movies[i] && g_movies[i]->guest == obj) {
            m = g_movies[i];
            g_movies[i] = NULL;
            plat_atomic_dec(&g_count);
        }
    pthread_rwlock_unlock(&g_lock);
    if (!m) return;
    plat_mutex_lock(&m->mu);
    m->quit = 1;
    m->gen++;
    plat_cond_broadcast(&m->work);
    plat_mutex_unlock(&m->mu);
    int away = kern_core_away();
    plat_thread_join(m->thread);
    kern_core_back(&away);
    rt_log("movie: decoder %#llx destroyed after %llu frames (%llu dropped to keep time)", (unsigned long long)obj,
           (unsigned long long)m->st.frames, (unsigned long long)m->st.dropped);
    release_codec(m);
    free(m->csd);
    free(m);
    ST64(obj, G.vptr);   // what the library's destructor leaves (no DecoderImpl)
    ST64(obj + 8, 0);
}

static void ov_d2(Cpu *c) {
    uint64_t obj = arg_x(c, 0);
    if (!find(obj)) { pass(c, MOVIE_D2); return; }
    destroy(c, obj);
}
// Deleting destructor: free the host state, then the library's deleting destructor frees the object (impl now 0).
static void ov_d0(Cpu *c) {
    uint64_t obj = arg_x(c, 0);
    if (find(obj)) destroy(c, obj);
    pass(c, MOVIE_D0);
}

// The first calls on a null or H.264 decoder, logged by name: the player's protocol, for reading its reaction.
static void note_entry(const Movie *m, const char *name) {
    if (!m->unsupported && !m->h264 && !m->silent) return;
    static volatile int32_t calls, scalls;
    if (m->silent ? plat_atomic_inc(&scalls) <= 60 : plat_atomic_inc(&calls) <= 60)
        rt_log("movie: %s decoder %#llx: %s", m->h264 ? "H.264" : m->silent ? "AAC (silent)" : "null",
               (unsigned long long)m->guest, name);
}

// Status Configure(MediaData *format)
static void ov_configure(Cpu *c) {
    uint64_t obj = arg_x(c, 0), md = arg_x(c, 1);
    Movie *m = find(obj);
    if (!m) { pass(c, MOVIE_CONFIGURE); return; }
    if (!md) { ret_x(c, ST_NULL); return; }
    if (m->format != MOVIE_FORMAT_NV12 && !m->unsupported && !m->texture && !m->silent) {
        rt_log("movie: output format %d not implemented on the host (NV12 = 4 only)", m->format);
        ret_x(c, ST_UNSUPPORTED);
        return;
    }
    uint64_t mime_p = 0;
    int w = 0, h = 0;
    rt_call(c, G.md_find_string, 3, md, U("mime"), U(&mime_p));
    md_find_int32(c, md, "width", &w);
    md_find_int32(c, md, "height", &h);
    const char *mime = (const char *)(uintptr_t)mime_p;
    uint8_t *csd = NULL;
    size_t csd_len = 0;
    int ch = 0, rate = 0;
    if (m->silent) {
        md_find_int32(c, md, "channel-count", &ch);
        md_find_int32(c, md, "sample-rate", &rate);
        if (ch <= 0 || ch > 8) ch = 2;
        if (rate <= 0) rate = 48000;
        rt_log("movie: AAC %s, %d channels at %d Hz: silence stands in", mime ? mime : "(no mime)", ch, rate);
    } else if (m->h264) {   // the parameter sets come as "csd-0" (SPS) and "csd-1" (PPS) buffers, Annex B
        if (!mime || strcmp(mime, "video/avc") || w <= 0 || h <= 0 || w > 8192 || h > 8192 || (w | h) & 1) {
            rt_log("movie: configure: mime %s %dx%d is not H.264", mime ? mime : "(none)", w, h);
            ret_x(c, ST_UNSUPPORTED);
            return;
        }
        for (int k = 0; k < 2; k++) {
            uint64_t b = 0;
            if (!(uint8_t)rt_call(c, G.md_find_buffer, 3, md, U(k ? "csd-1" : "csd-0"), U(&b)) || !b) continue;
            const uint8_t *base = (const uint8_t *)(uintptr_t)rt_call(c, G.buf_base, 1, b);
            uint64_t n = rt_call(c, G.buf_size, 1, b);
            if (!base || !n || n > 65536) continue;
            csd = realloc(csd, csd_len + n);
            if (!csd) rt_fatal("movie: parameter sets");
            memcpy(csd + csd_len, base, (size_t)n);
            csd_len += n;
        }
        // Country-Box: SDK 4.4.0's extractor (Android 5.1 stagefright, Tropical Freeze) gives no csd-0/csd-1 but the
        // MP4 "avcc" box (AVCDecoderConfigurationRecord, measured): its SPS and PPS become Annex B units here.
        uint64_t av = 0;
        if (!csd_len && (uint8_t)rt_call(c, G.md_find_buffer, 3, md, U("avcc"), U(&av)) && av) {
            const uint8_t *a = (const uint8_t *)(uintptr_t)rt_call(c, G.buf_base, 1, av);
            uint64_t n = rt_call(c, G.buf_size, 1, av), o = 5;
            for (int kind = 0; a && n >= 7 && n <= 65536 && kind < 2 && o < n; kind++) {   // SPS then PPS
                unsigned count = a[o++] & (kind ? 0xff : 0x1f);
                for (unsigned i = 0; i < count && o + 2 <= n; i++) {
                    unsigned len = (unsigned)a[o] << 8 | a[o + 1];
                    o += 2;
                    if (o + len > n) break;
                    csd = realloc(csd, csd_len + 4 + len);
                    if (!csd) rt_fatal("movie: parameter sets");
                    memcpy(csd + csd_len, "\0\0\0\1", 4);
                    memcpy(csd + csd_len + 4, a + o, len);
                    csd_len += 4 + len, o += len;
                }
            }
        }
        rt_log("movie: H.264 %dx%d, %zu bytes of parameter sets%s", w, h, csd_len, av ? " (from avcc)" : "");
    } else if (m->unsupported) {   // the null decoder takes any stream; its output frames are empty, their size nominal
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192) w = 1920, h = 1080;
        w &= ~1, h &= ~1;
        rt_log("movie: null decoder takes %s %dx%d: input is accepted and dropped, the stream ends with its input",
               mime ? mime : "(no mime)", w, h);
    } else if (!mime || strcmp(mime, "video/x-vnd.on2.vp9") || w <= 0 || h <= 0 || w > 8192 || h > 8192 || (w | h) & 1) {
        rt_log("movie: configure: mime %s %dx%d not supported", mime ? mime : "(none)", w, h);
        ret_x(c, ST_UNSUPPORTED);
        return;
    }
    plat_mutex_lock(&m->mu);
    int away = quiesce(m);
    release_codec(m);
    m->w = w;
    m->h = h;
    m->ch = ch, m->rate = rate;
    free(m->csd);
    m->csd = csd;
    m->csd_len = csd_len;
    if (m->h264 && !h264_open(m)) rt_fatal("movie: openh264 decoder initialisation");
    if (!m->unsupported && !m->h264 && !m->silent && !vpx_open(m)) rt_fatal("movie: libvpx VP9 decoder initialisation");
    for (int i = 0; i < IN_BUFS; i++) m->in[i] = mem_kernel_alloc(IN_CAP, 0x1000);
    for (int i = 0; i < OUT_BUFS; i++)
        if (!(m->out[i] = malloc((size_t)frame_bytes(m)))) rt_fatal("movie: output buffers");
    reset_queues(m);
    m->format_sent = 0;
    m->configured = 1;
    m->last_error = 0;
    unlock_back(m, away);
    rt_log("movie: configured %dx%d %s, %d input buffers of %d KiB, %d output frames of %d bytes", w, h,
           m->texture ? "H.264 (RGBA frames for the game's textures)" : m->h264 ? "H.264 (NV12 frames)" : m->unsupported ? "null" : "VP9",
           IN_BUFS, IN_CAP >> 10,
           OUT_BUFS, frame_bytes(m));
    ret_x(c, ST_OK);
}

// Status RegisterClientEvents(DecoderEvents *events)
static void ov_register_events(Cpu *c) {
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_REGISTER_EVENTS); return; }
    note_entry(m, "RegisterClientEvents");
    plat_mutex_lock(&m->mu);
    m->events = arg_x(c, 1);   // an unsupported decoder keeps them too: Start raises the error event through them
    plat_mutex_unlock(&m->mu);
    ret_x(c, ST_OK);
}

static void set_format_audio(Cpu *c, const Movie *m, uint64_t md) {   // Country-Box: the silent AAC decoder's PCM
    rt_call(c, G.md_set_string, 3, md, U("mime"), U("audio/raw"));
    md_set_int32(c, md, "channel-count", m->ch);
    md_set_int32(c, md, "sample-rate", m->rate);
    md_set_int32(c, md, "pcm-encoding", 2);   // 16-bit
}

static void set_format(Cpu *c, int w, int h, uint64_t md) {
    rt_call(c, G.md_set_string, 3, md, U("mime"), U("video/raw"));
    md_set_int32(c, md, "width", w);
    md_set_int32(c, md, "height", h);
    md_set_int32(c, md, "stride", w);
    md_set_int32(c, md, "slice-height", h);
    md_set_int32(c, md, "color-format", 21);   // COLOR_FormatYUV420SemiPlanar
    rt_call(c, G.md_set_rect, 6, md, U("crop"), (uint64_t)0, (uint64_t)0, (uint64_t)(uint32_t)(w - 1),
            (uint64_t)(uint32_t)(h - 1));
    md_set_int32(c, md, "crop-width", w);
    md_set_int32(c, md, "crop-height", h);
    md_set_int32(c, md, "nv12-y-stride", w);
    md_set_int32(c, md, "nv12-uv-stride", w);
    md_set_int32(c, md, "nv12-y-offset", 0);
    md_set_int32(c, md, "nv12-uv-offset", w * h);
}

// Status GetOutputFormat(MediaData *format)
static void ov_get_output_format(Cpu *c) {
    uint64_t md = arg_x(c, 1);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_GET_OUTPUT_FORMAT); return; }
    if (!md) { ret_x(c, ST_NULL); return; }
    plat_mutex_lock(&m->mu);
    int ok = m->configured, w = m->w, h = m->h;
    plat_mutex_unlock(&m->mu);
    if (!ok) { ret_x(c, ST_ERROR); return; }
    if (m->silent) set_format_audio(c, m, md);
    else set_format(c, w, h, md);
    ret_x(c, ST_OK);
}

static void ov_start(Cpu *c) {
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_START); return; }
    note_entry(m, "Start");
    Signals s = {0};
    plat_mutex_lock(&m->mu);
    int st = ST_NOT_CONFIGURED;
    if (m->configured) {
        m->started = 1;
        m->owed |= 1u << MOVIE_EV_INPUT;
        plat_cond_signal(&m->work);
        st = ST_OK;
    }
    collect(m, &s);
    plat_mutex_unlock(&m->mu);
    fire(c, &s);
    ret_x(c, (uint32_t)st);
}

static void ov_stop(Cpu *c) {
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_STOP); return; }
    note_entry(m, "Stop");
    plat_mutex_lock(&m->mu);
    int away = quiesce(m);
    rt_log("movie: stop after %llu frames (%llu dropped to keep time; decode %.2f ms per frame, max %.2f)",
           (unsigned long long)m->st.frames, (unsigned long long)m->st.dropped,
           m->st.frames ? m->st.decode_ns / 1e6 / (double)m->st.frames : 0.0, m->st.max_ns / 1e6);
    release_codec(m);
    unlock_back(m, away);
    ret_x(c, ST_OK);
}

static void ov_reset(Cpu *c) {
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_RESET); return; }
    note_entry(m, "Reset");
    plat_mutex_lock(&m->mu);
    int away = quiesce(m);
    release_codec(m);
    m->last_error = 0;
    unlock_back(m, away);
    ret_x(c, ST_OK);
}

// Drop queued work; the next input starts a new decode (after a seek: a key frame).
static void ov_flush(Cpu *c) {
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_FLUSH); return; }
    note_entry(m, "Flush");
    plat_mutex_lock(&m->mu);
    int away = quiesce(m);
    if (m->configured) {
        if (m->vpx_live) {
            vpx_codec_destroy(&m->vpx);
            m->vpx_live = 0;
            if (!vpx_open(m)) rt_fatal("movie: libvpx VP9 decoder re-initialisation");
        }
        if (m->h264_live) {   // a new openh264 decoder, fed the parameter sets again
            h264_close(m);
            if (!h264_open(m)) rt_fatal("movie: openh264 decoder re-initialisation");
        }
        reset_queues(m);
    }
    unlock_back(m, away);
    ret_x(c, ST_OK);
}

// Status AcquireInputBufferIndex(int *index, int *remaining)
static void ov_acquire_input(Cpu *c) {
    int *index = arg_p(c, 1), *remaining = arg_p(c, 2);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_ACQUIRE_INPUT); return; }
    note_entry(m, "AcquireInputBufferIndex");
    if (!index || !remaining) { ret_x(c, ST_NULL); return; }
    Signals s = {0};
    plat_mutex_lock(&m->mu);
    *index = -1;
    *remaining = 0;
    int st = ST_NO_INPUT;
    if (m->started && m->nin_free > 0) {
        *index = m->in_free[0];
        memmove(m->in_free, m->in_free + 1, (size_t)--m->nin_free * sizeof *m->in_free);
        *remaining = m->nin_free;
        st = ST_OK;
    }
    collect(m, &s);
    plat_mutex_unlock(&m->mu);
    fire(c, &s);
    ret_x(c, (uint32_t)st);
}

// Status GetInputBuffer(int index, Buffer *buffer)
static void ov_get_input(Cpu *c) {
    int index = (int)arg_x(c, 1);
    uint64_t buf = arg_x(c, 2);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_GET_INPUT); return; }
    note_entry(m, "GetInputBuffer");
    if (!buf) { ret_x(c, ST_NULL); return; }
    plat_mutex_lock(&m->mu);
    uint8_t *p = index >= 0 && index < IN_BUFS ? m->in[index] : NULL;
    plat_mutex_unlock(&m->mu);
    buf_set_range(c, buf, 0, p ? IN_CAP : 0);
    buf_set_data(c, buf, U(p), p ? IN_CAP : 0);
    ret_x(c, p ? ST_OK : ST_ERROR);
}

// Status SendInputBufferForDecode(int index, int offset, int size, long time, unsigned flags)
static void ov_send_input(Cpu *c) {
    int index = (int)arg_x(c, 1), offset = (int)arg_x(c, 2), size = (int)arg_x(c, 3);
    int64_t time = (int64_t)arg_x(c, 4);
    unsigned flags = (unsigned)arg_x(c, 5);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_SEND_INPUT); return; }
    note_entry(m, "SendInputBufferForDecode");
    if (flags & MOVIE_FLAG_EOS && cfg_get("MP4_MOVIE_EOS_BT")) {   // who ends a movie: the game's player, by its own count
        rt_log("movie: end of stream sent at %lld us; guest backtrace:", (long long)time);
        kern_backtrace(c);
    }
    if (m->unsupported) {   // the null decoder's intake: every 50th packet and the last, with its time
        static volatile int32_t sent;
        int32_t n = plat_atomic_inc(&sent);
        if (n % 50 == 1 || flags & MOVIE_FLAG_EOS)
            rt_log("movie: null decoder packet %ld: %d bytes at %lld us, flags %#x", (long)n, size, (long long)time, flags);
        if (n <= 3 && index >= 0 && index < IN_BUFS && m->in[index] && size > 0) {   // the framing: Annex B start codes or AVCC lengths
            char hex[3 * 24 + 1] = "";
            for (int i = 0; i < 24 && i < size; i++) snprintf(hex + 3 * i, 4, "%02x ", m->in[index][offset + i]);
            rt_log("movie: null decoder packet %ld bytes: %s", (long)n, hex);
        }
    }
    if (index < 0 || index >= IN_BUFS || offset < 0 || size < 0 || (int64_t)offset + size > IN_CAP) {
        ret_x(c, ST_ERROR);
        return;
    }
    Signals s = {0};
    plat_mutex_lock(&m->mu);
    int st = ST_OK;
    if (!m->started || m->npending == IN_BUFS) st = ST_ERROR;
    else if (m->eos_in) {   // after end of stream: hand the buffer back
        m->in_free[m->nin_free++] = index;
        m->owed |= 1u << MOVIE_EV_INPUT;
    } else {
        m->pending[m->npending++] = (Pending){index, offset, size, time, flags};
        plat_cond_signal(&m->work);
    }
    collect(m, &s);
    plat_mutex_unlock(&m->mu);
    fire(c, &s);
    ret_x(c, (uint32_t)st);
}

// Status AcquireOutputBufferIndex(int *index, long *time, unsigned *flags, int *remaining)
static void ov_acquire_output(Cpu *c) {
    int *index = arg_p(c, 1), *remaining = arg_p(c, 4);
    int64_t *time = arg_p(c, 2);
    unsigned *flags = arg_p(c, 3);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_ACQUIRE_OUTPUT); return; }
    note_entry(m, "AcquireOutputBufferIndex");
    if (!index || !time || !flags || !remaining) { ret_x(c, ST_NULL); return; }
    Signals s = {0};
    plat_mutex_lock(&m->mu);   // paced frames become ready only at presents (movie_tick)
    *index = -1;
    *time = 0;
    *flags = 0;
    *remaining = 0;
    int st = ST_NO_OUTPUT;
    if (m->nready > 0) {
        Ready r = m->ready[0];
        memmove(m->ready, m->ready + 1, (size_t)--m->nready * sizeof *m->ready);
        *index = r.index;
        *time = r.time;
        *flags = r.flags;
        *remaining = m->nready;
        st = ST_OK;
    }
    collect(m, &s);
    plat_mutex_unlock(&m->mu);
    fire(c, &s);
    ret_x(c, (uint32_t)st);
}

// Texture mode: the game's Buffer carries the NVNtexture the frame goes into as its data pointer (capacity 8: the
// game sets the pointer, not a span; it makes the textures with NVN_TEXTURE_FLAGS_VIDEO_DECODE, RGBA8_SRGB, and
// samples them). The frame is written through the back end's texel write, on the game's thread, as
// nvnTextureWriteTexels would be. The end-of-stream output carries no frame.
static int texture_write(Cpu *c, Movie *m, int index, uint8_t *data, uint64_t cap) {
    const tex_t *t = (const tex_t *)data;
    uint32_t magic;   // the Buffer's pointer is the game's: read the magic without faulting on an unmapped page
    _Static_assert(sizeof magic == sizeof t->magic, "tex_t magic is read as 32 bits");
    if (!t || !plat_mem_read(U(&t->magic), &magic, sizeof magic) || magic != NVN_TEX_MAGIC) {
        static volatile int32_t said;
        if (plat_atomic_inc(&said) <= 3)
            rt_log("movie: texture mode: output buffer %d holds no texture (%p, capacity %llu)", index, (const void *)t,
                   (unsigned long long)cap);
        return 0;
    }
    int bytes;
    plat_mutex_lock(&m->mu);
    bytes = index >= 0 && index < OUT_BUFS && m->out_held[index] ? m->out_bytes[index] : -1;
    plat_mutex_unlock(&m->mu);
    if (bytes < 0) return 0;
    if (!bytes) return 1;   // the end-of-stream output
    static volatile int32_t seen;
    if (plat_atomic_inc(&seen) <= 2)
        rt_log("movie: frame %d into texture %p (%dx%d format %#x flags %#x)", index, (const void *)t, t->d.width, t->d.height,
               t->d.format, t->d.flags);
    int w = m->w < t->d.width ? m->w : t->d.width, h = m->h < t->d.height ? m->h : t->d.height;
    NVNcopyRegion r = {0, 0, 0, w, h, 1};
    {
        NVN_BLOCKING();
        gfx_backend()->write_texels(t, NULL, &r, m->out[index], (uint64_t)m->w * 4, 0);
    }
    // In texture mode the frame now lives in the game's texture and the library's buffer is free again: the game
    // never calls ReleaseOutputBufferIndex for it (it did not for the first eight frames, and the decoder stalled with
    // every slot held), so the slot is released here.
    plat_mutex_lock(&m->mu);
    m->out_held[index] = 0;
    plat_cond_signal(&m->work);
    plat_mutex_unlock(&m->mu);
    return 1;
}

static int get_output(Cpu *c, Movie *m, int index, uint64_t buf) {
    buf_set_range(c, buf, 0, 0);
    uint8_t *dst = (uint8_t *)(uintptr_t)rt_call(c, G.buf_base, 1, buf);
    uint64_t cap = rt_call(c, G.buf_capacity, 1, buf);
    if (m->texture && texture_write(c, m, index, dst, cap)) return ST_OK;
    int st = ST_ERROR, bytes = 0;
    plat_mutex_lock(&m->mu);
    if (index >= 0 && index < OUT_BUFS && m->out_held[index]) {
        bytes = m->out_bytes[index];
        if (!bytes) st = ST_OK;
        else if (dst && cap >= (uint64_t)bytes) {
            memcpy(dst, m->out[index], (size_t)bytes);
            st = ST_OK;
        } else {
            rt_log("movie: output buffer of %llu bytes for a %d-byte frame", (unsigned long long)cap, bytes);
            bytes = 0;
        }
    }
    plat_mutex_unlock(&m->mu);
    if (st == ST_OK && bytes) buf_set_range(c, buf, 0, bytes);
    return st;
}

// Status GetOutputBuffer(int index, Buffer *buffer)
static void ov_get_output(Cpu *c) {
    int index = (int)arg_x(c, 1);
    uint64_t buf = arg_x(c, 2);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_GET_OUTPUT); return; }
    note_entry(m, "GetOutputBuffer");
    ret_x(c, buf ? (uint32_t)get_output(c, m, index, buf) : ST_NULL);
}

// Status GetOutputBufferSize(int *size, int *count)
static void ov_get_output_size(Cpu *c) {
    int *size = arg_p(c, 1), *count = arg_p(c, 2);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_GET_OUTPUT_SIZE); return; }
    note_entry(m, "GetOutputBufferSize");
    if (!size || !count) { ret_x(c, ST_NULL); return; }
    plat_mutex_lock(&m->mu);
    *count = 1;
    *size = m->configured ? frame_bytes(m) : 0;
    plat_mutex_unlock(&m->mu);
    ret_x(c, ST_OK);
}

// Status ReleaseOutputBufferIndex(int index)
static void ov_release_output(Cpu *c) {
    int index = (int)arg_x(c, 1);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_RELEASE_OUTPUT); return; }
    note_entry(m, "ReleaseOutputBufferIndex");
    Signals s = {0};
    plat_mutex_lock(&m->mu);
    int st = ST_ERROR;
    if (index >= 0 && index < OUT_BUFS && m->out_held[index]) {
        int queued = 0;
        for (int i = 0; i < m->nready; i++) queued |= m->ready[i].index == index;
        for (int i = 0; i < m->ndecoded; i++) queued |= m->decoded[i].index == index;
        if (!queued) {
            m->out_held[index] = 0;
            plat_cond_signal(&m->work);
            st = ST_OK;
        }
    }
    collect(m, &s);
    plat_mutex_unlock(&m->mu);
    fire(c, &s);
    ret_x(c, (uint32_t)st);
}

static void ov_get_last_error(Cpu *c) {
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_GET_LAST_ERROR); return; }
    note_entry(m, "GetLastError");
    plat_mutex_lock(&m->mu);
    int e = m->last_error;   // an unsupported decoder's: ST_UNSUPPORTED once Start was asked
    plat_mutex_unlock(&m->mu);
    ret_x(c, (uint32_t)e);
}

static int output_format(Cpu *c, Movie *m, int index, uint64_t md) {
    plat_mutex_lock(&m->mu);
    int ok = index >= 0 && index < OUT_BUFS && m->out_held[index], w = m->w, h = m->h;
    plat_mutex_unlock(&m->mu);
    if (!ok) return ST_ERROR;
    if (m->silent) set_format_audio(c, m, md);
    else set_format(c, w, h, md);
    return ST_OK;
}

// Status GetOutputBufferFormat(int index, MediaData *format)
static void ov_get_output_buffer_format(Cpu *c) {
    int index = (int)arg_x(c, 1);
    uint64_t md = arg_x(c, 2);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_GET_OUTPUT_BUFFER_FORMAT); return; }
    note_entry(m, "GetOutputBufferFormat");
    ret_x(c, md ? (uint32_t)output_format(c, m, index, md) : ST_NULL);
}

// Status GetOutputBuffer(int index, Buffer *buffer, MediaData *format)
static void ov_get_output_md(Cpu *c) {
    int index = (int)arg_x(c, 1);
    uint64_t buf = arg_x(c, 2), md = arg_x(c, 3);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_GET_OUTPUT_MD); return; }
    note_entry(m, "GetOutputBuffer(md)");
    if (!buf && !md) { ret_x(c, ST_NULL); return; }
    int st = md ? output_format(c, m, index, md) : ST_OK;
    if (st == ST_OK && buf) st = get_output(c, m, index, buf);
    ret_x(c, (uint32_t)st);
}

HOST_OVERRIDE("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", ov_ctor);
static void ov_ctor3(Cpu *c) {
    t_ctor3 = 1;
    ov_ctor(c);
    t_ctor3 = 0;
}
HOST_OVERRIDE(CTOR3, ov_ctor3);
HOST_OVERRIDE("_ZN5movie7DecoderD2Ev", ov_d2);
// 1.4.3's vtable and a PLT call name the complete-object destructor; without virtual bases it is D2.
HOST_OVERRIDE("_ZN5movie7DecoderD1Ev", ov_d2);
HOST_OVERRIDE("_ZN5movie7DecoderD0Ev", ov_d0);
HOST_OVERRIDE("_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE", ov_configure);
HOST_OVERRIDE("_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", ov_register_events);
HOST_OVERRIDE("_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE", ov_get_output_format);
HOST_OVERRIDE("_ZN5movie7Decoder5StartEv", ov_start);
HOST_OVERRIDE("_ZN5movie7Decoder4StopEv", ov_stop);
HOST_OVERRIDE("_ZN5movie7Decoder5ResetEv", ov_reset);
HOST_OVERRIDE("_ZN5movie7Decoder5FlushEv", ov_flush);
HOST_OVERRIDE("_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", ov_acquire_input);
HOST_OVERRIDE("_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE", ov_get_input);
HOST_OVERRIDE("_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj", ov_send_input);
HOST_OVERRIDE("_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_", ov_acquire_output);
HOST_OVERRIDE("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE", ov_get_output);
HOST_OVERRIDE("_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_", ov_get_output_size);
HOST_OVERRIDE("_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", ov_release_output);
HOST_OVERRIDE("_ZN5movie7Decoder12GetLastErrorEv", ov_get_last_error);
HOST_OVERRIDE("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", ov_get_output_md);
HOST_OVERRIDE("_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE", ov_get_output_buffer_format);
