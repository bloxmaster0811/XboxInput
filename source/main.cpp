#include <xtl.h>
#include <xkelib.h>
#include <string>
#include <fstream>
#include <time.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <fstream>
#include <sstream>
#include <vector>
#include "Detours.h"
#include "hid_parser.h"
#include "usb.h"
#include "mapping.h"
#include "gip_protocol.h"
#include "xboxinput_config.h"

extern "C" void* _ReturnAddress(void);
#pragma intrinsic(_ReturnAddress)

// ---------------------------------------------------------------------------
// Additive diagnostic build ladder retained for controlled fault isolation.
//
// Why this exists: the disconnect freeze was chased by SUBTRACTIVE bisection —
// take the full driver, remove one subsystem, observe. Every such build still
// froze, and each result was recorded as "that subsystem is not the cause".
//
// That inference is only valid if there is exactly ONE cause. It is not sound
// otherwise, and we have positive reason to think there are two: the `killtest`
// build froze while containing NONE of our code (it never claims the dongle),
// which means upstream hiddriver360 alone is sufficient to freeze. Every
// "remove X" build kept the claim path, so a second, independent cause would
// have masked every single one of those results.
//
// So: build UP from nothing instead of DOWN from everything. Each level adds
// exactly one subsystem. The lowest level that freezes contains the cause.
//
//   0 NULL      load, print the banner, return. No patches, detours or threads.
//   1 RESOLVE   + resolve kernel/XAM pointers. Reads only, no writes anywhere.
//   2 NOPS      + the two "double registration" NOPs at 0x800D8F00/0x800D8EF0.
//               *** NEVER TESTED IN ANY PREVIOUS BUILD *** — they sit outside
//               every #ifdef, so killtest, noreset, giponly, nonotify,
//               fixremove, silentremove, noreclaim and noclose ALL applied
//               them, and a no-plugin boot does not. That makes them the only
//               kernel write common to every freeze and absent from every
//               survival.
//   3 XAMHOOKS  + XamInput 400/401/402/685 and XInputd 486 detours.
//   4 NOTIFY    + XAM notification patches (custom type 80, timer, JRPC2 branch).
//   5 HID       + HID add/remove detours, JSON mappings, mapping thread.
//   6 USBRESET  + USB bugcheck patches and the "dirty" USB driver reset.
//   7 FULL      + Usbd add/remove detours, the GIP claim, auth, XAM gamepad.
//
// Level 7 is byte-for-byte the shipping driver; the ladder adds no behaviour.
// ---------------------------------------------------------------------------
#define XBOXINPUT_LEVEL_NULL      0
#define XBOXINPUT_LEVEL_RESOLVE   1
#define XBOXINPUT_LEVEL_NOPS      2
#define XBOXINPUT_LEVEL_XAMHOOKS  3
#define XBOXINPUT_LEVEL_NOTIFY    4
#define XBOXINPUT_LEVEL_HID       5
#define XBOXINPUT_LEVEL_USBRESET  6
#define XBOXINPUT_LEVEL_FULL      7

// Map the ladder onto the flags the existing code already tests, so the levels
// are a re-expression of known-good conditionals rather than a new code path.
#define XBOXINPUT_BUILD_LEVEL XBOXINPUT_LEVEL_FULL

#if XBOXINPUT_BUILD_LEVEL < XBOXINPUT_LEVEL_HID
#define XBOXINPUT_GIP_ONLY 1
#endif
#if XBOXINPUT_BUILD_LEVEL < XBOXINPUT_LEVEL_NOTIFY
#define XBOXINPUT_NO_NOTIFY_PATCH 1
#endif
#if XBOXINPUT_BUILD_LEVEL < XBOXINPUT_LEVEL_USBRESET
#define XBOXINPUT_NO_USB_RESET 1
#endif

// ---------------------------------------------------------------------------
// Logging levels.
//
// XBOXINPUT_LOG   - always on. Milestones and errors only: load, claim, endpoints,
//            controller ready, XAM registration, teardown, anything that failed.
// XBOXINPUT_DBG   - per-packet chatter (GIP chunks, input reports, USB probes,
//            capability queries). OFF by default.
//
// This is not just tidiness: DbgPrint on a hot path saturates the xbdm debug
// channel and HANGS THE CONSOLE. That happened twice during development - once
// dumping every protocol packet, and once logging every capability query (which
// the dash polls ~8x per 100ms). Keep hot paths under XBOXINPUT_DBG.
// ---------------------------------------------------------------------------
//#define XBOXINPUT_VERBOSE 1

// Debug Monitor is not present on most retail RGH setups. Mirror milestone logs
// to a small FTP-readable file as well. XBOXINPUT_LOG is deliberately never used for
// normal input packets, so this does not put filesystem I/O in the input path.
// Keep diagnostics usable on consoles without an HDD (for example BadAvatar
// installs that run from internal MU or USB).  The first writable device is
// remembered so the normal and compatibility logs stay together.
static const char* const kXboxInputLogPaths[] = {
	"HDD:\\XboxInputGip.log",
	"Usb:\\XboxInputGip.log",
	"Usb0:\\XboxInputGip.log",
	"Usb1:\\XboxInputGip.log",
	"Mu:\\XboxInputGip.log",
	"Mu0:\\XboxInputGip.log",
	"UsbMu:\\XboxInputGip.log",
	"FlashMu:\\XboxInputGip.log",
	"IntMu:\\XboxInputGip.log",
	"MmcMu:\\XboxInputGip.log",
};
static const char* XboxInputKnownControllerName(uint16_t vid, uint16_t pid);

// USB callbacks can run above PASSIVE_LEVEL, so they queue compact events here.
// The existing logger thread is the only code that writes them to HDD.
#define XBOXINPUT_LOG_EVENT_COUNT 128
enum XboxInputLogEventType {
	XBOXINPUT_LOG_CONTROLLER_DETECTED = 1,
	XBOXINPUT_LOG_CONTROLLER_READY = 2,
	XBOXINPUT_LOG_CONTROLLER_REMOVED = 3,
	XBOXINPUT_LOG_USB_FAILURE = 4,
	XBOXINPUT_LOG_INIT_STEP = 5,
	XBOXINPUT_LOG_USB_STEP = 6,
	XBOXINPUT_LOG_FIRST_INPUT = 7,
	XBOXINPUT_LOG_RUMBLE = 8,
	XBOXINPUT_LOG_NOTIFICATION = 9,
	XBOXINPUT_LOG_PROFILE_DECISION = 10,
	XBOXINPUT_LOG_NOTIFICATION_CANCELLED = 11,
	XBOXINPUT_LOG_CONFIG_PATH = 12,
};

enum XboxInputInitStep {
	XBOXINPUT_INIT_ENTRY = 1,
	XBOXINPUT_INIT_LOGGER_STARTED,
	XBOXINPUT_INIT_ENVIRONMENT,
	XBOXINPUT_INIT_CONFIG_LOADED,
	XBOXINPUT_INIT_FUNCTIONS_READY,
	XBOXINPUT_INIT_USB_HOOKS_READY,
	XBOXINPUT_INIT_XAM_HOOKS_READY,
	XBOXINPUT_INIT_USB_RESET_SKIPPED,
	XBOXINPUT_INIT_COMPLETE,
	XBOXINPUT_INIT_ABORT_UNSUPPORTED,
	XBOXINPUT_INIT_ABORT_FUNCTIONS,
};

enum XboxInputUsbStep {
	XBOXINPUT_USB_CANDIDATE = 1,
	XBOXINPUT_USB_CLAIM_BEGIN,
	XBOXINPUT_USB_CLAIM_COMPLETE,
	XBOXINPUT_USB_DEFAULT_ENDPOINT_OPEN,
	XBOXINPUT_USB_SET_CONFIG_QUEUED,
	XBOXINPUT_USB_SET_CONFIG_COMPLETE,
	XBOXINPUT_USB_INTERRUPT_IN_OPEN,
	XBOXINPUT_USB_INTERRUPT_OUT_OPEN,
	XBOXINPUT_USB_READ_QUEUED,
	XBOXINPUT_USB_ANNOUNCE,
	XBOXINPUT_USB_IDENTIFY_SENT,
	XBOXINPUT_USB_IDENTIFY_COMPLETE,
	XBOXINPUT_USB_XAM_REGISTER,
	XBOXINPUT_USB_REMOVE_BEGIN,
	XBOXINPUT_USB_REMOVE_COMPLETE,
	XBOXINPUT_USB_EARLY_POWER_QUEUED,
	XBOXINPUT_USB_EARLY_POWER_COMPLETE,
	XBOXINPUT_USB_RESUMED_FROM_INPUT,
	XBOXINPUT_USB_READ_LOOP_STOPPED,
	XBOXINPUT_USB_PRE_READ_LED_QUEUED,
	XBOXINPUT_USB_PRE_READ_LED_COMPLETE,
	XBOXINPUT_USB_PRE_READ_AUTH_QUEUED,
	XBOXINPUT_USB_PRE_READ_AUTH_COMPLETE,
	XBOXINPUT_USB_AUDIO_INTERFACE_DISABLE_QUEUED,
	XBOXINPUT_USB_AUDIO_INTERFACE_DISABLE_COMPLETE,
	XBOXINPUT_USB_INIT_POWER_QUEUED,
	XBOXINPUT_USB_INIT_POWER_COMPLETE,
	XBOXINPUT_USB_INIT_RUMBLE_SETUP_QUEUED,
	XBOXINPUT_USB_INIT_RUMBLE_SETUP_COMPLETE,
	XBOXINPUT_USB_POST_IDENTIFY_STARTUP_COMPLETE,
	XBOXINPUT_USB_POWERA_STAGE = 100,
};
struct XboxInputLogEvent {
	volatile LONG serial; // Published last by the producer.
	DWORD tick;
	DWORD type;
	DWORD value1;
	DWORD value2;
	DWORD value3;
	DWORD value4;
	DWORD value5;
};
static volatile LONG g_xboxInputLogEventSerial = 0;
static XboxInputLogEvent g_xboxInputLogEvents[XBOXINPUT_LOG_EVENT_COUNT];
static volatile LONG g_xboxInputRemovedUserMask = 0;
static volatile LONG g_xboxInputRemovedUiMask = 0;
static volatile LONG g_xboxInputDisconnectedUiMask = 0;
static volatile LONG g_xboxInputUiWorkerRunning = 0;
static volatile LONG g_xboxInputUiWorkerTitle = 0;
static void XboxInputQueueLogEvent(DWORD type, DWORD value1, DWORD value2);
static void XboxInputQueueLogEventEx(DWORD type, DWORD value1, DWORD value2,
	DWORD value3, DWORD value4, DWORD value5);

static void XboxInputSetMaskBit(volatile LONG* mask, LONG bit) {
	for (;;) {
		LONG before = *mask;
		if (InterlockedCompareExchange(mask, before | bit, before) == before)
			return;
	}
}

static void XboxInputClearMaskBit(volatile LONG* mask, LONG bit) {
	for (;;) {
		LONG before = *mask;
		if (InterlockedCompareExchange(mask, before & ~bit, before) == before)
			return;
	}
}

// USB removal callbacks may run above PASSIVE_LEVEL. They may publish one bit here,
// but must never enter XAM UI code directly; the logger/system thread drains it.
static void XboxInputQueueRemovedControllerNotification(BYTE userIndex) {
	if (userIndex >= 4)
		return;
	const LONG bit = (LONG)(1u << userIndex);
	XboxInputSetMaskBit(&g_xboxInputRemovedUserMask, bit);
	XboxInputSetMaskBit(&g_xboxInputRemovedUiMask, bit);
	XboxInputSetMaskBit(&g_xboxInputDisconnectedUiMask, bit);
}

static void XboxInputCancelRemovedControllerNotification(BYTE userIndex) {
	if (userIndex >= 4)
		return;
	const LONG bit = (LONG)(1u << userIndex);
	XboxInputClearMaskBit(&g_xboxInputRemovedUserMask, bit);
	XboxInputClearMaskBit(&g_xboxInputRemovedUiMask, bit);
	XboxInputClearMaskBit(&g_xboxInputDisconnectedUiMask, bit);
}

static void XboxInputProcessControllerNotifications() {
	LONG removed = InterlockedExchange(&g_xboxInputRemovedUserMask, 0);
	DWORD broadcastResult = removed
		? XNotifyBroadcast(XN_SYS_INPUTDEVICESCHANGED, 0)
		: 0;
	for (BYTE user = 0; user < 4; ++user) {
		if (removed & (1u << user)) {
			XboxInputQueueLogEvent(XBOXINPUT_LOG_NOTIFICATION, user,
				broadcastResult);
		}
	}
}

static void XboxInputQueueLogEvent(DWORD type, DWORD value1, DWORD value2) {
	XboxInputQueueLogEventEx(type, value1, value2, 0, 0, 0);
}

static void XboxInputQueueLogEventEx(DWORD type, DWORD value1, DWORD value2,
	DWORD value3, DWORD value4, DWORD value5) {
	LONG serial = InterlockedIncrement(&g_xboxInputLogEventSerial);
	XboxInputLogEvent* event =
		&g_xboxInputLogEvents[(serial - 1) % XBOXINPUT_LOG_EVENT_COUNT];
	event->serial = 0;
	event->tick = GetTickCount();
	event->type = type;
	event->value1 = value1;
	event->value2 = value2;
	event->value3 = value3;
	event->value4 = value4;
	event->value5 = value5;
	__sync();
	event->serial = serial;
}
#ifdef XBOXINPUT_COMPAT_PROBE
static const char* const kXboxInputCompatProbeLogPaths[] = {
	"HDD:\\XboxInputCompatProbe.log",
	"Usb:\\XboxInputCompatProbe.log",
	"Usb0:\\XboxInputCompatProbe.log",
	"Usb1:\\XboxInputCompatProbe.log",
	"Mu:\\XboxInputCompatProbe.log",
	"Mu0:\\XboxInputCompatProbe.log",
	"UsbMu:\\XboxInputCompatProbe.log",
	"FlashMu:\\XboxInputCompatProbe.log",
	"IntMu:\\XboxInputCompatProbe.log",
	"MmcMu:\\XboxInputCompatProbe.log",
};
#define XBOXINPUT_COMPAT_PROBE_RECORDS 16
struct XboxInputCompatProbeRecord {
	volatile LONG serial; // Published last by the USB callback.
	DWORD vidPid;
	DWORD devClass;
	DWORD iface;
	DWORD protocol;
};
static volatile LONG  g_xboxInputCompatProbeSerial = 0;
static XboxInputCompatProbeRecord g_xboxInputCompatProbeRecords[XBOXINPUT_COMPAT_PROBE_RECORDS];
#endif
static volatile LONG g_xboxInputDiagStage = 0;
static volatile DWORD g_xboxInputGuideCaller = 0;
static volatile DWORD g_xboxInputGuideUiState = 0;
static volatile LONG g_xboxInputLogPathIndex = -1;
static volatile LONG g_xboxInputLoggerReady = 0;

#define XBOXINPUT_PLUGIN_LOCAL_LOG_INDEX (-2)

static const char* XboxInputInitStepName(DWORD step) {
	switch (step) {
	case XBOXINPUT_INIT_ENTRY: return "dll_entry";
	case XBOXINPUT_INIT_LOGGER_STARTED: return "logger_thread_started";
	case XBOXINPUT_INIT_ENVIRONMENT: return "environment_checked";
	case XBOXINPUT_INIT_CONFIG_LOADED: return "config_loaded";
	case XBOXINPUT_INIT_FUNCTIONS_READY: return "function_pointers_ready";
	case XBOXINPUT_INIT_USB_HOOKS_READY: return "usb_hooks_installed";
	case XBOXINPUT_INIT_XAM_HOOKS_READY: return "xam_hooks_installed";
	case XBOXINPUT_INIT_USB_RESET_SKIPPED: return "usb_reset_skipped";
	case XBOXINPUT_INIT_COMPLETE: return "startup_complete";
	case XBOXINPUT_INIT_ABORT_UNSUPPORTED: return "startup_inert_unsupported_environment";
	case XBOXINPUT_INIT_ABORT_FUNCTIONS: return "startup_inert_function_resolution_failed";
	default: return "unknown";
	}
}

static const char* XboxInputUsbStepName(DWORD step) {
	switch (step) {
	case XBOXINPUT_USB_CANDIDATE: return "supported_candidate_seen";
	case XBOXINPUT_USB_CLAIM_BEGIN: return "claim_begin";
	case XBOXINPUT_USB_CLAIM_COMPLETE: return "claim_complete";
	case XBOXINPUT_USB_DEFAULT_ENDPOINT_OPEN: return "default_endpoint_open";
	case XBOXINPUT_USB_SET_CONFIG_QUEUED: return "set_configuration_queued";
	case XBOXINPUT_USB_SET_CONFIG_COMPLETE: return "set_configuration_complete";
	case XBOXINPUT_USB_INTERRUPT_IN_OPEN: return "interrupt_in_open";
	case XBOXINPUT_USB_INTERRUPT_OUT_OPEN: return "interrupt_out_open";
	case XBOXINPUT_USB_READ_QUEUED: return "first_interrupt_read_queued";
	case XBOXINPUT_USB_ANNOUNCE: return "announce_received";
	case XBOXINPUT_USB_IDENTIFY_SENT: return "identify_sent";
	case XBOXINPUT_USB_IDENTIFY_COMPLETE: return "identify_complete";
	case XBOXINPUT_USB_XAM_REGISTER: return "xam_registered";
	case XBOXINPUT_USB_REMOVE_BEGIN: return "remove_begin";
	case XBOXINPUT_USB_REMOVE_COMPLETE: return "remove_complete";
	case XBOXINPUT_USB_EARLY_POWER_QUEUED: return "early_power_queued";
	case XBOXINPUT_USB_EARLY_POWER_COMPLETE: return "early_power_complete";
	case XBOXINPUT_USB_RESUMED_FROM_INPUT: return "resumed_from_valid_input";
	case XBOXINPUT_USB_READ_LOOP_STOPPED: return "read_loop_stopped";
	case XBOXINPUT_USB_PRE_READ_LED_QUEUED: return "pre_read_led_queued";
	case XBOXINPUT_USB_PRE_READ_LED_COMPLETE: return "pre_read_led_complete";
	case XBOXINPUT_USB_PRE_READ_AUTH_QUEUED: return "pre_read_auth_queued";
	case XBOXINPUT_USB_PRE_READ_AUTH_COMPLETE: return "pre_read_auth_complete";
	case XBOXINPUT_USB_AUDIO_INTERFACE_DISABLE_QUEUED: return "audio_interface_disable_queued";
	case XBOXINPUT_USB_AUDIO_INTERFACE_DISABLE_COMPLETE: return "audio_interface_disable_complete";
	case XBOXINPUT_USB_INIT_POWER_QUEUED: return "init_power_queued";
	case XBOXINPUT_USB_INIT_POWER_COMPLETE: return "init_power_complete";
	case XBOXINPUT_USB_INIT_RUMBLE_SETUP_QUEUED: return "init_rumble_setup_queued";
	case XBOXINPUT_USB_INIT_RUMBLE_SETUP_COMPLETE: return "init_rumble_setup_complete";
	case XBOXINPUT_USB_POST_IDENTIFY_STARTUP_COMPLETE: return "post_identify_startup_complete";
	default: return step >= XBOXINPUT_USB_POWERA_STAGE ? "powera_init_stage" : "unknown";
	}
}

static FILE* XboxInputOpenLog(bool compatibilityProbe, const char* mode) {
	const DWORD pathCount = sizeof(kXboxInputLogPaths) / sizeof(kXboxInputLogPaths[0]);
	LONG selected = g_xboxInputLogPathIndex;
	if (selected == XBOXINPUT_PLUGIN_LOCAL_LOG_INDEX) {
#ifdef XBOXINPUT_COMPAT_PROBE
		const char* path = compatibilityProbe ? XBOXINPUT_PROBE_PATH : XBOXINPUT_LOG_PATH;
#else
		UNREFERENCED_PARAMETER(compatibilityProbe);
		const char* path = XBOXINPUT_LOG_PATH;
#endif
		FILE* file = fopen(path, mode);
		if (file)
			return file;
	}
	if (selected >= 0 && (DWORD)selected < pathCount) {
#ifdef XBOXINPUT_COMPAT_PROBE
		const char* path = compatibilityProbe
			? kXboxInputCompatProbeLogPaths[selected]
			: kXboxInputLogPaths[selected];
#else
		UNREFERENCED_PARAMETER(compatibilityProbe);
		const char* path = kXboxInputLogPaths[selected];
#endif
		FILE* file = fopen(path, mode);
		if (file)
			return file;
	}

	// Prefer the XEX directory. A private symbolic link is used when the loader
	// supplied a native \\Device path, so this remains available before Usb: aliases.
	if (selected != XBOXINPUT_PLUGIN_LOCAL_LOG_INDEX) {
#ifdef XBOXINPUT_COMPAT_PROBE
		const char* path = compatibilityProbe ? XBOXINPUT_PROBE_PATH : XBOXINPUT_LOG_PATH;
#else
		const char* path = XBOXINPUT_LOG_PATH;
#endif
		FILE* file = fopen(path, mode);
		if (file) {
			InterlockedExchange(&g_xboxInputLogPathIndex,
				XBOXINPUT_PLUGIN_LOCAL_LOG_INDEX);
			return file;
		}
	}

	for (DWORD i = 0; i < pathCount; i++) {
		if ((LONG)i == selected)
			continue;
#ifdef XBOXINPUT_COMPAT_PROBE
		const char* path = compatibilityProbe
			? kXboxInputCompatProbeLogPaths[i]
			: kXboxInputLogPaths[i];
#else
		const char* path = kXboxInputLogPaths[i];
#endif
		FILE* file = fopen(path, mode);
		if (file) {
			InterlockedExchange(&g_xboxInputLogPathIndex, (LONG)i);
			return file;
		}
	}
	return NULL;
}

static void XboxInputLogReset() {
	FILE* file = XboxInputOpenLog(false, "w");
	if (file)
		fclose(file);
#ifdef XBOXINPUT_COMPAT_PROBE
	file = XboxInputOpenLog(true, "w");
	if (file)
		fclose(file);
#endif
}

static void XboxInputSetDiagStage(LONG stage) {
	InterlockedExchange(&g_xboxInputDiagStage, stage);
}

// USB callbacks may run above PASSIVE_LEVEL, where filesystem calls can deadlock.
// Keep their normal diagnostics on DbgPrint and let a system thread persist just
// the latest coarse-grained stage to the FTP-readable file.
#define XBOXINPUT_LOG(...) DbgPrint(__VA_ARGS__)

// XBOXINPUT_TRACE_LOG - breadcrumbs on the device add/remove path only, for the `trace` variant.
// Deliberately NOT tied to XBOXINPUT_VERBOSE: that also turns on per-packet logging,
// which floods xbdm and is itself a hazard. These fire a handful of times per plug event.
#ifdef XBOXINPUT_TRACE
#define XBOXINPUT_TRACE_LOG(...) DbgPrint(__VA_ARGS__)
#else
#define XBOXINPUT_TRACE_LOG(...) ((void)0)
#endif
#ifdef XBOXINPUT_VERBOSE
#define XBOXINPUT_DBG(...) DbgPrint(__VA_ARGS__)
#else
#define XBOXINPUT_DBG(...) ((void)0)
#endif

// ---------------------------------------------------------------------------
// USB bugcheck patch save/restore.
//
// hiddriver360 NOPs out two USB bugchecks so it can tear down and re-enter the USB
// driver at load. That works, but it also disables fault containment permanently, and
// the evidence points at that being why a device removal freezes the console instead
// of raising a survivable exception.
//
// We keep the patches for the reset, then put the original instructions back.
// ---------------------------------------------------------------------------
struct GipSavedPatch {
	DWORD* addr;
	DWORD  original;
	bool   valid;
};
static GipSavedPatch g_gipSavedPatches[2];

static void GipSavePatch(int slot, DWORD* addr) {
	if (slot < 0 || slot >= 2)
		return;
	g_gipSavedPatches[slot].addr = addr;
	g_gipSavedPatches[slot].original = *addr;
	g_gipSavedPatches[slot].valid = true;
}

static void GipRestoreUsbBugchecks() {
	for (int i = 0; i < 2; i++) {
		GipSavedPatch* p = &g_gipSavedPatches[i];
		if (!p->valid)
			continue;
		*p->addr = p->original;
		// Push the store out of the data cache. There is no __icbi intrinsic in
		// ppcintrinsics.h, but hiddriver360 patches and detours live kernel code with
		// plain stores throughout and those take effect, so this follows the same
		// precedent with a dcbst/sync added for good measure.
		__dcbst(0, p->addr);
		__sync();
		DbgPrint("XBOXINPUT: restored USB bugcheck at %p -> 0x%08X\r\n",
			p->addr, p->original);
	}
}

#ifdef XBOXINPUT_RESTORE_WGC_MATCH
// XeUnshackle includes both halves of UsbdSecPatch.  The authentication bypass
// is useful to us, but its second patch changes WgcAddDevice's descriptor test
// from a conditional branch to an unconditional one.  That can let the stock
// XUSB driver claim a GIP controller before this plugin sees it as unclaimed.
//
// This diagnostic restores only the original 17559 conditional branch.  It is
// intentionally guarded by the exact patched opcode: never overwrite an
// unknown kernel or another project's different modification.
static DWORD g_xboxInputWgcOpcodeBefore = 0;
static DWORD g_xboxInputWgcOpcodeAfter = 0;

static void XboxInputRestoreWgcDescriptorCheck() {
	const DWORD kWgcMatchAddress = 0x800F98E0;
	const DWORD kXeUnshackleOpcode = 0x48000010;
	const DWORD kRetail17559Opcode = 0x409A0010;

	if (XboxKrnlVersion->Build != 17559)
		return;

	volatile DWORD* instruction = (volatile DWORD*)kWgcMatchAddress;
	g_xboxInputWgcOpcodeBefore = *instruction;
	if (g_xboxInputWgcOpcodeBefore == kXeUnshackleOpcode) {
		*instruction = kRetail17559Opcode;
		doSync((void*)instruction);
	}
	g_xboxInputWgcOpcodeAfter = *instruction;
	XBOXINPUT_LOG("XBOXINPUT: WGC descriptor branch %08X -> %08X\r\n",
		g_xboxInputWgcOpcodeBefore, g_xboxInputWgcOpcodeAfter);
}
#endif

Detour HidAddDeviceDetour;
Detour HidRemoveDeviceDetour;
Detour XamInputSetStateDetour;
Detour XamInputGetCapabilitiesDetour;
Detour XamInputGetStateDetour;
Detour XInputdReadStateDetour;
Detour XamInputGetCapabilitiesDetour2;   // plain XamInputGetCapabilities, ordinal 400
#define XNOTIFYUI_CUSTOM (XNOTIFYQUEUEUI_TYPE)80
uint16_t swap_endianness_16(uint16_t val) {
	return (val >> 8) | (val << 8);
}

BOOL IsTrayOpen() {
	BYTE Input[0x10] = { 0 }, Output[0x10] = { 0 };
	Input[0] = 0xA;
	HalSendSMCMessage(Input, Output);
	return (Output[1] == 0x60);
}

// This console likes to kill non system threads on title switches
HANDLE MakeThread(LPTHREAD_START_ROUTINE Address, PVOID arg) {
	HANDLE Handle = 0;
	NTSTATUS status = ExCreateThread(&Handle, 0, 0, XapiThreadStartup, Address, arg,
		(EX_CREATE_FLAG_SUSPENDED | EX_CREATE_FLAG_SYSTEM | 0x18000424));
	if (status < 0 || !Handle) {
		DbgPrint("XBOXINPUT: ExCreateThread failed status=%08X handle=%p\r\n", status, Handle);
		return NULL;
	}
	XSetThreadProcessor(Handle, 4);
	SetThreadPriority(Handle, THREAD_PRIORITY_NORMAL);
	ResumeThread(Handle);
	return Handle;
}

static bool XboxInputFinishLogWrite(FILE* file) {
	if (!file)
		return false;
	int flushResult = fflush(file);
	int streamError = ferror(file);
	int closeResult = fclose(file);
	return flushResult == 0 && streamError == 0 && closeResult == 0;
}

// When HDD: is the primary log, mirror the complete file to every other mounted
// storage root. This is intentionally a snapshot copy rather than a second set of
// event cursors: a USB device inserted later still receives the full startup history,
// and an internal HDD cannot hide the diagnostics from a BadAvatar/BadUpdate user.
// Called only by XboxInputLogThread, never by DllMain or a USB callback.
static bool XboxInputLogFilesEqual(FILE* source, const char* destinationPath) {
	FILE* destination = fopen(destinationPath, "rb");
	if (!destination)
		return false;
	BYTE sourceBuffer[512];
	BYTE destinationBuffer[512];
	bool equal = true;
	fseek(source, 0, SEEK_SET);
	for (;;) {
		size_t sourceBytes = fread(sourceBuffer, 1, sizeof(sourceBuffer), source);
		size_t destinationBytes = fread(destinationBuffer, 1, sizeof(destinationBuffer), destination);
		if (sourceBytes != destinationBytes ||
			(sourceBytes && memcmp(sourceBuffer, destinationBuffer, sourceBytes) != 0)) {
			equal = false;
			break;
		}
		if (sourceBytes < sizeof(sourceBuffer)) {
			if (ferror(source) || ferror(destination))
				equal = false;
			break;
		}
	}
	fclose(destination);
	fseek(source, 0, SEEK_SET);
	return equal;
}

static void XboxInputMirrorOneLog(const char* sourcePath, const char* destinationPath) {
	FILE* source = fopen(sourcePath, "rb");
	if (!source)
		return;
	// Polling discovers USB devices inserted after boot, but identical files incur
	// reads only. Avoid repeatedly truncating and rewriting removable media.
	if (XboxInputLogFilesEqual(source, destinationPath)) {
		fclose(source);
		return;
	}
	FILE* destination = fopen(destinationPath, "wb");
	if (!destination) {
		fclose(source);
		return;
	}

	BYTE buffer[512];
	bool ok = true;
	for (;;) {
		size_t got = fread(buffer, 1, sizeof(buffer), source);
		if (got && fwrite(buffer, 1, got, destination) != got) {
			ok = false;
			break;
		}
		if (got < sizeof(buffer)) {
			if (ferror(source))
				ok = false;
			break;
		}
	}
	if (fflush(destination) != 0 || ferror(destination))
		ok = false;
	if (fclose(destination) != 0)
		ok = false;
	fclose(source);
	UNREFERENCED_PARAMETER(ok); // A failed/partial mirror is replaced on the next pass.
}

static void XboxInputMirrorLogsToOtherStorage() {
	// Index zero is HDD:. If another device became the primary, the log is already
	// removable and copying between potentially aliased Usb:/Usb0: names could
	// truncate the source. The HDD -> removable direction has no aliasing hazard.
	if (g_xboxInputLogPathIndex != 0)
		return;
	// Mirror only to removable USB names. Do not generate continuous diagnostic
	// copies on internal flash/MU devices merely because HDD: also exists.
	static const BYTE usbPathIndices[] = { 1, 2, 3, 6 }; // Usb, Usb0, Usb1, UsbMu
	for (DWORD n = 0; n < sizeof(usbPathIndices) / sizeof(usbPathIndices[0]); ++n) {
		DWORD i = usbPathIndices[n];
		XboxInputMirrorOneLog(kXboxInputLogPaths[0], kXboxInputLogPaths[i]);
#ifdef XBOXINPUT_COMPAT_PROBE
		XboxInputMirrorOneLog(kXboxInputCompatProbeLogPaths[0],
			kXboxInputCompatProbeLogPaths[i]);
#endif
	}
}

static DWORD XboxInputLogThread(PVOID) {
	// Storage devices are not guaranteed to be mounted when DashLaunch calls
	// DllMain.  Keep retrying forever: a late HDD/USB mount must still result in
	// a log containing all initialization events retained in the ring.
	for (;;) {
		XboxInputProcessControllerNotifications();
		FILE* file = XboxInputOpenLog(false, "w");
		if (file) {
			LONG pathIndex = g_xboxInputLogPathIndex;
			fprintf(file, "XboxInput detailed diagnostic log\r\n");
			fprintf(file, "build=%s %s kernel=%u ladder=%u\r\n",
				__DATE__, __TIME__, XboxKrnlVersion->Build,
				(DWORD)XBOXINPUT_BUILD_LEVEL);
			fprintf(file, "logPath=%s retryPolicy=continuous eventBuffer=%u privateMount=%u status=%08X\r\n",
				pathIndex == XBOXINPUT_PLUGIN_LOCAL_LOG_INDEX ? XBOXINPUT_LOG_PATH :
				((pathIndex >= 0 && pathIndex < (LONG)(sizeof(kXboxInputLogPaths) / sizeof(kXboxInputLogPaths[0])))
					? kXboxInputLogPaths[pathIndex] : "unknown"),
				(DWORD)XBOXINPUT_LOG_EVENT_COUNT,
				(DWORD)(g_xboxInputUsingPrivateMount ? 1 : 0), g_xboxInputPrivateMountStatus);
			if (XboxInputFinishLogWrite(file)) {
				InterlockedExchange(&g_xboxInputLoggerReady, 1);
				break;
			}
		}
		Sleep(250);
	}
#ifdef XBOXINPUT_COMPAT_PROBE
	// The normal log selected a writable device. Reset the companion file here,
	// never in DllMain or a USB callback.
	FILE* compatReset = XboxInputOpenLog(true, "w");
	if (compatReset)
		XboxInputFinishLogWrite(compatReset);
#endif

	LONG written = -1;
	DWORD writtenGuideCaller = 0;
	DWORD writtenGuideUiState = 0;
	LONG writtenEventSerial = 0;
#ifdef XBOXINPUT_RESTORE_WGC_MATCH
	bool wroteWgcPatchState = false;
#endif
#ifdef XBOXINPUT_COMPAT_PROBE
	LONG writtenProbeSerial = 0;
#endif
	DWORD lastMirrorTick = 0;
	for (;;) {
		XboxInputProcessControllerNotifications();
#ifdef XBOXINPUT_RESTORE_WGC_MATCH
		if (!wroteWgcPatchState) {
			FILE* file = XboxInputOpenLog(false, "a");
			if (file) {
				fprintf(file, "wgcDescriptorBranch=%08X->%08X\r\n",
					g_xboxInputWgcOpcodeBefore, g_xboxInputWgcOpcodeAfter);
				wroteWgcPatchState = XboxInputFinishLogWrite(file);
			}
		}
#endif
		LONG stage = g_xboxInputDiagStage;
		if (stage != written) {
			FILE* file = XboxInputOpenLog(false, "a");
			if (file) {
				fprintf(file, "stage=%ld\r\n", stage);
				if (XboxInputFinishLogWrite(file))
					written = stage;
			}
		}
		DWORD guideCaller = g_xboxInputGuideCaller;
		DWORD guideUiState = g_xboxInputGuideUiState;
		if (guideCaller && (guideCaller != writtenGuideCaller ||
			guideUiState != writtenGuideUiState)) {
			FILE* file = XboxInputOpenLog(false, "a");
			if (file) {
				fprintf(file, "guideCaller=%08X xenonUi=%u\r\n",
					guideCaller, guideUiState);
				if (XboxInputFinishLogWrite(file)) {
					writtenGuideCaller = guideCaller;
					writtenGuideUiState = guideUiState;
				}
			}
		}
		LONG eventSerial = g_xboxInputLogEventSerial;
		if (eventSerial - writtenEventSerial > XBOXINPUT_LOG_EVENT_COUNT) {
			LONG dropped = eventSerial - writtenEventSerial - XBOXINPUT_LOG_EVENT_COUNT;
			FILE* overflow = XboxInputOpenLog(false, "a");
			if (overflow) {
				fprintf(overflow, "tick=%lu event=logger_overflow dropped=%ld\r\n",
					GetTickCount(), dropped);
				if (XboxInputFinishLogWrite(overflow))
					writtenEventSerial = eventSerial - XBOXINPUT_LOG_EVENT_COUNT;
			}
		}
		while (writtenEventSerial < eventSerial) {
			LONG wanted = writtenEventSerial + 1;
			XboxInputLogEvent* event =
				&g_xboxInputLogEvents[(wanted - 1) % XBOXINPUT_LOG_EVENT_COUNT];
			if (event->serial != wanted)
				break;
			FILE* file = XboxInputOpenLog(false, "a");
			if (file) {
				switch (event->type) {
				case XBOXINPUT_LOG_CONTROLLER_DETECTED:
					fprintf(file, "tick=%lu event=controller_detected vid=%04X pid=%04X model=%s interface=%u endpoints=%u\r\n",
						event->tick,
						(WORD)(event->value1 >> 16), (WORD)event->value1,
						XboxInputKnownControllerName((WORD)(event->value1 >> 16),
							(WORD)event->value1),
						(BYTE)(event->value2 >> 8), (BYTE)event->value2);
					break;
				case XBOXINPUT_LOG_CONTROLLER_READY:
					fprintf(file, "tick=%lu event=controller_ready user=%u context=%08X\r\n",
						event->tick, (BYTE)event->value1, event->value2);
					break;
				case XBOXINPUT_LOG_CONTROLLER_REMOVED:
					fprintf(file, "tick=%lu event=controller_removed user=%u\r\n",
						event->tick, (BYTE)event->value1);
					break;
				case XBOXINPUT_LOG_USB_FAILURE:
					fprintf(file, "tick=%lu event=usb_failure step=%u status=%08X\r\n",
						event->tick, event->value1, event->value2);
					break;
				case XBOXINPUT_LOG_INIT_STEP:
					fprintf(file, "tick=%lu event=init step=%u name=%s value=%08X\r\n",
						event->tick, event->value1,
						XboxInputInitStepName(event->value1), event->value2);
					break;
				case XBOXINPUT_LOG_USB_STEP:
					fprintf(file, "tick=%lu event=usb_step step=%u name=%s value=%08X\r\n",
						event->tick, event->value1,
						XboxInputUsbStepName(event->value1), event->value2);
					break;
				case XBOXINPUT_LOG_FIRST_INPUT:
					fprintf(file, "tick=%lu event=first_input user=%u packet=%u\r\n",
						event->tick, event->value1, event->value2);
					break;
				case XBOXINPUT_LOG_RUMBLE:
					fprintf(file, "tick=%lu event=rumble user=%u motors=%08X\r\n",
						event->tick, event->value1, event->value2);
					break;
				case XBOXINPUT_LOG_NOTIFICATION:
					fprintf(file, "tick=%lu event=disconnect_notification user=%u broadcast=%08X\r\n",
						event->tick, event->value1, event->value2);
					break;
				case XBOXINPUT_LOG_NOTIFICATION_CANCELLED:
					fprintf(file, "tick=%lu event=disconnect_notification_cancelled reason=%s remaining=%08X\r\n",
						event->tick, event->value1 ? "timeout" : "reconnected",
						event->value2);
					break;
				case XBOXINPUT_LOG_CONFIG_PATH:
					fprintf(file, "tick=%lu event=config_path state=%s path=%s\r\n",
						event->tick,
						event->value1 == 1 ? "loaded" :
						(event->value1 == 2 ? "generated" : "unavailable"),
						XBOXINPUT_CFG_PATH);
					break;
				case XBOXINPUT_LOG_PROFILE_DECISION: {
					WORD vid = (WORD)(event->value1 >> 16);
					WORD pid = (WORD)event->value1;
					WORD revision = (WORD)(event->value2 >> 16);
					XboxInputProfileMatchResult result =
						(XboxInputProfileMatchResult)(event->value2 & 0xFFFF);
					const XboxInputControllerProfile* profile =
						XboxInputFindProfileById(vid, pid, 0);
					fprintf(file,
						"tick=%lu event=profile_decision vid=%04X pid=%04X revision=%04X "
						"interface=%u alt=%u endpoints=%u class=%02X/%02X/%02X "
						"usbStatus=%08X result=%s action=%s profile=%s transport=%s parser=%s init=%s "
						"caps=%08X quirks=%08X expected=%u/%u/%u/%02X/%02X/%02X\r\n",
						event->tick, vid, pid, revision,
						(BYTE)(event->value3 >> 24), (BYTE)(event->value3 >> 16),
						(BYTE)(event->value3 >> 8), (BYTE)event->value3,
						(BYTE)(event->value4 >> 8), (BYTE)event->value4,
						event->value5, XboxInputProfileMatchResultName(result),
						result == XBOXINPUT_PROFILE_MATCHED ? "claim_candidate" : "observe_only",
						profile ? profile->name : "none",
						profile ? XboxInputTransportName(profile->transport) : "none",
						profile ? XboxInputParserName(profile->parser) : "none",
						profile ? XboxInputInitProfileName(profile->initProfile) : "none",
						profile ? profile->capabilities : 0,
						profile ? profile->quirks : 0,
						profile ? profile->interfaceIdentity.number : 0,
						profile ? profile->interfaceIdentity.alternateSetting : 0,
						profile ? profile->interfaceIdentity.endpointCount : 0,
						profile ? profile->interfaceIdentity.interfaceClass : 0,
						profile ? profile->interfaceIdentity.interfaceSubClass : 0,
						profile ? profile->interfaceIdentity.interfaceProtocol : 0);
					break;
				}
				default:
					fprintf(file, "tick=%lu event=unknown type=%u value1=%08X value2=%08X\r\n",
						event->tick, event->type, event->value1, event->value2);
					break;
				}
				if (XboxInputFinishLogWrite(file))
					writtenEventSerial = wanted;
			}
			else
				break; // Storage is not ready yet; retry this record next pass.
		}
#ifdef XBOXINPUT_COMPAT_PROBE
		LONG probeSerial = g_xboxInputCompatProbeSerial;
		if (probeSerial - writtenProbeSerial > XBOXINPUT_COMPAT_PROBE_RECORDS)
			writtenProbeSerial = probeSerial - XBOXINPUT_COMPAT_PROBE_RECORDS;
		while (writtenProbeSerial < probeSerial) {
			LONG wanted = writtenProbeSerial + 1;
			XboxInputCompatProbeRecord* record =
				&g_xboxInputCompatProbeRecords[(wanted - 1) % XBOXINPUT_COMPAT_PROBE_RECORDS];
			if (record->serial != wanted)
				break; // A burst exceeded the small ring; retain later records safely.
			DWORD vp = record->vidPid;
			DWORD dc = record->devClass;
			DWORD iface = record->iface;
			FILE* probe = XboxInputOpenLog(true, "a");
			if (probe) {
				fprintf(probe, "serial=%ld vid=%04X pid=%04X devclass=%02X/%02X/%02X if=%u endpoints=%u ifclass=%02X/%02X/%02X\r\n",
					wanted, (WORD)(vp >> 16), (WORD)vp,
					(BYTE)(dc >> 16), (BYTE)(dc >> 8), (BYTE)dc,
					(BYTE)(iface >> 24), (BYTE)(iface >> 16),
					(BYTE)(iface >> 8), (BYTE)iface, (BYTE)record->protocol);
				if (XboxInputFinishLogWrite(probe))
					writtenProbeSerial = wanted;
			}
			else
				break; // Storage is not ready yet; retry this record next pass.
		}
#endif
		// Re-copy periodically even when no new event was generated. This lets a USB
		// device connected after boot receive the complete existing HDD log.
		DWORD mirrorTick = GetTickCount();
		if ((DWORD)(mirrorTick - lastMirrorTick) >= 2000) {
			XboxInputMirrorLogsToOtherStorage();
			lastMirrorTick = mirrorTick;
		}
		Sleep(250);
	}
}

void XNotifyUI(XNOTIFYQUEUEUI_TYPE Type, PWCHAR String) { XNotifyQueueUI(Type, XUSER_INDEX_ANY, XNOTIFYUI_PRIORITY_DEFAULT, String, 0); }

struct UsbTrb {
	DWORD endpoint;
	DWORD callback;
	DWORD savedEndpoint;
	BYTE  padding[4];
	BYTE  flags;
	BYTE  controllerIndex;   // written by UsbdQueueAsyncTransfer
	BYTE  pad2;
	BYTE  endpointIndex;     // written by UsbdQueueAsyncTransfer
	void* buffer;
	DWORD length;
};

struct UsbPacket {
	BYTE  bmRequestType;
	BYTE  bRequest;
	WORD  wValue;
	WORD  wIndex;
	WORD  wLength;
};

struct UsbControlTrb {
	UsbTrb          trb;          
	BYTE            pad[4];       
	UsbPacket  packet;  
};

struct deviceHandle;
struct __declspec(align(2)) HidControllerExtension
{
	deviceHandle* deviceHandle;
	UsbTrb interruptTrb;
	BYTE interfaceNumber;
	BYTE gap20[3];
	UsbControlTrb controlTrb;
	BYTE gap4C[4];
	DWORD cleanupHandler;
	BYTE gap54[24];
	DWORD queue;
	BYTE alwaysOne;
	BYTE alwaysOneTwo;
	BYTE unknownFlag;
	BYTE alwaysZero;
	BYTE cleanupDone;
	BYTE initTransferPending;
	BYTE alwaysZeroTwo;
	unsigned __int8 deviceType;
	BYTE alwaysZeroThree;
	BYTE alwaysZeroFour;
};

struct deviceHandle {
	HidControllerExtension* driver;
};

typedef struct _XINPUT_CAPABILITIESEX
{
	BYTE                                Type;
	BYTE                                SubType;
	WORD                                Flags;
	XINPUT_GAMEPAD                      Gamepad;
	XINPUT_VIBRATION                    Vibration;
	DWORD unk1;
	DWORD unk2;
	DWORD unk3;
} XINPUT_CAPABILITIES_EX, * PXINPUT_CAPABILITIES_EX;

enum InitState
{
	INIT_SET_CONFIGURATION,
	INIT_GET_REPORT_DESCRIPTOR,
	INIT_DONE,
	INIT_FAILED
};

InitState g_InitState;
#define USB_ENDPOINT_TYPE_CONTROL     0x00
#define USB_ENDPOINT_TYPE_ISOCHRONOUS 0x01
#define USB_ENDPOINT_TYPE_BULK        0x02
#define USB_ENDPOINT_TYPE_INTERRUPT   0x03
#define USB_DIRECTION_IN  1
#define USB_DIRECTION_OUT 0

uint16_t clamp_u16(uint16_t val, uint16_t lo, uint16_t hi) {
	if (val < lo) return lo;
	if (val > hi) return hi;
	return val;
}

// Nintendo specific start
const uint16_t NINTENDO_VENDOR_ID = 0x057E;
const uint16_t SWITCH_PRO_PRODUCT_ID = 0x2009;

const unsigned char nintendo_handshake[2] = { 0x80, 0x02 };
const unsigned char hid_only_mode[2] = { 0x80, 0x04 };

#pragma pack(push, 1)
struct switch_pro_input_report {
	uint8_t  timer;
	uint8_t  battery_conn;   // upper nibble = battery, lower = connection type
	uint8_t  buttons_right;  // Y X B A, R_SR, R_SL, R, ZR
	uint8_t  buttons_mid;    // minus, plus, R_stick, L_stick, home, capture
	uint8_t  buttons_left;   // dpad down/up/right/left, L_SR, L_SL, L, ZL
	uint8_t  left_stick[3];  // 12-bit packed: lx in bits [11:0], ly in bits [23:12]
	uint8_t  right_stick[3]; // same packing for rx, ry
	uint8_t  vibrator;
	uint8_t  imu[48];        // 3 ï¿½ (accel xyz + gyro xyz), each int16_t
};
#pragma pack(pop)

// buttons1
// Face buttons
#define SWITCH_BTN_Y        (1 << 1)
#define SWITCH_BTN_X        (1 << 0)
#define SWITCH_BTN_B        (1 << 2)
#define SWITCH_BTN_A        (1 << 3)

// Right shoulder cluster
#define SWITCH_BTN_R        (1 << 6)
#define SWITCH_BTN_ZR       (1 << 7)

// System buttons
#define SWITCH_BTN_MINUS    (1 << 8)
#define SWITCH_BTN_PLUS     (1 << 9)

// Sticks
#define SWITCH_BTN_R_STICK  (1 << 10)
#define SWITCH_BTN_L_STICK  (1 << 11)

// System
#define SWITCH_BTN_HOME     (1 << 12)
#define SWITCH_BTN_CAPTURE  (1 << 13)

// buttons2

#define SWITCH_DPAD_DOWN    (1 << 0)
#define SWITCH_DPAD_UP      (1 << 1)
#define SWITCH_DPAD_RIGHT   (1 << 2)
#define SWITCH_DPAD_LEFT    (1 << 3)

#define SWITCH_BTN_L        (1 << 6)
#define SWITCH_BTN_ZL       (1 << 7)

static uint16_t STICK_MIN = 500;
static uint16_t STICK_MAX = 3500;
static uint16_t STICK_CENTER = 2000;

int16_t normalize_stick(uint16_t raw) {
	raw = clamp_u16(raw, STICK_MIN, STICK_MAX);
	if (raw >= STICK_CENTER) {
		return (int16_t)((int32_t)(raw - STICK_CENTER) * 32767 / (STICK_CENTER - STICK_MIN));
	}
	else {
		return (int16_t)((int32_t)(STICK_CENTER - raw) * -32768 / (STICK_CENTER - STICK_MIN));
	}
};

bool NeedsNintendoHandshake(uint16_t vid, uint16_t pid) {
	if (vid != NINTENDO_VENDOR_ID) return false;
	return pid == SWITCH_PRO_PRODUCT_ID;
}


// nintendo specific end

// dualshock 3 specific start
const uint16_t SONY_VENDOR_ID = 0x054C;
const uint16_t DS3_PRODUCT_ID = 0x0268;
const unsigned char DS3_HANDSHAKE[4] = { 0x42, 0x0C, 0x00, 0x00 };

bool NeedsDualshock3Handshake(uint16_t vid, uint16_t pid) {
	if (vid != SONY_VENDOR_ID) return false;
	return pid == DS3_PRODUCT_ID;
}

enum DS3_FEATURE_VALUE
{
	Ds3FeatureDeviceAddress = 0x03F2,
	Ds3FeatureStartDevice = 0x03F4,
	Ds3FeatureHostAddress = 0x03F5

};
// dualshock 3 specific end

typedef usb_device_descriptor* (*usb_device_descriptor_func_t)(deviceHandle* handle);
typedef usb_interface_descriptor* (*usb_interface_descriptor_func_t)(deviceHandle* handle);
typedef int(*usb_add_device_complete_func_t)(deviceHandle* handle, int status_code);
typedef int(*usb_get_device_speed_func_t)(deviceHandle* handle);
typedef int(*usb_queue_async_transfer_func_t)(deviceHandle* handle, void* endpoint);
typedef NTSTATUS(*usb_queue_close_endpoint_func_t)(deviceHandle* handle, void* endpoint);
typedef NTSTATUS(*usb_remove_device_complete_func_t)(deviceHandle* handle);
typedef NTSTATUS(*usb_close_default_endpoint_func_t)(deviceHandle* handle, DWORD* endpoint);
typedef NTSTATUS(*usb_open_default_endpoint_func_t)(deviceHandle* handle, DWORD* endpoint);
typedef NTSTATUS(*usb_open_endpoint_func_t)(deviceHandle* handle, int transfertype, int endpointAddress, int maxPacketLength, int interval, DWORD* endpoint);
typedef usb_endpoint_descriptor* (*usb_endpoint_descriptor_func_t)(deviceHandle* handle, int index, int transfertype, int direction);
typedef int(*xam_user_bind_device_callback_func_t)(unsigned int controllerId, unsigned int context, unsigned __int8 category, bool disconnect, unsigned __int8* userIndex);
typedef BOOL(*xam_is_sys_ui_invoked_by_xenon_button_func_t)();
typedef int(*usbd_powerdown_notification_func_t)();
typedef void(*mm_free_physical_memory_func_t)(DWORD type, DWORD address);

usb_device_descriptor_func_t UsbdGetDeviceDescriptor = nullptr;
usb_interface_descriptor_func_t UsbdGetInterfaceDescriptor = nullptr;
usb_endpoint_descriptor_func_t UsbdGetEndpointDescriptor = nullptr;
usb_add_device_complete_func_t UsbdAddDeviceComplete = nullptr;
usb_open_default_endpoint_func_t UsbdOpenDefaultEndpoint = nullptr;
usb_open_endpoint_func_t UsbdOpenEndpoint = nullptr;
usb_get_device_speed_func_t UsbdGetDeviceSpeed = nullptr;
usb_queue_async_transfer_func_t UsbdQueueAsyncTransfer = nullptr;
usb_queue_close_endpoint_func_t UsbdQueueCloseEndpoint = nullptr;
usb_close_default_endpoint_func_t UsbdQueueCloseDefaultEndpoint = nullptr;
usb_remove_device_complete_func_t UsbdRemoveDeviceComplete = nullptr;
xam_user_bind_device_callback_func_t XamUserBindDeviceCallback = nullptr;
xam_is_sys_ui_invoked_by_xenon_button_func_t XamIsSysUiInvokedByXenonButton = nullptr;
usbd_powerdown_notification_func_t UsbdPowerDownNotification = nullptr;
usbd_powerdown_notification_func_t UsbdDriverEntry = nullptr;
mm_free_physical_memory_func_t MmFreePhysicalMemory = nullptr;

enum NINTENDO_HANDSHAKE_STATE {
	INITIAL,
	HANDSHAKE,
	DONE
};
struct Controller {
	deviceHandle* deviceHandle;
	HidControllerExtension* controllerDriver;
	ButtonsReport currentState;
	uint8_t userIndex;
	uint32_t deviceContext;
	uint16_t vendorId;
	uint16_t productId;
	uint32_t packetNumber;
	HID_ReportInfo_t* reportInfo;
	uint8_t reportId;
	void* reportData;
	const HidDeviceMapping* map;

	// for nintendo specific handshake
	NINTENDO_HANDSHAKE_STATE nintendo_handshake_state;
	UsbTrb interruptTrb;
} __declspec(align(4));

struct MappingState {
	volatile bool active;
	volatile uint8_t pressedButtonIdx;
	volatile int16_t axisValues[6];
	uint8_t reportId;
	HID_ReportInfo_t* reportInfo;
	int controllerIndex;
	uint8_t availableButtons[256];
	uint8_t availableButtonCount;
	volatile uint8_t previousPressedButtonIdx;
	volatile uint32_t holdCount;
} __declspec(align(4));

Controller connectedControllers[4];
Controller c;
usb_hid_descriptor hidDescriptorBuffer;
int globalIndex = -1;
void* reportDescriptorBuffer;
MappingState g_mappingState;

int interruptHandler(DWORD deviceHandle, int32_t a2);

// Keep every IN report item; the driver uses all axes, the hat, and buttons.
bool CALLBACK_HIDParser_FilterHIDReportItem(HID_ReportItem_t* const CurrentItem) {
	return (CurrentItem->ItemType == HID_REPORT_ITEM_In);
}

HID_ReportItem_t* FindItemByUsage(
	HID_ReportInfo_t* info,
	uint16_t usagePage,
	uint16_t usage,
	uint8_t  reportId) {
	for (HID_ReportItem_t* item = info->FirstReportItem; item; item = item->Next) {
		if (item->ItemType != HID_REPORT_ITEM_In)
			continue;
		if (item->Attributes.Usage.Page != usagePage)
			continue;
		if (item->Attributes.Usage.Usage != usage)
			continue;
		// When the device uses report IDs, only match the right report.
		if (info->UsingReportIDs && item->ReportID != reportId)
			continue;
		return item;
	}
	return nullptr;
}

HID_ReportItem_t* FindButtonItem(
	HID_ReportInfo_t* info,
	uint8_t buttonIdx,
	uint8_t reportId) {
	return FindItemByUsage(info, HID_USAGE_PAGE_BUTTON, buttonIdx + 1, reportId);
}

// Find the report ID that carries gamepad information
// Returns 0 when the device doesn't use report IDs.
uint8_t FindGamepadReportId(HID_ReportInfo_t* info) {
	if (!info->UsingReportIDs)
		return 0;

	for (HID_ReportItem_t* item = info->FirstReportItem; item; item = item->Next) {
		if (item->ItemType != HID_REPORT_ITEM_In)
			continue;
		if (item->Attributes.Usage.Page != HID_USAGE_PAGE_GENERIC_DESKTOP)
			continue;
		uint16_t u = item->Attributes.Usage.Usage;
		if (u >= HID_USAGE_AXIS_X && u <= HID_USAGE_AXIS_RZ)
			return item->ReportID;
	}
	return 0;
}

// Returns UsbdQueueAsyncTransfer's status. It used to be discarded, which meant a
// control transfer that was never accepted looked identical to one that was accepted
// and never completed - exactly the ambiguity the noclaim run left us in.
int32_t SendControlRequest(
	deviceHandle* deviceHandle,
	UsbControlTrb* controlTrb,
	uint8_t bmRequestType,
	uint8_t bRequest,
	uint16_t wValue,
	uint16_t wIndex,
	uint16_t wLength,
	void* data,
	DWORD completionCallback) {
	controlTrb->packet.bmRequestType = bmRequestType;
	controlTrb->packet.bRequest = bRequest;
	controlTrb->packet.wValue = swap_endianness_16(wValue);
	controlTrb->packet.wIndex = swap_endianness_16(wIndex);
	controlTrb->packet.wLength = swap_endianness_16(wLength);
	controlTrb->trb.buffer = data;
	controlTrb->trb.length = wLength;
	controlTrb->trb.flags = 1;
	controlTrb->trb.callback = completionCallback;
	controlTrb->trb.savedEndpoint = controlTrb->trb.endpoint;
	return UsbdQueueAsyncTransfer(deviceHandle, controlTrb);
}

void SendInterruptRequest(
	deviceHandle* deviceHandle,
	UsbTrb* interruptTrb,
	void* data,
	uint32_t length,
	DWORD completionCallback) {
	interruptTrb->buffer = data;
	interruptTrb->length = length;
	interruptTrb->flags = 1;
	interruptTrb->callback = completionCallback;
	interruptTrb->savedEndpoint = interruptTrb->endpoint;
	UsbdQueueAsyncTransfer(deviceHandle, interruptTrb);
}

int32_t noopCompleteHandler(DWORD deviceHandle, int32_t status) {
	return 0;
}

int32_t setConfigurationComplete(DWORD deviceHandle, int32_t status) {
	HidControllerExtension* controllerDriver = (HidControllerExtension*)((BYTE*)deviceHandle - 36);

	if (status != 0) {
		DbgPrint("EINTIM: Control transfer failed with status %x!\n", status);
		g_InitState = INIT_FAILED;
		return status;
	}

	if (g_InitState == InitState::INIT_SET_CONFIGURATION) {
		// SET_CONFIGURATION just completed, now fetch the report descriptor
		DbgPrint("EINTIM: SET_CONFIGURATION completed. Requesting report descriptor.\n");
		
		// Prepare report descriptor buffer
		hidDescriptorBuffer.wDescriptorLength = swap_endianness_16(hidDescriptorBuffer.wDescriptorLength);
		DbgPrint("EINTIM: Report descriptor length: %d\n", hidDescriptorBuffer.wDescriptorLength);
		
		if (hidDescriptorBuffer.wDescriptorLength == 0) {
			DbgPrint("EINTIM: ERROR - HID descriptor length is 0!\n");
			g_InitState = INIT_FAILED;
			return -1;
		}

		reportDescriptorBuffer = calloc(1, hidDescriptorBuffer.wDescriptorLength);

		g_InitState = InitState::INIT_GET_REPORT_DESCRIPTOR;
		DbgPrint("EINTIM: Fetching report descriptor. Interface: %d, Length: %d\n",
			controllerDriver->interfaceNumber, hidDescriptorBuffer.wDescriptorLength);
		
		SendControlRequest(
			controllerDriver->deviceHandle,
			&controllerDriver->controlTrb,
			0x81,
			0x06,
			0x2200,
			controllerDriver->interfaceNumber,
			hidDescriptorBuffer.wDescriptorLength,
			reportDescriptorBuffer,
			(DWORD)setConfigurationComplete);
	}
	else if (g_InitState == InitState::INIT_GET_REPORT_DESCRIPTOR) {
		// Report descriptor request completed
		DbgPrint("EINTIM: Report descriptor request completed successfully\n");
		g_InitState = InitState::INIT_DONE;

		HID_ReportInfo_t* reportInfo = nullptr;
		uint8_t parseResult = USB_ProcessHIDReport((const uint8_t*)reportDescriptorBuffer,
			hidDescriptorBuffer.wDescriptorLength,
			&reportInfo);

		c.reportInfo = reportInfo;
		c.reportId = FindGamepadReportId(reportInfo);

		DbgPrint("EINTIM: Parsed descriptor. UsingReportIDs: %d, Report ID: %d\r\n",
			(int)reportInfo->UsingReportIDs, c.reportId);

		if (parseResult != HID_PARSE_Successful || !reportInfo) {
			DbgPrint("EINTIM: Failed to parse HID descriptor: error %d\r\n", parseResult);
			g_InitState = InitState::INIT_FAILED;
			free(reportDescriptorBuffer);
			return -1;
		}

		DbgPrint("EINTIM: parse done stage 1\r\n");
		g_InitState = InitState::INIT_DONE;

		c.reportInfo = reportInfo;
		c.reportId = FindGamepadReportId(reportInfo);

		DbgPrint("EINTIM: Parsed descriptor. UsingReportIDs: %d, Report ID: %d\r\n",
			(int)reportInfo->UsingReportIDs, c.reportId);

		free(reportDescriptorBuffer);

		usb_endpoint_descriptor* endpoint_descriptor = UsbdGetEndpointDescriptor(
			controllerDriver->deviceHandle, 0, USB_ENDPOINT_TYPE_INTERRUPT, USB_DIRECTION_IN);

		status = UsbdOpenEndpoint(
			controllerDriver->deviceHandle,
			3,
			endpoint_descriptor->bEndpointAddress,
			swap_endianness_16(endpoint_descriptor->wMaxPacketSize) & 0x7FF,
			endpoint_descriptor->bInterval,
			(DWORD*)&controllerDriver->interruptTrb);

		if (NT_ERROR(status)) {
			DbgPrint("EINTIM: Failed to open interrupt endpoint %x!\n", status);
			return status;
		}

		uint16_t pktSize = swap_endianness_16(endpoint_descriptor->wMaxPacketSize) & 0x7FF;
		c.reportData = malloc(pktSize * 2);
		memset(c.reportData, 0, pktSize * 2);

		controllerDriver->interruptTrb.savedEndpoint = controllerDriver->interruptTrb.endpoint; 
		controllerDriver->interruptTrb.length = pktSize;
		controllerDriver->interruptTrb.callback = (DWORD)interruptHandler;
		controllerDriver->interruptTrb.buffer = c.reportData;

		c.controllerDriver = controllerDriver;

		uint8_t  userIndex = -1;
		uint32_t context = 0x0000000010000005 + globalIndex;
		XamUserBindDeviceCallback(0xa7553952 + globalIndex, context, 0, false, &userIndex);
		c.userIndex = userIndex;
		c.deviceContext = context;
		connectedControllers[globalIndex] = c;

		DbgPrint("EINTIM: Registered virtual controller inside XAM with index: %d.\n", userIndex);

		if (NeedsDualshock3Handshake(c.vendorId, c.productId)) {
			DbgPrint("EINTIM: Sending dualshock3 handshake!\r\n");
			SendControlRequest(controllerDriver->deviceHandle,
				&controllerDriver->controlTrb,
				0x21,
				0x09, 
				Ds3FeatureStartDevice, 
				0, 
				sizeof(DS3_HANDSHAKE), 
				(void*)DS3_HANDSHAKE, 
				(DWORD)noopCompleteHandler);
		}
		return UsbdQueueAsyncTransfer(controllerDriver->deviceHandle, &controllerDriver->interruptTrb);
	}

	return 0;
}

uint8_t NormalizeHat(int32_t v) {
	if (v >= 0 && v <= 7)
		return (uint8_t)v;

	if (v == 0xFF || v > 7)
		return HatSwitch::HAT_NEUTRAL;

	return HatSwitch::HAT_NEUTRAL;
}

HID_ReportItem_t* FindHatItem(HID_ReportInfo_t* info, uint8_t reportId) {
	for (HID_ReportItem_t* item = info->FirstReportItem; item; item = item->Next) {
		if (item->ItemType != HID_REPORT_ITEM_In)
			continue;

		if (item->Attributes.Usage.Page != HID_USAGE_PAGE_GENERIC_DESKTOP)
			continue;

		if (item->Attributes.Usage.Usage != HID_USAGE_HAT_SWITCH)
			continue;

		if (info->UsingReportIDs && item->ReportID != reportId)
			continue;

		int32_t min = item->Attributes.Logical.Minimum;
		int32_t max = item->Attributes.Logical.Maximum;

		if (max - min > 16) // hats are never huge ranges
			continue;

		return item;
	}

	return nullptr;
}

void DiscoverAvailableButtons(HID_ReportInfo_t* info, uint8_t reportId,
                              uint8_t* outButtonIndices, uint8_t* outCount) {
	uint8_t count = 0;
	for (HID_ReportItem_t* item = info->FirstReportItem; item && count < 256; item = item->Next) {
		if (item->ItemType != HID_REPORT_ITEM_In)
			continue;
		if (item->Attributes.Usage.Page != HID_USAGE_PAGE_BUTTON)
			continue;
		if (info->UsingReportIDs && item->ReportID != reportId)
			continue;

		uint16_t usage = item->Attributes.Usage.Usage;
		if (usage >= 1 && usage <= 256) {
			uint8_t buttonIdx = usage - 1;
			outButtonIndices[count++] = buttonIdx;
		}
	}
	*outCount = count;
}

void DiscoverAvailableAxes(HID_ReportInfo_t* info, uint8_t reportId,
                           uint16_t* outAxisUsages, uint8_t* outCount) {
	uint8_t count = 0;
	for (HID_ReportItem_t* item = info->FirstReportItem; item && count < 6; item = item->Next) {
		if (item->ItemType != HID_REPORT_ITEM_In)
			continue;
		if (item->Attributes.Usage.Page != HID_USAGE_PAGE_GENERIC_DESKTOP)
			continue;
		if (info->UsingReportIDs && item->ReportID != reportId)
			continue;

		uint16_t usage = item->Attributes.Usage.Usage;
		if (usage >= HID_USAGE_AXIS_X && usage <= HID_USAGE_AXIS_RZ) {
			outAxisUsages[count++] = usage;
		}
	}
	*outCount = count;
}

void HidFillButtonsReport(
	const uint8_t* payload,
	HID_ReportInfo_t* info,
	ButtonsReport* out,
	uint8_t reportId,
	const HidDeviceMapping* map) {
	// Axes
	const auto* axisMap = map->axisMap;
	uint8_t axisCount = map->axisMapCount;

	for (uint8_t i = 0; i < axisCount; i++) {
		const auto& entry = axisMap[i];

		HID_ReportItem_t* item = FindItemByUsage(
			info,
			HID_USAGE_PAGE_GENERIC_DESKTOP,
			entry.usage,
			reportId
		);

		if (!item || !USB_GetHIDReportItemInfo(reportId, payload, item))
			continue;

		int32_t logMin = (int32_t)item->Attributes.Logical.Minimum;
		int32_t logMax = (int32_t)item->Attributes.Logical.Maximum;
		int32_t raw = (int32_t)item->Value;

		int32_t result = 0;

		if (logMax > logMin) {
			if (raw < logMin) raw = logMin;
			if (raw > logMax) raw = logMax;

			int64_t numerator = (int64_t)(raw - logMin) * 65535;
			int32_t denominator = (logMax - logMin);

			int32_t scaled = (int32_t)((numerator + denominator / 2) / denominator);
			result = scaled - 32768;
		}
		else {
			result = raw;
		}

		// apply inversion
		switch (entry.usage) {
		case HID_USAGE_AXIS_X:
			if (map->invert.invertX) result = -result;
			break;
		case HID_USAGE_AXIS_Y:
			if (map->invert.invertY) result = -result;
			break;
		case HID_USAGE_AXIS_Z:
			if (map->invert.invertZ) result = -result;
			break;
		case HID_USAGE_AXIS_RX:
			if (map->invert.invertRX) result = -result;
			break;
		case HID_USAGE_AXIS_RY:
			if (map->invert.invertRY) result = -result;
			break;
		case HID_USAGE_AXIS_RZ:
			if (map->invert.invertRZ) result = -result;
			break;
		}

		if (result > 32767) result = 32767; if (result < -32768) result = -32768;

		out->*entry.field = (int16_t)result;
	}

	// Hat switch
	HID_ReportItem_t* hatItem = FindHatItem(info, reportId);
	if (hatItem && USB_GetHIDReportItemInfo(reportId, payload, hatItem)) {
		out->has_hat_switch = true;
		out->hatSwitch = NormalizeHat(hatItem->Value);
	} else {
		out->has_hat_switch = false;
	}

	// Buttons
	const auto* buttonMap = map->buttonMap;

	for (uint8_t i = 0; i < map->buttonMapCount; i++) {
		const auto& entry = buttonMap[i];

		HID_ReportItem_t* item = FindButtonItem(info, entry.idx, reportId);
		if (item && USB_GetHIDReportItemInfo(reportId, payload, item)) {
			out->*entry.field = (uint8_t)item->Value;
		}
	}
}

unsigned int __stdcall MappingThreadProc(void* param);
unsigned int __stdcall MappingManagerThreadProc(void* param){
	// This thread monitors for controllers needing mapping and spawns mapping threads
	while (true) {
		for (int i = 0; i < 4; i++) {
			// Check if controller exists, has reportInfo, but no mapping, and mapping not already in progress
			if (connectedControllers[i].controllerDriver &&
				connectedControllers[i].reportInfo &&
				!connectedControllers[i].map &&
				!g_mappingState.active) {

				DbgPrint("EINTIM: Starting mapping for controller %d (VID:%04x PID:%04x)\n",
					i, connectedControllers[i].vendorId, connectedControllers[i].productId);

				// Initialize mapping state
				memset(&g_mappingState, 0, sizeof(MappingState));
				g_mappingState.active = true;
				g_mappingState.reportId = connectedControllers[i].reportId;
				g_mappingState.reportInfo = connectedControllers[i].reportInfo;
				g_mappingState.controllerIndex = i;
				g_mappingState.pressedButtonIdx = 0xFF;

				HANDLE mappingThread = MakeThread((LPTHREAD_START_ROUTINE)MappingThreadProc, &connectedControllers[i]);
				if (mappingThread) {
					CloseHandle(mappingThread);
				} else {
					DbgPrint("EINTIM: Failed to create mapping thread!\n");
					g_mappingState.active = false;
				}
			}
		}
		Sleep(100); 
	}
	return 0;
}

unsigned int __stdcall MappingThreadProc(void* param) {
	XNotifyUI(XNOTIFYUI_CUSTOM, L"Unknown controller connected. Starting mapping process...");
	Controller* controller = (Controller*)param;
	HID_ReportInfo_t* info = controller->reportInfo;
	uint8_t reportId = controller->reportId;
	int controllerIndex = -1;

	for (int i = 0; i < 4; i++) {
		if (&connectedControllers[i] == controller) {
			controllerIndex = i;
			break;
		}
	}

	if (controllerIndex == -1)
		return -1;

	// Discover available buttons and axes
	uint8_t availableButtons[256] = {};
	uint8_t buttonCount = 0;
	DiscoverAvailableButtons(info, reportId, availableButtons, &buttonCount);

	uint16_t availableAxes[6] = {};
	uint8_t axisCount = 0;
	DiscoverAvailableAxes(info, reportId, availableAxes, &axisCount);

	// Store discovered buttons in mapping state
	g_mappingState.availableButtonCount = buttonCount;
	for (uint8_t i = 0; i < buttonCount; i++) {
		g_mappingState.availableButtons[i] = availableButtons[i];
	}

	std::vector<HidButtonMapEntry> mappedButtons;
	std::vector<HidAxisMapEntry> mappedAxes;
	HidAxisInvertFlags inverts = {0};

	// Invert vertical axes by default
	inverts.invertY = true;   // Left stick vertical
	inverts.invertRZ = true;  // Right stick vertical

	// Check for hat switch and analog triggers
	bool hasHatSwitch = FindHatItem(info, reportId) != nullptr;

	// If there are more than 4 axes (4 for dual analog sticks), the extra ones are analog triggers
	bool hasAnalogTriggers = axisCount > 4;

	// Track which HID usages we've mapped during this session
	uint16_t mappedUsages[6] = {};
	uint8_t mappedCount = 0;

	// Map buttons in predefined Xbox order
	const struct {
		uint8_t field_idx;
		const char* xbox_name;
		uint8_t ButtonsReport::* field;
	} xbox_buttons[] = {
		{0, "A", &ButtonsReport::a_button},
		{1, "B", &ButtonsReport::b_button},
		{2, "X", &ButtonsReport::x_button},
		{3, "Y", &ButtonsReport::y_button},
		{4, "LB", &ButtonsReport::l1},
		{5, "RB", &ButtonsReport::r1},
		{6, "Back", &ButtonsReport::back},
		{7, "Start", &ButtonsReport::start},
		{8, "Left Stick Click", &ButtonsReport::l3},
		{9, "Right Stick Click", &ButtonsReport::r3},
		{10, "Xbox", &ButtonsReport::xbox},
		{11, "LT", &ButtonsReport::l2},
		{12, "RT", &ButtonsReport::r2},
	};

	// Skip L2/R2 if we have analog triggers
	uint8_t buttonEndIdx = sizeof(xbox_buttons) / sizeof(xbox_buttons[0]);
	if (hasAnalogTriggers) {
		buttonEndIdx-=2;  // Only map up to RB, skip LT and RT
	}

	for (size_t i = 0; i < buttonEndIdx; i++) {
		if (!g_mappingState.active)
			break;

		static wchar_t msg[256];
		swprintf(msg, 256, L"Press %hs on controller (hold 3s to skip)", xbox_buttons[i].xbox_name);
		XNotifyUI(XNOTIFYUI_CUSTOM, msg);

		uint8_t previousButtonIdx = 0xFF;
		uint8_t foundButtonIdx = 0xFF;
		uint32_t pressWaitCount = 0;
		bool skipped = false;

		while (g_mappingState.active && foundButtonIdx == 0xFF && !skipped) {
			uint8_t currentButtonIdx = g_mappingState.pressedButtonIdx;

			if (previousButtonIdx == 0xFF && currentButtonIdx != 0xFF) {
				// Button just pressed
				pressWaitCount = 0;
				g_mappingState.holdCount = 0;
			} else if (previousButtonIdx != 0xFF && currentButtonIdx == 0xFF) {
				// Button just released
				if (pressWaitCount > 0) {
					foundButtonIdx = previousButtonIdx;
				}
				pressWaitCount = 0;
				g_mappingState.holdCount = 0;
			} else if (currentButtonIdx != 0xFF) {
				pressWaitCount++;
				g_mappingState.holdCount++;
				// 3 second hold = 60 iterations at 50ms each
				if (g_mappingState.holdCount >= 60) {
					skipped = true;
					XNotifyUI(XNOTIFYUI_CUSTOM, L"Mapping skipped");
					// Wait for button release
					while (g_mappingState.active && g_mappingState.pressedButtonIdx != 0xFF) {
						Sleep(50);
					}
				}
			}

			previousButtonIdx = currentButtonIdx;
			Sleep(50);
		}

		if (foundButtonIdx != 0xFF) {
			HidButtonMapEntry entry = {foundButtonIdx, xbox_buttons[i].field};
			mappedButtons.push_back(entry);
		}
	}

	// Map D-Pad buttons if no hat switch
	if (!hasHatSwitch && g_mappingState.active) {
		const struct {
			const char* dpad_name;
			uint8_t ButtonsReport::* field;
		} dpad_buttons[] = {
			{"D-Pad Left", &ButtonsReport::dpad_left},
			{"D-Pad Right", &ButtonsReport::dpad_right},
			{"D-Pad Up", &ButtonsReport::dpad_up},
			{"D-Pad Down", &ButtonsReport::dpad_down},
		};

		for (size_t i = 0; i < sizeof(dpad_buttons) / sizeof(dpad_buttons[0]); i++) {
			wchar_t msg[256];
			swprintf(msg, 256, L"Press %hs on controller (hold 3s to skip)", dpad_buttons[i].dpad_name);
			XNotifyUI(XNOTIFYUI_CUSTOM, msg);

			uint8_t previousButtonIdx = 0xFF;
			uint8_t foundButtonIdx = 0xFF;
			uint32_t pressWaitCount = 0;
			bool skipped = false;

			while (g_mappingState.active && foundButtonIdx == 0xFF && !skipped) {
				uint8_t currentButtonIdx = g_mappingState.pressedButtonIdx;

				if (previousButtonIdx == 0xFF && currentButtonIdx != 0xFF) {
					pressWaitCount = 0;
					g_mappingState.holdCount = 0;
				} else if (previousButtonIdx != 0xFF && currentButtonIdx == 0xFF) {
					if (pressWaitCount > 0) {
						foundButtonIdx = previousButtonIdx;
					}
					pressWaitCount = 0;
					g_mappingState.holdCount = 0;
				} else if (currentButtonIdx != 0xFF) {
					pressWaitCount++;
					g_mappingState.holdCount++;
					if (g_mappingState.holdCount >= 60) {
						skipped = true;
						XNotifyUI(XNOTIFYUI_CUSTOM, L"Mapping skipped");
						// Wait for button release
						while (g_mappingState.active && g_mappingState.pressedButtonIdx != 0xFF) {
							Sleep(50);
						}
					}
				}

				previousButtonIdx = currentButtonIdx;
				Sleep(50);
			}

			if (foundButtonIdx != 0xFF) {
				HidButtonMapEntry entry = {foundButtonIdx, dpad_buttons[i].field};
				mappedButtons.push_back(entry);
			}
		}
	}

	// use static axis mapping for now as for gamepads it should be the same for every gamepad
	HidAxisMapEntry entry;

	entry.usage = HID_USAGE_AXIS_X;
	entry.field = &ButtonsReport::x;
	mappedAxes.push_back(entry);

	entry.usage = HID_USAGE_AXIS_Y;
	entry.field = &ButtonsReport::y;
	mappedAxes.push_back(entry);

	entry.usage = HID_USAGE_AXIS_Z;
	entry.field = &ButtonsReport::z;
	mappedAxes.push_back(entry);

	entry.usage = HID_USAGE_AXIS_RX;
	entry.field = &ButtonsReport::rx;
	mappedAxes.push_back(entry);

	entry.usage = HID_USAGE_AXIS_RY;
	entry.field = &ButtonsReport::ry;
	mappedAxes.push_back(entry);

	entry.usage = HID_USAGE_AXIS_RZ;
	entry.field = &ButtonsReport::rz;
	mappedAxes.push_back(entry);

	// Build dynamic mapping
	std::unique_ptr<DynamicMappingData> dynamicData(new DynamicMappingData());
	dynamicData->axisEntries = mappedAxes;
	dynamicData->buttonEntries = mappedButtons;

	HidDeviceMapping newMapping = {0};
	newMapping.vendorId = controller->vendorId;
	newMapping.productId = controller->productId;
	newMapping.axisMapCount = (uint8_t)mappedAxes.size();
	newMapping.buttonMapCount = (uint8_t)mappedButtons.size();
	newMapping.invert = inverts;

	if (!mappedAxes.empty()) {
		newMapping.axisMap = dynamicData->axisEntries.data();
	}
	if (!mappedButtons.empty()) {
		newMapping.buttonMap = dynamicData->buttonEntries.data();
	}

	// Only save if mapping process wasn't interrupted
	if (!g_mappingState.active) {
		DbgPrint("EINTIM: Mapping was interrupted - not saving incomplete mapping\n");
		memset(&g_mappingState, 0, sizeof(MappingState));
		g_mappingState.pressedButtonIdx = 0xFF;
		return 0;
	}

	// Apply mapping to controller
	g_dynamicData.push_back(std::move(dynamicData));
	g_dynamicData.back()->axisEntries = mappedAxes;
	g_dynamicData.back()->buttonEntries = mappedButtons;

	HidDeviceMapping finalMapping = newMapping;
	finalMapping.axisMap = g_dynamicData.back()->axisEntries.data();
	finalMapping.buttonMap = g_dynamicData.back()->buttonEntries.data();

	g_dynamicMappings.push_back(finalMapping);
	connectedControllers[controllerIndex].map = &g_dynamicMappings.back();

	SaveMappingsToFile("HDD:\\hiddriver.json");

	XNotifyUI(XNOTIFYUI_CUSTOM, L"Mapping complete! Controller ready.");
	g_mappingState.active = false;

	return 0;
}

int interruptHandler(DWORD deviceHandle, int32_t a2) {
	HidControllerExtension* driverExtension = (HidControllerExtension*)((deviceHandle - 4));
	Report* report = (Report*)driverExtension->interruptTrb.buffer;

	if (!driverExtension || !driverExtension->deviceHandle ||
		!driverExtension->deviceHandle->driver ||
		driverExtension->deviceHandle->driver->cleanupDone)
		return 0;

	int index = -1;
	for (int i = 0; i < (sizeof(connectedControllers) / sizeof(Controller)); i++) {
		if (connectedControllers[i].controllerDriver == driverExtension) {
			index = i;
			break;
		}
	}

	if (NeedsNintendoHandshake(connectedControllers[index].vendorId, connectedControllers[index].productId) && connectedControllers[index].nintendo_handshake_state != DONE) {
		if (connectedControllers[index].nintendo_handshake_state == INITIAL) {
			DbgPrint("EINTIM: Gotta do nintendo handshake for this one!\r\n");
			usb_endpoint_descriptor* endpoint_descriptor = UsbdGetEndpointDescriptor(
				driverExtension->deviceHandle, 0,
				USB_ENDPOINT_TYPE_INTERRUPT, USB_DIRECTION_OUT);

			if (!endpoint_descriptor) {
				DbgPrint("EINTIM: Failed to find output descriptor\r\n");
				return -1;
			}

			NTSTATUS status = UsbdOpenEndpoint(
				driverExtension->deviceHandle,
				3,
				endpoint_descriptor->bEndpointAddress,
				swap_endianness_16(endpoint_descriptor->wMaxPacketSize) & 0x7FF,
				endpoint_descriptor->bInterval,
				(DWORD*)&connectedControllers[index].interruptTrb);

			if (NT_ERROR(status)) {
				DbgPrint("EINTIM: Failed to open interrupt OUT endpoint %x!\n", status);
				return status;
			}
			connectedControllers[index].nintendo_handshake_state = HANDSHAKE;
			SendInterruptRequest(driverExtension->deviceHandle, &connectedControllers[index].interruptTrb, (void*)nintendo_handshake, sizeof(nintendo_handshake), (DWORD)noopCompleteHandler);
		}

		else if (connectedControllers[index].nintendo_handshake_state == HANDSHAKE) {
			connectedControllers[index].nintendo_handshake_state = DONE;
			SendInterruptRequest(driverExtension->deviceHandle, &connectedControllers[index].interruptTrb, (void*)hid_only_mode, sizeof(hid_only_mode), (DWORD)noopCompleteHandler);
		}
	}

	bool hasReportId = connectedControllers[index].reportInfo &&
		connectedControllers[index].reportInfo->UsingReportIDs; 
	if (report->reportId == connectedControllers[index].reportId || !hasReportId) {
		ButtonsReport buttonReport;
		memset(&buttonReport, 0, sizeof(ButtonsReport));

		const uint8_t* payload = (const uint8_t*)report;
		
		// Check if correct report ID, skip report ID byte for parsing if present
		if (hasReportId) {
			if (payload[0] != connectedControllers[index].reportId)
				return UsbdQueueAsyncTransfer(driverExtension->deviceHandle,
					&driverExtension->interruptTrb);

			payload++;
		}

		// This special case is needed because:
		// https://gbatemp.net/threads/reverse-engineering-the-switch-pro-controller-wired-mode.475226/
		/*
		"WARNING: The HID descriptor does not match the data in the controller payload at all. My guess is it's just the Bluetooth HID descriptor c/p over. Because of that, if you enable the controller on Windows by poking that enable interrupt packet with your favorite USB tool, Windows will go crazy trying to interpret the packets it gets. I now have this on-screen controller keyboard I don't know how to get rid of."
		*/
		if (NeedsNintendoHandshake(connectedControllers[index].vendorId, connectedControllers[index].productId)) {
			switch_pro_input_report* switch_report = (switch_pro_input_report*)payload;
			
			uint16_t lx = switch_report->left_stick[0] | ((switch_report->left_stick[1] & 0x0F) << 8);
			uint16_t ly = (switch_report->left_stick[1] >> 4) | (switch_report->left_stick[2] << 4);
			uint16_t rx = switch_report->right_stick[0] | ((switch_report->right_stick[1] & 0x0F) << 8);
			uint16_t ry = (switch_report->right_stick[1] >> 4) | (switch_report->right_stick[2] << 4);

			// TODO: read the actual calibration values for normalization
			buttonReport.x = normalize_stick(lx);
			buttonReport.y = normalize_stick(ly);
			buttonReport.z = normalize_stick(rx);
			buttonReport.rz = normalize_stick(ry);

			uint16_t b1 = switch_report->buttons_right | ((uint16_t)switch_report->buttons_mid << 8);
			uint8_t  b2 = switch_report->buttons_left;

			buttonReport.a_button = (b1 & SWITCH_BTN_B) ? 1 : 0;
			buttonReport.b_button = (b1 & SWITCH_BTN_A) ? 1 : 0;
			buttonReport.x_button = (b1 & SWITCH_BTN_X) ? 1 : 0;
			buttonReport.y_button = (b1 & SWITCH_BTN_Y) ? 1 : 0;
			buttonReport.r1 = (b1 & SWITCH_BTN_R) ? 1 : 0;
			buttonReport.l1 = (b2 & SWITCH_BTN_L) ? 1 : 0;
			buttonReport.r2 = (b1 & SWITCH_BTN_ZR) ? 1 : 0;
			buttonReport.l2 = (b2 & SWITCH_BTN_ZL) ? 1 : 0;

			buttonReport.r3 = (b1 & SWITCH_BTN_R_STICK) ? 1 : 0;
			buttonReport.l3 = (b1 & SWITCH_BTN_L_STICK) ? 1 : 0;
			buttonReport.start = (b1 & SWITCH_BTN_PLUS) ? 1 : 0;
			buttonReport.back = (b1 & SWITCH_BTN_MINUS) ? 1 : 0;
			buttonReport.xbox = (b1 & SWITCH_BTN_HOME) ? 1 : 0;

			buttonReport.has_hat_switch = false;
			buttonReport.dpad_up = (b2 & SWITCH_DPAD_UP) ? 1 : 0;
			buttonReport.dpad_down = (b2 & SWITCH_DPAD_DOWN) ? 1 : 0;
			buttonReport.dpad_left = (b2 & SWITCH_DPAD_LEFT) ? 1 : 0;
			buttonReport.dpad_right = (b2 & SWITCH_DPAD_RIGHT) ? 1 : 0;
		}

		else if (connectedControllers[index].map) {
			HidFillButtonsReport(
				payload,
				connectedControllers[index].reportInfo,
				&buttonReport,
				connectedControllers[index].reportId,
				connectedControllers[index].map);
		}
		else if (g_mappingState.active && g_mappingState.controllerIndex == index) {
			// Collect raw button states during mapping - only check discovered buttons
			g_mappingState.pressedButtonIdx = 0xFF;
			uint8_t currentPressCount = 0;

			for (uint8_t i = 0; i < g_mappingState.availableButtonCount; i++) {
				uint8_t buttonIdx = g_mappingState.availableButtons[i];
				HID_ReportItem_t* item = FindButtonItem(connectedControllers[index].reportInfo, buttonIdx, connectedControllers[index].reportId);
				if (item && USB_GetHIDReportItemInfo(connectedControllers[index].reportId, payload, item)) {
					if (item->Value) {
						g_mappingState.pressedButtonIdx = buttonIdx;
						currentPressCount++;
					}
				}
			}

			// Clear if multiple buttons pressed (avoid accidental mappings)
			if (currentPressCount != 1) {
				g_mappingState.pressedButtonIdx = 0xFF;
			}

			// Collect raw axis states
			for (uint8_t i = 0; i < 6; i++) {
				static const uint16_t usages[] = {HID_USAGE_AXIS_X, HID_USAGE_AXIS_Y, HID_USAGE_AXIS_Z,
									  HID_USAGE_AXIS_RX, HID_USAGE_AXIS_RY, HID_USAGE_AXIS_RZ};
				HID_ReportItem_t* item = FindItemByUsage(connectedControllers[index].reportInfo,
					HID_USAGE_PAGE_GENERIC_DESKTOP, usages[i], connectedControllers[index].reportId);
				if (item && USB_GetHIDReportItemInfo(connectedControllers[index].reportId, payload, item)) {
					int32_t logMin = (int32_t)item->Attributes.Logical.Minimum;
					int32_t logMax = (int32_t)item->Attributes.Logical.Maximum;
					int32_t raw = (int32_t)item->Value;

					int16_t result = 0;
					if (logMax > logMin) {
						if (raw < logMin) raw = logMin;
						if (raw > logMax) raw = logMax;
						int64_t numerator = (int64_t)(raw - logMin) * 65535;
						int32_t denominator = (logMax - logMin);
						int32_t scaled = (int32_t)((numerator + denominator / 2) / denominator);
						result = (int16_t)(scaled - 32768);
					} else {
						result = (int16_t)raw;
					}
					g_mappingState.axisValues[i] = result;
				}
			}
		}

		connectedControllers[index].currentState = buttonReport;
	}

	return UsbdQueueAsyncTransfer(driverExtension->deviceHandle, &driverExtension->interruptTrb);
}


int HidRemoveDeviceHook(deviceHandle* deviceHandle2) {
	bool found = false;
	int index = 0;
	for (int i = 0; i < (sizeof(connectedControllers) / sizeof(Controller)); i++) {
		if (connectedControllers[i].deviceHandle == deviceHandle2) {
			found = true;
			index = i;
			break;
		}
	}

	if (!found) {
		DbgPrint("EINTIM: Returning original for handle %p\n", deviceHandle2);
		return HidRemoveDeviceDetour.GetOriginal<decltype(&HidRemoveDeviceHook)>()(deviceHandle2);
	}

	DbgPrint("EINTIM: Removing controller with handle %p\n", deviceHandle2);

	// Check if this controller is currently in mapping process and stop it
	if (g_mappingState.active && g_mappingState.controllerIndex == index) {
		DbgPrint("EINTIM: Stopping mapping process for removed controller %d\n", index);
		g_mappingState.active = false;
		memset(&g_mappingState, 0, sizeof(MappingState));
		g_mappingState.pressedButtonIdx = 0xFF;
	}

	if (!deviceHandle2->driver->cleanupDone) {
		deviceHandle2->driver->cleanupDone = 1;
		connectedControllers[index].controllerDriver = nullptr;

		// Free HID report info for this controller slot
		if (connectedControllers[index].reportInfo) {
			USB_FreeReportInfo(connectedControllers[index].reportInfo);
			connectedControllers[index].reportInfo = nullptr;
		}
		// Clear mapping data
		connectedControllers[index].map = nullptr;
		memset(&connectedControllers[index], 0, sizeof(Controller));
		delete deviceHandle2->driver;
		deviceHandle2->driver = nullptr;
		free(connectedControllers[index].reportData);
		DbgPrint("EINTIM: Removed controller with handle %p\n", deviceHandle2);
		XamUserBindDeviceCallback(0xa7553952 + index, 0x0000000010000005 + index, 0, true, 0);
		DbgPrint("EINTIM: Removed virtual controller from XAM.\n");
		return 0;
	}
}

// ---------------------------------------------------------------------------
// Phase 0.5 "kill test" instrumentation.
// LOGGING ONLY - this must not change behaviour in any way. Its sole purpose is
// to answer: does a vendor-class GIP device reach this hook at all, and if so
// where does the driver drop it?  Remove or gate once the question is settled.
// ---------------------------------------------------------------------------
static const char* KtXferName(int t) {
	switch (t & 3) {
	case USB_ENDPOINT_TYPE_CONTROL:     return "CONTROL";
	case USB_ENDPOINT_TYPE_ISOCHRONOUS: return "ISOCHRONOUS";
	case USB_ENDPOINT_TYPE_BULK:        return "BULK";
	case USB_ENDPOINT_TYPE_INTERRUPT:   return "INTERRUPT";
	}
	return "?";
}

static void KtLogDevice(deviceHandle* handle, usb_device_descriptor* dd, usb_interface_descriptor* id) {
	XBOXINPUT_DBG("XBOXINPUT: ===== DEVICE REACHED HidAddDeviceHook (handle %p) =====\r\n", handle);

	if (dd) {
		XBOXINPUT_DBG("XBOXINPUT: VID=%04X PID=%04X bcdDevice=%04X bcdUSB=%04X\r\n",
			swap_endianness_16(dd->idVendor), swap_endianness_16(dd->idProduct),
			swap_endianness_16(dd->bcdDevice), swap_endianness_16(dd->bcdUSB));
		XBOXINPUT_DBG("XBOXINPUT: dev class=%02X subclass=%02X protocol=%02X maxPacket0=%d numCfg=%d\r\n",
			dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol,
			dd->bMaxPacketSize0, dd->bNumConfigurations);
	}
	else {
		XBOXINPUT_DBG("XBOXINPUT: device descriptor is NULL\r\n");
	}

	if (id) {
		XBOXINPUT_DBG("XBOXINPUT: iface #%d alt=%d numEndpoints=%d class=%02X subclass=%02X protocol=%02X\r\n",
			id->bInterfaceNumber, id->bAlternateSetting, id->bNumEndpoints,
			id->bInterfaceClass, id->bInterfaceSubClass, id->bInterfaceProtocol);
		// GIP signature per docs/GIP protocol notes section 2 (verified from capture).
		if (id->bInterfaceClass == 0xFF && id->bInterfaceSubClass == 0x47 && id->bInterfaceProtocol == 0xD0)
			XBOXINPUT_DBG("XBOXINPUT: *** GIP SIGNATURE FF/47/D0 - THIS IS A GIP DEVICE ***\r\n");
	}
	else {
		XBOXINPUT_DBG("XBOXINPUT: interface descriptor is NULL\r\n");
	}

	// The only endpoint accessor available is indexed by (transfer type, direction).
	// DEFENSIVE: upstream only ever calls this with index 0 and a transfer type the
	// device is known to have. We do not know that it bounds-checks, so:
	//   - cap the index by bNumEndpoints from the interface descriptor
	//   - hard-cap total probes, so a bogus descriptor cannot spin us
	//   - validate the returned struct really is an endpoint descriptor
	//     (bLength == 7, bDescriptorType == 5) before dereferencing anything else
	// A hang inside a USB driver callback takes the whole console down, so this
	// stays conservative even at the cost of missing an exotic endpoint.
	int maxIdx = (id && id->bNumEndpoints > 0 && id->bNumEndpoints <= 8) ? id->bNumEndpoints : 2;
	int probes = 0;
	int found = 0;
	for (int xfer = 0; xfer <= 3 && probes < 40; xfer++) {
		for (int dir = 0; dir <= 1 && probes < 40; dir++) {
			for (int idx = 0; idx < maxIdx && probes < 40; idx++) {
				probes++;
				usb_endpoint_descriptor* ep = UsbdGetEndpointDescriptor(handle, idx, xfer, dir);
				if (!ep)
					continue;
				if (ep->bLength != 7 || ep->bDescriptorType != 5)
					continue;   // not an endpoint descriptor - do not trust the rest
				found++;
				XBOXINPUT_DBG("XBOXINPUT:   EP %02X %s %s maxPacket=%d interval=%d\r\n",
					ep->bEndpointAddress,
					(ep->bEndpointAddress & 0x80) ? "IN" : "OUT",
					KtXferName(ep->bmAttributes),
					swap_endianness_16(ep->wMaxPacketSize) & 0x7FF,
					ep->bInterval);
			}
		}
	}
	XBOXINPUT_DBG("XBOXINPUT: %d endpoint(s) found in %d probes\r\n", found, probes);
	XBOXINPUT_DBG("XBOXINPUT: ======================================================\r\n");
}

// ---------------------------------------------------------------------------
// Phase 0.5b - USB stack probe.
//
// The kill test proved a non-HID device never reaches HidAddDeviceHook, even
// though the console enumerates it fine (flash drive mounts and is visible in
// storage settings). So: which kernel USB exports DO get called for a non-HID
// device, and WHO calls them?
//
// Each probe logs the caller's return address. That address is the thing we
// actually want - it identifies the driver routine handling the device, which
// is a candidate hook point, derived empirically without a kernel dump.
//
// Address-range key for reading the log:
//   0x800xxxxx = kernel   0x816xxxxx-0x817xxxxx = xam   0x81F0xxxx = THIS PLUGIN
// Calls attributed to 0x81F0xxxx are hiddriver360 calling these itself - ignore.
//
// LOGGING ONLY. Every probe calls through to the original.
// ---------------------------------------------------------------------------
Detour UsbdGetDeviceDescriptorDetour;
Detour UsbdGetInterfaceDescriptorDetour;
Detour UsbdGetDeviceSpeedDetour;
Detour UsbdAddDeviceCompleteDetour;
Detour UsbdOpenDefaultEndpointDetour;
Detour UsbdRemoveDeviceCompleteDetour;

// Rate limit: these can be hot, and flooding DbgPrint inside a driver callback
// is itself a hang risk. A handful of calls per function is all we need.
// Generous enough that boot-time enumeration of internal devices cannot exhaust the
// budget before the user plugs anything in, but still bounded. These are cold
// functions (the hot ones - QueueAsyncTransfer, OpenEndpoint - are deliberately
// NOT hooked), so this stays far below anything that could stall a driver callback.
#define PROBE_LIMIT 40
static int g_probeCount[6] = { 0, 0, 0, 0, 0, 0 };

static bool ProbeShouldLog(int slot) {
	if (g_probeCount[slot] > PROBE_LIMIT)
		return false;
	g_probeCount[slot]++;
	// Say so when we stop, rather than going silently quiet - a silent stop reads
	// exactly like "the device never called this", which is the opposite conclusion.
	if (g_probeCount[slot] > PROBE_LIMIT) {
		XBOXINPUT_DBG("XBOXINPUT: PROBE slot %d hit log limit (%d) - further calls NOT logged\r\n",
			slot, PROBE_LIMIT);
		return false;
	}
	return true;
}

static void ProbeLog(int slot, const char* name, void* handle) {
	if (!ProbeShouldLog(slot))
		return;
	XBOXINPUT_DBG("XBOXINPUT: PROBE %-28s handle=%p\r\n", name, handle);
}

usb_device_descriptor* UsbdGetDeviceDescriptorHook(deviceHandle* h) {
	usb_device_descriptor* d =
		UsbdGetDeviceDescriptorDetour.GetOriginal<decltype(&UsbdGetDeviceDescriptorHook)>()(h);
	if (ProbeShouldLog(0)) {
		if (d)
			XBOXINPUT_DBG("XBOXINPUT: PROBE UsbdGetDeviceDescriptor  handle=%p  VID=%04X PID=%04X devclass=%02X\r\n",
				h, swap_endianness_16(d->idVendor), swap_endianness_16(d->idProduct),
				d->bDeviceClass);
		else
			XBOXINPUT_DBG("XBOXINPUT: PROBE UsbdGetDeviceDescriptor  handle=%p  (NULL descriptor)\r\n", h);
	}
	return d;
}

usb_interface_descriptor* UsbdGetInterfaceDescriptorHook(deviceHandle* h) {
	usb_interface_descriptor* d =
		UsbdGetInterfaceDescriptorDetour.GetOriginal<decltype(&UsbdGetInterfaceDescriptorHook)>()(h);
	if (ProbeShouldLog(1)) {
		if (d)
			XBOXINPUT_DBG("XBOXINPUT: PROBE UsbdGetInterfaceDescriptor handle=%p  iface=%d class=%02X/%02X/%02X\r\n",
				h, d->bInterfaceNumber, d->bInterfaceClass,
				d->bInterfaceSubClass, d->bInterfaceProtocol);
		else
			XBOXINPUT_DBG("XBOXINPUT: PROBE UsbdGetInterfaceDescriptor handle=%p  (NULL)\r\n", h);
	}
	return d;
}

int UsbdGetDeviceSpeedHook(deviceHandle* h) {
	ProbeLog(2, "UsbdGetDeviceSpeed", h);
	return UsbdGetDeviceSpeedDetour.GetOriginal<decltype(&UsbdGetDeviceSpeedHook)>()(h);
}

// Supported controller identity - verified from the PC capture, docs/GIP protocol notes section 1,
// and confirmed on-console by probe2 (VID=0E6F PID=0248 class=FF/47/D0).
// NOTE: PID 0x0247 is the controller's BOOTLOADER ("PDP.Xbox.Controller.Bootloader" per
// refs/PlasticBand/Docs/Descriptor Dumps/Xbox One/PDP GIP wired (Bootloader).txt)
// - deliberately NOT matched here.
// Official Microsoft wired Xbox One / Series gamepads supported by the Linux
// xpad driver's upstream device table. Keep this an explicit controller list:
// other Microsoft GIP-class devices include adapters and accessories, which
// must never be claimed as a gamepad.
const uint16_t MICROSOFT_VENDOR_ID = 0x045E;
const uint16_t POWERA_VENDOR_ID = 0x24C6;
const uint16_t POWERA_1414134_PID = 0x543A;

static bool IsPowerA1414134(uint16_t vid, uint16_t pid) {
	return vid == POWERA_VENDOR_ID && pid == POWERA_1414134_PID;
}

static const char* XboxInputKnownControllerName(uint16_t vid, uint16_t pid) {
	const XboxInputControllerProfile* profile =
		XboxInputFindProfileById(vid, pid, 0);
	return profile ? profile->name : "Unknown";
}
static bool IsSupportedMicrosoftGamepadPid(uint16_t pid) {
	return XboxInputFindProfileById(MICROSOFT_VENDOR_ID, pid, 0) != 0;
}

static XboxInputUsbInterfaceIdentity XboxInputInterfaceIdentity(
	const usb_interface_descriptor* descriptor) {
	XboxInputUsbInterfaceIdentity identity = { 0 };
	if (!descriptor)
		return identity;
	identity.number = descriptor->bInterfaceNumber;
	identity.alternateSetting = descriptor->bAlternateSetting;
	identity.endpointCount = descriptor->bNumEndpoints;
	identity.interfaceClass = descriptor->bInterfaceClass;
	identity.interfaceSubClass = descriptor->bInterfaceSubClass;
	identity.interfaceProtocol = descriptor->bInterfaceProtocol;
	return identity;
}

// Budget claim attempts across a connection bounce. It is refilled only after a
// controller reaches a valid ready/input state, never during teardown.
//
// Reusing one static extension across disconnect/reconnect was dangerous because an
// asynchronous completion from the old device could arrive after the same TRB had
// been queued for the new one. Claims now receive a fresh fixed session slot and
// teardown retires it for the rest of the boot. The attempt cap remains a second
// boundary against devices that repeatedly bounce during enumeration.
#define GIP_CLAIM_MAX_ATTEMPTS 3
static int g_gipClaimAttempts = 0;
#define GIP_READ_BUF_SIZE 64
#define GIP_TX_BUFS 12
// PowerA 24C6:543A needs a host-initiated POWER packet before ANNOUNCE and a
// short, ordered post-IDENTIFY sequence before it starts reporting input.
// Keep this entirely separate from normal rumble so every init transfer owns
// the OUT TRB until its completion callback fires.
enum PowerAInitStage {
	POWERA_INIT_IDLE = 0,
	POWERA_INIT_EARLY_POWER,
	POWERA_INIT_WAIT_IDENTIFY,
	POWERA_INIT_LED,
	POWERA_INIT_AUTH_DONE,
	POWERA_INIT_RUMBLE_START,
	POWERA_INIT_RUMBLE_STOP,
	POWERA_INIT_COMPLETE,
};
static uint32_t g_gipChunkTotal = 0;

// ---- unified controller-session ownership ----------------------------------
//
// USB completion callbacks recover HidControllerExtension from the address of a
// field inside it (interruptTrb is at +4 and controlTrb at +36).  A multi-pad
// implementation must therefore give every claimed device a permanently distinct
// extension, transfer requests and buffers; sharing the old globals across two
// devices can re-queue a TRB still owned by the USB stack.
//
// Slots are fixed rather than heap allocated because claim and completion callbacks
// can run at an IRQL where allocation is unsafe. The primary slot is permanent and
// owns the same extension, TRBs and buffers as every additional controller. Keeping
// it separate from the retired-slot pool preserves the proven rule that a late USB
// completion can never land in storage already reused by a different device.
#define GIP_MAX_SESSIONS 32
#define GIP_MAX_ADDITIONAL_ACTIVE 3
struct GipSessionSlot {
	bool                    reserved;
	bool                    primary;
	XboxInputControllerRuntime runtime;
	HidControllerExtension  ext;
	UsbTrb                  outTrb;
	BYTE                    earlyPowerBuf[5];
	BYTE                    readBuf[GIP_READ_BUF_SIZE];
	BYTE                    outBuf[GIP_TX_BUFS][64];
	BYTE                    rumbleBuf[64];
	int                     outBufIndex;
	bool                    outOpen;
	volatile LONG           rumbleInFlight;
	volatile LONG           rumblePending;
	BYTE                    rumbleRequestedLeft;
	BYTE                    rumbleRequestedRight;
	BYTE                    rumbleLastLeft;
	BYTE                    rumbleLastRight;
	bool                    rumbleHaveLast;
	uint8_t                 sequence;
	bool                    identifySent;
	bool                    identifyReplySeen;
	DWORD                   lastIdentifyTick;
	bool                    poweredOn;
	int                     packetsSeen;
	int                     inputsSeen;
	int                     readErrors;
	bool                    readLoopStopped;
	uint16_t                vendorId;
	uint16_t                productId;
	volatile LONG           earlyPowerBusy;
	deviceHandle*           earlyPowerHandle;
	uint16_t                earlyPowerPacketSize;
	bool                    guideOverlayOpen;
	volatile LONG           powerAInitStage;
	volatile LONG           powerAIdentifyComplete;
	BYTE                    powerAInitBuf[64];
	volatile LONG           preReadInitStage;
	BYTE                    preReadInitBuf[16];
	volatile LONG           readBeforeInitPending;
	bool                    deferredReadValid;
	DWORD                   deferredReadLength;
	BYTE                    deferredReadBuf[GIP_READ_BUF_SIZE];
};
static GipSessionSlot g_gipSessions[GIP_MAX_SESSIONS];
static GipSessionSlot* g_gipPrimarySession = &g_gipSessions[0];

// Primary protocol code remains byte-for-byte familiar while its storage now has
// one owner. These aliases are transitional names, not separate state. Callbacks
// that recover the extension/TRB address therefore resolve to this permanent slot.
#define g_gipExt                    (g_gipPrimarySession->ext)
#define g_gipRuntime                (g_gipPrimarySession->runtime)
#define g_gipVendorId               (g_gipPrimarySession->vendorId)
#define g_gipProductId              (g_gipPrimarySession->productId)
#define g_gipReadBuf                (g_gipPrimarySession->readBuf)
#define g_gipPacketsSeen            (g_gipPrimarySession->packetsSeen)
#define g_gipInputsSeen             (g_gipPrimarySession->inputsSeen)
#define g_gipGuideOverlayOpen       (g_gipPrimarySession->guideOverlayOpen)
#define g_gipOutTrb                 (g_gipPrimarySession->outTrb)
#define g_gipOutBuf                 (g_gipPrimarySession->outBuf)
#define g_gipOutBufIdx              (g_gipPrimarySession->outBufIndex)
#define g_gipOutOpen                (g_gipPrimarySession->outOpen)
#define g_gipEarlyPowerBuf          (g_gipPrimarySession->earlyPowerBuf)
#define g_gipEarlyPowerBusy         (g_gipPrimarySession->earlyPowerBusy)
#define g_gipEarlyPowerHandle       (g_gipPrimarySession->earlyPowerHandle)
#define g_gipEarlyPowerPacketSize   (g_gipPrimarySession->earlyPowerPacketSize)
#define g_gipRumbleBuf              (g_gipPrimarySession->rumbleBuf)
#define g_gipRumbleInFlight         (g_gipPrimarySession->rumbleInFlight)
#define g_gipRumblePending          (g_gipPrimarySession->rumblePending)
#define g_gipRumbleRequestedLeft    (g_gipPrimarySession->rumbleRequestedLeft)
#define g_gipRumbleRequestedRight   (g_gipPrimarySession->rumbleRequestedRight)
#define g_gipRumbleLastLeft         (g_gipPrimarySession->rumbleLastLeft)
#define g_gipRumbleLastRight        (g_gipPrimarySession->rumbleLastRight)
#define g_gipRumbleHaveLast         (g_gipPrimarySession->rumbleHaveLast)
#define g_gipSeq                    (g_gipPrimarySession->sequence)
#define g_gipIdentifySent           (g_gipPrimarySession->identifySent)
#define g_gipIdentifyReplySeen      (g_gipPrimarySession->identifyReplySeen)
#define g_gipLastIdentifyTick       (g_gipPrimarySession->lastIdentifyTick)
#define g_gipPoweredOn              (g_gipPrimarySession->poweredOn)
#define g_powerAInitStage           (g_gipPrimarySession->powerAInitStage)
#define g_powerAIdentifyComplete    (g_gipPrimarySession->powerAIdentifyComplete)
#define g_powerAInitBuf             (g_gipPrimarySession->powerAInitBuf)

static GipSessionSlot* GipSessionFromExtension(HidControllerExtension* ext) {
	for (int i = 0; i < GIP_MAX_SESSIONS; ++i) {
		if (&g_gipSessions[i].ext == ext)
			return &g_gipSessions[i];
	}
	return 0;
}

static GipSessionSlot* GipFindFreeSession() {
	for (int i = 0; i < GIP_MAX_SESSIONS; ++i) {
		if (!g_gipSessions[i].reserved)
			return &g_gipSessions[i];
	}
	return 0;
}

static GipSessionSlot* GipReservePrimarySession(
	const XboxInputControllerProfile* profile, deviceHandle* handle,
	BYTE interfaceNumber, uint16_t vendorId, uint16_t productId) {
	GipSessionSlot* session = GipFindFreeSession();
	if (!session || !profile || !handle)
		return 0;
	memset(session, 0, sizeof(*session));
	session->reserved = true;
	session->primary = true;
	session->sequence = 1;
	session->earlyPowerPacketSize = GIP_READ_BUF_SIZE;
	session->earlyPowerBuf[0] = GIP_CMD_POWER;
	session->earlyPowerBuf[1] = GIP_OPT_INTERNAL;
	session->earlyPowerBuf[2] = 0;
	session->earlyPowerBuf[3] = 1;
	session->earlyPowerBuf[4] = 0;
	session->vendorId = vendorId;
	session->productId = productId;
	session->ext.deviceHandle = handle;
	session->ext.interfaceNumber = interfaceNumber;
	session->ext.deviceType = 0;
	session->ext.interruptTrb.flags = 1;
	XboxInputInitializeRuntime(&session->runtime, profile);
	session->runtime.lifecycle = XBOXINPUT_SESSION_INITIALIZING;
	g_gipPrimarySession = session;
	return session;
}

static int GipActiveSessionCount() {
	int count = 0;
	for (int i = 0; i < GIP_MAX_SESSIONS; ++i)
		if (g_gipSessions[i].reserved && !g_gipSessions[i].primary &&
			g_gipSessions[i].ext.deviceHandle)
			++count;
	return count;
}

enum GipControllerKind {
	GIP_CONTROLLER_NONE = 0,
	GIP_CONTROLLER_PRIMARY,
	GIP_CONTROLLER_ADDITIONAL,
};

struct GipControllerRef {
	GipControllerKind kind;
	GipSessionSlot* session;
};

static bool GipControllerFromUser(uint8_t user, GipControllerRef* result) {
	if (!result)
		return false;
	result->kind = GIP_CONTROLLER_NONE;
	result->session = 0;
	if (XboxInputRuntimeIsReady(&g_gipRuntime) &&
		g_gipRuntime.playerIndex == user) {
		result->kind = GIP_CONTROLLER_PRIMARY;
		result->session = g_gipPrimarySession;
		return true;
	}
	for (int i = 0; i < GIP_MAX_SESSIONS; ++i) {
		if (g_gipSessions[i].reserved && !g_gipSessions[i].primary &&
			XboxInputRuntimeIsReady(&g_gipSessions[i].runtime) &&
			g_gipSessions[i].runtime.playerIndex == user) {
			result->kind = GIP_CONTROLLER_ADDITIONAL;
			result->session = &g_gipSessions[i];
			return true;
		}
	}
	return false;
}

static bool GipControllerFromContext(uint32_t context, GipControllerRef* result) {
	if (!result)
		return false;
	result->kind = GIP_CONTROLLER_NONE;
	result->session = 0;
	if (XboxInputRuntimeIsReady(&g_gipRuntime) &&
		g_gipRuntime.deviceContext == context) {
		result->kind = GIP_CONTROLLER_PRIMARY;
		result->session = g_gipPrimarySession;
		return true;
	}
	for (int i = 0; i < GIP_MAX_SESSIONS; ++i) {
		if (g_gipSessions[i].reserved && !g_gipSessions[i].primary &&
			XboxInputRuntimeIsReady(&g_gipSessions[i].runtime) &&
			g_gipSessions[i].runtime.deviceContext == context) {
			result->kind = GIP_CONTROLLER_ADDITIONAL;
			result->session = &g_gipSessions[i];
			return true;
		}
	}
	return false;
}

static const XboxInputControllerProfile* GipControllerProfile(
	const GipControllerRef* controller) {
	if (!controller)
		return 0;
	return controller->session ? controller->session->runtime.profile : 0;
}

// Sequence is adapter-global and never zero - refs/xone/bus/protocol.c:335-337.
static uint8_t GipNextSeq() {
	uint8_t s = g_gipSeq++;
	if (g_gipSeq == 0)
		g_gipSeq = 1;
	return s;
}

static bool IsSupportedGipGamepad(uint16_t vid, uint16_t pid) {
	return XboxInputFindProfileById(vid, pid, 0) != 0;
}

static int GipQueueGamepadRumble(deviceHandle* h, BYTE leftMotor, BYTE rightMotor);

// Completion callbacks execute in the USB stack.  Do not log or allocate here;
// either release the single-flight gate or immediately submit the newest cached
// state.  A new XAM caller that wins the gate first simply owns the next transfer.
static int32_t GipGamepadRumbleComplete(DWORD trbAddr, int32_t status) {
	// A retired session may complete after a reconnect. It owns different TRB
	// storage and must never release the new session's single-flight gate.
	if (trbAddr != (DWORD)&g_gipOutTrb)
		return status;
	InterlockedExchange(&g_gipRumbleInFlight, 0);
	if (!g_gipOutOpen || !g_gipExt.deviceHandle)
		return status;
	if (InterlockedExchange(&g_gipRumblePending, 0) == 0)
		return status;
	if (InterlockedCompareExchange(&g_gipRumbleInFlight, 1, 0) != 0)
		return status;
	int queued = GipQueueGamepadRumble(g_gipExt.deviceHandle,
		g_gipRumbleRequestedLeft, g_gipRumbleRequestedRight);
	if (queued != 0)
		InterlockedExchange(&g_gipRumbleInFlight, 0);
	return status;
}

static int GipQueueGamepadRumble(deviceHandle* h, BYTE leftMotor, BYTE rightMotor) {
	if (!g_gipOutOpen || !h)
		return -1;
	BYTE payload[9];
	XboxInputBuildGipRumblePayload(leftMotor, rightMotor, payload);
	int i = 0;
	g_gipRumbleBuf[i++] = GIP_CMD_RUMBLE;
	g_gipRumbleBuf[i++] = 0x00;
	g_gipRumbleBuf[i++] = GipNextSeq();
	g_gipRumbleBuf[i++] = (BYTE)sizeof(payload);
	memcpy(g_gipRumbleBuf + i, payload, sizeof(payload));
	i += sizeof(payload);
	g_gipOutTrb.buffer = g_gipRumbleBuf;
	g_gipOutTrb.length = i;
	g_gipOutTrb.flags = 1;
	g_gipOutTrb.callback = (DWORD)GipGamepadRumbleComplete;
	g_gipOutTrb.savedEndpoint = g_gipOutTrb.endpoint;
	return UsbdQueueAsyncTransfer(h, &g_gipOutTrb);
}

//
// Build and send one GIP packet on the interrupt OUT endpoint.
// All packets we send have payloads well under 128 bytes, so the length varint is a
// single byte and the header is 4 bytes (already even, no padding needed).
//
static int GipSendSeq(deviceHandle* h, uint8_t cmd, uint8_t options, uint8_t seq,
                      const BYTE* payload, int payloadLen) {
	if (!g_gipOutOpen || !h || payloadLen < 0 || payloadLen > 60)
		return -1;

	BYTE* buf = g_gipOutBuf[g_gipOutBufIdx];
	g_gipOutBufIdx = (g_gipOutBufIdx + 1) % GIP_TX_BUFS;

	int i = 0;
	buf[i++] = cmd;
	buf[i++] = options;
	buf[i++] = seq;
	buf[i++] = (BYTE)payloadLen;
	if (payload && payloadLen > 0) {
		memcpy(buf + i, payload, payloadLen);
		i += payloadLen;
	}

	SendInterruptRequest(h, &g_gipOutTrb, buf, i, (DWORD)noopCompleteHandler);
	return 0;
}

// Normal single packets allocate a fresh sequence.
static int GipSend(deviceHandle* h, uint8_t cmd, uint8_t options,
                   const BYTE* payload, int payloadLen) {
	return GipSendSeq(h, cmd, options, GipNextSeq(), payload, payloadLen);
}

// Direct Motor Command is a normal GIP command with flags 0x00, an incrementing
// sequence and a 9-byte payload. The second payload byte enables both grip
// motors; the next two are the trigger motors, which have no 360 equivalent.
static int GipSendGamepadRumble(deviceHandle* h, BYTE leftMotor, BYTE rightMotor) {
	if (!h || !g_gipOutOpen)
		return -1;
	if (g_gipRumbleHaveLast && leftMotor == g_gipRumbleLastLeft &&
		rightMotor == g_gipRumbleLastRight)
		return 0;
	g_gipRumbleHaveLast = true;
	g_gipRumbleLastLeft = leftMotor;
	g_gipRumbleLastRight = rightMotor;
	g_gipRumbleRequestedLeft = leftMotor;
	g_gipRumbleRequestedRight = rightMotor;
	XboxInputQueueLogEvent(XBOXINPUT_LOG_RUMBLE,
		0xFF, // primary slot is recorded by controller_ready; avoid hot-path lookup here
		((DWORD)leftMotor << 8) | rightMotor);
	if (InterlockedCompareExchange(&g_gipRumbleInFlight, 1, 0) != 0) {
		InterlockedExchange(&g_gipRumblePending, 1);
		return 0;
	}
	int queued = GipQueueGamepadRumble(h, leftMotor, rightMotor);
	if (queued != 0)
		InterlockedExchange(&g_gipRumbleInFlight, 0);
	return queued;
}

// Session-local outbound path used by the multi-controller implementation.  It
// intentionally has no shared sequence counter, transfer request or buffer.
static uint8_t GipSessionNextSeq(GipSessionSlot* session) {
	uint8_t seq = session->sequence++;
	if (session->sequence == 0)
		session->sequence = 1;
	return seq;
}

static int GipSessionSend(GipSessionSlot* session, uint8_t cmd, uint8_t options,
	const BYTE* payload, int payloadLen) {
	if (!session || !session->outOpen || !session->ext.deviceHandle ||
		payloadLen < 0 || payloadLen > 60)
		return -1;
	BYTE* buf = session->outBuf[session->outBufIndex];
	session->outBufIndex = (session->outBufIndex + 1) % GIP_TX_BUFS;
	int i = 0;
	buf[i++] = cmd;
	buf[i++] = options;
	buf[i++] = GipSessionNextSeq(session);
	buf[i++] = (BYTE)payloadLen;
	if (payload && payloadLen) {
		memcpy(buf + i, payload, payloadLen);
		i += payloadLen;
	}
	SendInterruptRequest(session->ext.deviceHandle, &session->outTrb, buf, i,
		(DWORD)noopCompleteHandler);
	return 0;
}

static int GipSessionSendSeq(GipSessionSlot* session, uint8_t cmd, uint8_t options,
	uint8_t seq, const BYTE* payload, int payloadLen) {
	if (!session || !session->outOpen || !session->ext.deviceHandle ||
		payloadLen < 0 || payloadLen > 60)
		return -1;
	BYTE* buf = session->outBuf[session->outBufIndex];
	session->outBufIndex = (session->outBufIndex + 1) % GIP_TX_BUFS;
	int i = 0;
	buf[i++] = cmd; buf[i++] = options; buf[i++] = seq; buf[i++] = (BYTE)payloadLen;
	if (payload && payloadLen) { memcpy(buf + i, payload, payloadLen); i += payloadLen; }
	SendInterruptRequest(session->ext.deviceHandle, &session->outTrb, buf, i,
		(DWORD)noopCompleteHandler);
	return 0;
}

static int GipSessionQueueRumble(GipSessionSlot* session, BYTE leftMotor, BYTE rightMotor);

static int32_t GipSessionRumbleComplete(DWORD trbAddr, int32_t status) {
	GipSessionSlot* session = 0;
	for (int i = 0; i < GIP_MAX_SESSIONS; ++i) {
		if ((DWORD)&g_gipSessions[i].outTrb == trbAddr) {
			session = &g_gipSessions[i];
			break;
		}
	}
	if (!session)
		return status;
	InterlockedExchange(&session->rumbleInFlight, 0);
	if (!session->reserved || !session->outOpen || !session->ext.deviceHandle)
		return status;
	if (InterlockedExchange(&session->rumblePending, 0) == 0)
		return status;
	if (InterlockedCompareExchange(&session->rumbleInFlight, 1, 0) != 0)
		return status;
	int queued = GipSessionQueueRumble(session,
		session->rumbleRequestedLeft, session->rumbleRequestedRight);
	if (queued != 0)
		InterlockedExchange(&session->rumbleInFlight, 0);
	return status;
}

static int GipSessionQueueRumble(GipSessionSlot* session, BYTE leftMotor, BYTE rightMotor) {
	if (!session || !session->outOpen || !session->ext.deviceHandle)
		return -1;
	BYTE payload[9];
	XboxInputBuildGipRumblePayload(leftMotor, rightMotor, payload);
	int i = 0;
	session->rumbleBuf[i++] = GIP_CMD_RUMBLE;
	session->rumbleBuf[i++] = 0x00;
	session->rumbleBuf[i++] = GipSessionNextSeq(session);
	session->rumbleBuf[i++] = (BYTE)sizeof(payload);
	memcpy(session->rumbleBuf + i, payload, sizeof(payload));
	i += sizeof(payload);
	session->outTrb.buffer = session->rumbleBuf;
	session->outTrb.length = i;
	session->outTrb.flags = 1;
	session->outTrb.callback = (DWORD)GipSessionRumbleComplete;
	session->outTrb.savedEndpoint = session->outTrb.endpoint;
	return UsbdQueueAsyncTransfer(session->ext.deviceHandle, &session->outTrb);
}

static int GipSessionSendRumble(GipSessionSlot* session, BYTE leftMotor, BYTE rightMotor) {
	if (!session || !session->outOpen || !session->ext.deviceHandle)
		return -1;
	if (session->rumbleHaveLast && leftMotor == session->rumbleLastLeft &&
		rightMotor == session->rumbleLastRight)
		return 0;
	session->rumbleHaveLast = true;
	session->rumbleLastLeft = leftMotor;
	session->rumbleLastRight = rightMotor;
	session->rumbleRequestedLeft = leftMotor;
	session->rumbleRequestedRight = rightMotor;
	if (InterlockedCompareExchange(&session->rumbleInFlight, 1, 0) != 0) {
		InterlockedExchange(&session->rumblePending, 1);
		return 0;
	}
	int queued = GipSessionQueueRumble(session, leftMotor, rightMotor);
	if (queued != 0)
		InterlockedExchange(&session->rumbleInFlight, 0);
	return queued;
}

static void GipRegisterWithXam();
static int GipStartPostIdentifyStartup(GipSessionSlot* session);
static void GipFillGamepadCaps(BYTE& type, BYTE& subType, WORD& flags, XINPUT_GAMEPAD& pad);
static void GipUnregisterFromXam();

static int GipPowerAQueueStage(LONG stage);

static int32_t GipPowerAInitComplete(DWORD trbAddr, int32_t status) {
	if (trbAddr != (DWORD)&g_gipOutTrb)
		return status;
	LONG completed = g_powerAInitStage;
	XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP,
		XBOXINPUT_USB_POWERA_STAGE + completed, status);
	if (status != 0 || !g_gipOutOpen || !g_gipExt.deviceHandle) {
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_FAILURE, 70 + completed, status);
		return status;
	}

	if (completed == POWERA_INIT_EARLY_POWER) {
		InterlockedExchange(&g_powerAInitStage, POWERA_INIT_WAIT_IDENTIFY);
		// A very fast controller may finish IDENTIFY while POWER is completing.
		if (InterlockedCompareExchange(&g_powerAIdentifyComplete, 0, 0) != 0)
			return GipPowerAQueueStage(POWERA_INIT_LED);
		return status;
	}

	if (completed >= POWERA_INIT_LED && completed < POWERA_INIT_RUMBLE_STOP)
		return GipPowerAQueueStage(completed + 1);

	if (completed == POWERA_INIT_RUMBLE_STOP) {
		InterlockedExchange(&g_powerAInitStage, POWERA_INIT_COMPLETE);
		if (!XboxInputRuntimeIsReady(&g_gipRuntime)) {
			XboxInputRuntimeSetReady(&g_gipRuntime, true);
			g_gipClaimAttempts = 0;
			GipRegisterWithXam();
		}
	}
	return status;
}

static int GipPowerAQueueStage(LONG stage) {
	if (!g_gipOutOpen || !g_gipExt.deviceHandle || !IsPowerA1414134(g_gipVendorId, g_gipProductId))
		return -1;

	BYTE command = 0;
	BYTE options = 0;
	const BYTE* payload = 0;
	int payloadLen = 0;
	static const BYTE powerOn[1] = { 0x00 };
	static const BYTE ledOn[3] = { 0x00, 0x01, 0x14 };
	static const BYTE authDone[2] = { 0x01, 0x00 };
	static const BYTE rumbleStart[9] = {
		0x00, 0x0F, 0x00, 0x00, 0x1D, 0x1D, 0xFF, 0x00, 0x00
	};
	static const BYTE rumbleStop[9] = {
		0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	switch (stage) {
	case POWERA_INIT_EARLY_POWER:
		command = GIP_CMD_POWER; options = GIP_OPT_INTERNAL;
		payload = powerOn; payloadLen = sizeof(powerOn); break;
	case POWERA_INIT_LED:
		command = GIP_CMD_LED; options = GIP_OPT_INTERNAL;
		payload = ledOn; payloadLen = sizeof(ledOn); break;
	case POWERA_INIT_AUTH_DONE:
		command = GIP_CMD_AUTHENTICATE; options = GIP_OPT_INTERNAL;
		payload = authDone; payloadLen = sizeof(authDone); break;
	case POWERA_INIT_RUMBLE_START:
		command = GIP_CMD_RUMBLE; options = 0;
		payload = rumbleStart; payloadLen = sizeof(rumbleStart); break;
	case POWERA_INIT_RUMBLE_STOP:
		command = GIP_CMD_RUMBLE; options = 0;
		payload = rumbleStop; payloadLen = sizeof(rumbleStop); break;
	default:
		return -1;
	}

	int length = 0;
	g_powerAInitBuf[length++] = command;
	g_powerAInitBuf[length++] = options;
	g_powerAInitBuf[length++] = GipNextSeq();
	g_powerAInitBuf[length++] = (BYTE)payloadLen;
	memcpy(g_powerAInitBuf + length, payload, payloadLen);
	length += payloadLen;
	InterlockedExchange(&g_powerAInitStage, stage);
	XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP,
		XBOXINPUT_USB_POWERA_STAGE + stage, 0x80000000 | (DWORD)command);
	SendInterruptRequest(g_gipExt.deviceHandle, &g_gipOutTrb,
		g_powerAInitBuf, length, (DWORD)GipPowerAInitComplete);
	return 0;
}

//
// ---- XAM virtual controller -------------------------------------------------
// Registration mirrors what hiddriver360 does for a claimed HID controller
// (main.cpp:498-505): bind a device callback with a magic context, keep the user
// index XAM hands back, and answer the capability/state hooks for it.
//
// We deliberately do NOT reuse hiddriver360's connectedControllers[] slot machinery.
// Its hooks read a ButtonsReport built by the HID parser, which has nothing to do with
// our GIP state. Keeping a separate identity means our branches can run first and the
// upstream paths are left completely untouched for real HID pads.
//
static int      g_gipCapsLogged = 0;
static int      g_gipCaps2Logged = 0;
extern void* XamGetCurrentTitleIdPtr;


static void GipFillGamepadCaps(BYTE& type, BYTE& subType, WORD& flags, XINPUT_GAMEPAD& pad) {
	type    = XINPUT_DEVTYPE_GAMEPAD;
	subType = XINPUT_DEVSUBTYPE_GAMEPAD;
	flags   = 0;

	memset(&pad, 0, sizeof(pad));
	pad.wButtons = 0xF3FF;
	pad.bLeftTrigger = 0xFF;
	pad.bRightTrigger = 0xFF;
	pad.sThumbLX = pad.sThumbLY = pad.sThumbRX = pad.sThumbRY = (SHORT)0xFFC0;
}

static void GipRegisterWithXam() {
	if (g_gipRuntime.playerIndex != 0xFF)
		return;                       // already registered

	// Pick a slot not used by an existing controller.
	int idx = -1;
	for (int i = 0; i < (int)(sizeof(connectedControllers) / sizeof(Controller)); i++) {
		if (connectedControllers[i].controllerDriver)
			continue;
		const uint32_t context = 0x0000000010000005 + i;
		bool used = false;
		for (int j = 0; j < GIP_MAX_SESSIONS; ++j) {
			if (g_gipSessions[j].reserved &&
				XboxInputRuntimeIsReady(&g_gipSessions[j].runtime) &&
				g_gipSessions[j].runtime.deviceContext == context) {
				used = true;
				break;
			}
		}
		if (!used) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		XBOXINPUT_LOG("XBOXINPUT: no free XAM slot!\r\n");
		return;
	}

	uint8_t userIndex = 0xFF;
	uint32_t context = 0x0000000010000005 + idx;
	XamUserBindDeviceCallback(0xa7553952 + idx, context, 0, false, &userIndex);

	g_gipRuntime.playerIndex = userIndex;
	g_gipRuntime.deviceContext = context;
	XboxInputRuntimeSetReady(&g_gipRuntime, userIndex != 0xFF);
	XboxInputCancelRemovedControllerNotification(userIndex);
	XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_XAM_REGISTER,
		((DWORD)userIndex << 24) | (context & 0x00FFFFFF));
	XboxInputQueueLogEvent(XBOXINPUT_LOG_CONTROLLER_READY, userIndex, context);
	XBOXINPUT_LOG("XBOXINPUT: registered virtual GAMEPAD in XAM, user index %d\r\n",
		userIndex);
}

static void GipUnregisterFromXam() {
	XboxInputRuntimeSetReady(&g_gipRuntime, false);
	if (g_gipRuntime.playerIndex == 0xFF)
		return;
	int idx = (int)(g_gipRuntime.deviceContext - 0x0000000010000005);
	XamUserBindDeviceCallback(0xa7553952 + idx,
		g_gipRuntime.deviceContext, 0, true, 0);
	// XBOXINPUT_DBG, not XBOXINPUT_LOG: only caller is UsbdRemoveDeviceCompleteHook, and DbgPrint
	// inside the USB removal completion is the freeze suspect. See the banner there.
	XBOXINPUT_DBG("XBOXINPUT: removed virtual gamepad from XAM\r\n");
	g_gipRuntime.playerIndex = 0xFF;
	g_gipRuntime.deviceContext = 0;
}

static void GipSessionRegisterWithXam(GipSessionSlot* session) {
	if (!session || session->runtime.playerIndex != 0xFF)
		return;
	for (int idx = 0; idx < 4; ++idx) {
		uint32_t context = 0x10000005 + idx;
		bool used = (context == g_gipRuntime.deviceContext);
		for (int i = 0; i < GIP_MAX_SESSIONS; ++i)
			if (g_gipSessions[i].reserved &&
				g_gipSessions[i].runtime.deviceContext == context)
				used = true;
		if (used || connectedControllers[idx].controllerDriver)
			continue;
		uint8_t user = 0xFF;
		XamUserBindDeviceCallback(0xa7553952 + idx, context, 0, false, &user);
		session->runtime.playerIndex = user;
		session->runtime.deviceContext = context;
		XboxInputRuntimeSetReady(&session->runtime, user != 0xFF);
		XboxInputCancelRemovedControllerNotification(user);
		if (XboxInputRuntimeIsReady(&session->runtime))
			XboxInputQueueLogEvent(XBOXINPUT_LOG_CONTROLLER_READY, user, context);
		return;
	}
}

//

//
// ACK a chunked packet. Layout is struct gip_pkt_acknowledge,
// refs/xone/bus/protocol.c:70-77 - and it matches the captured bytes exactly:
//   00 04 20 3A 00 00 00 B1 00
//   => unknown=0, command=0x04, options=0x20, length=58, pad, remaining=177 (235-58)
//
static void GipSendAck(deviceHandle* h, const GipHeader* in) {
	// refs/xone/auth/... gip_acknowledge_pkt: hdr.sequence = ack->sequence.
	// The ACK must ECHO the sequence it is acknowledging, not allocate a new one.
	uint32_t received;

	if (in->options & GIP_OPT_CHUNK_START) {
		// On the first chunk the offset field carries the TOTAL length.
		g_gipChunkTotal = in->chunkOffset;
		received = in->packetLength;
	}
	else {
		received = in->chunkOffset + in->packetLength;
	}

	uint32_t remaining = (g_gipChunkTotal > received) ? (g_gipChunkTotal - received) : 0;

	BYTE p[9];
	p[0] = 0x00;
	p[1] = in->command;
	p[2] = GIP_OPT_INTERNAL;             // client id 0
	p[3] = (BYTE)(received & 0xFF);      // le16
	p[4] = (BYTE)((received >> 8) & 0xFF);
	p[5] = 0x00;
	p[6] = 0x00;
	p[7] = (BYTE)(remaining & 0xFF);     // le16
	p[8] = (BYTE)((remaining >> 8) & 0xFF);

	GipSendSeq(h, GIP_CMD_ACKNOWLEDGE, GIP_OPT_INTERNAL, in->sequence, p, sizeof(p));
}

//
// Decode whatever arrived on the interrupt IN endpoint.
// A single USB transfer can carry SEVERAL back-to-back GIP packets
// (refs/xone/bus/protocol.c:1509-1533), so loop rather than assuming one.
//
static void GipHandleTransfer(const BYTE* data, int len) {
	int off = 0;
	while (off + 4 <= len) {
		GipHeader hdr;
		if (!GipDecodeHeader(data + off, len - off, &hdr))
			break;

		// Trailing zero padding in a 64-byte transfer decodes as cmd=0/len=0, which
		// would advance 4 bytes at a time and spam the log at ~40 Hz. There is no
		// GIP command 0x00 (protocol.c:30-49), so treat it as end-of-data.
		if (hdr.command == 0x00)
			break;

		const int total = hdr.headerLength + (int)hdr.packetLength;
		if (total <= 0 || off + total > len) {
			XBOXINPUT_DBG("XBOXINPUT: GIP truncated pkt cmd=%02X len=%u (have %d)\r\n",
				hdr.command, hdr.packetLength, len - off);
			break;
		}

		const BYTE* payload = data + off + hdr.headerLength;
		g_gipPacketsSeen++;

		switch (hdr.command) {
		case GIP_CMD_ANNOUNCE: {
			if (!g_gipIdentifySent)
				XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_ANNOUNCE,
					hdr.sequence);
			// Payload offsets 8-11 are VID/PID little-endian - verified in the capture.
			// The controller will keep announcing until it sees IDENTIFY.  Re-send it
			// at a modest rate until the first IDENTIFY reply proves the packet arrived.
			// This recovers a transient first-transfer loss without re-claiming or
			// resetting the USB device (both are unsafe while a prior TRB may be live).
			DWORD now = GetTickCount();
			bool retryIdentify = g_gipIdentifySent && !g_gipIdentifyReplySeen &&
				((DWORD)(now - g_gipLastIdentifyTick) >= 1000);
			if (!g_gipIdentifySent || retryIdentify) {
				if (hdr.packetLength >= 12)
					XBOXINPUT_DBG("XBOXINPUT: GIP ANNOUNCE seq=%d VID=%04X PID=%04X\r\n",
						hdr.sequence,
						payload[8] | (payload[9] << 8),
						payload[10] | (payload[11] << 8));
				XBOXINPUT_DBG("XBOXINPUT: -> sending IDENTIFY%s\r\n",
					retryIdentify ? " (retry)" : "");
				g_gipIdentifySent = true;
				g_gipLastIdentifyTick = now;
				g_gipChunkTotal = 0;
				XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_IDENTIFY_SENT,
					retryIdentify ? 1 : 0);
				GipSend(g_gipExt.deviceHandle, GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0, 0);
			}
			break;
		}

		case GIP_CMD_IDENTIFY:
			// Chunked descriptor reply. ACK when the device asks us to, then power on
			// once the terminating zero-length chunk arrives.
			g_gipIdentifyReplySeen = true;
			XBOXINPUT_DBG("XBOXINPUT: GIP IDENTIFY chunk opts=%02X len=%u off=%u\r\n",
				hdr.options, hdr.packetLength, hdr.chunkOffset);

			if (hdr.options & GIP_OPT_ACKNOWLEDGE)
				GipSendAck(g_gipExt.deviceHandle, &hdr);

			if (hdr.packetLength == 0 && !g_gipPoweredOn) {
				g_gipPoweredOn = true;
				XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_IDENTIFY_COMPLETE, 0);
				XBOXINPUT_DBG("XBOXINPUT: -> identify complete, sending init sequence\r\n");

				if (IsPowerA1414134(g_gipVendorId, g_gipProductId)) {
					// The early POWER transfer is normally complete before IDENTIFY can
					// finish. If it is still completing, its callback observes this flag
					// and starts the ordered post-identify sequence itself.
					InterlockedExchange(&g_powerAIdentifyComplete, 1);
					if (InterlockedCompareExchange(&g_powerAInitStage, 0, 0) ==
						POWERA_INIT_WAIT_IDENTIFY)
						GipPowerAQueueStage(POWERA_INIT_LED);
					break;
				}

				const XboxInputControllerProfile* profile = g_gipRuntime.profile;
				if (profile &&
					(profile->quirks & XBOXINPUT_QUIRK_LED_AUTH_BEFORE_INPUT) != 0) {
					if ((profile->quirks & XBOXINPUT_QUIRK_RUMBLE_SETUP) != 0) {
						GipStartPostIdentifyStartup(g_gipPrimarySession);
						break;
					}
					if (!XboxInputRuntimeIsReady(&g_gipRuntime)) {
						XboxInputRuntimeSetReady(&g_gipRuntime, true);
						g_gipClaimAttempts = 0;
						GipRegisterWithXam();
					}
					break;
				}

				// Replay what the Windows host sent, in order, from the captured
				// enumeration (docs/GIP protocol notes section 5 stage 2). Previously we
				// sent only POWER ON and the device went quiet then disconnected.

				// 1. 15-byte POWER packet with ASCII "US" at payload offset 7-8.
				//    Not something xone ever emits; purpose unconfirmed, but it is what
				//    the real host sends immediately before power-on.
				static const BYTE locale[15] = {
					0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					'U',  'S',
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00
				};
				GipSend(g_gipExt.deviceHandle, GIP_CMD_POWER, GIP_OPT_INTERNAL,
					locale, sizeof(locale));

				// 2. POWER ON - capture: 05 20 03 01 00, GIP_PWR_ON (protocol.h:29-34).
				BYTE mode = 0x00;
				GipSend(g_gipExt.deviceHandle, GIP_CMD_POWER, GIP_OPT_INTERNAL, &mode, 1);

				// 3. LED - capture: 0A 20 04 03 00 01 14. Cosmetic, but sent for fidelity
				//    with the working host in case the device expects the full sequence.
				static const BYTE led[3] = { 0x00, 0x01, 0x14 };
				GipSend(g_gipExt.deviceHandle, GIP_CMD_LED, GIP_OPT_INTERNAL, led, sizeof(led));

				XBOXINPUT_DBG("XBOXINPUT: -> sent locale, POWER ON, LED\r\n");

				// Standard wired gamepads become ready after IDENTIFY and POWER.
				if (!XboxInputRuntimeIsReady(&g_gipRuntime)) {
					XboxInputRuntimeSetReady(&g_gipRuntime, true);
					// Reaching this point is a successful session and may refill the safe
					// reconnect budget after a sleep/wake bounce.
					g_gipClaimAttempts = 0;
					GipRegisterWithXam();
				}
			}
			break;

		case GIP_CMD_STATUS:
			if (hdr.packetLength >= 1) {
				XBOXINPUT_DBG("XBOXINPUT: GIP STATUS 0x%02X (%s)\r\n",
					payload[0], (payload[0] & 0x80) ? "connected" : "DISCONNECTED");
			}
			break;

		case GIP_CMD_AUTHENTICATE:
			break;

		case GIP_CMD_VIRTUAL_KEY:
			XboxInputApplyGipGuidePayload(payload, (int)hdr.packetLength,
				&g_gipRuntime.guideDown, &g_gipRuntime.guidePending);
			break;

		case GIP_CMD_INPUT:
			if (GipParseGamepadInput(payload, (int)hdr.packetLength,
				&g_gipRuntime.state)) {
				// A controller can retain its GIP session across a quick USB bounce.
				// In that case it resumes streaming INPUT without a new ANNOUNCE or
				// IDENTIFY. A valid parsed report on our claimed, supported interface
				// is enough to restore the XAM slot; otherwise input is lost at user 255.
				if (!XboxInputRuntimeIsReady(&g_gipRuntime) && g_gipExt.deviceHandle &&
					IsSupportedGipGamepad(g_gipVendorId, g_gipProductId)) {
					XboxInputRuntimeSetReady(&g_gipRuntime, true);
					g_gipPoweredOn = true;
					g_gipClaimAttempts = 0;
					XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP,
						XBOXINPUT_USB_RESUMED_FROM_INPUT, hdr.sequence);
					GipRegisterWithXam();
				}
				if (g_gipInputsSeen == 0)
					XboxInputQueueLogEvent(XBOXINPUT_LOG_FIRST_INPUT,
						g_gipRuntime.playerIndex == 0xFF ? 0xFF :
						g_gipRuntime.playerIndex, hdr.sequence);
				g_gipInputsSeen++;
				// Rate-limited: these arrive at ~40 Hz and would flood the log.
				if (g_gipInputsSeen <= 3 || (g_gipInputsSeen % 400) == 0)
					XBOXINPUT_DBG("XBOXINPUT: GIP INPUT #%d btn=%04X\r\n",
						g_gipInputsSeen, g_gipRuntime.state.buttons);
			}
			break;

		case GIP_CMD_ACKNOWLEDGE:
			XBOXINPUT_DBG("XBOXINPUT: GIP ACK seq=%d len=%u\r\n",
				hdr.sequence, hdr.packetLength);
			break;

		default:
			XBOXINPUT_DBG("XBOXINPUT: GIP cmd=%02X opts=%02X seq=%d len=%u\r\n",
				hdr.command, hdr.options, hdr.sequence, hdr.packetLength);
			break;
		}

		off += total;
	}
}

// Minimal gamepad-only GIP path for additional controller sessions.  Authentication
// packets are intentionally not shared with the gamepad path: official
// wired Microsoft gamepads become ready after IDENTIFY/POWER and stream INPUT.
static void GipSessionSendAck(GipSessionSlot* session, const GipHeader* in) {
	uint32_t total = (in->options & GIP_OPT_CHUNK_START) ? in->chunkOffset : 0;
	uint32_t received = (in->options & GIP_OPT_CHUNK_START) ?
		in->packetLength : in->chunkOffset + in->packetLength;
	uint32_t remaining = (total > received) ? total - received : 0;
	BYTE payload[9] = { 0x00, in->command, GIP_OPT_INTERNAL,
		(BYTE)received, (BYTE)(received >> 8), 0, 0,
		(BYTE)remaining, (BYTE)(remaining >> 8) };
	GipSessionSendSeq(session, GIP_CMD_ACKNOWLEDGE, GIP_OPT_INTERNAL,
		in->sequence, payload, sizeof(payload));
}

static void GipSessionHandleTransfer(GipSessionSlot* session, const BYTE* data, int len) {
	if (!session)
		return;
	int off = 0;
	while (off + 4 <= len) {
		GipHeader hdr;
		if (!GipDecodeHeader(data + off, len - off, &hdr) || hdr.command == 0)
			break;
		int total = hdr.headerLength + (int)hdr.packetLength;
		if (total <= 0 || off + total > len)
			break;
		const BYTE* payload = data + off + hdr.headerLength;
		session->packetsSeen++;
		switch (hdr.command) {
		case GIP_CMD_ANNOUNCE: {
			DWORD now = GetTickCount();
			bool retry = session->identifySent && !session->identifyReplySeen &&
				(DWORD)(now - session->lastIdentifyTick) >= 1000;
			if (!session->identifySent || retry) {
				session->identifySent = true;
				session->lastIdentifyTick = now;
				GipSessionSend(session, GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0, 0);
			}
			break;
		}
		case GIP_CMD_IDENTIFY:
			session->identifyReplySeen = true;
			if (hdr.options & GIP_OPT_ACKNOWLEDGE)
				GipSessionSendAck(session, &hdr);
			if (hdr.packetLength == 0 && !session->poweredOn) {
				const XboxInputControllerProfile* profile = session->runtime.profile;
				if (profile &&
					(profile->quirks & XBOXINPUT_QUIRK_RUMBLE_SETUP) != 0) {
					session->poweredOn = true;
					GipStartPostIdentifyStartup(session);
					break;
				}
				static const BYTE locale[15] = { 6,0,0,0,0,0,0,'U','S',0,0,0,0,0,0 };
				static const BYTE led[3] = { 0,1,0x14 };
				BYTE mode = 0;
				session->poweredOn = true;
				GipSessionSend(session, GIP_CMD_POWER, GIP_OPT_INTERNAL, locale, sizeof(locale));
				GipSessionSend(session, GIP_CMD_POWER, GIP_OPT_INTERNAL, &mode, 1);
				GipSessionSend(session, GIP_CMD_LED, GIP_OPT_INTERNAL, led, sizeof(led));
				GipSessionRegisterWithXam(session);
			}
			break;
		case GIP_CMD_VIRTUAL_KEY:
			XboxInputApplyGipGuidePayload(payload, (int)hdr.packetLength,
				&session->runtime.guideDown, &session->runtime.guidePending);
			break;
		case GIP_CMD_INPUT:
			if (GipParseGamepadInput(payload, (int)hdr.packetLength,
				&session->runtime.state)) {
				if (!XboxInputRuntimeIsReady(&session->runtime) &&
					session->ext.deviceHandle) {
					session->poweredOn = true;
					XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP,
						XBOXINPUT_USB_RESUMED_FROM_INPUT, hdr.sequence);
					GipSessionRegisterWithXam(session);
				}
				session->inputsSeen++;
			}
			break;
		}
		off += total;
	}
}

//
// Interrupt IN completion. Re-arms the read so the stream keeps flowing.
// Extension base is recovered by subtracting interruptTrb's offset (4), matching
// upstream's interruptHandler convention.
//
// Budget of consecutive failed interrupt reads before the read loop gives up for good.
// Small on purpose: the loop it bounds runs at raised IRQL, so every iteration is time
// the rest of the console does not get. Reset to zero by any successful read.
#define GIP_MAX_CONSECUTIVE_READ_ERRORS 4
static int  g_gipReadErrors = 0;
static bool g_gipReadLoopStopped = false;

enum GipPreReadInitStage {
	GIP_PRE_READ_INIT_IDLE = 0,
	GIP_PRE_READ_INIT_POWER,
	GIP_PRE_READ_INIT_LED,
	GIP_PRE_READ_INIT_AUTH_DONE,
	GIP_PRE_READ_INIT_RUMBLE_SETUP,
	GIP_PRE_READ_INIT_COMPLETE,
};

static void GipResumeAfterPreReadInit(GipSessionSlot* session);

int32_t GipInterruptComplete(DWORD trbAddr, int32_t status) {
	HidControllerExtension* ext = (HidControllerExtension*)((BYTE*)trbAddr - 4);

	// Bail out before touching anything if the device is already gone. This callback
	// can still fire once after teardown with a completion that was already in flight;
	// parsing or re-arming at that point is a use-after-free.
	if (!ext || !ext->deviceHandle ||
		GipSessionFromExtension(ext) != g_gipPrimarySession)
		return 0;
	const XboxInputControllerProfile* completionProfile = g_gipRuntime.profile;
	const bool readBeforeInit = completionProfile &&
		(completionProfile->quirks & XBOXINPUT_QUIRK_READ_BEFORE_INIT) != 0;
	if (readBeforeInit)
		InterlockedExchange(&g_gipPrimarySession->readBeforeInitPending, 2);

	// -----------------------------------------------------------------------
	// THE DISCONNECT FREEZE LIVED HERE. Do not remove this guard.
	//
	// This callback used to re-arm the transfer unconditionally, looking only at
	// ext->deviceHandle and never at `status`. When the device goes away the
	// in-flight read completes with an error, we re-queue, that read fails
	// immediately too, and the completion fires again - an unbounded loop inside a
	// USB completion callback, which runs at raised IRQL. One hardware thread gets
	// pinned there, nothing else is scheduled on it, and the console dies by
	// degrees: the render thread stops submitting, the GPU drains its ring and goes
	// idle, D3D's watchdog misreports that as a GPU deadlock, and ~5 s later even
	// xbdm is gone.
	//
	// ext->deviceHandle was not a sufficient guard because it is only cleared in
	// UsbdRemoveDeviceComplete, which is downstream of the failing reads - and in
	// the `passive` variant is never cleared at all. Both froze.
	//
	// Proof it is starvation and not a real GPU fault, from the `passive` run's
	// register dump (docs/KNOWN_ISSUES.md, "Diagnostics that mislead"):
	//     CP_RB_RPTR: 0x000010cb == CP_RB_WPTR: 0x000010cb   ring buffer EMPTY
	//     CP_IB1_BUFSZ: 0, CP_IB2_BUFSZ: 0                   nothing pending
	// The GPU had consumed every packet submitted and was waiting for more.
	//
	// So: a bounded number of consecutive failures, then stop for good. Bounded
	// rather than zero-tolerance because a single transient error on an interrupt
	// endpoint should not permanently kill a working controller.
	// -----------------------------------------------------------------------
	// `fix1` bounded ERROR completions only, and did not survive. So either the reads
	// are not failing at all, or the loop is not the mechanism. The likely miss:
	// a completion with status 0 and NO DATA. The buffer is memset before every read
	// and GIP command 0x00 means end-of-data, so byte 0 still being zero after a
	// successful completion means nothing arrived. That path reset the error counter
	// and re-armed immediately - the same unbounded loop, just with status 0.
	//
	// So bound UNPRODUCTIVE completions, whatever their status, and reset only on a
	// completion that actually delivered a packet.
	bool productive = (status == 0 && g_gipReadBuf[0] != 0);
	if (readBeforeInit &&
		InterlockedCompareExchange(&g_gipPrimarySession->preReadInitStage, 0, 0) !=
		GIP_PRE_READ_INIT_COMPLETE) {
		g_gipReadErrors = 0;
		if (productive) {
			DWORD length = ext->interruptTrb.length;
			if (length > sizeof(g_gipPrimarySession->deferredReadBuf))
				length = sizeof(g_gipPrimarySession->deferredReadBuf);
			memcpy(g_gipPrimarySession->deferredReadBuf, g_gipReadBuf, length);
			g_gipPrimarySession->deferredReadLength = length;
			__sync();
			g_gipPrimarySession->deferredReadValid = true;
		}
		__sync();
		InterlockedExchange(&g_gipPrimarySession->readBeforeInitPending, 0);
		if (InterlockedCompareExchange(&g_gipPrimarySession->preReadInitStage, 0, 0) ==
			GIP_PRE_READ_INIT_COMPLETE &&
			InterlockedCompareExchange(
				&g_gipPrimarySession->readBeforeInitPending, 3, 0) == 0)
			GipResumeAfterPreReadInit(g_gipPrimarySession);
		return status;
	}
	if (productive) {
		g_gipReadErrors = 0;
		GipHandleTransfer(g_gipReadBuf, (int)ext->interruptTrb.length);
	}
	else if (++g_gipReadErrors >= GIP_MAX_CONSECUTIVE_READ_ERRORS) {
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP,
			XBOXINPUT_USB_READ_LOOP_STOPPED, status);
		// Stop permanently. Nulling the handle is what every other path already
		// treats as "this device is finished", so teardown stays unchanged.
		ext->deviceHandle = 0;
		g_gipReadLoopStopped = true;
		// Report HERE, not from the removal hook. fix1 put the confirmation print in
		// UsbdRemoveDeviceCompleteHook, which never ran, so a silent log could not be
		// told apart from a guard that never fired. One line, once per boot.
		static bool reported = false;
		if (!reported) {
			reported = true;
			XBOXINPUT_LOG("XBOXINPUT: read loop STOPPED after %d unproductive completions "
				"(last status 0x%08X) - disconnect guard fired\r\n",
				GIP_MAX_CONSECUTIVE_READ_ERRORS, status);
		}
		return status;
	}
	else if (status != 0 && g_gipPacketsSeen < 4) {
		XBOXINPUT_DBG("XBOXINPUT: GIP read status 0x%08X\r\n", status);
	}

	if (!ext->deviceHandle)
		return 0;

	// Zero before re-arming. interruptTrb.length is the REQUESTED size, not the number
	// of bytes actually received, so the parser cannot know where real data ends. With a
	// cleared buffer the leftover tail reads as command 0x00, which GipHandleTransfer
	// treats as end-of-data. Without this we walked off into stale bytes and produced
	// "GIP truncated pkt cmd=9B len=8877".
	memset(g_gipReadBuf, 0, sizeof(g_gipReadBuf));

	ext->interruptTrb.savedEndpoint = ext->interruptTrb.endpoint;
	ext->interruptTrb.length = GIP_READ_BUF_SIZE;
	ext->interruptTrb.buffer = g_gipReadBuf;
	ext->interruptTrb.callback = (DWORD)GipInterruptComplete;

	// A failed re-arm is the same hazard by another route: if the queue itself starts
	// rejecting, the caller may keep driving us. Stop on the same budget.
	if (readBeforeInit)
		InterlockedExchange(&g_gipPrimarySession->readBeforeInitPending, 1);
	int32_t queued = UsbdQueueAsyncTransfer(ext->deviceHandle, &ext->interruptTrb);
	if (queued != 0 && ++g_gipReadErrors >= GIP_MAX_CONSECUTIVE_READ_ERRORS) {
		ext->deviceHandle = 0;
		g_gipReadLoopStopped = true;
	}
	return queued;
}

static int32_t GipStartPrimaryRead(HidControllerExtension* ext, uint16_t packetSize) {
	if (!ext || !ext->deviceHandle ||
		GipSessionFromExtension(ext) != g_gipPrimarySession)
		return -1;
#ifdef XBOXINPUT_NO_READ
	UNREFERENCED_PARAMETER(packetSize);
	return 0;
#else
	if (packetSize > GIP_READ_BUF_SIZE)
		packetSize = GIP_READ_BUF_SIZE;
	memset(g_gipReadBuf, 0, sizeof(g_gipReadBuf));
	ext->interruptTrb.savedEndpoint = ext->interruptTrb.endpoint;
	ext->interruptTrb.length = packetSize;
	ext->interruptTrb.buffer = g_gipReadBuf;
	ext->interruptTrb.callback = (DWORD)GipInterruptComplete;
	ext->interruptTrb.flags = 1;
	if (g_gipRuntime.profile &&
		(g_gipRuntime.profile->quirks & XBOXINPUT_QUIRK_READ_BEFORE_INIT) != 0)
		InterlockedExchange(&g_gipPrimarySession->readBeforeInitPending, 1);
	int32_t queued = UsbdQueueAsyncTransfer(ext->deviceHandle, &ext->interruptTrb);
	XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_READ_QUEUED, queued);
	return queued;
#endif
}

static int GipQueuePreReadInit(GipSessionSlot* session, LONG stage);
static int32_t GipPreReadInitComplete(DWORD trbAddr, int32_t status);
static int32_t GipSessionStartRead(GipSessionSlot* session);
static int32_t GipSessionEarlyPowerComplete(DWORD trbAddr, int32_t status);

static int GipStartPostIdentifyStartup(GipSessionSlot* session) {
	if (!session || !session->runtime.profile ||
		(session->runtime.profile->quirks & XBOXINPUT_QUIRK_RUMBLE_SETUP) == 0)
		return -1;
	return GipQueuePreReadInit(session, GIP_PRE_READ_INIT_POWER);
}

static void GipResumeAfterPreReadInit(GipSessionSlot* session) {
	if (!session || !session->reserved) {
		if (session)
			InterlockedExchange(&session->readBeforeInitPending, 0);
		return;
	}
	if (session->deferredReadValid) {
		session->deferredReadValid = false;
		__sync();
		if (session->primary)
			GipHandleTransfer(session->deferredReadBuf,
				(int)session->deferredReadLength);
		else
			GipSessionHandleTransfer(session, session->deferredReadBuf,
				(int)session->deferredReadLength);
	}
	if (!session->ext.deviceHandle || !session->runtime.profile) {
		InterlockedExchange(&session->readBeforeInitPending, 0);
		return;
	}
	if (session->primary)
		GipStartPrimaryRead(&session->ext,
			session->runtime.profile->endpointIdentity.maximumPacketSize);
	else
		GipSessionStartRead(session);
}

static int GipQueuePreReadInit(GipSessionSlot* session, LONG stage) {
	if (!session || !session->reserved || !session->outOpen ||
		!session->ext.deviceHandle || !session->runtime.profile)
		return -1;

	BYTE command = 0;
	BYTE options = GIP_OPT_INTERNAL;
	const BYTE* payload = 0;
	int payloadLength = 0;
	DWORD queuedStep = 0;
	static const BYTE powerOn[1] = { 0x00 };
	static const BYTE ledOn[3] = { 0x00, 0x01, 0x14 };
	static const BYTE authDone[2] = { 0x01, 0x00 };
	static const BYTE rumbleSetup[9] = {
		0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xEB
	};
	if (stage == GIP_PRE_READ_INIT_POWER) {
		command = GIP_CMD_POWER;
		payload = powerOn;
		payloadLength = sizeof(powerOn);
		queuedStep = XBOXINPUT_USB_INIT_POWER_QUEUED;
	}
	else if (stage == GIP_PRE_READ_INIT_LED) {
		command = GIP_CMD_LED;
		payload = ledOn;
		payloadLength = sizeof(ledOn);
		queuedStep = XBOXINPUT_USB_PRE_READ_LED_QUEUED;
	}
	else if (stage == GIP_PRE_READ_INIT_AUTH_DONE) {
		command = GIP_CMD_AUTHENTICATE;
		payload = authDone;
		payloadLength = sizeof(authDone);
		queuedStep = XBOXINPUT_USB_PRE_READ_AUTH_QUEUED;
	}
	else if (stage == GIP_PRE_READ_INIT_RUMBLE_SETUP) {
		command = GIP_CMD_RUMBLE;
		options = 0;
		payload = rumbleSetup;
		payloadLength = sizeof(rumbleSetup);
		queuedStep = XBOXINPUT_USB_INIT_RUMBLE_SETUP_QUEUED;
	}
	else {
		return -1;
	}

	int length = 0;
	session->preReadInitBuf[length++] = command;
	session->preReadInitBuf[length++] = options;
	session->preReadInitBuf[length++] = GipSessionNextSeq(session);
	session->preReadInitBuf[length++] = (BYTE)payloadLength;
	memcpy(session->preReadInitBuf + length, payload, payloadLength);
	length += payloadLength;
	InterlockedExchange(&session->preReadInitStage, stage);
	XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, queuedStep, command);
	SendInterruptRequest(session->ext.deviceHandle, &session->outTrb,
		session->preReadInitBuf, length, (DWORD)GipPreReadInitComplete);
	return 0;
}

static int32_t GipPreReadInitComplete(DWORD trbAddr, int32_t status) {
	GipSessionSlot* session = 0;
	for (int i = 0; i < GIP_MAX_SESSIONS; ++i) {
		if ((DWORD)&g_gipSessions[i].outTrb == trbAddr) {
			session = &g_gipSessions[i];
			break;
		}
	}
	if (!session || !session->reserved || !session->ext.deviceHandle)
		return status;

	LONG completed = InterlockedCompareExchange(&session->preReadInitStage, 0, 0);
	DWORD completeStep = XBOXINPUT_USB_PRE_READ_AUTH_COMPLETE;
	if (completed == GIP_PRE_READ_INIT_POWER)
		completeStep = XBOXINPUT_USB_INIT_POWER_COMPLETE;
	else if (completed == GIP_PRE_READ_INIT_LED)
		completeStep = XBOXINPUT_USB_PRE_READ_LED_COMPLETE;
	else if (completed == GIP_PRE_READ_INIT_RUMBLE_SETUP)
		completeStep = XBOXINPUT_USB_INIT_RUMBLE_SETUP_COMPLETE;
	XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, completeStep, status);
	if (status != 0) {
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_FAILURE, 80 + completed, status);
		return status;
	}
	if (completed == GIP_PRE_READ_INIT_POWER)
		return GipQueuePreReadInit(session, GIP_PRE_READ_INIT_LED);
	if (completed == GIP_PRE_READ_INIT_LED)
		return GipQueuePreReadInit(session, GIP_PRE_READ_INIT_AUTH_DONE);
	const XboxInputControllerProfile* profile = session->runtime.profile;
	if (completed == GIP_PRE_READ_INIT_AUTH_DONE && profile &&
		(profile->quirks & XBOXINPUT_QUIRK_RUMBLE_SETUP) != 0)
		return GipQueuePreReadInit(session, GIP_PRE_READ_INIT_RUMBLE_SETUP);
	if (completed != GIP_PRE_READ_INIT_AUTH_DONE &&
		completed != GIP_PRE_READ_INIT_RUMBLE_SETUP)
		return status;

	InterlockedExchange(&session->preReadInitStage, GIP_PRE_READ_INIT_COMPLETE);
	if (profile && session->identifyReplySeen && session->poweredOn &&
		(profile->quirks & XBOXINPUT_QUIRK_RUMBLE_SETUP) != 0) {
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP,
			XBOXINPUT_USB_POST_IDENTIFY_STARTUP_COMPLETE, status);
		if (!XboxInputRuntimeIsReady(&session->runtime)) {
			if (session->primary) {
				XboxInputRuntimeSetReady(&session->runtime, true);
				g_gipClaimAttempts = 0;
				GipRegisterWithXam();
			}
			else {
				GipSessionRegisterWithXam(session);
			}
		}
	}
	if (profile &&
		(profile->quirks & XBOXINPUT_QUIRK_READ_BEFORE_INIT) != 0) {
		if (InterlockedCompareExchange(&session->readBeforeInitPending, 3, 0) == 0)
			GipResumeAfterPreReadInit(session);
		return status;
	}
	if (session->primary)
		return GipStartPrimaryRead(&session->ext,
			profile->endpointIdentity.maximumPacketSize);
	return GipSessionStartRead(session);
}

static int32_t GipEarlyPowerComplete(DWORD trbAddr, int32_t status) {
	if (trbAddr != (DWORD)&g_gipOutTrb)
		return status;
	deviceHandle* poweredHandle = g_gipEarlyPowerHandle;
	uint16_t packetSize = g_gipEarlyPowerPacketSize;
	g_gipEarlyPowerHandle = 0;
	InterlockedExchange(&g_gipEarlyPowerBusy, 0);
	XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_EARLY_POWER_COMPLETE, status);
	// A disconnect may have occurred while this asynchronous transfer was in
	// flight. Never start a read on a new claim from an old completion.
	if (poweredHandle && g_gipExt.deviceHandle == poweredHandle) {
		const XboxInputControllerProfile* profile = g_gipRuntime.profile;
		if (status == 0 && profile &&
			(profile->quirks & XBOXINPUT_QUIRK_LED_AUTH_BEFORE_INPUT) != 0)
			return GipQueuePreReadInit(g_gipPrimarySession, GIP_PRE_READ_INIT_LED);
		if (profile &&
			(profile->quirks & XBOXINPUT_QUIRK_READ_BEFORE_INIT) != 0)
			return status;
		return GipStartPrimaryRead(&g_gipExt, packetSize);
	}
	return status;
}

static int32_t GipDisableAudioInterfaceComplete(DWORD trbAddr, int32_t status) {
	HidControllerExtension* ext = (HidControllerExtension*)((BYTE*)trbAddr - 36);
	GipSessionSlot* session = GipSessionFromExtension(ext);
	if (!session || !session->reserved || !ext || !ext->deviceHandle)
		return status;
	XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP,
		XBOXINPUT_USB_AUDIO_INTERFACE_DISABLE_COMPLETE, status);
	if (status != 0)
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_FAILURE, 90, status);

	const XboxInputControllerProfile* profile = session->runtime.profile;
	if (!profile ||
		(profile->quirks & XBOXINPUT_QUIRK_POWER_BEFORE_ANNOUNCE) == 0)
		return status;

	if (session->primary) {
		if (!session->outOpen ||
			InterlockedCompareExchange(&session->earlyPowerBusy, 1, 0) != 0)
			return status;
		session->earlyPowerHandle = ext->deviceHandle;
		session->earlyPowerPacketSize = profile->endpointIdentity.maximumPacketSize;
		session->outTrb.buffer = session->earlyPowerBuf;
		session->outTrb.length = sizeof(session->earlyPowerBuf);
		session->outTrb.flags = 1;
		session->outTrb.callback = (DWORD)GipEarlyPowerComplete;
		session->outTrb.savedEndpoint = session->outTrb.endpoint;
		int32_t token = UsbdQueueAsyncTransfer(ext->deviceHandle, &session->outTrb);
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP,
			XBOXINPUT_USB_EARLY_POWER_QUEUED, token);
		return status;
	}

	const BYTE powerOn[5] = { GIP_CMD_POWER, GIP_OPT_INTERNAL, 0, 1, 0 };
	memcpy(session->earlyPowerBuf, powerOn, sizeof(powerOn));
	session->outTrb.buffer = session->earlyPowerBuf;
	session->outTrb.length = sizeof(session->earlyPowerBuf);
	session->outTrb.flags = 1;
	session->outTrb.callback = (DWORD)GipSessionEarlyPowerComplete;
	session->outTrb.savedEndpoint = session->outTrb.endpoint;
	int32_t token = UsbdQueueAsyncTransfer(ext->deviceHandle, &session->outTrb);
	XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP,
		XBOXINPUT_USB_EARLY_POWER_QUEUED, token);
	return status;
}

//
// SET_CONFIGURATION completed - open the GIP interrupt IN endpoint and start reading.
// Extension base is recovered by subtracting controlTrb's offset (36).
//
int32_t GipSetConfigComplete(DWORD trbAddr, int32_t status) {
	XboxInputSetDiagStage(50);
	XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_SET_CONFIG_COMPLETE, status);
	HidControllerExtension* ext = (HidControllerExtension*)((BYTE*)trbAddr - 36);
	if (!ext || !ext->deviceHandle ||
		GipSessionFromExtension(ext) != g_gipPrimarySession)
		return status;

	XBOXINPUT_LOG("XBOXINPUT: SET_CONFIGURATION completed status=0x%08X\r\n", status);
	if (status != 0)
	{
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_FAILURE, 50, status);
		return status;
	}

	// Descriptor lookup is unreliable for devices claimed through this rejected-device
	// path.  The old fallback assumed every GIP controller used the Xbox One S endpoint
	// pair (82/02), which opened nonexistent endpoints on 02D1/02DD controllers.  The
	// refactored profile is the trusted source of endpoint topology for known devices.
	const XboxInputControllerProfile* activeProfile = g_gipRuntime.profile;
	if (!activeProfile) {
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_FAILURE, 59, 0xFFFFFFFF);
		return -1;
	}
	const XboxInputUsbEndpointIdentity* endpoints = &activeProfile->endpointIdentity;
	BYTE epAddr = endpoints->inputAddress;
	uint16_t pkt = endpoints->maximumPacketSize;
	BYTE interval = endpoints->interval;

	XBOXINPUT_DBG("XBOXINPUT: opening EP %02X maxPacket=%d interval=%d\r\n", epAddr, pkt, interval);

	NTSTATUS s = UsbdOpenEndpoint(ext->deviceHandle, endpoints->transferType,
		epAddr, pkt, interval, (DWORD*)&ext->interruptTrb);
	XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_INTERRUPT_IN_OPEN,
		((DWORD)epAddr << 24) | ((DWORD)pkt << 8) | (NT_ERROR(s) ? 0x80 : interval));
	if (NT_ERROR(s)) {
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_FAILURE, 60, s);
		XBOXINPUT_LOG("XBOXINPUT: UsbdOpenEndpoint FAILED 0x%08X\r\n", s);
		return s;
	}
	XBOXINPUT_LOG("XBOXINPUT: *** interrupt IN endpoint OPEN - starting GIP reads ***\r\n");
	XboxInputSetDiagStage(60);

	// Open the matching profile-owned OUT endpoint too - without it we can never
	// answer ANNOUNCE. Early Xbox One controllers use EP1; newer pads use EP2.
	{
		BYTE outAddr = endpoints->outputAddress;
		uint16_t outPkt = endpoints->maximumPacketSize;
		BYTE outInterval = endpoints->interval;
		NTSTATUS os = UsbdOpenEndpoint(ext->deviceHandle, endpoints->transferType,
			outAddr, outPkt, outInterval, (DWORD*)&g_gipOutTrb);
		g_gipOutOpen = !NT_ERROR(os);
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_INTERRUPT_OUT_OPEN,
			((DWORD)outAddr << 24) | ((DWORD)outPkt << 8) | (NT_ERROR(os) ? 0x80 : outInterval));
		XBOXINPUT_LOG("XBOXINPUT: interrupt OUT EP %02X -> 0x%08X %s\r\n",
			outAddr, os, g_gipOutOpen ? "OK" : "FAILED");
		if (g_gipOutOpen && IsPowerA1414134(g_gipVendorId, g_gipProductId)) {
			// 2015-era Xbox One firmware may not ANNOUNCE until the host powers it
			// on. PowerA 24C6:543A additionally needs its post-IDENTIFY rumble kick.
			if (GipPowerAQueueStage(POWERA_INIT_EARLY_POWER) != 0)
				XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_FAILURE, 70, 0xFFFFFFFF);
		}
	}

#ifdef XBOXINPUT_NO_READ
	// L7-noread: claim the device and open all three endpoints exactly as normal, then
	// never queue a single interrupt read. Nothing ever completes, so GipInterruptComplete
	// can never run and the read loop cannot exist in any form.
	//
	// This is the split that should have come before any attempted fix: it separates
	// "having claimed the device and opened its endpoints" from "servicing it". The
	// controller will not work - no reads means no input or initialization.
	XBOXINPUT_LOG("XBOXINPUT: interrupt reads NOT started (noread variant)\r\n");
	return 0;
#endif
	const bool readBeforeInit =
		(activeProfile->quirks & XBOXINPUT_QUIRK_READ_BEFORE_INIT) != 0;
	if (readBeforeInit) {
		InterlockedExchange(&g_gipPrimarySession->preReadInitStage,
			GIP_PRE_READ_INIT_POWER);
		GipStartPrimaryRead(ext, pkt);
	}
	if ((activeProfile->quirks & XBOXINPUT_QUIRK_DISABLE_AUDIO_INTERFACE) != 0) {
		int32_t token = SendControlRequest(ext->deviceHandle, &ext->controlTrb,
			0x01, 0x0B, 0, 1, 0, 0,
			(DWORD)GipDisableAudioInterfaceComplete);
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP,
			XBOXINPUT_USB_AUDIO_INTERFACE_DISABLE_QUEUED, token);
		return 0;
	}
	if (g_gipOutOpen &&
		(activeProfile->quirks & XBOXINPUT_QUIRK_POWER_BEFORE_ANNOUNCE) != 0 &&
		InterlockedCompareExchange(&g_gipEarlyPowerBusy, 1, 0) == 0) {
		g_gipEarlyPowerHandle = ext->deviceHandle;
		g_gipEarlyPowerPacketSize = pkt;
		g_gipOutTrb.buffer = g_gipEarlyPowerBuf;
		g_gipOutTrb.length = sizeof(g_gipEarlyPowerBuf);
		g_gipOutTrb.flags = 1;
		g_gipOutTrb.callback = (DWORD)GipEarlyPowerComplete;
		g_gipOutTrb.savedEndpoint = g_gipOutTrb.endpoint;
		int32_t queueToken = UsbdQueueAsyncTransfer(ext->deviceHandle, &g_gipOutTrb);
		// This API returns an opaque transfer token (009B121D on the tested
		// console), NOT a zero/nonzero status. Only the completion callback says
		// whether POWER succeeded; it alone may start the first IN read.
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP,
			XBOXINPUT_USB_EARLY_POWER_QUEUED, queueToken);
		return 0;
	}
	return readBeforeInit ? 0 : GipStartPrimaryRead(ext, pkt);
}

// Additional controllers use the same session-owned storage model with a compact
// callback chain. The primary path retains its proven protocol callbacks, but their
// extension, TRBs and buffers are aliases into its current session slot.
static int32_t GipSessionInterruptComplete(DWORD trbAddr, int32_t status) {
	HidControllerExtension* ext = (HidControllerExtension*)((BYTE*)trbAddr - 4);
	GipSessionSlot* session = GipSessionFromExtension(ext);
	if (!session || !session->reserved || !ext || !ext->deviceHandle)
		return 0;
	const XboxInputControllerProfile* completionProfile = session->runtime.profile;
	const bool readBeforeInit = completionProfile &&
		(completionProfile->quirks & XBOXINPUT_QUIRK_READ_BEFORE_INIT) != 0;
	if (readBeforeInit)
		InterlockedExchange(&session->readBeforeInitPending, 2);
	const bool productive = status == 0 && session->readBuf[0] != 0;
	if (readBeforeInit &&
		InterlockedCompareExchange(&session->preReadInitStage, 0, 0) !=
		GIP_PRE_READ_INIT_COMPLETE) {
		session->readErrors = 0;
		if (productive) {
			DWORD length = ext->interruptTrb.length;
			if (length > sizeof(session->deferredReadBuf))
				length = sizeof(session->deferredReadBuf);
			memcpy(session->deferredReadBuf, session->readBuf, length);
			session->deferredReadLength = length;
			__sync();
			session->deferredReadValid = true;
		}
		__sync();
		InterlockedExchange(&session->readBeforeInitPending, 0);
		if (InterlockedCompareExchange(&session->preReadInitStage, 0, 0) ==
			GIP_PRE_READ_INIT_COMPLETE &&
			InterlockedCompareExchange(&session->readBeforeInitPending, 3, 0) == 0)
			GipResumeAfterPreReadInit(session);
		return status;
	}
	if (productive) {
		session->readErrors = 0;
		GipSessionHandleTransfer(session, session->readBuf, (int)ext->interruptTrb.length);
	}
	else if (++session->readErrors >= GIP_MAX_CONSECUTIVE_READ_ERRORS) {
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP,
			XBOXINPUT_USB_READ_LOOP_STOPPED, status);
		ext->deviceHandle = 0;
		session->readLoopStopped = true;
		return status;
	}
	if (!ext->deviceHandle)
		return 0;
	memset(session->readBuf, 0, sizeof(session->readBuf));
	ext->interruptTrb.savedEndpoint = ext->interruptTrb.endpoint;
	ext->interruptTrb.length = GIP_READ_BUF_SIZE;
	ext->interruptTrb.buffer = session->readBuf;
	ext->interruptTrb.callback = (DWORD)GipSessionInterruptComplete;
	if (readBeforeInit)
		InterlockedExchange(&session->readBeforeInitPending, 1);
	return UsbdQueueAsyncTransfer(ext->deviceHandle, &ext->interruptTrb);
}

static int32_t GipSessionStartRead(GipSessionSlot* session) {
	if (!session || !session->reserved || !session->ext.deviceHandle)
		return -1;
	HidControllerExtension* ext = &session->ext;
	memset(session->readBuf, 0, sizeof(session->readBuf));
	ext->interruptTrb.savedEndpoint = ext->interruptTrb.endpoint;
	ext->interruptTrb.length = GIP_READ_BUF_SIZE;
	ext->interruptTrb.buffer = session->readBuf;
	ext->interruptTrb.callback = (DWORD)GipSessionInterruptComplete;
	ext->interruptTrb.flags = 1;
	if (session->runtime.profile &&
		(session->runtime.profile->quirks & XBOXINPUT_QUIRK_READ_BEFORE_INIT) != 0)
		InterlockedExchange(&session->readBeforeInitPending, 1);
	int32_t queued = UsbdQueueAsyncTransfer(ext->deviceHandle, &ext->interruptTrb);
	XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_READ_QUEUED, queued);
	return queued;
}

static int32_t GipSessionEarlyPowerComplete(DWORD trbAddr, int32_t status) {
	for (int i = 0; i < GIP_MAX_SESSIONS; ++i) {
		GipSessionSlot* session = &g_gipSessions[i];
		if ((DWORD)&session->outTrb != trbAddr)
			continue;
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP,
			XBOXINPUT_USB_EARLY_POWER_COMPLETE, status);
		if (!session->reserved || !session->ext.deviceHandle)
			return status;
		const XboxInputControllerProfile* profile = session->runtime.profile;
		if (status == 0 && profile &&
			(profile->quirks & XBOXINPUT_QUIRK_LED_AUTH_BEFORE_INPUT) != 0)
			return GipQueuePreReadInit(session, GIP_PRE_READ_INIT_LED);
		if (profile &&
			(profile->quirks & XBOXINPUT_QUIRK_READ_BEFORE_INIT) != 0)
			return status;
		return GipSessionStartRead(session);
	}
	return status;
}

static int32_t GipSessionSetConfigComplete(DWORD trbAddr, int32_t status) {
	HidControllerExtension* ext = (HidControllerExtension*)((BYTE*)trbAddr - 36);
	GipSessionSlot* session = GipSessionFromExtension(ext);
	if (!session || !session->reserved || status != 0 || !ext->deviceHandle)
		return status;
	const XboxInputControllerProfile* profile = session->runtime.profile;
	if (!profile)
		return -1;
	const XboxInputUsbEndpointIdentity* endpoints = &profile->endpointIdentity;
	NTSTATUS inStatus = UsbdOpenEndpoint(ext->deviceHandle, endpoints->transferType,
		endpoints->inputAddress, endpoints->maximumPacketSize, endpoints->interval,
		(DWORD*)&ext->interruptTrb);
	if (NT_ERROR(inStatus))
		return inStatus;
	NTSTATUS outStatus = UsbdOpenEndpoint(ext->deviceHandle, endpoints->transferType,
		endpoints->outputAddress, endpoints->maximumPacketSize, endpoints->interval,
		(DWORD*)&session->outTrb);
	session->outOpen = !NT_ERROR(outStatus);
	if (!session->outOpen)
		return outStatus;
	const bool readBeforeInit =
		(profile->quirks & XBOXINPUT_QUIRK_READ_BEFORE_INIT) != 0;
	if (readBeforeInit) {
		InterlockedExchange(&session->preReadInitStage,
			GIP_PRE_READ_INIT_POWER);
		GipSessionStartRead(session);
	}
	if ((profile->quirks & XBOXINPUT_QUIRK_DISABLE_AUDIO_INTERFACE) != 0) {
		int32_t token = SendControlRequest(ext->deviceHandle, &ext->controlTrb,
			0x01, 0x0B, 0, 1, 0, 0,
			(DWORD)GipDisableAudioInterfaceComplete);
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP,
			XBOXINPUT_USB_AUDIO_INTERFACE_DISABLE_QUEUED, token);
		return 0;
	}
	if ((profile->quirks & XBOXINPUT_QUIRK_POWER_BEFORE_ANNOUNCE) == 0)
		return readBeforeInit ? 0 : GipSessionStartRead(session);
	const BYTE powerOn[5] = { GIP_CMD_POWER, GIP_OPT_INTERNAL, 0, 1, 0 };
	memcpy(session->earlyPowerBuf, powerOn, sizeof(powerOn));
	session->outTrb.buffer = session->earlyPowerBuf;
	session->outTrb.length = sizeof(session->earlyPowerBuf);
	session->outTrb.flags = 1;
	session->outTrb.callback = (DWORD)GipSessionEarlyPowerComplete;
	session->outTrb.savedEndpoint = session->outTrb.endpoint;
	int32_t queueToken = UsbdQueueAsyncTransfer(ext->deviceHandle, &session->outTrb);
	XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP,
		XBOXINPUT_USB_EARLY_POWER_QUEUED, queueToken);
	return 0; // The callback starts the read unless the profile armed it first.
}

static int GipClaimAdditionalSession(deviceHandle* h, BYTE interfaceNumber,
	const XboxInputControllerProfile* profile) {
	GipSessionSlot* session = GipFindFreeSession();
	if (!session || !profile)
		return -1;
	memset(session, 0, sizeof(*session));
	session->reserved = true;
	XboxInputInitializeRuntime(&session->runtime, profile);
	session->runtime.lifecycle = XBOXINPUT_SESSION_INITIALIZING;
	session->sequence = 1;
	session->ext.deviceHandle = h;
	session->ext.interfaceNumber = interfaceNumber;
	session->ext.deviceType = 0;
	session->ext.interruptTrb.flags = 1;
	h->driver = &session->ext;

	typedef int(*usbd_add_complete_t)(deviceHandle*, int);
	int result = UsbdAddDeviceCompleteDetour.GetOriginal<usbd_add_complete_t>()(h, 0);
	NTSTATUS openStatus = UsbdOpenDefaultEndpoint(h, (DWORD*)&session->ext.controlTrb);
	if (NT_ERROR(openStatus)) {
		session->ext.deviceHandle = 0;
		session->reserved = false;
		return result;
	}
	SendControlRequest(h, &session->ext.controlTrb,
		0x00, 0x09, 1, 0, 0, nullptr, (DWORD)GipSessionSetConfigComplete);
	return result;
}

int UsbdAddDeviceCompleteHook(deviceHandle* h, int status) {
	// This is the ONLY export that fires for the GIP controller, so identify the
	// device here rather than assuming. Nobody called UsbdGetDeviceDescriptor for it,
	// but the handle is live at this point, so we can ask ourselves.
	//
	// status is the whole point: 0 == a driver accepted the device, non-zero == the
	// add failed / nothing claimed it. Not logging it the first time was an oversight.
	usb_device_descriptor* dd = UsbdGetDeviceDescriptor ? UsbdGetDeviceDescriptor(h) : 0;
	usb_interface_descriptor* id = UsbdGetInterfaceDescriptor ? UsbdGetInterfaceDescriptor(h) : 0;

	XBOXINPUT_DBG("XBOXINPUT: ADDCOMPLETE handle=%p status=0x%08X (%s) driver=%p\r\n",
		h, status, (status == 0) ? "CLAIMED" : "not claimed",
		h ? h->driver : 0);

	if (dd)
		XBOXINPUT_DBG("XBOXINPUT:   dev VID=%04X PID=%04X class=%02X/%02X/%02X\r\n",
			swap_endianness_16(dd->idVendor), swap_endianness_16(dd->idProduct),
			dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol);
	else
		XBOXINPUT_DBG("XBOXINPUT:   dev descriptor NULL\r\n");

	if (id)
		XBOXINPUT_DBG("XBOXINPUT:   iface #%d alt=%d nEP=%d class=%02X/%02X/%02X%s\r\n",
			id->bInterfaceNumber, id->bAlternateSetting, id->bNumEndpoints,
			id->bInterfaceClass, id->bInterfaceSubClass, id->bInterfaceProtocol,
			(id->bInterfaceClass == 0xFF && id->bInterfaceSubClass == 0x47 &&
			 id->bInterfaceProtocol == 0xD0) ? "   <<< GIP" : "");
	else
		XBOXINPUT_DBG("XBOXINPUT:   iface descriptor NULL\r\n");

#ifdef XBOXINPUT_COMPAT_PROBE
	// Observation only: the build must be safe to give to users with unknown
	// hardware.  It never claims, configures, resets or opens the device.
	if (dd) {
		LONG serial = InterlockedIncrement(&g_xboxInputCompatProbeSerial);
		XboxInputCompatProbeRecord* record =
			&g_xboxInputCompatProbeRecords[(serial - 1) % XBOXINPUT_COMPAT_PROBE_RECORDS];
		record->serial = 0;
		record->vidPid =
			((DWORD)swap_endianness_16(dd->idVendor) << 16) |
			swap_endianness_16(dd->idProduct);
		record->devClass = ((DWORD)dd->bDeviceClass << 16) |
			((DWORD)dd->bDeviceSubClass << 8) | dd->bDeviceProtocol;
		if (id) {
			record->iface = ((DWORD)id->bInterfaceNumber << 24) |
				((DWORD)id->bNumEndpoints << 16) |
				((DWORD)id->bInterfaceClass << 8) | id->bInterfaceSubClass;
			record->protocol = id->bInterfaceProtocol;
		}
		else {
			record->iface = 0;
			record->protocol = 0;
		}
		__sync();
		record->serial = serial;
	}
	return UsbdAddDeviceCompleteDetour.GetOriginal<decltype(&UsbdAddDeviceCompleteHook)>()(h, status);
#endif

	// Once the proven primary path already owns one modern gamepad, every later
	// matching controller gets a distinct fixed session instead of overwriting the
	// primary extension and its asynchronous USB transfers.
	const uint16_t detectedVid = dd ? swap_endianness_16(dd->idVendor) : 0;
	const uint16_t detectedPid = dd ? swap_endianness_16(dd->idProduct) : 0;
	const uint16_t detectedRevision = dd ? swap_endianness_16(dd->bcdDevice) : 0;
	const XboxInputUsbInterfaceIdentity detectedInterface =
		XboxInputInterfaceIdentity(id);
	const XboxInputControllerProfile* profileCandidate = 0;
	XboxInputProfileMatchResult profileResult;
	if (!dd)
		profileResult = XBOXINPUT_PROFILE_MISSING_DEVICE_DESCRIPTOR;
	else if (!id)
		profileResult = XBOXINPUT_PROFILE_MISSING_INTERFACE_DESCRIPTOR;
	else
		profileResult = XboxInputDiagnoseProfileMatch(detectedVid, detectedPid,
			detectedRevision, &detectedInterface, &profileCandidate);
	const XboxInputControllerProfile* detectedProfile =
		profileResult == XBOXINPUT_PROFILE_MATCHED ? profileCandidate : 0;
	XboxInputQueueLogEventEx(XBOXINPUT_LOG_PROFILE_DECISION,
		((DWORD)detectedVid << 16) | detectedPid,
		((DWORD)detectedRevision << 16) | (DWORD)profileResult,
		((DWORD)detectedInterface.number << 24) |
		((DWORD)detectedInterface.alternateSetting << 16) |
		((DWORD)detectedInterface.endpointCount << 8) |
		detectedInterface.interfaceClass,
		((DWORD)detectedInterface.interfaceSubClass << 8) |
		detectedInterface.interfaceProtocol,
		(DWORD)status);

	if (status != 0 && h && g_gipExt.deviceHandle && detectedProfile &&
		detectedProfile->initProfile == XBOXINPUT_INIT_GIP_STANDARD &&
		GipActiveSessionCount() < GIP_MAX_ADDITIONAL_ACTIVE && GipFindFreeSession()) {
		XboxInputQueueLogEvent(XBOXINPUT_LOG_CONTROLLER_DETECTED,
			((DWORD)detectedVid << 16) | detectedPid,
			((DWORD)id->bInterfaceNumber << 8) | id->bNumEndpoints);
		return GipClaimAdditionalSession(h, id->bInterfaceNumber, detectedProfile);
	}

	// ---------------------------------------------------------------------
	// CLAIM ATTEMPT (Phase 0.5c)
	//
	// The kernel enumerates the dongle fully, then reports STATUS_UNSUCCESSFUL
	// (0xC0000001) with a NULL driver - nothing wanted it. We hold a live handle
	// here, so try the same claim sequence hiddriver360 performs for HID devices
	// in HidAddDeviceHook: attach a driver extension, then complete with status 0.
	//
	// Conditions are deliberately narrow. Only the GIP data interface of the
	// GIP controller, only when the kernel has already given up on it.
	// ---------------------------------------------------------------------
	// Breadcrumb #1. Fires for EVERY device the core gives up on, ours or not, so a
	// replug that never re-enumerates is distinguishable from one we declined to claim.
	XBOXINPUT_TRACE_LOG("XBOXINPUT: TRACE ADDCOMPLETE h=%p status=0x%08X\r\n", h, status);
#ifdef XBOXINPUT_CLAIM_ONCE
	// L7-once: claim the dongle exactly ONE time per boot, ever. Every later arrival
	// is left unclaimed, i.e. treated exactly as a no-plugin boot treats it.
	//
	// Turning the controller off makes the dongle bounce off and back onto USB ~6 times.
	// Each arrival currently re-enters the claim with the SAME static g_gipExt, and
	// re-queues TRBs the kernel may still own from the previous incarnation. This
	// build removes the storm entirely without touching teardown, so it separates
	// "re-claiming during the bounce" from "the single removal itself".
	static bool s_claimedOnce = false;
	if (s_claimedOnce && status != 0)
		return UsbdAddDeviceCompleteDetour.GetOriginal<decltype(&UsbdAddDeviceCompleteHook)>()(h, status);
#endif
	if (status != 0 && dd && id && h &&
		g_gipClaimAttempts < GIP_CLAIM_MAX_ATTEMPTS && GipFindFreeSession()) {
		uint16_t vid = detectedVid;
		uint16_t pid = detectedPid;
		const XboxInputControllerProfile* profile = detectedProfile;

		if (profile &&
			(!IsPowerA1414134(vid, pid) || !g_gipExt.deviceHandle)) {
			GipSessionSlot* primarySession = GipReservePrimarySession(
				profile, h, id->bInterfaceNumber, vid, pid);
			if (!primarySession)
				return UsbdAddDeviceCompleteDetour.GetOriginal<decltype(&UsbdAddDeviceCompleteHook)>()(h, status);
			XboxInputSetDiagStage(20);
			XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_CANDIDATE,
				((DWORD)vid << 16) | pid);

			g_gipClaimAttempts++;
			XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_CLAIM_BEGIN,
				g_gipClaimAttempts);
			XboxInputQueueLogEvent(XBOXINPUT_LOG_CONTROLLER_DETECTED,
				((DWORD)vid << 16) | pid,
				((DWORD)id->bInterfaceNumber << 8) | id->bNumEndpoints);
#ifdef XBOXINPUT_CLAIM_ONCE
			s_claimedOnce = true;
#endif
			// Breadcrumb #2. Promoted from XBOXINPUT_DBG: in the `noread` run nothing at all
			// appeared in xbWatson after a replug, and because these were compiled out
			// there was no way to tell "the dongle never re-enumerated" from "it was
			// re-claimed silently". Under `trace` the claim is always visible.
			XBOXINPUT_TRACE_LOG("XBOXINPUT: TRACE CLAIM ATTEMPT %d handle=%p\r\n",
				g_gipClaimAttempts, h);
			XBOXINPUT_DBG("XBOXINPUT: *** CLAIM ATTEMPT %d on GIP dongle (handle %p) ***\r\n",
				g_gipClaimAttempts, h);

			// Statically allocated rather than new'd: we do not know the IRQL this
			// callback runs at, and a failed allocation here would be a hang.
			InterlockedExchange(&g_powerAInitStage, POWERA_INIT_IDLE);
			InterlockedExchange(&g_powerAIdentifyComplete, 0);

			// -------------------------------------------------------------------
			// THE CLAIM. `claimonly` proved these two lines alone are sufficient to
			// freeze the console on removal - no endpoints, no transfers, no XAM,
			// no initialization, and it still dies.
			//
			// What they do is tell the USB core that a driver claimed this device,
			// when none did. The core was iterating drivers, every one declined, and
			// we convert that final failure into success while pointing h->driver at
			// a HidControllerExtension we fabricated. On removal the core hands the
			// device back to whichever driver it believes owns it - a driver that has
			// no record of it - and the console dies ~immediately (the 5 s to the D3D
			// banner is just its GPU watchdog timeout).
			//
			// Compare a legitimate claim, from the Phase 0.5b probe in docs/usb_stack.md:
			//     ADDCOMPLETE handle=E1EBF3B0 status=0x00000000 (CLAIMED) driver=801A87E0
			// Mass storage's driver pointer is 0x801A87E0 - kernel .data, a real driver
			// object. Ours points into the plugin at 0x81F0xxxx.
			//
			// XBOXINPUT_NO_CLAIM tests the obvious alternative: do not claim at all.
			// Let the core record the device as unclaimed exactly as it does with no
			// plugin loaded - the configuration that is PROVEN to survive removal, by
			// every one of ladder levels 0-6 and by the no-plugin control - and drive
			// the endpoints on the live handle anyway. The handle is valid here either
			// way; the open question is only whether the core permits endpoint
			// operations on a device it considers unowned.
			// -------------------------------------------------------------------
#if defined(XBOXINPUT_NO_CLAIM_LATE)
			// noclaim3: report NOTHING to the core yet. Open the endpoints and queue
			// SET_CONFIGURATION while the device is still mid-claim from the core's
			// point of view, and only report the failure status on the way out.
			//
			// `noclaim2` proved the core accepts the transfer but never services it once
			// the device is filed as unowned. The bet here is that servicing is decided
			// when the transfer is queued, not continuously - so a transfer queued before
			// the device is written off may still run, and its completion chain
			// (interrupt endpoints, reads, auth) may keep running with it.
			int r = 0;
#elif defined(XBOXINPUT_NO_CLAIM)
			int r = UsbdAddDeviceCompleteDetour.GetOriginal<decltype(&UsbdAddDeviceCompleteHook)>()(h, status);
			XBOXINPUT_LOG("XBOXINPUT: NOT claiming - passed original status 0x%08X through, "
				"driver stays %p\r\n", status, h->driver);
#elif defined(XBOXINPUT_KEEP_DRIVER)
			// Claim the device - so the core keeps scheduling transfers for it - but do
			// NOT overwrite h->driver. The fabricated extension was the fatal half of
			// the old claim; completing with status 0 by itself may be harmless.
			//
			// We can afford this because NOTHING of ours ever reads h->driver:
			//   - GipInterruptComplete recovers the extension from the TRB address
			//   - GipSetConfigComplete does the same from controlTrb - 36
			//   - UsbdRemoveDeviceCompleteHook compares h against g_gipExt.deviceHandle
			// g_gipExt is a static we own outright; the handle never needed to point at it.
			//
			// The noclaim run showed the core writes its own value here (E1EBF3D0) on the
			// unclaimed path, so leaving the field alone keeps whatever the core expects
			// to find rather than replacing it with a pointer into our plugin.
			void* before = h->driver;
			int r = UsbdAddDeviceCompleteDetour.GetOriginal<decltype(&UsbdAddDeviceCompleteHook)>()(h, 0);
			XBOXINPUT_LOG("XBOXINPUT: claimed WITHOUT touching driver - was %p, now %p\r\n",
				before, h->driver);
			XboxInputSetDiagStage(30);
#else
			h->driver = &g_gipExt;

			int r = UsbdAddDeviceCompleteDetour.GetOriginal<decltype(&UsbdAddDeviceCompleteHook)>()(h, 0);
#endif
			XBOXINPUT_DBG("XBOXINPUT: claim AddDeviceComplete(status=0) returned 0x%08X, driver now=%p\r\n",
				r, h->driver);
			XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_CLAIM_COMPLETE, r);

#ifdef XBOXINPUT_CLAIM_ONLY
			// L7-claimonly: take the device and stop. No default endpoint, no
			// SET_CONFIGURATION, no interrupt endpoints, no transfers ever.
			//
			// This is the last split available. `noread` proved the read loop is not
			// the cause but still had all three endpoints open; if this survives, the
			// fault is in OPENING endpoints on a device we claimed this way. If it
			// freezes, the bare claim is sufficient and the problem is that the core
			// believes a driver owns a device that has none.
			XBOXINPUT_LOG("XBOXINPUT: claimed and stopped (claimonly variant)\r\n");
			return r;
#endif
			NTSTATUS s = UsbdOpenDefaultEndpoint(h, (DWORD*)&g_gipExt.controlTrb);
			XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_DEFAULT_ENDPOINT_OPEN, s);
			// XBOXINPUT_LOG, not XBOXINPUT_DBG: under NO_CLAIM this is the whole question - whether
			// the core will open an endpoint on a device it considers unowned.
			XBOXINPUT_LOG("XBOXINPUT: UsbdOpenDefaultEndpoint -> 0x%08X %s\r\n",
				s, NT_ERROR(s) ? "FAILED" : "OK");
			XboxInputSetDiagStage(40);
			if (NT_ERROR(s))
				return r;

			// Bring the device up. Per the captured enumeration
			// (docs/GIP protocol notes section 5) SET_CONFIGURATION is the last control
			// transfer; everything after it is GIP over the interrupt endpoints, and the
			// device then sends ANNOUNCE (0x02) unprompted.
			int32_t q = SendControlRequest(h, &g_gipExt.controlTrb,
				0x00,   // host->device, standard, device
				0x09,   // SET_CONFIGURATION
				1,      // bConfigurationValue
				0, 0, nullptr,
				(DWORD)GipSetConfigComplete);
			XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_SET_CONFIG_QUEUED, q);
			// NOT an NTSTATUS. This returns a handle-like value (observed 0xE1EBF3C0,
			// i.e. the device handle) on BOTH the claimed run, where SET_CONFIGURATION
			// then completed normally, and the unclaimed run, where it never completed.
			// So the queue call accepts the transfer either way, and the difference is
			// purely whether the core ever SERVICES it. Do not read this as success or
			// failure - it is only here to prove the call was reached and returned.
			XBOXINPUT_LOG("XBOXINPUT: SET_CONFIGURATION queued -> 0x%08X (not a status)\r\n", q);

#ifdef XBOXINPUT_NO_CLAIM_LATE
			// Only now tell the core the device was not claimed - after our endpoints
			// are open and the control transfer is already queued.
			r = UsbdAddDeviceCompleteDetour.GetOriginal<decltype(&UsbdAddDeviceCompleteHook)>()(h, status);
			XBOXINPUT_LOG("XBOXINPUT: deferred unclaim - reported 0x%08X after setup, driver=%p\r\n",
				status, h->driver);
#endif
			return r;
		}
	}

	return UsbdAddDeviceCompleteDetour.GetOriginal<decltype(&UsbdAddDeviceCompleteHook)>()(h, status);
}

NTSTATUS UsbdOpenDefaultEndpointHook(deviceHandle* h, DWORD* ep) {
	ProbeLog(4, "UsbdOpenDefaultEndpoint", h);
	return UsbdOpenDefaultEndpointDetour.GetOriginal<decltype(&UsbdOpenDefaultEndpointHook)>()(h, ep);
}

// ---------------------------------------------------------------------------
// NOTHING IN THIS FUNCTION MAY CALL DbgPrint IN A DEFAULT BUILD.
//
// This runs inside the kernel's USB device-removal completion. DbgPrint goes out
// over xbdm, which takes a lock and does network I/O - already proven twice on this
// console to be able to hang it outright when called from a hot path.
//
// It is the only thing every freezing configuration ever shared, and the only
// hypothesis consistent with all six observations:
//
//   killtest (VERBOSE, never claims) - ProbeLog DbgPrint here -> FROZE
//   noreset  (no USB reset/patches)  - XBOXINPUT_LOG DbgPrints here  -> FROZE
//   giponly  (no HID detours/thread) - XBOXINPUT_LOG DbgPrints here  -> FROZE
//   nonotify (no XAM notify patches) - XBOXINPUT_LOG DbgPrints here  -> FROZE
//   fixremove(h->driver detached)    - XBOXINPUT_LOG DbgPrints here  -> FROZE
//   NO PLUGIN                        - nothing prints here    -> SURVIVES
//
// Every earlier hypothesis (bugcheck patches, USB reset, HID detours, mapping
// thread, notification patches, the dangling h->driver) was disproven by a build
// that removed it and froze anyway - and every one of those builds still logged
// from in here.
//
// All logging on this path is therefore XBOXINPUT_DBG, which compiles to nothing unless
// XBOXINPUT_VERBOSE is defined. If you need to trace teardown, set a flag here and
// print it later from a safe context - do not print from inside this call.
// ---------------------------------------------------------------------------
NTSTATUS UsbdRemoveDeviceCompleteHook(deviceHandle* h) {
	// Breadcrumb #3. If a freeze produces NO "REMOVE ENTER" line, the hang happens
	// BEFORE this function is ever reached - i.e. in whatever the USB core dispatches
	// to first on removal, walking the g_gipExt we handed it at claim time. That would
	// explain why `passive` (which does nothing here) froze identically, and it would
	// mean no amount of work inside this function can help.
	XBOXINPUT_TRACE_LOG("XBOXINPUT: TRACE REMOVE ENTER h=%p ours=%d\r\n",
		h, (h && h == g_gipExt.deviceHandle) ? 1 : 0);
#ifdef XBOXINPUT_PASSIVE_REMOVE
	// L7-passive: claim and drive the device exactly as normal, but do NOTHING on
	// removal — no state reset, no XAM unregister, no endpoint work, just hand
	// straight to the kernel. Leaks a stale extension by design; this build is a
	// probe, not a candidate. If the freeze survives this, teardown is not the cause.
	return UsbdRemoveDeviceCompleteDetour.GetOriginal<decltype(&UsbdRemoveDeviceCompleteHook)>()(h);
#else
	ProbeLog(5, "UsbdRemoveDeviceComplete", h);

	// Additional-controller sessions follow the same removal rule as the proven
	// primary path: stop all plugin traffic, detach the fabricated extension and
	// return without entering the kernel's owner-removal path.  The slot remains
	// reserved until reboot; reusing its asynchronous TRBs during this removal
	// window is precisely the corruption pattern this refactor is avoiding.
	if (h) {
		GipSessionSlot* session = 0;
		for (int i = 0; i < GIP_MAX_SESSIONS; ++i) {
			if (g_gipSessions[i].reserved && !g_gipSessions[i].primary &&
				g_gipSessions[i].ext.deviceHandle == h) {
				session = &g_gipSessions[i];
				break;
			}
		}
		if (session) {
			const uint8_t removedUser = session->runtime.playerIndex;
			XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_REMOVE_BEGIN,
				removedUser);
			XboxInputRuntimeSetReady(&session->runtime, false);
			session->runtime.guidePending = false;
			session->outOpen = false;
			session->ext.deviceHandle = 0;
			session->ext.cleanupDone = 1;
			h->driver = 0;
			if (session->runtime.playerIndex != 0xFF) {
				int idx = (int)(session->runtime.deviceContext - 0x10000005);
				XamUserBindDeviceCallback(0xa7553952 + idx,
					session->runtime.deviceContext, 0, true, 0);
			}
			XboxInputRetireRuntime(&session->runtime);
			if (removedUser != 0xFF)
				XboxInputQueueRemovedControllerNotification(removedUser);
			if (removedUser != 0xFF)
				XboxInputQueueLogEvent(XBOXINPUT_LOG_CONTROLLER_REMOVED, removedUser, 0);
			XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_REMOVE_COMPLETE,
				removedUser);
			return 0;
		}
	}

	// ---------------------------------------------------------------------
	// CLEANUP for our side-door claim.
	//
	// hiddriver360 tears down HID devices in HidRemoveDeviceHook - but that hook
	// lives in the HID driver and is NEVER called for our GIP device, for the same
	// reason HidAddDeviceHook isn't (docs/usb_stack.md Phase 0.5). So nothing was
	// cleaning up after us, and GipInterruptComplete kept re-arming
	// UsbdQueueAsyncTransfer on a handle the kernel had already destroyed - a
	// use-after-free in a completion callback, re-armed forever. That is the hard
	// freeze seen on controller disconnect.
	//
	// Stop the read loop FIRST, then release our references.
	// ---------------------------------------------------------------------
	if (h && h == g_gipExt.deviceHandle) {
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_REMOVE_BEGIN,
			g_gipRuntime.playerIndex);
		XBOXINPUT_DBG("XBOXINPUT: *** device removed - tearing down GIP state ***\r\n");

		// Stop presenting as a live controller before releasing USB state.

		// 1. Stop the re-arm loop and the send path. Null the handle before anything
		//    else so a completion that fires mid-teardown cannot re-arm.
		//    Keep a local copy - the close calls below still need it.
		deviceHandle* dead = g_gipExt.deviceHandle;
		g_gipExt.deviceHandle = 0;
		g_gipVendorId = 0;
		g_gipProductId = 0;
		InterlockedExchange(&g_powerAInitStage, POWERA_INIT_IDLE);
		InterlockedExchange(&g_powerAIdentifyComplete, 0);
		g_gipOutOpen = false;

		// 2. Do NOT close the endpoints here.
		//
		//    `[VERIFIED]` by control test 2026-08-08: a wired PS5 controller plugs and
		//    unplugs cleanly with this same plugin loaded, so the removal detour and the
		//    kernel's own removal path are both fine. The difference is WHEN and WHERE
		//    each driver tears down:
		//
		//      PS5 / any HID device -> HidRemoveDeviceHook. Runs EARLY, at the HID
		//        driver layer, BEFORE the kernel's Usbd removal. Closes no kernel
		//        endpoints at all - it frees its own extension and returns 0
		//        (main.cpp:1269-1275). Survives.
		//
		//      Our GIP device -> here, inside UsbdRemoveDeviceComplete. This runs
		//        WHILE THE KERNEL IS ALREADY DESTROYING THE DEVICE. Calling
		//        UsbdQueueCloseEndpoint on `dead` at this point operates on endpoint
		//        structures the kernel has torn down or is tearing down.
		//
		//    Nulling deviceHandle above already stops the re-arm loop, which was the
		//    original reason for cleaning up here. The endpoints belong to a device the
		//    kernel is destroying anyway, so it reclaims them - there is nothing to leak.
		//
		//    Set XBOXINPUT_CLOSE_ENDPOINTS_ON_REMOVE to restore the old behaviour.
#ifdef XBOXINPUT_CLOSE_ENDPOINTS_ON_REMOVE
		if (UsbdQueueCloseEndpoint) {
			UsbdQueueCloseEndpoint(dead, &g_gipExt.interruptTrb);
			UsbdQueueCloseEndpoint(dead, &g_gipOutTrb);
		}
		if (UsbdQueueCloseDefaultEndpoint)
			UsbdQueueCloseDefaultEndpoint(dead, (DWORD*)&g_gipExt.controlTrb);
#endif
		XBOXINPUT_DBG("XBOXINPUT: endpoints closed\r\n");

		// 3. Mark cleanup done, then DETACH our extension from the handle.
		//
		//    This is where the disconnect freeze lived. The previous version left
		//    h->driver pointing at g_gipExt and then called the original, reasoning
		//    that the kernel needed "a driver to tear down". That is backwards:
		//
		//      - g_gipExt is a STATIC struct of ours. It is not a heap-allocated
		//        kernel device extension. Upstream's own remove path does
		//        `delete deviceHandle2->driver` (main.cpp:1269) - so the teardown
		//        path this pointer feeds expects heap memory and real contents.
		//      - Evidence from the console: the last line ever logged is
		//        "teardown done, ready for replug" (below), and the very next
		//        statement is the call to the original. The hang is inside the
		//        kernel's UsbdRemoveDeviceComplete, walking this pointer.
		//      - With NO plugin loaded at all, the dongle is never claimed, so
		//        h->driver is NULL when the kernel removes it - and the console
		//        SURVIVES the disconnect. Nulling it here reproduces exactly that
		//        known-good state.
		g_gipExt.cleanupDone = 1;
		dead->driver = 0;

		// 4. Release the XAM virtual controller.
		//    This was MISSING: the controller stayed registered after the device was
		//    gone, so XAM kept a controller bound to a dead device indefinitely.
		const uint8_t removedUser = g_gipRuntime.playerIndex;
		GipUnregisterFromXam();
		XboxInputRetireRuntime(&g_gipRuntime);
		if (removedUser != 0xFF)
			XboxInputQueueRemovedControllerNotification(removedUser);
		if (removedUser != 0xFF)
			XboxInputQueueLogEvent(XBOXINPUT_LOG_CONTROLLER_REMOVED, removedUser, 0);

		// 5. Reset the session so a replug starts clean rather than resuming
		//    half-initialised state.
			g_gipIdentifySent = false;
			g_gipIdentifyReplySeen = false;
			g_gipLastIdentifyTick = 0;
			g_gipPoweredOn = false;
		g_gipGuideOverlayOpen = false;
		g_gipChunkTotal = 0;
		// g_gipClaimAttempts is deliberately NOT reset here. Refilling the budget on
		// every teardown let a disconnect bounce-storm re-claim without limit, which is
		// what re-queues a TRB the kernel still owns. Only a connection that reaches
		// A fully ready connection refills it. See GIP_CLAIM_MAX_ATTEMPTS.
		g_gipPacketsSeen = 0;
		g_gipInputsSeen = 0;
		g_gipSeq = 1;
		g_gipReadErrors = 0;
		g_gipReadLoopStopped = false;
		g_gipCapsLogged = 0;
		g_gipCaps2Logged = 0;

		XBOXINPUT_DBG("XBOXINPUT: teardown done, ready for replug\r\n");

		// ===================================================================
		// THE DISCONNECT FIX. `[VERIFIED on hardware 2026-08-08]`
		//
		// Clean up and return WITHOUT calling the original, for our device only.
		// Every other device still takes the kernel's normal path below - the trace
		// confirms `ours=0` handles get PRE-ORIG/POST-ORIG as usual.
		//
		// This is the same shape upstream uses for HID devices: HidRemoveDeviceHook
		// frees its extension and returns 0 without calling through (main.cpp:1275).
		//
		// How it was found, so nobody re-litigates it. An additive build ladder showed
		// levels 0-6 - all of stock hiddriver360 - survive removal, and only L7 froze.
		// Then `claimonly`, whose entire contribution is:
		//     h->driver = &g_gipExt;
		//     UsbdAddDeviceComplete(h, 0);
		// froze with no endpoints, no transfers, no XAM and no initialization. And `keepdriver`,
		// which claims but never writes h->driver (it stayed 00000000), froze too. So
		// the fatal act is reporting the claim, not the fabricated pointer: the core
		// then believes a driver owns a device that driver never registered, and on
		// removal it hands it back to that owner.
		//
		// `noclaim` (report the failure status through) survives removal perfectly but
		// leaves the device dead - the core accepts transfers for an unowned device and
		// never services them, so SET_CONFIGURATION never completes.
		//
		// So: claim it, drive it, and then simply never tell the core the removal
		// finished. The console survives an unplug, survives the controller powering off,
		// and re-claims cleanly on replug (verified twice in one boot, handles
		// E1EBF3C0 then E1EBF3E0, full initialization and XAM registration both times).
		//
		// KNOWN COST: the core never completes teardown of that device object, so a
		// handle pair is consumed per plug cycle. Bounded and slow, but real - see
		// docs/HOW_IT_WORKS.md and docs/KNOWN_ISSUES.md. Define XBOXINPUT_REMOVE_CALL_ORIGINAL to get the
		// old (freezing) behaviour back for testing.
		// ===================================================================
#ifndef XBOXINPUT_REMOVE_CALL_ORIGINAL
		XBOXINPUT_DBG("XBOXINPUT: skipping kernel removal path\r\n");
		XboxInputQueueLogEvent(XBOXINPUT_LOG_USB_STEP, XBOXINPUT_USB_REMOVE_COMPLETE,
			removedUser);
		return 0;
#endif
	}

	// h->driver has been detached above for our device, so the kernel takes the same
	// path it takes for any unclaimed device - the path that is known to survive.
	XBOXINPUT_TRACE_LOG("XBOXINPUT: TRACE REMOVE PRE-ORIG h=%p\r\n", h);
	NTSTATUS rr = UsbdRemoveDeviceCompleteDetour.GetOriginal<decltype(&UsbdRemoveDeviceCompleteHook)>()(h);
	XBOXINPUT_TRACE_LOG("XBOXINPUT: TRACE REMOVE POST-ORIG h=%p -> 0x%08X\r\n", h, rr);
	return rr;
#endif // XBOXINPUT_PASSIVE_REMOVE
}

static void InstallUsbProbes() {
	// Resolved pointers come from initFunctionPointers(); bail on any that are null
	// rather than detouring address 0.
	// Only TWO of these are load-bearing:
	//   UsbdAddDeviceComplete    - where we claim the dongle
	//   UsbdRemoveDeviceComplete - where we tear down
	// The other four existed purely to answer the Phase 0.5b question of which kernel
	// USB exports fire for a non-HID device (docs/usb_stack.md). That question is
	// answered, so they are compiled out by default - four fewer kernel detours is
	// four fewer things that can go wrong in a driver that now actually gets used.
	struct { void* target; const void* hook; Detour* det; const char* name; } probes[] = {
		{ (void*)UsbdAddDeviceComplete,      (void*)UsbdAddDeviceCompleteHook,      &UsbdAddDeviceCompleteDetour,      "UsbdAddDeviceComplete" },
		{ (void*)UsbdRemoveDeviceComplete,   (void*)UsbdRemoveDeviceCompleteHook,   &UsbdRemoveDeviceCompleteDetour,   "UsbdRemoveDeviceComplete" },
#ifdef XBOXINPUT_VERBOSE
		{ (void*)UsbdGetDeviceDescriptor,    (void*)UsbdGetDeviceDescriptorHook,    &UsbdGetDeviceDescriptorDetour,    "UsbdGetDeviceDescriptor" },
		{ (void*)UsbdGetInterfaceDescriptor, (void*)UsbdGetInterfaceDescriptorHook, &UsbdGetInterfaceDescriptorDetour, "UsbdGetInterfaceDescriptor" },
		{ (void*)UsbdGetDeviceSpeed,         (void*)UsbdGetDeviceSpeedHook,         &UsbdGetDeviceSpeedDetour,         "UsbdGetDeviceSpeed" },
		{ (void*)UsbdOpenDefaultEndpoint,    (void*)UsbdOpenDefaultEndpointHook,    &UsbdOpenDefaultEndpointDetour,    "UsbdOpenDefaultEndpoint" },
#endif
	};

	for (int i = 0; i < (sizeof(probes) / sizeof(probes[0])); i++) {
		if (!probes[i].target) {
			XBOXINPUT_DBG("XBOXINPUT: PROBE SKIP %s - null pointer\r\n", probes[i].name);
			continue;
		}
		*probes[i].det = Detour(probes[i].target, probes[i].hook);
		probes[i].det->Install();
		XBOXINPUT_DBG("XBOXINPUT: PROBE installed on %s @ %p\r\n", probes[i].name, probes[i].target);
	}
}

int reportData = 0;
int HidAddDeviceHook(deviceHandle* deviceHandle) {
	DbgPrint("EINTIM: HID add device %p\n", deviceHandle);
	usb_device_descriptor* device_descriptor = UsbdGetDeviceDescriptor(deviceHandle);
	usb_interface_descriptor* interface_descriptor = UsbdGetInterfaceDescriptor(deviceHandle);

	// Kill test: log EVERY device that gets here, before any class filtering.
	KtLogDevice(deviceHandle, device_descriptor, interface_descriptor);

	uint16_t vendorId = swap_endianness_16(device_descriptor->idVendor);
	uint16_t productId = swap_endianness_16(device_descriptor->idProduct);

	int speed = UsbdGetDeviceSpeed(deviceHandle);
	bool isOhci = speed == 0;

	DbgPrint("EINTIM: IS USB1.0: %d\n", isOhci);
	DbgPrint("EINTIM: USB device descriptor Pointer: %p\n", device_descriptor);
	DbgPrint("EINTIM: HID device vendor id: %x, product id: %x\n", vendorId, productId);

	if (interface_descriptor->bInterfaceClass == 0x03 &&
		interface_descriptor->bInterfaceSubClass == 0 &&
		interface_descriptor->bInterfaceProtocol == 0) {
		DbgPrint("EINTIM: Controller detected. Initialising custom handler.\n");
		
		// Extract HID descriptor from memory right after interface descriptor
		BYTE* hid_descriptor_ptr = ((BYTE*)interface_descriptor) + interface_descriptor->bLength;
		usb_hid_descriptor* hid_descriptor = (usb_hid_descriptor*)hid_descriptor_ptr;
		
		DbgPrint("EINTIM: Found HID descriptor at offset %d: type=%02x, length=%d\n",
			interface_descriptor->bLength, hid_descriptor->bDescriptorType, hid_descriptor->wDescriptorLength);
		
		if (hid_descriptor->bDescriptorType != 0x21) {
			DbgPrint("EINTIM: ERROR - Invalid HID descriptor type %02x!\n", hid_descriptor->bDescriptorType);
			XBOXINPUT_DBG("XBOXINPUT: DROP REASON = no valid HID descriptor (0x21) after interface descriptor\r\n");
			return HidAddDeviceDetour.GetOriginal<decltype(&HidAddDeviceHook)>()(deviceHandle);
		}
		
		int index = -1;
		for (int i = 0; i < (sizeof(connectedControllers) / sizeof(Controller)); i++) {
			if (!connectedControllers[i].controllerDriver) {
				DbgPrint("Assigning controller to index %d\n", i);
				index = i;
				break;
			}
		}

		if (index == -1) {
			DbgPrint("EINTIM: No free index!\n");
			XBOXINPUT_DBG("XBOXINPUT: DROP REASON = all 4 controller slots in use\r\n");
			return HidAddDeviceDetour.GetOriginal<decltype(&HidAddDeviceHook)>()(deviceHandle);
		}
		globalIndex = index;

		c = Controller();
		memset(&c, 0, sizeof(Controller));
		c.packetNumber = 0;
		c.reportInfo = nullptr;   // will be filled in INIT_GET_REPORT_DESCRIPTOR
		c.vendorId = vendorId;
		c.productId = productId;
		c.map = FindMapping(vendorId, productId);
		c.nintendo_handshake_state = NINTENDO_HANDSHAKE_STATE::INITIAL;

		HidControllerExtension* controllerDriver = new HidControllerExtension();
		c.deviceHandle = deviceHandle;
		controllerDriver->deviceType = 0;
		deviceHandle->driver = controllerDriver;
		controllerDriver->deviceHandle = deviceHandle;
		controllerDriver->interfaceNumber = interface_descriptor->bInterfaceNumber;
		DbgPrint("EINTIM: Storing interface number: %d\n", controllerDriver->interfaceNumber);
		controllerDriver->interruptTrb.flags = 1;

		// Copy HID descriptor to global buffer for later use
		memcpy(&hidDescriptorBuffer, hid_descriptor, sizeof(usb_hid_descriptor));
		DbgPrint("EINTIM: Copied HID descriptor. wDescriptorLength: %d\n", hidDescriptorBuffer.wDescriptorLength);

		UsbdAddDeviceComplete(deviceHandle, 0);

		NTSTATUS status = UsbdOpenDefaultEndpoint(deviceHandle, (DWORD*)&controllerDriver->controlTrb);
		if (NT_ERROR(status)) {
			DbgPrint("EINTIM: Failed to open control endpoint %x!\n", status);
			return status;
		}

		// Set device configuration (required for proper USB enumeration)
		g_InitState = InitState::INIT_SET_CONFIGURATION;
		DbgPrint("EINTIM: Sending SET_CONFIGURATION\n");
		SendControlRequest(
			controllerDriver->deviceHandle,
			&controllerDriver->controlTrb,
			0x00,
			0x09,
			1, 0, 0,
			nullptr,
			(DWORD)setConfigurationComplete);

		return 0;
	}

	DbgPrint("EINTIM: Unrelated USB Device. Calling original...\n");
	XBOXINPUT_DBG("XBOXINPUT: DROP REASON = interface is not HID (class/subclass/protocol != 03/00/00). "
		"Saw %02X/%02X/%02X. Device DID reach the hook.\r\n",
		interface_descriptor ? interface_descriptor->bInterfaceClass : 0xFF,
		interface_descriptor ? interface_descriptor->bInterfaceSubClass : 0xFF,
		interface_descriptor ? interface_descriptor->bInterfaceProtocol : 0xFF);
	return HidAddDeviceDetour.GetOriginal<decltype(&HidAddDeviceHook)>()(deviceHandle);
}

static void GipControllerSendRumble(const GipControllerRef* controller,
	BYTE left, BYTE right) {
	if (!controller)
		return;
	const XboxInputControllerProfile* profile = GipControllerProfile(controller);
	if (!profile || !(profile->capabilities & XBOXINPUT_CAP_RUMBLE))
		return;
	if (controller->kind == GIP_CONTROLLER_PRIMARY)
		GipSendGamepadRumble(g_gipExt.deviceHandle, left, right);
	else if (controller->kind == GIP_CONTROLLER_ADDITIONAL && controller->session)
		GipSessionSendRumble(controller->session, left, right);
}

static NTSTATUS GipControllerReadState(const GipControllerRef* controller,
	PDWORD packetNumber, PXINPUT_GAMEPAD inputData, PBOOL unk) {
	if (!controller || !inputData)
		return ERROR_INVALID_PARAMETER;

	GipGamepadState* state = 0;
	bool* guidePending = 0;
	uint32_t* lastGuideTick = 0;
	uint32_t* nextPacket = 0;
	uint8_t userIndex = 0xFF;
	if (controller->session) {
		state = &controller->session->runtime.state;
		guidePending = &controller->session->runtime.guidePending;
		lastGuideTick = &controller->session->runtime.lastGuideTick;
		nextPacket = &controller->session->runtime.packetNumber;
		userIndex = controller->session->runtime.playerIndex;
	}
	if (!state || !guidePending || !lastGuideTick || !nextPacket)
		return ERROR_DEVICE_NOT_CONNECTED;

	GipGamepadToXInput(state, inputData);
	if (*guidePending && g_xboxInputConfig.guideButton) {
		*guidePending = false;
		DWORD now = GetTickCount();
		if (now - *lastGuideTick >= (DWORD)g_xboxInputConfig.guideCooldownMs) {
			*lastGuideTick = now;
			XamInputSendXenonButtonPress(userIndex);
		}
	}
	if (packetNumber)
		*packetNumber = ++(*nextPacket);
	if (unk)
		*unk = FALSE;
	return STATUS_SUCCESS;
}

DWORD XamInputSetStateHook(DWORD user, DWORD flags, XINPUT_VIBRATION* vibration) {
	DWORD status = XamInputSetStateDetour.GetOriginal<decltype(&XamInputSetStateHook)>()(user, flags, vibration);

	if ((user & 0xFF) == 0xFF)
		user = 0;
	GipControllerRef controller;
	if (GipControllerFromUser((uint8_t)user, &controller)) {
		const XboxInputMappingOptions* mapping = XboxInputGetActiveMapping();
		const BYTE left = XboxInputScaleRumble(
			vibration ? (BYTE)(vibration->wLeftMotorSpeed >> 8) : 0,
			mapping->rumblePercent);
		const BYTE right = XboxInputScaleRumble(
			vibration ? (BYTE)(vibration->wRightMotorSpeed >> 8) : 0,
			mapping->rumblePercent);
		GipControllerSendRumble(&controller, left, right);
		return ERROR_SUCCESS;
	}

	if (status == ERROR_DEVICE_NOT_CONNECTED) {
		Controller* c = nullptr;
		for (int i = 0; i < (sizeof(connectedControllers) / sizeof(Controller)); i++) {
			if (connectedControllers[i].controllerDriver &&
				connectedControllers[i].userIndex == user) {
				c = &connectedControllers[i];
				break;
			}
		}
		if (!c)
			return status;
		return ERROR_SUCCESS;
	}

	return status;
}

DWORD XamInputGetCapabilitiesExHook(DWORD unk, DWORD user, DWORD flags, XINPUT_CAPABILITIES_EX* capabilities) {
	DWORD status = XamInputGetCapabilitiesDetour.GetOriginal<decltype(&XamInputGetCapabilitiesExHook)>()(unk, user, flags, capabilities);

	if ((user & 0xFF) == 0xFF)
		user = 0;

	if (!capabilities)
		return status;
	GipControllerRef controller;
	if (GipControllerFromUser((uint8_t)user, &controller)) {
		// Rate limited: the dash/game polls capabilities many times per second, and
		// logging every call floods xbdm and hangs the console.
		if (g_gipCapsLogged < 3) {
			g_gipCapsLogged++;
			XBOXINPUT_DBG("XBOXINPUT: XamInputGetCapabilitiesEx(user=%d) -> GAMEPAD\r\n", user);
		}
		GipFillGamepadCaps(capabilities->Type, capabilities->SubType,
			capabilities->Flags, capabilities->Gamepad);
		capabilities->Vibration.wLeftMotorSpeed = 0;
		capabilities->Vibration.wRightMotorSpeed = 0;
		return ERROR_SUCCESS;
	}

	if (status == ERROR_DEVICE_NOT_CONNECTED) {
		Controller* c = nullptr;
		for (int i = 0; i < (sizeof(connectedControllers) / sizeof(Controller)); i++) {
			if (connectedControllers[i].controllerDriver &&
				connectedControllers[i].userIndex == user) {
				c = &connectedControllers[i];
				break;
			}
		}
		if (!c)
			return status;

		capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
		capabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
		capabilities->Flags = 0;

		XINPUT_STATE state;
		memset(&state, 0, sizeof(XINPUT_STATE));
		XInputGetState(user, &state);
		capabilities->Gamepad = state.Gamepad;
		capabilities->Vibration.wLeftMotorSpeed = 0;
		capabilities->Vibration.wRightMotorSpeed = 0;
		return ERROR_SUCCESS;
	}

	// UPSTREAM BUG, inherited verbatim (verified against 6159866: the function ends
	// right here with no return). Every call about a REAL, CONNECTED controller —
	// which is the overwhelmingly common case, ~8 per 100 ms from the dash — falls off
	// the end of a non-void function, so the caller reads whatever r3 happens to hold.
	// It has worked by accident because MSVC leaves `status` in r3 on this path, but
	// that is a register-allocation coincidence, not a guarantee, and it changes with
	// any edit to the function.
	return status;
}

//
// ---- the plain capability entry point hiddriver360 does not hook --------------
// Xenia's export table (refs/xenia/src/xenia/kernel/xam/xam_table.inc:197-199, 481)
// lists FOUR separate input entry points, and upstream only detours two of them:
//
//   0x190 = 400  XamInputGetCapabilities      <- NOT hooked upstream
//   0x192 = 402  XamInputSetState             <- hooked
//   0x2AD = 685  XamInputGetCapabilitiesEx    <- hooked
//
// Different titles use different capability exports, so both paths are covered.
//
// This also independently cross-checks our ordinal table (docs/xam_api.md): Xenia and
// hiddriver360 agree on 402 and 685.
//
DWORD XamInputGetCapabilitiesHook(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
	DWORD status = XamInputGetCapabilitiesDetour2
		.GetOriginal<decltype(&XamInputGetCapabilitiesHook)>()(user, flags, caps);

	if ((user & 0xFF) == 0xFF)
		user = 0;
	if (!caps)
		return status;
	GipControllerRef controller;
	if (GipControllerFromUser((uint8_t)user, &controller)) {
		if (g_gipCaps2Logged < 3) {
			g_gipCaps2Logged++;
			XBOXINPUT_DBG("XBOXINPUT: XamInputGetCapabilities(user=%d flags=%d) -> GAMEPAD\r\n",
				user, flags);
		}
		GipFillGamepadCaps(caps->Type, caps->SubType, caps->Flags, caps->Gamepad);
		caps->Vibration.wLeftMotorSpeed = 0;
		caps->Vibration.wRightMotorSpeed = 0;
		return ERROR_SUCCESS;
	}
	return status;
}

static DWORD XboxInputTitleUiWorker(PVOID parameter) {
	DWORD workerTitle = (DWORD)(ULONG_PTR)parameter;
	typedef DWORD(*xam_get_current_title_id_t)(void);
	bool notificationActive = false;
	bool cancellationIssued = false;
	DWORD notificationStarted = 0;
	LONG pendingRemoved = 0;
	BOOL savedShow = TRUE, savedMovie = TRUE, savedSound = TRUE, savedIptv = TRUE;
	DWORD lastMappingReloadCheck = 0;
	for (;;) {
		if (!XamGetCurrentTitleIdPtr ||
			((xam_get_current_title_id_t)XamGetCurrentTitleIdPtr)() != workerTitle)
			break;

		const DWORD now = GetTickCount();
		if ((DWORD)(now - lastMappingReloadCheck) >= 500) {
			lastMappingReloadCheck = now;
			FILE* request = fopen(XBOXINPUT_RELOAD_PATH, "r");
			if (request) {
				fclose(request);
				XboxInputMappingOptions reloaded;
				if (XboxInputLoadGamepadMapping(XBOXINPUT_CFG_PATH, &reloaded)) {
					XboxInputPublishMapping(&reloaded);
					XBOXINPUT_LOG("XBOXINPUT: mapping reloaded on request\r\n");
				}
				remove(XBOXINPUT_RELOAD_PATH);
			}
		}

		pendingRemoved |= InterlockedExchange(&g_xboxInputRemovedUiMask, 0);
		if (pendingRemoved && !notificationActive && g_xboxInputDisconnectedUiMask != 0) {
			XNotifyUIGetOptions(&savedShow, &savedMovie, &savedSound, &savedIptv);
			// A reconnect warning must remain visible even when ordinary toast
			// previews were disabled. Restore the user's preference afterwards.
			XNotifyUISetOptions(TRUE, savedMovie, savedSound, savedIptv);
			// Persistent priority gives us a supported cancellation path. The worker
			// still enforces the former seven-second maximum below.
			XNotifyQueueUI(XNOTIFYUI_TYPE_CONSOLEMESSAGE, XUSER_INDEX_ANY,
				XNOTIFYUI_PRIORITY_PERSISTENT, L"Please reconnect controller", 0);
			notificationStarted = GetTickCount();
			notificationActive = true;
			cancellationIssued = false;
			pendingRemoved = 0;
		}

		if (notificationActive) {
			const LONG disconnected = g_xboxInputDisconnectedUiMask;
			const DWORD elapsed = (DWORD)(GetTickCount() - notificationStarted);
			const bool timedOut = elapsed >= 7000;
			if (!cancellationIssued && (disconnected == 0 || timedOut)) {
				// The cancel request must use the same UI area/priority value as the
				// notification it targets. Using DEFAULT here was ignored on 17559.
				XNotifyQueueUI(XNOTIFYUI_TYPE_CANCELPERSISTENT, XUSER_INDEX_ANY,
					XNOTIFYUI_PRIORITY_PERSISTENT, 0, 0);
				XboxInputQueueLogEvent(XBOXINPUT_LOG_NOTIFICATION_CANCELLED,
					timedOut ? 1 : 0, disconnected);
				cancellationIssued = true;
			}
			// Even after requesting cancellation, do not restore pfShow until the
			// original toast plus its exit animation must have expired. If 17559
			// ignores cancellation this preserves the validated complete animation;
			// restoring after two seconds was what froze the previous test build.
			if (elapsed >= 9000) {
				XNotifyUISetOptions(savedShow, savedMovie, savedSound, savedIptv);
				notificationActive = false;
				cancellationIssued = false;
				if (g_xboxInputDisconnectedUiMask == 0)
					pendingRemoved = 0;
			}
		}
		Sleep(50);
	}
	if (notificationActive) {
		XNotifyQueueUI(XNOTIFYUI_TYPE_CANCELPERSISTENT, XUSER_INDEX_ANY,
			XNOTIFYUI_PRIORITY_PERSISTENT, 0, 0);
		const DWORD elapsed = (DWORD)(GetTickCount() - notificationStarted);
		if (elapsed < 9000)
			Sleep(9000 - elapsed);
		XNotifyUISetOptions(savedShow, savedMovie, savedSound, savedIptv);
	}
	if ((DWORD)g_xboxInputUiWorkerTitle == workerTitle)
		InterlockedExchange(&g_xboxInputUiWorkerRunning, 0);
	return 0;
}

static void XboxInputEnsureTitleUiWorker() {
	if (!XamGetCurrentTitleIdPtr || KeGetCurrentProcessType() != PROC_USER)
		return;
	typedef DWORD(*xam_get_current_title_id_t)(void);
	DWORD title = ((xam_get_current_title_id_t)XamGetCurrentTitleIdPtr)();
	if ((DWORD)g_xboxInputUiWorkerTitle != title) {
		InterlockedExchange(&g_xboxInputUiWorkerTitle, (LONG)title);
		InterlockedExchange(&g_xboxInputUiWorkerRunning, 0);
	}
	if (InterlockedCompareExchange(&g_xboxInputUiWorkerRunning, 1, 0) != 0)
		return;
	HANDLE thread = CreateThread(0, 0,
		(LPTHREAD_START_ROUTINE)XboxInputTitleUiWorker,
		(PVOID)(ULONG_PTR)title, 0, 0);
	if (thread)
		CloseHandle(thread);
	else
		InterlockedExchange(&g_xboxInputUiWorkerRunning, 0);
}

// Lazily establish a title-owned watcher while the game is still polling.
// It remains alive after the disconnect causes the game to stop polling.
HRESULT XamInputGetStateHook(DWORD user, DWORD deviceContext, XINPUT_STATE* state) {
	XboxInputEnsureTitleUiWorker();
	HRESULT status = XamInputGetStateDetour
		.GetOriginal<decltype(&XamInputGetStateHook)>()(user, deviceContext, state);
	return status;
}

NTSTATUS XInputdReadStateHook(DWORD dwDeviceContext, PDWORD pdwPacketNumber, PXINPUT_GAMEPAD pInputData, PBOOL unk) {
	GipControllerRef controller;
	if (GipControllerFromContext(dwDeviceContext, &controller))
		return GipControllerReadState(&controller, pdwPacketNumber, pInputData, unk);

	if (dwDeviceContext >= 0x0000000010000005) {
		if (!pInputData)
			return ERROR_INVALID_PARAMETER;

		static DWORD lastPressTime = 0;
		static const DWORD cooldownDuration = 1000;

		ButtonsReport b;
		Controller* c = nullptr;
		for (int i = 0; i < (sizeof(connectedControllers) / sizeof(Controller)); i++) {
			if (connectedControllers[i].controllerDriver &&
				connectedControllers[i].deviceContext == dwDeviceContext) {
				c = &connectedControllers[i];
				b = connectedControllers[i].currentState;
				break;
			}
		}

		if (!c)
			return ERROR_INVALID_PARAMETER;

		if (b.xbox) {
			DWORD now = GetTickCount();
			if (now - lastPressTime >= cooldownDuration) {
				lastPressTime = now;
				XamInputSendXenonButtonPress(c->userIndex);
			}
		}

		if (b.a_button)    pInputData->wButtons |= XINPUT_GAMEPAD_A;
		if (b.b_button)   pInputData->wButtons |= XINPUT_GAMEPAD_B;
		if (b.y_button) pInputData->wButtons |= XINPUT_GAMEPAD_Y;
		if (b.x_button)   pInputData->wButtons |= XINPUT_GAMEPAD_X;
		if (b.start)    pInputData->wButtons |= XINPUT_GAMEPAD_START;
		if (b.back)     pInputData->wButtons |= XINPUT_GAMEPAD_BACK;
		if (b.r3)       pInputData->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
		if (b.l3)       pInputData->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
		if (b.l1)       pInputData->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
		if (b.r1)       pInputData->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;

		if (b.has_hat_switch) {
			switch (b.hatSwitch) {
			case HatSwitch::HAT_UP:
				pInputData->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
				break;
			case HatSwitch::HAT_UP_RIGHT:
				pInputData->wButtons |= XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT;
				break;
			case HatSwitch::HAT_RIGHT:
				pInputData->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
				break;
			case HatSwitch::HAT_DOWN_RIGHT:
				pInputData->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT;
				break;
			case HatSwitch::HAT_DOWN:
				pInputData->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
				break;
			case HatSwitch::HAT_DOWN_LEFT:
				pInputData->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT;
				break;
			case HatSwitch::HAT_LEFT:
				pInputData->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
				break;
			case HatSwitch::HAT_UP_LEFT:
				pInputData->wButtons |= XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT;
				break;
			case HatSwitch::HAT_NEUTRAL:
				break;
			}
		}
		else {
			if(b.dpad_left)
				pInputData->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
			if(b.dpad_right)
				pInputData->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
			if(b.dpad_up)
				pInputData->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
			if(b.dpad_down)
				pInputData->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
		}
		
		pInputData->sThumbRX = b.z;
		pInputData->sThumbRY = b.rz;
		pInputData->sThumbLX = b.x;
		pInputData->sThumbLY = b.y;
		pInputData->bLeftTrigger = b.rx ? b.rx : (b.l2 ? 255 : 0);
		pInputData->bRightTrigger = b.ry ? b.ry : (b.r2 ? 255 : 0);

		if (pdwPacketNumber)
			*pdwPacketNumber = ++c->packetNumber;
		if (unk)
			*unk = FALSE;

		return STATUS_SUCCESS;
	}
	return XInputdReadStateDetour.GetOriginal<decltype(&XInputdReadStateHook)>()(dwDeviceContext, pdwPacketNumber, pInputData, unk);
}


void* XamInputSetState = nullptr;
void* XamInputGetCapabilitiesEx = nullptr;
void* XInputdReadStatePtr = nullptr;
uint16_t* XNotifyTimerPtr = nullptr;
bool isDevkit = true;
DWORD UsbPhysicalPage = 0;
void* NotificationPatchPtr = nullptr;
void* XamInputGetCapabilitiesPtr = nullptr;   // ordinal 400
void* XamInputGetStatePtr = nullptr;          // ordinal 401
void* XamGetCurrentTitleIdPtr = nullptr;      // ordinal 463
static DWORD g_xboxInputMissingFunctions = 0;
bool initFunctionPointers() {
	isDevkit = *(uint32_t*)(0x8010D334) == 0x00000000;
	HANDLE kernelHandle = GetModuleHandleA("xboxkrnl.exe");

	if (!kernelHandle) {
		DbgPrint("EINTIM: COULDNT GET KERNEL HANDLE!\n");
		return false;
	}

	HANDLE xamHandle = GetModuleHandleA("xam.xex");
	if (!xamHandle) {
		g_xboxInputMissingFunctions = 0x80000000;
		DbgPrint("XBOXINPUT: could not get xam.xex handle\r\n");
		return false;
	}

	XexGetProcedureAddress(kernelHandle, 759, &UsbdGetDeviceDescriptor);
	XexGetProcedureAddress(kernelHandle, 744, &UsbdGetEndpointDescriptor);
	XexGetProcedureAddress(kernelHandle, 740, &UsbdAddDeviceComplete);
	XexGetProcedureAddress(kernelHandle, 746, &UsbdOpenDefaultEndpoint);
	XexGetProcedureAddress(kernelHandle, 747, &UsbdOpenEndpoint);
	XexGetProcedureAddress(kernelHandle, 742, &UsbdGetDeviceSpeed);
	XexGetProcedureAddress(kernelHandle, 748, &UsbdQueueAsyncTransfer);
	XexGetProcedureAddress(kernelHandle, 750, &UsbdQueueCloseEndpoint);
	XexGetProcedureAddress(kernelHandle, 749, &UsbdQueueCloseDefaultEndpoint);
	XexGetProcedureAddress(kernelHandle, 751, &UsbdRemoveDeviceComplete);
	XexGetProcedureAddress(kernelHandle, 189, &MmFreePhysicalMemory);
	XexGetProcedureAddress(kernelHandle, 486, &XInputdReadStatePtr);

	// Ordinals cross-checked against Xenia's export table
	// (refs/xenia/src/xenia/kernel/xam/xam_table.inc:197-199, 481).
	XexGetProcedureAddress(xamHandle, 400, &XamInputGetCapabilitiesPtr);
	XexGetProcedureAddress(xamHandle, 401, &XamInputGetStatePtr);
	XexGetProcedureAddress(xamHandle, 746, &XamIsSysUiInvokedByXenonButton);
	XexGetProcedureAddress(xamHandle, 685, &XamInputGetCapabilitiesEx);
	XexGetProcedureAddress(xamHandle, 402, &XamInputSetState);
	XexGetProcedureAddress(xamHandle, 1183, &NotificationPatchPtr);

	// XamGetCurrentTitleId - xam ordinal 463 (0x1CF), no arguments, returns the title ID.
	// refs/xenia/src/xenia/kernel/xam/xam_table.inc:260, implementation at
	// refs/xenia/src/xenia/kernel/xam/xam_info.cc:225.
	// Optional: used by the title-owned disconnect notification worker.
	XexGetProcedureAddress(xamHandle, 463, &XamGetCurrentTitleIdPtr);
	XBOXINPUT_LOG("XBOXINPUT: XamGetCurrentTitleId (463) %s\r\n",
		XamGetCurrentTitleIdPtr ? "resolved" : "DID NOT RESOLVE");

	// Validate everything that the active controller path will call before any
	// detour is installed.  The mask is persisted in the init-abort event.
	DWORD missing = 0;
	if (!UsbdGetDeviceDescriptor)       missing |= 0x00000001;
	if (!UsbdGetEndpointDescriptor)     missing |= 0x00000002;
	if (!UsbdAddDeviceComplete)         missing |= 0x00000004;
	if (!UsbdOpenDefaultEndpoint)       missing |= 0x00000008;
	if (!UsbdOpenEndpoint)              missing |= 0x00000010;
	if (!UsbdQueueAsyncTransfer)        missing |= 0x00000020;
	if (!UsbdQueueCloseEndpoint)        missing |= 0x00000040;
	if (!UsbdQueueCloseDefaultEndpoint) missing |= 0x00000080;
	if (!UsbdRemoveDeviceComplete)      missing |= 0x00000100;
	if (!XInputdReadStatePtr)           missing |= 0x00000200;
	if (!XamInputGetCapabilitiesEx)     missing |= 0x00000400;
	if (!XamInputSetState)              missing |= 0x00000800;
	if (!XamInputGetCapabilitiesPtr)    missing |= 0x00001000;
	if (!XamInputGetStatePtr)           missing |= 0x00008000;
#ifndef XBOXINPUT_NO_NOTIFY_PATCH
	if (!NotificationPatchPtr)          missing |= 0x00002000;
#endif
#ifndef XBOXINPUT_NO_USB_RESET
	if (!MmFreePhysicalMemory)           missing |= 0x00004000;
#endif
	g_xboxInputMissingFunctions = missing;
	if (missing) {
		DbgPrint("XBOXINPUT: required export validation failed mask=%08X\r\n", missing);
		return false;
	}

	if (isDevkit) {
		DbgPrint("EINTIM: Running in devkit mode\n");
		UsbdGetInterfaceDescriptor = (usb_interface_descriptor_func_t)0x8010D2D0; // 89 43 ? ? 3D 60 ? ? 89 2D ? ? 39 6B ? ? 55 4A FF 3A 2B 09 ? ? 7D 6A 58 2E ? ? ? ? ? ? ? ? 89 4D ? ? 2B 0A ? ? ? ? ? ? ? ? ? ? 81 4B ? ? 7F 03 50 40 ? ? ? ? ? ? ? ? A1 4B very bad direct signature. XREF sig: 89 63 ? ? 38 A1
		XamUserBindDeviceCallback = (xam_user_bind_device_callback_func_t)0x817A34B8; // 7C 8B 23 78 7C A4 2B 78 54 CA 06 3F
		UsbdPowerDownNotification = (usbd_powerdown_notification_func_t)0x8010E140; // argument to last function call in UsbdDriverEntry
		UsbdDriverEntry = (usbd_powerdown_notification_func_t)0x8010DE48; // 7D 88 02 A6 ? ? ? ? 94 21 ? ? 3C 80 ? ? 38 A0 

		// Remove two usb related bugchecks to allow reinitialisation of the usb driver
		*(DWORD*)0x80116298 = 0x48000018;
		*(DWORD*)0x801132A4 = 0x48000018;

		// DEVKIT only: Remove assertions(Microsoft did not think that we'd come and reset the usb driver, never let them know your next move typa shit)
		/*
		*(DWORD*)0x80096B84 = 0x60000000;
		*(DWORD*)0x80095F6C = 0x60000000;
		*(DWORD*)0x80116584 = 0x60000000;
		*(DWORD*)0x80116598 = 0x60000000;
		*/

		// Prevent double registration of Usbd handlers because the console wont shutdown cleanly otherwise
		* (DWORD*)0x8010E04C = 0x60000000;
		*(DWORD*)0x8010E05C = 0x60000000;
		UsbPhysicalPage = 0x8020A9B8;

		*(uint16_t*)0x8176A7C6 = 80; // Register custom notification type condition
		XNotifyTimerPtr = (uint16_t*)0x8176a7ca;
	}
	else {
		DbgPrint("EINTIM: Running in retail mode\n");
		UsbdGetInterfaceDescriptor = (usb_interface_descriptor_func_t)0x800D8500; // 89 43 ? ? 3D 60 ? ? 39 6B ? ? 55 4A FF 3A 7D 6A 58 2E A1 4B
		XamUserBindDeviceCallback = (xam_user_bind_device_callback_func_t)0x816D9060; // 7C 8B 23 78 7C A4 2B 78 54 CA 06 3F
		UsbdPowerDownNotification = (usbd_powerdown_notification_func_t)0x800D8FC8; // argument to last function call in UsbdDriverEntry
		UsbdDriverEntry = (usbd_powerdown_notification_func_t)0x800D8D08; // 7D 88 02 A6 ? ? ? ? 94 21 ? ? 3C 80 ? ? 38 A0 

		// Remove two usb related bugchecks to allow reinitialisation of the usb driver
		//
		// These are needed ONLY for the one-time USB driver reset that DllMain performs
		// right after loading (UsbdPowerDownNotification -> MmFreePhysicalMemory ->
		// UsbdDriverEntry). Building without them hangs the console before bootanim
		// finishes, which proves they are load-bearing for that sequence.
		//
		// But leaving them applied permanently removes fault-containment at RUNTIME, and
		// that is the prime suspect for the disconnect freeze (docs/usb_stack.md):
		//   - no plugin at all: controller power-on floods STATUS_ACCESS_VIOLATION
		//     (0xC0000005) FirstChance and the console KEEPS RUNNING
		//   - killtest build (never claims, opens no endpoints): FREEZES on disconnect
		//   - the only thing killtest shares with us that a no-plugin boot lacks is
		//     these patches
		//
		// So: record the originals here, and restore the two BUGCHECK sites once the
		// reset is done (GipRestoreUsbBugchecks, called at the end of DllMain).
		// Only needed because of the USB driver reset below. If that is skipped, these
		// are not applied at all and kernel fault containment is never disturbed.
#ifndef XBOXINPUT_NO_USB_RESET
		GipSavePatch(0, (DWORD*)0x800E05E4);
		GipSavePatch(1, (DWORD*)0x800DD8E0);
		*(DWORD*)0x800E05E4 = 0x48000018;
		*(DWORD*)0x800DD8E0 = 0x48000018;
#endif

		// Prevent double registration of Usbd handlers because the console wont shutdown
		// cleanly otherwise.
		//
		// *** PRIME SUSPECT, AND NEVER ONCE REMOVED IN A TEST BUILD. ***
		// These two writes sat outside every #ifdef, so they were present in killtest,
		// noreset, giponly, nonotify, fixremove, silentremove, noreclaim and noclose —
		// every configuration that froze — and absent from the one that survives, which
		// is a boot with no plugin at all. Level < 2 is the first build ever to skip them.
		//
		// They also only matter if UsbdDriverEntry runs a SECOND time: both addresses are
		// inside it (UsbdDriverEntry = 0x800D8D08, UsbdPowerDownNotification = 0x800D8FC8,
		// so 0x800D8EF0/0x800D8F00 are +0x1E8/+0x1F8, well within that range — see
		// docs/kernel_api.md). With the USB reset skipped, UsbdDriverEntry is never
		// re-entered, so at levels 2–5 these are inert as far as execution goes.
#if XBOXINPUT_BUILD_LEVEL >= XBOXINPUT_LEVEL_NOPS
		*(DWORD*)0x800D8F00 = 0x60000000;
		*(DWORD*)0x800D8EF0 = 0x60000000;
#endif
		UsbPhysicalPage = 0x801A8098;

	// XAM notification patches.
	//
	// hiddriver360 needs these for its mapping-assistant UI (custom XNotifyQueueUI type
	// 80, and a JRPC2-free notification path). WE DO NOT USE ANY OF THAT.
	//
	// They are also the last thing every freezing configuration still shares:
	//   no plugin          -> notifications intact -> disconnect SURVIVES
	//   killtest           -> patched -> FREEZE
	//   noreset (no reset, no bugcheck patches)   -> patched -> FREEZE
	//   giponly (no HID detours, no mapping thread) -> patched -> FREEZE
	//
	// USB device arrival and removal are delivered as system notifications, and the
	// NotificationPatchPtr+48 write turns a conditional branch (0x409A bne) into an
	// unconditional one (0x4800) inside that dispatch.
#ifndef XBOXINPUT_NO_NOTIFY_PATCH
		*(uint16_t*)0x816AB7A6 = 80; // Register custom notification type condition
		XNotifyTimerPtr = (uint16_t*)0x816ab7aa;
#endif
	}

#ifndef XBOXINPUT_NO_NOTIFY_PATCH
	*XNotifyTimerPtr = 1500;
#endif

	// Patches notification handling to work without JRPC2, Thanks crow!
	// 0x409A is a conditional branch (bne); 0x4800 makes it unconditional.
#ifndef XBOXINPUT_NO_NOTIFY_PATCH
	if (*(short*)((uintptr_t)(NotificationPatchPtr) + 48) == 0x409A) {
		*(short*)((uintptr_t)(NotificationPatchPtr) + 48) = 0x4800;
	}
#else
	DbgPrint("XBOXINPUT: XAM notification patches SKIPPED\r\n");
#endif

	return true;
}

static DWORD XboxInputInitializeThread(PVOID) {
		// Resolve and test the plugin-local configuration path before starting the
		// logger so both files use one immutable location. This avoids a race where
		// the logger opens HDD while configuration is still discovering USB.
		bool cfgExisted = false;
		bool cfgPathReady = XboxInputSelectConfigPath((PVOID)&XboxInputInitializeThread, &cfgExisted);
#ifndef XBOXINPUT_DISABLE_FILE_LOG
		// This function runs only after DllMain has returned. Start persistence before
		// configuration, export resolution, patches, or hooks. Do not wait for
		// storage here: boot enumeration can finish during even a short delay.
		HANDLE loggerThread = MakeThread((LPTHREAD_START_ROUTINE)XboxInputLogThread, nullptr);
		if (loggerThread) {
			CloseHandle(loggerThread);
			XboxInputQueueLogEvent(XBOXINPUT_LOG_INIT_STEP, XBOXINPUT_INIT_LOGGER_STARTED, 0);
		}
#endif
		XboxInputSetDiagStage(1);
#ifdef XBOXINPUT_LOAD_NOTIFY
		// Diagnostic-only proof that DashLaunch reached this module's entry point.
		// Keep this before every version gate and subsystem initialization step.
		XNotifyUI(XNOTIFYUI_TYPE_PREFERRED_REVIEW, L"XboxInput diagnostic build loaded");
#endif
		// Fires before ANY check below, so "did our build load at all?" is answerable
		// even when the version/tray gate aborts the launch. Build stamp distinguishes
		// this xex from any other hiddriver360 build on the console.
		XBOXINPUT_LOG("XBOXINPUT: *** XboxInput GIP driver loaded - built " __DATE__ " " __TIME__ " ***\r\n");
		BOOL trayOpen = IsTrayOpen();
		XBOXINPUT_LOG("XBOXINPUT: kernel build %d, tray open = %d\r\n",
			XboxKrnlVersion->Build, trayOpen ? 1 : 0);
		XboxInputQueueLogEvent(XBOXINPUT_LOG_INIT_STEP, XBOXINPUT_INIT_ENVIRONMENT,
			((XboxKrnlVersion->Build & 0xFFFF) << 16) | (trayOpen ? 1 : 0));

		if ((XboxKrnlVersion->Build != 17559 && XboxKrnlVersion->Build != 17489) || trayOpen) {
			XBOXINPUT_LOG("XBOXINPUT: ABORTING - unsupported kernel build or disc tray open\r\n");
			DbgPrint("EINTIM: Only 17559 and 17489 dashboards are currently supported or the disk tray is open. Aborting launch...\n");
			XboxInputQueueLogEvent(XBOXINPUT_LOG_INIT_STEP, XBOXINPUT_INIT_ABORT_UNSUPPORTED,
				((XboxKrnlVersion->Build & 0xFFFF) << 16) | (trayOpen ? 1 : 0));
			// Stay resident but inert so the logger can explain the safe abort.
			return TRUE;
		}

		XBOXINPUT_LOG("XBOXINPUT: *** BUILD LADDER LEVEL %d ***\r\n", XBOXINPUT_BUILD_LEVEL);

#ifdef XBOXINPUT_RESTORE_WGC_MATCH
		XboxInputRestoreWgcDescriptorCheck();
#endif

		// Level 0 is the control: a plugin that loads into the same process, at the
		// same base address, and then does nothing at all. If a disconnect freezes
		// even this, the cause is DashLaunch/plugin residency itself and no amount of
		// work inside the driver will fix it. If it survives, we have a clean floor to
		// add subsystems onto — which is the thing the subtractive bisection lacked.
#if XBOXINPUT_BUILD_LEVEL == XBOXINPUT_LEVEL_NULL
		XBOXINPUT_LOG("XBOXINPUT: level 0 - loaded and doing nothing. Disconnect the dongle now.\r\n");
		XboxInputQueueLogEvent(XBOXINPUT_LOG_INIT_STEP, XBOXINPUT_INIT_COMPLETE, 0);
		XboxInputSetDiagStage(12);
		return TRUE;
#else
		// Optional user settings. Missing file is normal and not an error - it means
		// built-in defaults, and one gets written out with comments for next boot.
		// Read here, at load, and never again: everything downstream only reads the
		// parsed globals, so no hot path or raised-IRQL context ever touches the disk.
		XboxInputSetDefaultMapping(&g_xboxInputGamepadMapping);
		bool cfgFound = cfgPathReady && XboxInputLoadConfig(XBOXINPUT_CFG_PATH);
		XBOXINPUT_LOG("XBOXINPUT: config %s %s - guide=%d cooldown=%dms\r\n",
			cfgFound ? (cfgExisted ? "loaded from" : "generated at") :
				"defaults; no writable path for",
			XBOXINPUT_CFG_PATH,
			g_xboxInputConfig.guideButton, g_xboxInputConfig.guideCooldownMs);
		XBOXINPUT_LOG("XBOXINPUT: mapping swapSticks=%d swapTriggers=%d invert=%d/%d/%d/%d "
			"stickDz=%u/%u triggerDz=%u/%u rumble=%u%%\r\n",
			g_xboxInputGamepadMapping.swapSticks,
			g_xboxInputGamepadMapping.swapTriggers,
			g_xboxInputGamepadMapping.invertLeftX,
			g_xboxInputGamepadMapping.invertLeftY,
			g_xboxInputGamepadMapping.invertRightX,
			g_xboxInputGamepadMapping.invertRightY,
			g_xboxInputGamepadMapping.leftStickDeadzone,
			g_xboxInputGamepadMapping.rightStickDeadzone,
			g_xboxInputGamepadMapping.leftTriggerDeadzone,
			g_xboxInputGamepadMapping.rightTriggerDeadzone,
			g_xboxInputGamepadMapping.rumblePercent);
		XboxInputQueueLogEvent(XBOXINPUT_LOG_INIT_STEP, XBOXINPUT_INIT_CONFIG_LOADED,
			(cfgFound ? 0x80000000 : 0));
		XboxInputQueueLogEvent(XBOXINPUT_LOG_CONFIG_PATH,
			cfgFound ? (cfgExisted ? 1 : 2) : 0, 0);

		DbgPrint("EINTIM: HELLO from xbox 360 HID controller driver version 0.6 beta\n");
		if (!initFunctionPointers()) {
			XboxInputQueueLogEvent(XBOXINPUT_LOG_INIT_STEP, XBOXINPUT_INIT_ABORT_FUNCTIONS,
				g_xboxInputMissingFunctions);
			// No detours have been installed. Remain loaded only to persist the error.
			return TRUE;
		}
		XboxInputQueueLogEvent(XBOXINPUT_LOG_INIT_STEP, XBOXINPUT_INIT_FUNCTIONS_READY, 0);

		DbgPrint("EINTIM: Loading mappings!\r\n");
#ifndef XBOXINPUT_GIP_ONLY
		if (!LoadMappingsFromFile("HDD:\\hiddriver.json")) {
			DbgPrint("EINTIM: Failed to load mappings(JSON either doesn't exist yet or syntax error)!\r\n");
		}

#endif
		// HID detours: NOT needed for the XboxInput. Our claim goes through
		// UsbdAddDeviceComplete, not the HID driver. These patch live code at
		// 0x800E4D68 / 0x800E4D28 - the HID device add/remove path, i.e. exactly
		// the code that runs when a USB device disappears. Prime remaining
		// suspect for the disconnect freeze.
#ifndef XBOXINPUT_GIP_ONLY
		if (isDevkit) {
			HidAddDeviceDetour = Detour((void*)0x8011AE38, (void*)HidAddDeviceHook); // 7D 88 02 A6 ? ? ? ? 94 21 ? ? 7C 7C 1B 78 ? ? ? ? 7C 7F 1B 79
			HidRemoveDeviceDetour = Detour((void*)0x8011ADF8, (void*)HidRemoveDeviceHook); // 81 63 ? ? 39 40 ? ? 39 20 ? ? 99 4B
		}
		else {
			HidAddDeviceDetour = Detour((void*)0x800E4D68, (void*)HidAddDeviceHook); // 7D 88 02 A6 ? ? ? ? 94 21 ? ? 7C 7B 1B 78 ? ? ? ? 7C 7F 1B 79
			HidRemoveDeviceDetour = Detour((void*)0x800E4D28, (void*)HidRemoveDeviceHook); // 81 63 ? ? 39 40 ? ? 39 20 ? ? 99 4B
		}

		HidAddDeviceDetour.Install();
		HidRemoveDeviceDetour.Install();
#endif

		// Phase 0.5b: probe the kernel USB exports to find who handles non-HID devices.
		// This is also where the GIP claim lives (UsbdAddDeviceComplete), so below
		// level 7 the dongle is left unclaimed exactly as a no-plugin boot leaves it.
#if XBOXINPUT_BUILD_LEVEL >= XBOXINPUT_LEVEL_FULL
		InstallUsbProbes();
		XboxInputSetDiagStage(10);
		XboxInputQueueLogEvent(XBOXINPUT_LOG_INIT_STEP, XBOXINPUT_INIT_USB_HOOKS_READY, 0);
#endif

#if XBOXINPUT_BUILD_LEVEL >= XBOXINPUT_LEVEL_XAMHOOKS
		XamInputGetCapabilitiesDetour = Detour(XamInputGetCapabilitiesEx, (void*)XamInputGetCapabilitiesExHook);
		XamInputSetStateDetour = Detour(XamInputSetState, (void*)XamInputSetStateHook);
		XamInputGetStateDetour = Detour(XamInputGetStatePtr, (void*)XamInputGetStateHook);
		if (XamInputGetCapabilitiesPtr) {
			XamInputGetCapabilitiesDetour2 = Detour(XamInputGetCapabilitiesPtr, (void*)XamInputGetCapabilitiesHook);
			XamInputGetCapabilitiesDetour2.Install();
			XBOXINPUT_LOG("XBOXINPUT: hooked XamInputGetCapabilities (400) @ %p\r\n", XamInputGetCapabilitiesPtr);
		}
		else XBOXINPUT_LOG("XBOXINPUT: ordinal 400 did NOT resolve!\r\n");
		XInputdReadStateDetour = Detour(XInputdReadStatePtr, (void*)XInputdReadStateHook);

		XamInputSetStateDetour.Install();
		XamInputGetCapabilitiesDetour.Install();
		XamInputGetStateDetour.Install();
		XInputdReadStateDetour.Install();
		XboxInputSetDiagStage(11);
		XboxInputQueueLogEvent(XBOXINPUT_LOG_INIT_STEP, XBOXINPUT_INIT_XAM_HOOKS_READY, 0);
		DbgPrint("EINTIM: Hooks installed\n");
#else
		XBOXINPUT_LOG("XBOXINPUT: XamInput/XInputd detours SKIPPED (level %d)\r\n", XBOXINPUT_BUILD_LEVEL);
#endif

		// This is a dirty way of forcing the system to reenumerate USB devices so you don't need to replug the controllers
		//
		// XBOXINPUT_NO_USB_RESET skips it. This is the strongest remaining suspect for
		// the disconnect freeze: it is a full USB stack teardown and re-entry (the author
		// calls it "dirty", and it frees a physical page out from under the driver), it is
		// shared by the killtest build and this one, and it is absent from a no-plugin
		// boot - which is the exact configuration that survives a disconnect.
		//
		// We do not need it. Its only benefit is that devices already plugged in when the
		// plugin loads get re-enumerated; the controller is powered on after boot anyway, so
		// our UsbdAddDeviceComplete detour sees it arrive normally.
		//
		// Skipping it also makes the bugcheck patches unnecessary, since those exist
		// solely to let this sequence run - which is why the earlier "skip the patches but
		// still do the reset" build hung at boot.
#ifndef XBOXINPUT_NO_USB_RESET
		DbgPrint("EINTIM: Resetting USB driver!\n");
		UsbdPowerDownNotification();

		// For some reason microsoft doesnt clean up this page by themselves in the shutdown notification, so ill do it for them, call me mr nice guy :)
		MmFreePhysicalMemory(0, *(DWORD*)UsbPhysicalPage);
		DbgPrint("EINTIM: USB driver shutdown complete.\n");

		UsbdDriverEntry();
		DbgPrint("EINTIM: USB driver reset complete.\n");

		// The USB driver reset is done, so the bugchecks are no longer in the way.
		// Put them back: with fault containment restored, a USB fault at runtime
		// (e.g. the controller going to sleep) should raise a survivable exception the way
		// it does with no plugin loaded, instead of hanging the console.
		// OFF by default. Restoring the bugchecks did NOT fix the disconnect freeze, and
		// it coincided with titles failing to launch - so it is a suspected regression,
		// not a neutral change. Only enable to re-test.
#ifdef XBOXINPUT_RESTORE_BUGCHECKS
		GipRestoreUsbBugchecks();
#endif
#else
		XBOXINPUT_LOG("XBOXINPUT: USB driver reset SKIPPED - power the controller on AFTER boot\r\n");
		XboxInputQueueLogEvent(XBOXINPUT_LOG_INIT_STEP, XBOXINPUT_INIT_USB_RESET_SKIPPED, 0);
#endif

		XboxInputSetDiagStage(12);
		XboxInputQueueLogEvent(XBOXINPUT_LOG_INIT_STEP, XBOXINPUT_INIT_COMPLETE, 0);

		// Start mapping manager thread.
		// Not needed for the XboxInput: our mapping is fixed and known, so the JSON
		// mapping system and its background thread are dead weight (and the project's
		// release criteria say the plugin must need no config file).
#ifndef XBOXINPUT_GIP_ONLY
		MakeThread((LPTHREAD_START_ROUTINE)MappingManagerThreadProc, nullptr);
#endif
#endif // XBOXINPUT_BUILD_LEVEL == XBOXINPUT_LEVEL_NULL
	return 0;
}

BOOL APIENTRY DllMain(HANDLE Handle, DWORD Reason, PVOID Reserved) {
	UNREFERENCED_PARAMETER(Handle);
	UNREFERENCED_PARAMETER(Reserved);
	if (Reason == DLL_PROCESS_ATTACH) {
		// Keep loader entry minimal. Performing config I/O, export resolution and
		// detour installation under the module-loader lock can prevent the logger
		// thread from ever running if an early initialization step faults.
		XboxInputQueueLogEvent(XBOXINPUT_LOG_INIT_STEP, XBOXINPUT_INIT_ENTRY, 0);
		HANDLE initThread = MakeThread((LPTHREAD_START_ROUTINE)XboxInputInitializeThread, nullptr);
		if (!initThread)
			return FALSE;
		CloseHandle(initThread);
	}
	return TRUE;
}
