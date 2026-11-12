// Country-Box: SDK overrides Donkey Kong Country Returns HD (1.0.0, SDK 18.3.2, Unity 2022.3) needs and the MP4 host
// does not have, in the role ov_mp4.c has for Metroid Prime 4: the SDK function is replaced by symbol (host.h
// HOST_OVERRIDE), so the service behind it is never opened. Each entry names the call that reached it on the --boot
// path and what the game does with the answer.
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "ov.h"

// ==== nn::fs
// void EnableGlobalFileDataCache(void *buffer, size_t size): nnMain (main+0xe1e5e4) hands the SDK a buffer for its
// global file data cache. A read cache in front of the game card; reads here come from the host's page cache, so the
// buffer is left unused.
static void ov_fs_enable_global_cache(Cpu *c) { (void)c; }
HOST_OVERRIDE("_ZN2nn2fs25EnableGlobalFileDataCacheEPvm", ov_fs_enable_global_cache);

// ==== nn::ro
// Result Initialize(): nnMain (main+0xf3e838, Unity's start-up) prepares dynamic module loading. Returns HD ships no
// NRO/NRR (its RomFS holds none, measured), so nothing is ever loaded; LoadModule and the other ro calls stay lifted
// and would stop on the ldr:ro service if the game ever made one.
static void ov_ro_initialize(Cpu *c) { ret_x(c, 0); }
HOST_OVERRIDE("_ZN2nn2ro10InitializeEv", ov_ro_initialize);

// ==== network: offline. Returns HD plays without the network; its Unity player still brings up libcurl (nn::ssl),
// sockets and nifm at start. The console answers as one without a connection: the calls that only initialise succeed,
// every request for a connection is refused, and no service (ssl, bsd, nifm) is opened.
#define NIFM_R_UNAVAILABLE (110u | (300u << 9))   // supuesto: an nifm module result for "no connection" (module 110)

// nnsslInitialize() / nn::ssl::Initialize(): libcurl's start-up (main+0x45cf38). Contexts are never created offline.
static void ov_ssl_initialize(Cpu *c) { ret_x(c, 0); }
HOST_OVERRIDE("nnsslInitialize", ov_ssl_initialize);
HOST_OVERRIDE("_ZN2nn3ssl10InitializeEv", ov_ssl_initialize);

// nn::socket::Initialize(Config const&) and (void *pool, size_t, size_t, int): the socket library's set-up.
static void ov_socket_initialize(Cpu *c) { ret_x(c, 0); }
HOST_OVERRIDE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", ov_socket_initialize);
HOST_OVERRIDE("_ZN2nn6socket10InitializeEPvmmi", ov_socket_initialize);

// nn::nifm: Initialize succeeds; no network is ever available, a request is never on hold and ends refused.
static void ov_nifm_initialize(Cpu *c) { ret_x(c, 0); }
static void ov_nifm_false(Cpu *c) { ret_x(c, 0); }
static void ov_nifm_void(Cpu *c) { (void)c; }
static void ov_nifm_refused(Cpu *c) { ret_x(c, NIFM_R_UNAVAILABLE); }
HOST_OVERRIDE("_ZN2nn4nifm10InitializeEv", ov_nifm_initialize);
HOST_OVERRIDE("_ZN2nn4nifm18IsNetworkAvailableEv", ov_nifm_false);
HOST_OVERRIDE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", ov_nifm_false);
HOST_OVERRIDE("_ZN2nn4nifm20SubmitNetworkRequestEv", ov_nifm_void);
HOST_OVERRIDE("_ZN2nn4nifm20CancelNetworkRequestEv", ov_nifm_void);
HOST_OVERRIDE("_ZN2nn4nifm19SetLocalNetworkModeEb", ov_nifm_void);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", ov_nifm_refused);
HOST_OVERRIDE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", ov_nifm_refused);

// ==== nn::oe: calls ov_system.c lacks. Its Initialize keeps the SDK's own state unset, which these would read.
// void EnterExitRequestHandlingSection() / LeaveExitRequestHandlingSection(): the game brackets its save writes so a
// home-menu close waits for them. A host exit flushes the save itself (fs_save_flush on every exit path).
static void ov_oe_void(Cpu *c) { (void)c; }
HOST_OVERRIDE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", ov_oe_void);
HOST_OVERRIDE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", ov_oe_void);
// void SetPerformanceConfiguration(int configuration): the single-mode form; the host schedules the CPU itself.
HOST_OVERRIDE("_ZN2nn2oe27SetPerformanceConfigurationEi", ov_oe_void);

// void GetPseudoDeviceId(nn::util::Uuid *out): a per-console id the game may key data on. A fixed value derived from
// the program id, the same on every start (supuesto: the game only compares it with itself).
static void ov_pseudo_device_id(Cpu *c) {
    uint8_t *o = arg_p(c, 0);
    if (!o) return;
    uint64_t id = rt_program_id(), a = id * 0x9E3779B97F4A7C15ull, b = (id ^ 0x436f756e7472792dull) * 0xC2B2AE3D27D4EB4Full;
    memcpy(o, &a, 8);
    memcpy(o + 8, &b, 8);
}
HOST_OVERRIDE("_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", ov_pseudo_device_id);
