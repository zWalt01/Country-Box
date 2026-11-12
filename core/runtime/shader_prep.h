// Country-Box: shader preparation before play (shader_prep.c), game-agnostic. A game ships a variant list
// (shader-list.bin: code hashes and translator answers, no game code) and a pipeline list (pipeline-list.bin); the
// preparation finds each listed program in the user's own game files through the game's program source, translates
// every variant on every core, builds every listed pipeline and every variant's stage library into the GPU driver's disk
// cache, and writes a marker (shader-prepared.id) keyed on the translator and the Vulkan driver. Until the marker
// matches, the game shows a blocking "Preparing shaders" screen at boot (ui/prep_screen.cpp) instead of starting.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- program sources: where a game's NVN programs (control section + code) are found in its files
// sink gets every program found: stage GFX_STAGE_*, the 0x880-byte control section and the code (length bytes, the
// control section's +0x704 value). It may be called from several threads at once.
typedef void (*ShaderProgSink)(void *ctx, int stage, const uint8_t *control, const uint8_t *code, uint32_t length);
typedef struct {
    const char *name;
    int (*detect)(const char *romfs);                      // 1 when the RomFS tree is this source's layout
    // Calls sink for every program; *done and *total (bytes) follow the scan for the progress screen. Returns the
    // programs found, -1 on failure.
    long (*scan)(const char *romfs, int threads, ShaderProgSink sink, void *ctx, volatile int64_t *done,
                 volatile int64_t *total);
} ShaderProgSource;

extern const ShaderProgSource shader_src_unity;            // shader_src_unity.c: Unity NVN player data (Returns HD)
extern const ShaderProgSource shader_src_dktf;             // shader_src_dktf.c: MaterialArchive.arc (Tropical Freeze)

// ---- progress
enum {
    PREP_PHASE_START,      // reading the lists
    PREP_PHASE_SCAN,       // finding the programs in the game files (bytes)
    PREP_PHASE_XLATE,      // translating shader variants
    PREP_PHASE_PIPES,      // building the listed pipelines
    PREP_PHASE_LIBS,       // building every variant's stage library into the driver cache
    PREP_PHASE_DONE,
    PREP_PHASES
};
// The running preparation's counter: the phase that runs and its work done / to do. gfx_shader.c (translation) and
// vk_pipeline.c (pipelines, libraries) advance it; shader_prep.c reads it for the progress callback.
typedef struct { volatile int32_t phase; volatile int64_t done, total; } ShaderPrepCount;
extern ShaderPrepCount shader_prep_count;
static inline void shader_prep_phase(int phase, int64_t total) {
    __atomic_store_n(&shader_prep_count.done, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&shader_prep_count.total, total, __ATOMIC_SEQ_CST);
    __atomic_store_n(&shader_prep_count.phase, phase, __ATOMIC_SEQ_CST);
}
static inline void shader_prep_tick(void) { __atomic_add_fetch(&shader_prep_count.done, 1, __ATOMIC_SEQ_CST); }

// vk_pipeline.c: the GPU half of a preparation whose seed files (seed-answers.bin, seed-programs.bin) are in the
// pipeline cache's directory: translates them (gfx_shader_prepare_seed), builds the pipelines of pipeline-keys.bin and
// every prepared variant's stage library into the driver cache, and writes pipeline-cache.bin. keep_keys 1 (in-game):
// the key records stay loaded so the boot prewarm that follows still owns them. 0 on success.
int vk_pipeline_prepare(int keep_keys);
// The directory of pipeline-cache.bin (MP4_PIPELINE_CACHE's, else the executable's): where the caches live.
int vk_pipeline_cache_dir(char *out, size_t cap);

typedef struct {
    int phase;
    int64_t done, total;                                   // of the phase
    double elapsed_ms;                                     // since the preparation started
    double eta_ms;                                         // estimated time left for the whole preparation; < 0 unknown
    int64_t variants, pipelines;                           // the work: listed variants with a program, listed pipelines
    int threads;
    const char *why;                                       // why it runs (first run, translator or driver changed)
    int failed;                                            // with PREP_PHASE_DONE: 1 when it did not complete
} ShaderPrepProgress;
typedef void (*ShaderPrepProgressFn)(const ShaderPrepProgress *p, void *ctx);   // from the preparing thread

// ---- the preparation
typedef struct {
    const char *name;                                      // for the log and the screen ("Donkey Kong Country Returns HD")
    const char *romfs;                                     // the user's extracted RomFS
    const ShaderProgSource *source;                        // NULL: none (only recorded programs are used)
} ShaderPrepGame;

// The game this binary runs, from rt_config()'s RomFS (the source picked by detect).
const ShaderPrepGame *shader_prep_game(void);
// 1 when the marker does not match this translator and driver (or a list changed); why gets a short reason. Needs the
// Vulkan device (vk_init). 0 also when there is nothing to prepare (no list and nothing recorded).
int shader_prep_needed(char *why, size_t cap);
// Runs the whole preparation; progress (may be NULL) is called at every phase change and about 10 times a second.
// Writes the marker on success. 0 on success.
int shader_prep_run(const ShaderPrepGame *g, ShaderPrepProgressFn progress, void *ctx);
// rt_boot: when shader_prep_needed, shows the blocking screen until shader_prep_run finishes (CB_SHADER_PREP=0 skips,
// =force runs it anyway).
void shader_prep_boot(void);
// --prepare-shaders: shader_prep_run with progress on stdout, no window. 0 on success.
int shader_prep_cli(void);
// --shader-status (the launcher): one JSON line on stdout, {"status": "prepared" | "needs-prep" | "driver-changed",
// "reason": "..."}; 0 when it could be checked (Vulkan up), 1 when not.
int shader_prep_status_cli(void);

#ifdef __cplusplus
}
#endif
