// Country-Box: small Horizon services the SDK of Donkey Kong Country Returns HD (18.3.2) opens through sm: and the
// MP4 host never needed (its SDK and overrides kept them closed). Each is served at the IPC level, so the lifted SDK
// client code runs unchanged; commands not listed stop with a named fatal (ipc_unimplemented), as in ipc.c.
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "ipc.h"
#include "ipc_services.h"
#include "ov.h"

// ---- lm: the log manager. nn::lm::Initialize (from nninitInitializeSdkModule, before nnMain) opens it; the SDK's
// log macros write packets through ILogger. Logs have no outputs the game reads, so every ILogger command succeeds.
// MP4_TRACE=ipc shows each packet's size (ipc.c).
static void lm_logger(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s, (void)rs;
    switch (rq->cmd) {
    case 0:   // Log(buffer): a binary log packet
    case 1:   // SetDestination(u32)
    case 2:   // TransmitHashedLog (17.0.0+)
        return;
    default: ipc_unimplemented(s, rq);
    }
}

void ipc_service_lm(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0:   // OpenLogger(pid) -> ILogger
        rs->move[rs->nmove++] = ipc_session_new("lm:ILogger", lm_logger, NULL, NULL);
        return;
    default: ipc_unimplemented(s, rq);
    }
}

// ---- set: ISettingsServer, the public settings service. Country-Box: Tropical Freeze's SDK (4.4.0) opens it from
// COsContext's constructor (nnMain): the game takes nn::oe::GetDesiredLanguage (ov_system.c ov_language, MP4_LANG) and
// looks for it among LanguageCode::Make(i) for i below GetAvailableLanguageCodeCount (main+0x10bc, measured). So the
// console answers with every language of the Language enum, in its order: 15 for the first-generation commands (1, 3),
// 18 for the second (5, 6, as current firmware). Region 1 (Americas; supuesto: the games read only the language).
static const char *const set_languages[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl",
                                            "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};

static uint64_t set_code(const char *l) {
    uint64_t v = 0;
    memcpy(&v, l, strnlen(l, 8));
    return v;
}

// Writes n bytes to the first B buffer (type 6) or, failing that, the first C buffer (type 0xA, pointer data).
static uint32_t set_out_buffer(const IpcRequest *rq, IpcResponse *rs, const void *p, uint32_t n) {
    IpcBuffer b = rq->nb && rq->b[0].size ? rq->b[0] : rq->nc ? rq->c[0] : (IpcBuffer){0};
    if (n > b.size) n = (uint32_t)b.size;
    if (n) memcpy(ipc_guest(b.addr, n), p, n);
    if (!(rq->nb && rq->b[0].size) && rq->nc) rs->xout[rs->nxout++] = (IpcBuffer){b.addr, n, 0};
    return n;
}

void ipc_service_set(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    uint64_t codes[18];
    for (int i = 0; i < 18; i++) codes[i] = set_code(set_languages[i]);
    int32_t n = rq->cmd == 1 || rq->cmd == 3 ? 15 : 18, one = 1;
    switch (rq->cmd) {
    case 0: {   // GetLanguageCode() -> LanguageCode
        uint64_t code = set_code(ov_language());
        IPC_OUT(rs, code);
        return;
    }
    case 1:     // GetAvailableLanguageCodes() -> s32 count, C buffer of LanguageCode
    case 5: {   // GetAvailableLanguageCodes2() -> s32 count, B buffer of LanguageCode
        int32_t got = (int32_t)(set_out_buffer(rq, rs, codes, (uint32_t)n * 8) / 8);
        IPC_OUT(rs, got);
        return;
    }
    case 3:   // GetAvailableLanguageCodeCount() -> s32
    case 6:   // GetAvailableLanguageCodeCount2() -> s32
        IPC_OUT(rs, n);
        return;
    case 4:   // GetRegionCode() -> s32
        IPC_OUT(rs, one);
        return;
    default: ipc_unimplemented(s, rq);
    }
}
