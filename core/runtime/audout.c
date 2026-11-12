// Country-Box: the audout:u service (IAudioOutManager, IAudioOut), which Returns HD's Unity player opens next to the
// audio renderer (nn::audio::ListAudioOuts / OpenDefaultAudioOut from its sound start-up) and the MP4 host never
// served. Commands per the IPC interface as libnx documents it (audout.c, ISC), read for the layout only.
//
// An audio out is a queue of client buffers (AudioOutBuffer: next, sample pointer, capacity, data size, data offset;
// the client's pointer to it is the tag) played in order as 16-bit PCM. Each open audio out has an SDL audio stream on
// the default device (SDL mixes it with the renderer's) and a thread that keeps about AHEAD_MS of samples queued in
// the stream and releases a buffer once SDL has consumed all of it, signalling the buffer event as the console does.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "ipc.h"
#include "ipc_services.h"
#include "platform/platform.h"

#define NBUF 64                // queued plus released buffers an audio out holds (the SDK keeps at most 32 queued)
#define AHEAD_MS 40            // samples kept queued in SDL ahead of the device
#define TICK_NS 2000000ull     // the feeding thread's period
#define FMT_PCM16 2

typedef struct { uint64_t next, data, capacity, size, offset; } AudioOutBuffer;   // guest layout

typedef struct {
    uint64_t tag;              // client's AudioOutBuffer address
    uint64_t data, size;       // samples to play
    uint64_t end;              // stream byte position after its samples (pushed: end != 0)
} Queued;

typedef struct {
    PlatMutex lock;
    uint32_t rate, channels;
    int started;
    float volume;
    Queued q[NBUF];            // played in order
    int nq;
    uint64_t released[NBUF];
    int nrel;
    uint64_t pushed, played_frames;
    // Country-Box: underrun accounting, logged every 10 s by out_thread: the stream ran dry (SDL had nothing queued
    // after samples were pushed), with the client having no buffer ready (the game fell behind) or not.
    int dry;
    uint64_t dry_n, dry_starved_n, stat_t0, stat_frames0;
    uint32_t event;            // writable buffer event
    SDL_AudioStream *stream;
    PlatThread *thread;
    volatile int run;
} AudioOut;

static void need(int c, const char *m) {
    if (!c) rt_fatal("audout: %s", m);
}

// The first buffer the request names: the A/B list or the pointer (X/C) list, whichever the client filled ("Auto").
static IpcBuffer pick(const IpcBuffer *ab, unsigned nab, const IpcBuffer *xc, unsigned nxc, unsigned i) {
    if (i < nab && ab[i].size) return ab[i];
    if (i < nxc && xc[i].size) return xc[i];
    return (IpcBuffer){0, 0, i};
}

// A C (receive) buffer the service wrote: reported back as the X descriptor libnx expects for it.
static void note_c(const IpcRequest *rq, IpcResponse *rs, unsigned i) {
    if (i < rq->nc && rq->c[i].size && (i >= rq->nb || !rq->b[i].size))
        rs->xout[rs->nxout++] = (IpcBuffer){rq->c[i].addr, rq->c[i].size, i};
}

static void feed(AudioOut *o) {   // under o->lock
    if (!o->started) return;
    uint64_t frame = 2ull * o->channels;
    uint64_t ahead = (uint64_t)o->rate * AHEAD_MS / 1000 * frame;
    int queued = SDL_GetAudioStreamQueued(o->stream);
    uint64_t in_sdl = queued > 0 ? (uint64_t)queued : 0;
    for (int i = 0; i < o->nq && in_sdl < ahead; i++) {
        Queued *b = &o->q[i];
        if (b->end) continue;
        if (b->size) {
            if (o->volume == 1.0f) SDL_PutAudioStreamData(o->stream, (const void *)(uintptr_t)b->data, (int)b->size);
            else {
                int16_t tmp[4096];
                const int16_t *src = (const int16_t *)(uintptr_t)b->data;
                for (uint64_t k = 0; k < b->size / 2;) {
                    uint64_t n = b->size / 2 - k < 4096 ? b->size / 2 - k : 4096;
                    for (uint64_t j = 0; j < n; j++) tmp[j] = (int16_t)((float)src[k + j] * o->volume);
                    SDL_PutAudioStreamData(o->stream, tmp, (int)(n * 2));
                    k += n;
                }
            }
        }
        o->pushed += b->size;
        b->end = o->pushed ? o->pushed : 1;
        in_sdl += b->size;
    }
    queued = SDL_GetAudioStreamQueued(o->stream);
    if (o->pushed && queued <= 0 && !o->dry) {
        o->dry = 1, o->dry_n++;
        int ready = 0;
        for (int i = 0; i < o->nq; i++) ready |= !o->q[i].end;
        if (!ready) o->dry_starved_n++;
    } else if (queued > 0) {
        o->dry = 0;
    }
    uint64_t consumed = o->pushed - (queued > 0 ? (uint64_t)queued : 0);
    int freed = 0;
    while (o->nq && o->q[0].end && o->q[0].end <= (consumed ? consumed : 1) && o->nrel < NBUF) {
        o->released[o->nrel++] = o->q[0].tag;
        o->played_frames += o->q[0].size / frame;
        memmove(&o->q[0], &o->q[1], (size_t)(o->nq - 1) * sizeof o->q[0]);
        o->nq--;
        freed = 1;
    }
    if (freed) kern_event_signal(o->event);
}

static void out_thread(void *arg) {
    AudioOut *o = arg;
    while (__atomic_load_n(&o->run, __ATOMIC_ACQUIRE)) {
        plat_mutex_lock(&o->lock);
        feed(o);
        uint64_t now = plat_now_ns();
        if (!o->stat_t0) o->stat_t0 = now, o->stat_frames0 = o->played_frames;
        if (now - o->stat_t0 >= 10000000000ull && o->started) {
            int q = SDL_GetAudioStreamQueued(o->stream);
            double secs = (now - o->stat_t0) / 1e9;
            rt_log("audout: %.0f s: %llu frames played (%.0f expected at %u Hz), stream ran dry %llu times (%llu with no "
                   "buffer from the game), %.1f ms queued now", secs,
                   (unsigned long long)(o->played_frames - o->stat_frames0), secs * o->rate, o->rate,
                   (unsigned long long)o->dry_n, (unsigned long long)o->dry_starved_n,
                   q > 0 ? q * 1000.0 / (2.0 * o->channels * o->rate) : 0.0);
            o->stat_t0 = now, o->stat_frames0 = o->played_frames, o->dry_n = o->dry_starved_n = 0;
        }
        plat_mutex_unlock(&o->lock);
        plat_sleep_ns(TICK_NS);
    }
}

static void out_close(void *p) {
    AudioOut *o = p;
    __atomic_store_n(&o->run, 0, __ATOMIC_RELEASE);
    int away = kern_core_away();
    plat_thread_join(o->thread);
    kern_core_back(&away);
    SDL_DestroyAudioStream(o->stream);
    kern_handle_close(o->event);
    rt_log("audout: closed after %llu frames", (unsigned long long)o->played_frames);
    free(o);
}

static void out_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    AudioOut *o = s->state;
    switch (rq->cmd) {
    case 0: { uint32_t v = o->started ? 0 : 1; IPC_OUT(rs, v); return; }   // GetAudioOutState: 0 started, 1 stopped
    case 1:   // Start
        plat_mutex_lock(&o->lock);
        o->started = 1;
        SDL_ResumeAudioStreamDevice(o->stream);
        plat_mutex_unlock(&o->lock);
        return;
    case 2:   // Stop
        plat_mutex_lock(&o->lock);
        o->started = 0;
        SDL_PauseAudioStreamDevice(o->stream);
        plat_mutex_unlock(&o->lock);
        return;
    case 3: case 7: {   // AppendAudioOutBuffer(Auto)(u64 tag, in AudioOutBuffer)
        uint64_t tag = IPC_ARG(rq, uint64_t, 0);
        IpcBuffer in = pick(rq->a, rq->na, rq->x, rq->nx, 0);
        need(in.size >= sizeof(AudioOutBuffer), "AppendAudioOutBuffer without its buffer descriptor");
        AudioOutBuffer b;
        memcpy(&b, ipc_guest(in.addr, sizeof b), sizeof b);
        need(b.offset <= b.capacity && b.size <= b.capacity - b.offset, "AppendAudioOutBuffer data outside its buffer");
        plat_mutex_lock(&o->lock);
        need(o->nq + o->nrel < NBUF, "too many buffers queued");
        o->q[o->nq++] = (Queued){tag, b.data + b.offset, b.size & ~1ull, 0};
        ipc_guest(b.data + b.offset, b.size);   // range check
        feed(o);
        plat_mutex_unlock(&o->lock);
        return;
    }
    case 4: rs->copy[rs->ncopy++] = kern_event_readable(o->event); return;   // RegisterBufferEvent
    case 5: case 8: {   // GetReleasedAudioOutBuffers(Auto)(out u64 tags) -> u32 count
        IpcBuffer out = pick(rq->b, rq->nb, rq->c, rq->nc, 0);
        uint64_t *tags = out.size ? ipc_guest(out.addr, out.size) : NULL;
        uint32_t max = (uint32_t)(out.size / 8), n = 0;
        plat_mutex_lock(&o->lock);
        feed(o);
        while (n < max && n < (uint32_t)o->nrel) tags[n] = o->released[n], n++;
        memmove(o->released, o->released + n, (size_t)(o->nrel - (int)n) * 8);
        o->nrel -= (int)n;
        plat_mutex_unlock(&o->lock);
        if (rq->cmd == 8) note_c(rq, rs, 0);
        IPC_OUT(rs, n);
        return;
    }
    case 6: {   // ContainsAudioOutBuffer(u64 tag) -> bool
        uint64_t tag = IPC_ARG(rq, uint64_t, 0);
        uint8_t v = 0;
        plat_mutex_lock(&o->lock);
        for (int i = 0; i < o->nq && !v; i++) v = o->q[i].tag == tag;
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, v);
        return;
    }
    case 9: { plat_mutex_lock(&o->lock); uint32_t v = (uint32_t)o->nq; plat_mutex_unlock(&o->lock); IPC_OUT(rs, v); return; }
    case 10: {   // GetAudioOutPlayedSampleCount
        plat_mutex_lock(&o->lock);
        uint64_t v = o->played_frames;
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, v);
        return;
    }
    case 11: {   // FlushAudioOutBuffers: queued buffers become released unplayed
        plat_mutex_lock(&o->lock);
        uint8_t any = o->nq > 0;
        SDL_ClearAudioStream(o->stream);
        for (int i = 0; i < o->nq && o->nrel < NBUF; i++) o->released[o->nrel++] = o->q[i].tag;
        o->nq = 0;
        o->pushed = 0;
        if (any) kern_event_signal(o->event);
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, any);
        return;
    }
    case 12: { float v = IPC_ARG(rq, float, 0); plat_mutex_lock(&o->lock); o->volume = v; plat_mutex_unlock(&o->lock); return; }
    case 13: { float v = o->volume; IPC_OUT(rs, v); return; }
    default: ipc_unimplemented(s, rq);
    }
}

static const char k_device[] = "DeviceOut";

void ipc_service_audout(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: case 2: {   // ListAudioOuts(Auto)(out names of 0x100 bytes) -> u32 count
        IpcBuffer out = pick(rq->b, rq->nb, rq->c, rq->nc, 0);
        uint32_t n = 0;
        if (out.size >= 0x100) {
            char *names = ipc_guest(out.addr, 0x100);
            memset(names, 0, 0x100);
            memcpy(names, k_device, sizeof k_device);
            n = 1;
        }
        if (rq->cmd == 2) note_c(rq, rs, 0);
        IPC_OUT(rs, n);
        return;
    }
    case 1: case 3: {   // OpenAudioOut(Auto)(u32 rate, u16 channels, u16, u64 aruid; pid; copy process; in name) -> out
                        // name, {u32 rate, u32 channels, u32 format, u32 state}, IAudioOut
        uint32_t rate = IPC_ARG(rq, uint32_t, 0);
        uint16_t ch = IPC_ARG(rq, uint16_t, 4);
        if (!rate) rate = 48000;
        if (!ch) ch = 2;
        need(ch <= 6, "OpenAudioOut with more than 6 channels");
        IpcBuffer name = pick(rq->b, rq->nb, rq->c, rq->nc, 0);
        if (name.size >= sizeof k_device) {
            memcpy(ipc_guest(name.addr, sizeof k_device), k_device, sizeof k_device);
            if (rq->cmd == 3) note_c(rq, rs, 0);
        }
        AudioOut *o = calloc(1, sizeof *o);
        need(o != NULL, "out of memory");
        o->rate = rate, o->channels = ch, o->volume = 1.0f, o->event = kern_event_new(), o->run = 1;
        plat_mutex_init(&o->lock);
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) rt_fatal("audout: SDL audio: %s", SDL_GetError());
        SDL_AudioSpec spec = {SDL_AUDIO_S16LE, ch, (int)rate};
        o->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        need(o->stream != NULL, SDL_GetError());
        o->thread = plat_thread_new(out_thread, o, "audout");
        need(o->thread != NULL, "cannot start the feeding thread");
        uint32_t outp[4] = {rate, ch, FMT_PCM16, 1};   // stopped until Start
        ipc_out(rs, outp, sizeof outp);
        rs->move[rs->nmove++] = ipc_session_new("IAudioOut", out_handler, o, out_close);
        rt_log("audout: opened %u Hz x%u (PCM16) on the default SDL device", rate, ch);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}
