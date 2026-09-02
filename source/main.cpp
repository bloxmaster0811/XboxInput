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
#include "gip_riffmaster.h"
#include "gip_auth.h"
#include "riffmaster_build.h"

extern "C" void* _ReturnAddress(void);
#pragma intrinsic(_ReturnAddress)

// ---------------------------------------------------------------------------
// ADDITIVE BUILD LADDER — RIFFMASTER_LEVEL, set by tools/build.ps1 -Level N.
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
//   7 FULL      + Usbd add/remove detours, the GIP claim, auth, XAM guitar.
//
// Level 7 is byte-for-byte the shipping driver; the ladder adds no behaviour.
// ---------------------------------------------------------------------------
#define RM_LVL_NULL      0
#define RM_LVL_RESOLVE   1
#define RM_LVL_NOPS      2
#define RM_LVL_XAMHOOKS  3
#define RM_LVL_NOTIFY    4
#define RM_LVL_HID       5
#define RM_LVL_USBRESET  6
#define RM_LVL_FULL      7

// Map the ladder onto the flags the existing code already tests, so the levels
// are a re-expression of known-good conditionals rather than a new code path.
#if RIFFMASTER_LEVEL < RM_LVL_HID
#define RIFFMASTER_GIP_ONLY 1        // no HID detours, no JSON, no mapping thread
#endif
#if RIFFMASTER_LEVEL < RM_LVL_NOTIFY
#define RIFFMASTER_NO_NOTIFY_PATCH 1
#endif
#if RIFFMASTER_LEVEL < RM_LVL_USBRESET
#define RIFFMASTER_NO_USB_RESET 1    // also skips the two bugcheck patches
#endif

// ---------------------------------------------------------------------------
// Logging levels.
//
// RM_LOG   - always on. Milestones and errors only: load, claim, endpoints,
//            auth complete, XAM registration, teardown, anything that failed.
// RM_DBG   - per-packet chatter (GIP chunks, input reports, USB probes,
//            capability queries). OFF by default.
//
// This is not just tidiness: DbgPrint on a hot path saturates the xbdm debug
// channel and HANGS THE CONSOLE. That happened twice during development - once
// dumping certificate hex during auth, once logging every capability query
// (which the dash polls ~8x per 100ms). Keep hot paths under RM_DBG.
// ---------------------------------------------------------------------------
//#define RIFFMASTER_VERBOSE 1
//#define RIFFMASTER_NO_NOTIFY_PATCH 1   // enable to skip the XAM notification patches

// Debug Monitor is not present on most retail RGH setups. Mirror milestone logs
// to a small FTP-readable file as well. RM_LOG is deliberately never used for
// normal input packets, so this does not put filesystem I/O in the input path.
static const char* const kXboxInputLogPath = "HDD:\\XboxInputGip.log";
static volatile LONG g_xboxInputDiagStage = 0;
static volatile DWORD g_xboxInputGuideCaller = 0;
static volatile DWORD g_xboxInputGuideUiState = 0;

static void XboxInputLogReset() {
	FILE* file = fopen(kXboxInputLogPath, "w");
	if (file)
		fclose(file);
}

static void XboxInputSetDiagStage(LONG stage) {
	InterlockedExchange(&g_xboxInputDiagStage, stage);
}

// USB callbacks may run above PASSIVE_LEVEL, where filesystem calls can deadlock.
// Keep their normal diagnostics on DbgPrint and let a system thread persist just
// the latest coarse-grained stage to the FTP-readable file.
#define RM_LOG(...) DbgPrint(__VA_ARGS__)

// RM_TRACE - breadcrumbs on the device add/remove path only, for the `trace` variant.
// Deliberately NOT tied to RIFFMASTER_VERBOSE: that also turns on per-packet logging,
// which floods xbdm and is itself a hazard. These fire a handful of times per plug event.
#ifdef RIFFMASTER_TRACE
#define RM_TRACE(...) DbgPrint(__VA_ARGS__)
#else
#define RM_TRACE(...) ((void)0)
#endif
#ifdef RIFFMASTER_VERBOSE
#define RM_DBG(...) DbgPrint(__VA_ARGS__)
#else
#define RM_DBG(...) ((void)0)
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
		DbgPrint("RIFFMASTER: restored USB bugcheck at %p -> 0x%08X\r\n",
			p->addr, p->original);
	}
}

Detour HidAddDeviceDetour;
Detour HidRemoveDeviceDetour;
Detour XamInputSetStateDetour;
Detour XamInputGetCapabilitiesDetour;
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
	ExCreateThread(&Handle, 0, 0, XapiThreadStartup, Address, arg, (EX_CREATE_FLAG_SUSPENDED | EX_CREATE_FLAG_SYSTEM | 0x18000424));
	XSetThreadProcessor(Handle, 4);
	SetThreadPriority(Handle, THREAD_PRIORITY_NORMAL);
	ResumeThread(Handle);
	return Handle;
}

static DWORD XboxInputLogThread(PVOID) {
	LONG written = -1;
	DWORD writtenGuideCaller = 0;
	DWORD writtenGuideUiState = 0;
	for (;;) {
		LONG stage = g_xboxInputDiagStage;
		if (stage != written) {
			FILE* file = fopen(kXboxInputLogPath, "a");
			if (file) {
				fprintf(file, "stage=%ld\r\n", stage);
				fclose(file);
			}
			written = stage;
		}
		DWORD guideCaller = g_xboxInputGuideCaller;
		DWORD guideUiState = g_xboxInputGuideUiState;
		if (guideCaller && (guideCaller != writtenGuideCaller ||
			guideUiState != writtenGuideUiState)) {
			FILE* file = fopen(kXboxInputLogPath, "a");
			if (file) {
				fprintf(file, "guideCaller=%08X xenonUi=%u\r\n",
					guideCaller, guideUiState);
				fclose(file);
			}
			writtenGuideCaller = guideCaller;
			writtenGuideUiState = guideUiState;
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
	RM_DBG("RIFFMASTER: ===== DEVICE REACHED HidAddDeviceHook (handle %p) =====\r\n", handle);

	if (dd) {
		RM_DBG("RIFFMASTER: VID=%04X PID=%04X bcdDevice=%04X bcdUSB=%04X\r\n",
			swap_endianness_16(dd->idVendor), swap_endianness_16(dd->idProduct),
			swap_endianness_16(dd->bcdDevice), swap_endianness_16(dd->bcdUSB));
		RM_DBG("RIFFMASTER: dev class=%02X subclass=%02X protocol=%02X maxPacket0=%d numCfg=%d\r\n",
			dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol,
			dd->bMaxPacketSize0, dd->bNumConfigurations);
	}
	else {
		RM_DBG("RIFFMASTER: device descriptor is NULL\r\n");
	}

	if (id) {
		RM_DBG("RIFFMASTER: iface #%d alt=%d numEndpoints=%d class=%02X subclass=%02X protocol=%02X\r\n",
			id->bInterfaceNumber, id->bAlternateSetting, id->bNumEndpoints,
			id->bInterfaceClass, id->bInterfaceSubClass, id->bInterfaceProtocol);
		// GIP signature per docs/gip_riffmaster.md section 2 (verified from capture).
		if (id->bInterfaceClass == 0xFF && id->bInterfaceSubClass == 0x47 && id->bInterfaceProtocol == 0xD0)
			RM_DBG("RIFFMASTER: *** GIP SIGNATURE FF/47/D0 - THIS IS A GIP DEVICE ***\r\n");
	}
	else {
		RM_DBG("RIFFMASTER: interface descriptor is NULL\r\n");
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
				RM_DBG("RIFFMASTER:   EP %02X %s %s maxPacket=%d interval=%d\r\n",
					ep->bEndpointAddress,
					(ep->bEndpointAddress & 0x80) ? "IN" : "OUT",
					KtXferName(ep->bmAttributes),
					swap_endianness_16(ep->wMaxPacketSize) & 0x7FF,
					ep->bInterval);
			}
		}
	}
	RM_DBG("RIFFMASTER: %d endpoint(s) found in %d probes\r\n", found, probes);
	RM_DBG("RIFFMASTER: ======================================================\r\n");
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
		RM_DBG("RIFFMASTER: PROBE slot %d hit log limit (%d) - further calls NOT logged\r\n",
			slot, PROBE_LIMIT);
		return false;
	}
	return true;
}

static void ProbeLog(int slot, const char* name, void* handle) {
	if (!ProbeShouldLog(slot))
		return;
	RM_DBG("RIFFMASTER: PROBE %-28s handle=%p\r\n", name, handle);
}

usb_device_descriptor* UsbdGetDeviceDescriptorHook(deviceHandle* h) {
	usb_device_descriptor* d =
		UsbdGetDeviceDescriptorDetour.GetOriginal<decltype(&UsbdGetDeviceDescriptorHook)>()(h);
	if (ProbeShouldLog(0)) {
		if (d)
			RM_DBG("RIFFMASTER: PROBE UsbdGetDeviceDescriptor  handle=%p  VID=%04X PID=%04X devclass=%02X\r\n",
				h, swap_endianness_16(d->idVendor), swap_endianness_16(d->idProduct),
				d->bDeviceClass);
		else
			RM_DBG("RIFFMASTER: PROBE UsbdGetDeviceDescriptor  handle=%p  (NULL descriptor)\r\n", h);
	}
	return d;
}

usb_interface_descriptor* UsbdGetInterfaceDescriptorHook(deviceHandle* h) {
	usb_interface_descriptor* d =
		UsbdGetInterfaceDescriptorDetour.GetOriginal<decltype(&UsbdGetInterfaceDescriptorHook)>()(h);
	if (ProbeShouldLog(1)) {
		if (d)
			RM_DBG("RIFFMASTER: PROBE UsbdGetInterfaceDescriptor handle=%p  iface=%d class=%02X/%02X/%02X\r\n",
				h, d->bInterfaceNumber, d->bInterfaceClass,
				d->bInterfaceSubClass, d->bInterfaceProtocol);
		else
			RM_DBG("RIFFMASTER: PROBE UsbdGetInterfaceDescriptor handle=%p  (NULL)\r\n", h);
	}
	return d;
}

int UsbdGetDeviceSpeedHook(deviceHandle* h) {
	ProbeLog(2, "UsbdGetDeviceSpeed", h);
	return UsbdGetDeviceSpeedDetour.GetOriginal<decltype(&UsbdGetDeviceSpeedHook)>()(h);
}

// RiffMaster identity - verified from the PC capture, docs/gip_riffmaster.md section 1,
// and confirmed on-console by probe2 (VID=0E6F PID=0248 class=FF/47/D0).
// NOTE: PID 0x0247 is the guitar's BOOTLOADER ("PDP.Xbox.Controller.Bootloader" per
// refs/PlasticBand/Docs/Descriptor Dumps/Xbox One/PDP Riffmaster Wired (Bootloader).txt)
// - deliberately NOT matched here.
// Official Microsoft wired Xbox One / Series gamepads supported by the Linux
// xpad driver's upstream device table. Keep this an explicit controller list:
// other Microsoft GIP-class devices include adapters and accessories, which
// must never be claimed as a gamepad.
const uint16_t MICROSOFT_VENDOR_ID = 0x045E;
static bool IsSupportedMicrosoftGamepadPid(uint16_t pid) {
	switch (pid) {
	case 0x02D1: // Xbox One
	case 0x02DD: // Xbox One (2015 firmware)
	case 0x02E3: // Xbox One Elite
	case 0x02EA: // Xbox One S
	case 0x0B00: // Xbox One Elite Series 2
	case 0x0B12: // Xbox Series S|X (wired)
		return true;
	default:
		return false;
	}
}

// Budget of claim attempts, NOT reset by teardown - only by a connection that reaches
// AUTH HANDSHAKE COMPLETE (main.cpp, GIP_CMD_AUTHENTICATE handler).
//
// Why this matters: turning the guitar off makes the dongle bounce off and back onto
// USB repeatedly - six blade transitions were observed while holding the guide button,
// plus two more after power-down. Teardown used to reset this counter, so every bounce
// bought three fresh claims and the storm ran unbounded.
//
// That is dangerous because g_gipExt is a SINGLE STATIC struct shared by every
// incarnation of the device:
//   - teardown calls UsbdQueueCloseEndpoint(dead, &g_gipExt.interruptTrb), which is
//     asynchronous - the kernel still owns that TRB when it returns
//   - the next claim memsets g_gipExt and re-queues the very same TRB
//   - GipInterruptComplete recovers `ext` from the TRB address, so a completion still
//     in flight from the OLD device passes the `ext->deviceHandle` guard as soon as a
//     NEW handle has been stored there
// Re-queueing a TRB the kernel already has in its transfer list corrupts that list.
//
// Capping the storm is a mitigation, not a cure - the real fix is a per-device
// extension so incarnations cannot share a TRB.
#define GIP_CLAIM_MAX_ATTEMPTS 3
static int g_gipClaimAttempts = 0;
static HidControllerExtension g_gipExt;

// Read buffer for the GIP interrupt IN endpoint. wMaxPacketSize is 64
// (docs/gip_riffmaster.md section 2, read from the descriptor, not assumed).
// Static rather than malloc'd - this is touched from a USB completion callback.
#define GIP_READ_BUF_SIZE 64
static BYTE g_gipReadBuf[GIP_READ_BUF_SIZE];
static int  g_gipPacketsSeen = 0;
static int  g_gipInputsSeen = 0;
static GipGamepadState g_gipState;
static bool g_gipReady = false;
static bool g_gipGuideDown = false;
static bool g_gipGuidePending = false;
static bool g_gipGuideOverlayOpen = false;

// ---- host -> device side ----------------------------------------------------
// The device ANNOUNCEs every ~500 ms until the host answers with IDENTIFY. Without
// a reply it never advances to streaming input - exactly what we observed (seq 1..72).
static UsbTrb  g_gipOutTrb;
// Round-robin TX buffers. SendInterruptRequest is ASYNCHRONOUS: it hands the buffer to
// the USB stack and returns. Reusing one buffer meant a chunk ACK and the following
// POWER ON could clobber each other in flight - which is exactly what happened on the
// first run (identify ACKed fine, POWER ON sent, then silence).
#define GIP_TX_BUFS 12
static BYTE    g_gipOutBuf[GIP_TX_BUFS][64];
static int     g_gipOutBufIdx = 0;
static bool    g_gipOutOpen = false;
static uint8_t g_gipSeq = 1;
static bool    g_gipIdentifySent = false;
// A controller repeats ANNOUNCE until it receives IDENTIFY.  The original
// one-shot gate meant one dropped outbound transfer left that session stuck
// until the cable was physically replugged.
static bool    g_gipIdentifyReplySeen = false;
static DWORD   g_gipLastIdentifyTick = 0;
static bool    g_gipPoweredOn = false;
// The normal wired gamepad does not use the RiffMaster dongle's RSA path.
static bool    g_gipAuthStarted = true;
static uint32_t g_gipChunkTotal = 0;
static uint32_t g_gipAuthChunkTotal = 0;
static int      g_gipCertBytes = 0;
static int      g_gipAuthStage = 0;
static BYTE     g_gipHostRandom[32];    // our HOST_HELLO random
static BYTE     g_gipClientRandom[32];  // from CLIENT_HELLO
static BYTE     g_gipPms[48];           // premaster secret, input to the PRF
static BYTE     g_gipMasterSecret[48];
static GipTranscript g_gipTranscript;
static BYTE     g_gipHostFinish[50];
static bool     g_gipHostFinishReady = false;

// ---- multi-controller ownership foundation ---------------------------------
//
// USB completion callbacks recover HidControllerExtension from the address of a
// field inside it (interruptTrb is at +4 and controlTrb at +36).  A multi-pad
// implementation must therefore give every claimed device a permanently distinct
// extension, transfer requests and buffers; sharing the old globals across two
// devices can re-queue a TRB still owned by the USB stack.
//
// These slots are deliberately fixed rather than heap allocated: claim and
// completion callbacks can run at an IRQL where allocation is not safe.  They are
// not connected to the live single-controller path yet; the following refactor
// moves GIP/XAM state into the owning slot one subsystem at a time.
#define GIP_MAX_SESSIONS 4
struct GipSessionSlot {
	bool                    reserved;
	HidControllerExtension  ext;
	UsbTrb                  outTrb;
	BYTE                    readBuf[GIP_READ_BUF_SIZE];
	BYTE                    outBuf[GIP_TX_BUFS][64];
	int                     outBufIndex;
	bool                    outOpen;
	uint8_t                 sequence;
	bool                    identifySent;
	bool                    identifyReplySeen;
	DWORD                   lastIdentifyTick;
	bool                    poweredOn;
	bool                    ready;
	GipGamepadState         state;
	bool                    guideDown;
	bool                    guidePending;
	DWORD                   lastGuideTick;
	uint8_t                 userIndex;
	uint32_t                deviceContext;
	DWORD                   packetNumber;
	int                     packetsSeen;
	int                     inputsSeen;
	int                     readErrors;
	bool                    readLoopStopped;
};
static GipSessionSlot g_gipSessions[GIP_MAX_SESSIONS];

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

static GipSessionSlot* GipSessionFromUser(uint8_t user) {
	for (int i = 0; i < GIP_MAX_SESSIONS; ++i) {
		if (g_gipSessions[i].reserved && g_gipSessions[i].ready &&
			g_gipSessions[i].userIndex == user)
			return &g_gipSessions[i];
	}
	return 0;
}

static GipSessionSlot* GipSessionFromContext(uint32_t context) {
	for (int i = 0; i < GIP_MAX_SESSIONS; ++i) {
		if (g_gipSessions[i].reserved && g_gipSessions[i].ready &&
			g_gipSessions[i].deviceContext == context)
			return &g_gipSessions[i];
	}
	return 0;
}

// Sequence is adapter-global and never zero - refs/xone/bus/protocol.c:335-337.
static uint8_t GipNextSeq() {
	uint8_t s = g_gipSeq++;
	if (g_gipSeq == 0)
		g_gipSeq = 1;
	return s;
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
	const BYTE payload[9] = {
		0x00, 0x03, 0x00, 0x00, leftMotor, rightMotor,
		0xFF, 0x00, 0x00
	};
	return GipSend(h, GIP_CMD_RUMBLE, 0x00, payload, sizeof(payload));
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

static int GipSessionSendRumble(GipSessionSlot* session, BYTE leftMotor, BYTE rightMotor) {
	const BYTE payload[9] = {
		0x00, 0x03, 0x00, 0x00, leftMotor, rightMotor,
		0xFF, 0x00, 0x00
	};
	return GipSessionSend(session, GIP_CMD_RUMBLE, 0x00, payload, sizeof(payload));
}

//
// ---- GIP auth (command 0x06) -----------------------------------------------
// Layout is struct gip_auth_pkt_host_hello from refs/xone/auth/auth.c:77-85,
// built by gip_auth_send_pkt (auth.c:162-184):
//
//   handshake header (6): context, options, error, command, be16 length
//   data header      (4): command, version, be16 length
//   random          (32)
//   unknown1         (4)
//   unknown2         (4)
//   trailer          (8)
//                   = 58 bytes total
//
// data_len   = 58 - 6 - 8      = 44 = 0x2C  -> handshake.length
// data.length= 44 - 4          = 40 = 0x28
//
// Every one of those matches the captured host packet byte for byte:
//   06 30 01 3a | 00 41 00 01 00 2c | 01 01 00 28 | <32 random> ...
//
// NOTE the capture has a non-zero value at unknown2 (45 7B AF E9) where xone
// sends zeros. xone works with zeros, so we send zeros and record the difference.
//
#define GIP_AUTH_CTX_HANDSHAKE   0x00
#define GIP_AUTH_OPT_ACK         0x01
#define GIP_AUTH_OPT_FROM_HOST   0x40
#define GIP_AUTH_CMD_HOST_HELLO  0x01
#define GIP_AUTH_CMD_CLIENT_HELLO 0x02
#define GIP_AUTH_CMD_CLIENT_CERT 0x03

//
// NOT cryptographically secure - this is a probe. The real implementation must use
// the kernel's XeCryptRandom. Sufficient here because we only need the device to
// accept a well-formed hello and reply with its certificate.
//
static void GipFillWeakRandom(BYTE* p, int n) {
	static uint32_t s = 0;
	if (!s)
		s = GetTickCount() | 1;
	for (int i = 0; i < n; i++) {
		s = s * 1664525u + 1013904223u;      // Numerical Recipes LCG
		p[i] = (BYTE)(s >> 24);
	}
}

static void GipSendHostHello(deviceHandle* h) {
	BYTE p[58];
	memset(p, 0, sizeof(p));

	p[0] = GIP_AUTH_CTX_HANDSHAKE;                       // context
	p[1] = GIP_AUTH_OPT_ACK | GIP_AUTH_OPT_FROM_HOST;    // 0x41
	p[2] = 0x00;                                         // error
	p[3] = GIP_AUTH_CMD_HOST_HELLO;                      // 0x01
	p[4] = 0x00; p[5] = 0x2C;                            // be16 length = 44

	p[6] = GIP_AUTH_CMD_HOST_HELLO;                      // data.command
	p[7] = 0x01;                                         // data.version (v1)
	p[8] = 0x00; p[9] = 0x28;                            // be16 length = 40

	XeCryptRandom(p + 10, 32);                           // random[32]
	memcpy(g_gipHostRandom, p + 10, 32);                 // needed by the PRF
	// unknown1[4], unknown2[4], trailer[8] stay zero, as in xone.

	// HOST_HELLO is the first handshake message: reset the transcript and add it.
	// Sent packets contribute [6, 6+data_len); data_len = 58 - 6 - 8 = 44.
	GipTranscriptReset(&g_gipTranscript);
	GipTranscriptAdd(&g_gipTranscript, p + 6, 44);

	RM_DBG("RIFFMASTER: -> sending AUTH HOST_HELLO (58 bytes)\r\n");
	GipSend(h, GIP_CMD_AUTHENTICATE, GIP_OPT_INTERNAL | GIP_OPT_ACKNOWLEDGE,
		p, sizeof(p));
}

//
// Accumulate the chunked certificate so we can parse it, and settle the RSA
// endianness question with a deterministic known-answer test.
//
#define GIP_CERT_BUF_MAX 1100
static BYTE g_gipCertBuf[GIP_CERT_BUF_MAX];
static void GipHexDump(const char* tag, const BYTE* d, int n);   // defined below
static void GipSendChunked(deviceHandle* h, BYTE cmd, const BYTE* data, int total);
static void GipRegisterWithXam();
static void GipFillGuitarCaps(BYTE& type, BYTE& subType, WORD& flags, XINPUT_GAMEPAD& pad);
static void GipUnregisterFromXam();

#define GIP_AUTH_CMD_HOST_SECRET 0x05
#define GIP_AUTH_CMD_HOST_FINISH 0x07
#define GIP_AUTH_CMD_CLIENT_FINISH 0x08

static void GipRsaSelfTest() {
	// Auth payload = 10-byte header (handshake 6 + data 4) then the DER certificate.
	if (g_gipCertBytes <= 10) {
		RM_LOG("RIFFMASTER: RSA selftest skipped - no certificate\r\n");
		return;
	}
	const BYTE* der = g_gipCertBuf + 10;
	int derLen = g_gipCertBytes - 10;

	static BYTE modulus[RSA2048_BYTES];
	uint32_t pubExp = 0;
	if (!GipCertGetRsaPubKey(der, derLen, modulus, &pubExp)) {
		RM_LOG("RIFFMASTER: RSA selftest FAILED - could not parse pubkey from cert\r\n");
		return;
	}
	RM_DBG("RIFFMASTER: cert pubkey parsed: exponent=%u modulus starts %02X%02X%02X%02X\r\n",
		pubExp, modulus[0], modulus[1], modulus[2], modulus[3]);

	// Deterministic message: 48 bytes of 0xAA, PKCS#1 v1.5 with all-0xFF padding.
    // tools/rsa_check.py computes the expected ciphertext from the same modulus.
	static BYTE msg[48];
	static BYTE em[RSA2048_BYTES];
	static BYTE outA[RSA2048_BYTES];
	static BYTE outB[RSA2048_BYTES];
	memset(msg, 0xAA, sizeof(msg));
	if (!GipPkcs1Pad(msg, sizeof(msg), em, true)) {
		RM_LOG("RIFFMASTER: RSA selftest FAILED - padding\r\n");
		return;
	}

	// Expected ciphertext, computed offline from this modulus by tools/rsa_check.py.
	// Our own bignum modexp should reproduce it exactly.
	RM_DBG("RIFFMASTER: --- RSA SELFTEST (expect CA FA 27 9B ...) ---\r\n");

	bool ok = GipRsaPubCrypt(modulus, pubExp, em, outA);
	static const BYTE expect[8] = { 0xCA,0xFA,0x27,0x9B,0x03,0x68,0x3F,0x84 };
	bool pass = ok && !memcmp(outA, expect, 8);
	RM_LOG("RIFFMASTER: *** RSA SELFTEST: %s ***\r\n", pass ? "PASS" : "FAIL");
	if (!pass) {
		if (ok) GipHexDump("rsa", outA, 32);
		return;                       // do not build a handshake on broken crypto
	}
	(void)outB;

	// ---- HOST_SECRET -----------------------------------------------------
	// struct gip_auth_pkt_host_secret (refs/xone/auth/auth.c:87-93):
	//     header_full (10) + encrypted_pms (256) + trailer (8) = 274 bytes
	// data_len   = 274 - 6 - 8 = 260 = 0x0104   -> handshake.length
	// data.length= 260 - 4     = 256 = 0x0100
	static BYTE pms[48];
	static BYTE pkt[274];

	XeCryptRandom(pms, sizeof(pms));
	memcpy(g_gipPms, pms, sizeof(pms));      // kept for the PRF / master secret

	if (!GipPkcs1Pad(pms, sizeof(pms), em, false)) {
		RM_LOG("RIFFMASTER: HOST_SECRET padding failed\r\n");
		return;
	}
	if (!GipRsaPubCrypt(modulus, pubExp, em, pkt + 10)) {
		RM_LOG("RIFFMASTER: HOST_SECRET RSA failed\r\n");
		return;
	}

	memset(pkt, 0, 10);
	pkt[0] = GIP_AUTH_CTX_HANDSHAKE;
	pkt[1] = GIP_AUTH_OPT_ACK | GIP_AUTH_OPT_FROM_HOST;   // 0x41
	pkt[2] = 0x00;
	pkt[3] = GIP_AUTH_CMD_HOST_SECRET;                    // 0x05
	pkt[4] = 0x01; pkt[5] = 0x04;                         // be16 260
	pkt[6] = GIP_AUTH_CMD_HOST_SECRET;
	pkt[7] = 0x01;                                        // version 1
	pkt[8] = 0x01; pkt[9] = 0x00;                         // be16 256
	memset(pkt + 266, 0, 8);                              // trailer

	// Transcript: sent packets contribute [6, 6+data_len) - trailer excluded.
	GipTranscriptAdd(&g_gipTranscript, pkt + 6, 260);

	RM_DBG("RIFFMASTER: -> sending HOST_SECRET (274 bytes, chunked)\r\n");
	GipSendChunked(g_gipExt.deviceHandle, GIP_CMD_AUTHENTICATE, pkt, sizeof(pkt));
	g_gipAuthStage = 4;

	// ---- master secret + HOST_FINISH -------------------------------------
	// master_secret = PRF(pms, "Master Secret", host_random || client_random)[48]
	static BYTE randoms[64];
	memcpy(randoms, g_gipHostRandom, 32);
	memcpy(randoms + 32, g_gipClientRandom, 32);
	GipPrf(g_gipPms, sizeof(g_gipPms), "Master Secret", randoms, sizeof(randoms),
		g_gipMasterSecret, sizeof(g_gipMasterSecret));

	// verify_data = PRF(master_secret, "Host Finished", SHA256(transcript))[32]
	static BYTE transcript[32];
	GipTranscriptHash(&g_gipTranscript, transcript);

	static BYTE fin[50];
	memset(fin, 0, sizeof(fin));
	fin[0] = GIP_AUTH_CTX_HANDSHAKE;
	fin[1] = GIP_AUTH_OPT_ACK | GIP_AUTH_OPT_FROM_HOST;   // 0x41
	fin[2] = 0x00;
	fin[3] = GIP_AUTH_CMD_HOST_FINISH;                    // 0x07
	fin[4] = 0x00; fin[5] = 0x24;                         // be16 36
	fin[6] = GIP_AUTH_CMD_HOST_FINISH;
	fin[7] = 0x01;
	fin[8] = 0x00; fin[9] = 0x20;                         // be16 32
	GipPrf(g_gipMasterSecret, sizeof(g_gipMasterSecret), "Host Finished",
		transcript, sizeof(transcript), fin + 10, 32);
	// fin[42..49] trailer stays zero

	RM_DBG("RIFFMASTER: transcript %d bytes, hash %02X%02X%02X%02X\r\n",
		g_gipTranscript.len, transcript[0], transcript[1], transcript[2], transcript[3]);

	// HOST_FINISH is prepared now but NOT sent yet.
	//
	// The device must RSA-DECRYPT the premaster secret with its private key before it
	// can verify anything that depends on the master secret. In the capture that takes
	// ~680 ms, and the host waits for the device's response before sending HOST_FINISH.
	// The previous build sent both back to back and the handshake stalled.
	memcpy(g_gipHostFinish, fin, sizeof(fin));
	g_gipHostFinishReady = true;
	RM_DBG("RIFFMASTER: HOST_FINISH prepared, waiting for device to process HOST_SECRET\r\n");
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
static uint8_t  g_gipUserIndex = 0xFF;
static uint32_t g_gipDeviceContext = 0;
static DWORD    g_gipPacketNumber = 0;
static int      g_gipCapsLogged = 0;
static int      g_gipCaps2Logged = 0;

//
// Fill the capability fields the way a REAL Xbox 360 guitar reports them.
//
// The previous version put the live input state into capabilities->Gamepad. That is
// wrong: in XINPUT_CAPABILITIES the Gamepad member is a CAPABILITY MASK describing which
// inputs exist and at what resolution, not the current reading. Guitar Hero did not care;
// Rock Band refused to see the device at all, and this is the most likely reason.
//
// Values taken from a real wired Rock Band 1 Stratocaster:
//   refs/PlasticBand/Docs/Descriptor Dumps/Xbox 360/
//       Rock Band 1 Stratocaster Guitar Capabilities.txt
//     SubType:  0x06 (Guitar)
//     Flags:    0x000C (Voice, PluginModules)
//     Buttons:  0xF57F (DpadUp/Down/Left/Right, Start, Back, LeftThumb, LeftShoulder,
//                       Guide, A, B, X, Y)
//     RightThumb: X 0xFFC0  Y 0xFFC0
//
// ---------------------------------------------------------------------------
// Which SubType the virtual guitar reports.
//
// The XDK names two (xinputdefs.h:40-41):
//     XINPUT_DEVSUBTYPE_GUITAR           0x06   what Rock Band guitars report
//     XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE 0x07   what Guitar Hero guitars report
//
// `[VERIFIED on hardware]` 0x06 works in BOTH Rock Band and Guitar Hero: World Tour on
// the test console. PlasticBand documents GH guitars as reporting 0x07, but that is what
// the hardware ADVERTISES, not what the game REQUIRES - and empirically GH accepts 0x06.
//
// `[UNTESTED]` The older Guitar Hero titles - GH2, GH3, Aerosmith, Metallica,
// Van Halen, Smash Hits, Warriors of Rock - have not been tried. If one of them refuses
// to see the guitar, rebuild with:
//     -DRIFFMASTER_SUBTYPE=0x07
// and please report which title needed it, so this can become a documented list rather
// than a guess.
//
// Per-title automatic switching is the eventual answer and is NOT implemented, because
// it cannot be done honestly without hardware evidence. Sketch, for whoever does it:
// resolve XamGetCurrentTitleId, map the title ID to a subtype, and answer the capability
// hooks accordingly. The hard part is not the code - it is that every entry in that table
// needs a real console to confirm it, and a wrong entry silently breaks a game that
// currently works. Do not ship a table of guesses. See docs/KNOWN_ISSUES.md.
// ---------------------------------------------------------------------------
#ifndef RIFFMASTER_SUBTYPE
#define RIFFMASTER_SUBTYPE XINPUT_DEVSUBTYPE_GUITAR
#endif

// ---------------------------------------------------------------------------
// Per-title subtype overrides.
//
// *** THIS TABLE IS EMPTY ON PURPOSE. DO NOT POPULATE IT FROM MEMORY OR FROM A
// *** TITLE-ID LIST FOUND ONLINE. Every entry must come from a console log of the
// *** game actually failing with the default subtype.
//
// The default (0x06, Rock Band's subtype) is VERIFIED working in Rock Band 3 and
// Guitar Hero: World Tour. Until a title is observed to FAIL with it, adding an entry
// here can only break something that currently works - a wrong override is invisible
// until someone loads that game and finds no guitar.
//
// How to add an entry properly:
//   1. Boot the game with the guitar connected and this build loaded.
//   2. Read the "title id 0x........" line this prints on every title change.
//   3. If the guitar WORKED, add nothing.
//   4. If it did NOT work, add { <that id>, XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE, "name" }
//      and re-test the same game to confirm the override actually fixes it.
//
// The title ID is logged for every game either way, so a full pass over a library
// produces the ID list as a side effect, without anybody having to guess.
// ---------------------------------------------------------------------------
struct GipTitleSubType {
	DWORD       titleId;
	BYTE        subType;
	const char* name;
};

// Defined further down with the other resolved XAM pointers; declared here because the
// lookup below is defined before that block.
extern void* XamGetCurrentTitleIdPtr;

static const GipTitleSubType g_gipTitleSubTypes[] = {
	// Guitar Hero III: Legends of Rock.
	// `[VERIFIED both ways]` The ONLY title out of nine tested that does not see the
	// guitar with the default 0x06, and it works with 0x07. Every other Guitar Hero and
	// Rock Band title tested works with 0x06, so this is a per-title quirk and not an
	// era-wide split.
	// NTSC disc. A PAL or reissued copy may carry a different title ID and would need
	// its own entry - see the region warning in the README.
	{ 0x415607F7, XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE, "Guitar Hero III" },

	{ 0, 0, 0 }   // terminator
};

// Title IDs observed on hardware (NTSC discs, kernel 17559). 21 titles tested; exactly
// ONE needs an override. Kept as a comment rather than as table entries, because an entry
// matching the default would do nothing except create a place for a typo to break a
// working game.
//
//   Activision (0x4156)
//     0x415607E7  Guitar Hero II                          0x06  works
//     0x415607F7  Guitar Hero III: Legends of Rock        0x07  <- OVERRIDDEN above
//     0x41560819  Guitar Hero: Aerosmith                  0x06  works
//     0x4156081A  Guitar Hero: World Tour                 0x06  works
//     0x41560830  Guitar Hero: Metallica                  0x06  works
//     0x4156083D  Guitar Hero: Van Halen                  0x06  works
//     0x4156083E  Guitar Hero: Smash Hits                 0x06  works
//     0x41560840  Guitar Hero 5                           0x06  works
//     0x4156085C  Band Hero                               0x06  works
//     0x41560883  Guitar Hero 1                           0x06  works
//
//   Harmonix / MTV / EA (0x4541)
//     0x45410829  Rock Band 1                             0x06  works
//     0x45410869  Rock Band 2                             0x06  works
//     0x45410881  Rock Band Track Pack Vol. 2             0x06  works
//     0x45410889  Rock Band: AC/DC Live                   0x06  works
//     0x454108B0  Rock Band Track Pack: Classic Rock      0x06  works
//     0x454108B1  The Beatles: Rock Band                  0x06  works
//     0x454108CA  Rock Band Country Track Pack            0x06  works
//     0x454108CD  Rock Band Metal Track Pack              0x06  works
//     0x45410914  Rock Band 3                             0x06  works
//     0x4541092C  Rock Band Country Track Pack 2          0x06  works
//
//   Warner Bros. (0x5752)
//     0x575207F0  Lego Rock Band                          0x06  works
//
//   Untested: Green Day: Rock Band crashes on startup, twice, before input matters.
//   Not attributed to this plugin - it has not been checked with the plugin unloaded.
//
// DO NOT INFER A SUBTYPE FROM THE PUBLISHER PREFIX. Nine of the ten Activision titles
// work on 0x06, the *Rock Band* subtype, and the one exception (GH3) sits in the middle
// of that range. The split is per-title and has no pattern anyone has found.

static BYTE GipSubTypeForCurrentTitle() {
	// Cached and refreshed at most once a second. This runs on the capability path,
	// which the dash polls ~8 times per 100 ms, and it calls into XAM from inside a XAM
	// hook - re-entrancy that has bitten this project before. Once a second is plenty:
	// a title change is a disc load, not a hot event.
	static DWORD s_lastPoll = 0;
	static DWORD s_titleId = 0;
	static BYTE  s_subType = RIFFMASTER_SUBTYPE;

	DWORD now = GetTickCount();
	if (XamGetCurrentTitleIdPtr && (s_lastPoll == 0 || (now - s_lastPoll) >= 1000)) {
		s_lastPoll = now;
		typedef DWORD(*xam_get_current_title_id_t)(void);
		DWORD id = ((xam_get_current_title_id_t)XamGetCurrentTitleIdPtr)();

		if (id != s_titleId) {
			s_titleId = id;
			s_subType = g_rmCfg.defaultSubType;
			const char* why = "default";

			for (int i = 0; g_gipTitleSubTypes[i].titleId != 0; i++) {
				if (g_gipTitleSubTypes[i].titleId == id) {
					s_subType = g_gipTitleSubTypes[i].subType;
					why = g_gipTitleSubTypes[i].name;
					break;
				}
			}

			// riffmaster.ini wins over the built-in table. Title IDs differ between
			// regions and reissues, so a user with a PAL disc must be able to fix it
			// without rebuilding - and equally must be able to override an entry of
			// ours that turns out to be wrong on their console.
			BYTE user = RmCfgSubTypeOverride(id);
			if (user) {
				s_subType = user;
				why = "riffmaster.ini";
			}
			// One line per title change. Safe on this hot path precisely because a
			// title change is rare; do not move this outside the `id != s_titleId` test.
			RM_LOG("RIFFMASTER: title id 0x%08X -> SubType 0x%02X (%s)\r\n",
				id, s_subType, why);
		}
	}
	return s_subType;
}

static void GipFillGuitarCaps(BYTE& type, BYTE& subType, WORD& flags, XINPUT_GAMEPAD& pad) {
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
#ifdef RIFFMASTER_NO_XAM_REGISTER
	// L7-noxam: exercise the ENTIRE USB half — claim, both interrupt endpoints, the
	// re-arm loop, the full RSA auth handshake — and never touch XAM. The guitar will
	// not work; that is the point. It splits L7's two halves so the freeze can be
	// attributed to one of them instead of to "the claim path" as a whole.
	RM_LOG("RIFFMASTER: XAM registration SKIPPED (noxam variant)\r\n");
	return;
#endif
	if (g_gipUserIndex != 0xFF)
		return;                       // already registered

	// Pick a slot hiddriver360 is not using so a real pad and the guitar cannot
	// collide on the same XAM index.
	int idx = -1;
	for (int i = 0; i < (int)(sizeof(connectedControllers) / sizeof(Controller)); i++) {
		if (!connectedControllers[i].controllerDriver) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		RM_LOG("RIFFMASTER: no free XAM slot!\r\n");
		return;
	}

	uint8_t userIndex = 0xFF;
	uint32_t context = 0x0000000010000005 + idx;
	XamUserBindDeviceCallback(0xa7553952 + idx, context, 0, false, &userIndex);

	g_gipUserIndex = userIndex;
	g_gipDeviceContext = context;
	RM_LOG("XBOXINPUT: registered virtual GAMEPAD in XAM, user index %d\r\n",
		userIndex);
}

static void GipUnregisterFromXam() {
	g_gipReady = false;
	if (g_gipUserIndex == 0xFF)
		return;
	int idx = (int)(g_gipDeviceContext - 0x0000000010000005);
	XamUserBindDeviceCallback(0xa7553952 + idx, g_gipDeviceContext, 0, true, 0);
	// RM_DBG, not RM_LOG: only caller is UsbdRemoveDeviceCompleteHook, and DbgPrint
	// inside the USB removal completion is the freeze suspect. See the banner there.
	RM_DBG("RIFFMASTER: removed virtual guitar from XAM\r\n");
	g_gipUserIndex = 0xFF;
	g_gipDeviceContext = 0;
}

static void GipSessionRegisterWithXam(GipSessionSlot* session) {
	if (!session || session->userIndex != 0xFF)
		return;
	for (int idx = 0; idx < 4; ++idx) {
		uint32_t context = 0x10000005 + idx;
		bool used = (context == g_gipDeviceContext);
		for (int i = 0; i < GIP_MAX_SESSIONS; ++i)
			if (g_gipSessions[i].reserved && g_gipSessions[i].deviceContext == context)
				used = true;
		if (used || connectedControllers[idx].controllerDriver)
			continue;
		uint8_t user = 0xFF;
		XamUserBindDeviceCallback(0xa7553952 + idx, context, 0, false, &user);
		session->userIndex = user;
		session->deviceContext = context;
		session->ready = (user != 0xFF);
		return;
	}
}

//
// ---- chunked SEND -----------------------------------------------------------
// HOST_SECRET is 274 bytes (10 header + 256 encrypted PMS + 8 trailer) and cannot fit
// in one 64-byte transfer, so it must be chunked the same way the device chunks its
// replies. Encoding rules from refs/xone/bus/protocol.c:244-262, all confirmed against
// the captured host packets:
//
//   first chunk : options |= CHUNK_START|CHUNK, chunk_offset varint = TOTAL length
//   middle      : options |= CHUNK,             chunk_offset varint = running offset
//   terminator  : zero-length chunk at offset == total
//   the header must be padded to an EVEN length, by setting the continuation bit on
//   the last length byte and appending a 0x00
//
// Capture cross-check:
//   06 F0 04 3A 92 02   len=58, chunk=274 (0x92 0x02)      -> 6-byte header, even
//   06 A0 04 BA 00 3A   len=58 padded (BA 00), chunk=58    -> 6-byte header, even
//
static int GipEncodeVarint(BYTE* buf, uint32_t val) {
	int i;
	for (i = 0; i < 4; i++) {
		buf[i] = (BYTE)val;
		if (val > 0x7F)
			buf[i] |= 0x80;
		val >>= 7;
		if (!val)
			break;
	}
	return i + 1;
}

static int GipVarintLen(uint32_t val) {
	int n = 1;
	while (val > 0x7F) { val >>= 7; n++; }
	return n;
}

//
// Send one chunk. Returns bytes written to the wire, or -1.
//
static int GipSendChunk(deviceHandle* h, BYTE cmd, BYTE opts, BYTE seq,
                        uint32_t chunkOff, const BYTE* data, int len) {
	if (!g_gipOutOpen || !h)
		return -1;

	BYTE* buf = g_gipOutBuf[g_gipOutBufIdx];
	g_gipOutBufIdx = (g_gipOutBufIdx + 1) % GIP_TX_BUFS;

	int i = 0;
	buf[i++] = cmd;
	buf[i++] = opts;
	buf[i++] = seq;   // ALL chunks of one message share one sequence

	// Decide padding up front: header = 3 + len(varint) + len(chunk varint)
	int hdrLen = 3 + GipVarintLen((uint32_t)len) + GipVarintLen(chunkOff);
	bool pad = (hdrLen % 2) != 0;

	i += GipEncodeVarint(buf + i, (uint32_t)len);
	if (pad) {
		buf[i - 1] |= 0x80;      // continuation on the last length byte
		buf[i++] = 0x00;
	}
	i += GipEncodeVarint(buf + i, chunkOff);

	if (data && len > 0) {
		memcpy(buf + i, data, len);
		i += len;
	}

	SendInterruptRequest(h, &g_gipOutTrb, buf, i, (DWORD)noopCompleteHandler);
	return i;
}

//
// ACK-driven chunked send.
//
// The first attempt pushed all six chunks into the USB queue back to back. The device
// ACKed only the first and last and the handshake stalled - it never assembled a
// complete payload. The captured host waits for each acknowledgement before sending
// the next chunk (15 ms and 29 ms gaps around the two ACK points), so we do the same:
// every chunk requests an ACK, and the next one is sent from the ACK handler.
//
#define GIP_CHUNK_SIZE 58
static BYTE g_gipTxBuf[320];
static int  g_gipTxTotal = 0;
static int  g_gipTxOff = 0;
static BYTE g_gipTxCmd = 0;
static bool g_gipTxActive = false;
static BYTE g_gipTxSeq = 1;

//
// Drives the send in the three bursts the captured host uses:
//   burst 1: chunk 0 with CHUNK_START|ACK        -> wait for ACK
//   burst 2: all middle chunks (no ACK) then the
//            last data chunk with ACK            -> wait for ACK
//   burst 3: zero-length terminator (no ACK)     -> done
//
static void GipTxSendNext(deviceHandle* h) {
	if (!g_gipTxActive || !h)
		return;

	if (g_gipTxOff >= g_gipTxTotal) {
		GipSendChunk(h, g_gipTxCmd, GIP_OPT_INTERNAL | GIP_OPT_CHUNK, g_gipTxSeq,
			(uint32_t)g_gipTxTotal, 0, 0);
		g_gipTxActive = false;
		RM_DBG("RIFFMASTER: -> chunked send complete (%d bytes, seq=%d)\r\n",
			g_gipTxTotal, g_gipTxSeq);
		return;
	}

	// Emit chunks until one that requests an acknowledgement has been sent.
	for (;;) {
		int n = (g_gipTxTotal - g_gipTxOff > GIP_CHUNK_SIZE)
		        ? GIP_CHUNK_SIZE : (g_gipTxTotal - g_gipTxOff);
		bool first = (g_gipTxOff == 0);
		bool last = (g_gipTxOff + n >= g_gipTxTotal);

		BYTE opts = GIP_OPT_INTERNAL | GIP_OPT_CHUNK;
		if (first) opts |= GIP_OPT_CHUNK_START | GIP_OPT_ACKNOWLEDGE;
		if (last)  opts |= GIP_OPT_ACKNOWLEDGE;

		GipSendChunk(h, g_gipTxCmd, opts, g_gipTxSeq,
			first ? (uint32_t)g_gipTxTotal : (uint32_t)g_gipTxOff,
			g_gipTxBuf + g_gipTxOff, n);
		g_gipTxOff += n;

		if (opts & GIP_OPT_ACKNOWLEDGE)
			return;                       // wait for the device
		if (g_gipTxOff >= g_gipTxTotal)
			return;
	}
}

static void GipSendChunked(deviceHandle* h, BYTE cmd, const BYTE* data, int total) {
	if (total > (int)sizeof(g_gipTxBuf))
		return;
	memcpy(g_gipTxBuf, data, total);
	g_gipTxTotal = total;
	g_gipTxOff = 0;
	g_gipTxCmd = cmd;
	// ONE sequence number for the whole message. Verified in the capture: every
	// HOST_SECRET chunk, including the terminator, carries seq=4. Allocating a fresh
	// sequence per chunk makes the device see six unrelated messages and it can never
	// reassemble them - which is exactly what happened.
	g_gipTxSeq = GipNextSeq();
	g_gipTxActive = true;
	RM_DBG("RIFFMASTER: -> chunked send start cmd=%02X, %d bytes, seq=%d\r\n",
		cmd, total, g_gipTxSeq);
	GipTxSendNext(h);
}

//
// The device does not volunteer its hello or certificate - the host must REQUEST
// each one. struct gip_auth_request (auth.c:71-75) is just the 6-byte handshake
// header plus an 8-byte trailer = 14 bytes, built by gip_auth_request_pkt
// (auth.c:186-198) with:
//     options = REQUEST(0x02) | FROM_HOST(0x40) = 0x42
//     length  = be16(expected_payload + sizeof(gip_auth_header_data))
//
// Confirmed against the capture:
//     request CLIENT_HELLO       -> 00 42 00 02 00 54   (0x54 = 80 + 4)
//     request CLIENT_CERTIFICATE -> 00 42 00 03 04 04   (0x404 = 1024 + 4)
//
#define GIP_AUTH_OPT_REQUEST 0x02
#define GIP_AUTH_CLIENT_HELLO_LEN 80     // sizeof(gip_auth_pkt_client_hello) = 32 + 48
#define GIP_AUTH_CERT_MAX_LEN    1024    // GIP_AUTH_CERTIFICATE_MAX_LEN

static void GipSendAuthRequest(deviceHandle* h, BYTE cmd, uint16_t expectedLen) {
	BYTE p[14];
	memset(p, 0, sizeof(p));

	uint16_t dataLen = (uint16_t)(expectedLen + 4);   // + sizeof(gip_auth_header_data)
	p[0] = GIP_AUTH_CTX_HANDSHAKE;
	p[1] = GIP_AUTH_OPT_REQUEST | GIP_AUTH_OPT_FROM_HOST;   // 0x42
	p[2] = 0x00;
	p[3] = cmd;
	p[4] = (BYTE)(dataLen >> 8);      // big-endian
	p[5] = (BYTE)(dataLen & 0xFF);
	// trailer[8] stays zero

	RM_DBG("RIFFMASTER: -> REQUEST auth cmd=%02X (expect %u bytes)\r\n", cmd, expectedLen);
	GipSend(h, GIP_CMD_AUTHENTICATE, GIP_OPT_INTERNAL | GIP_OPT_ACKNOWLEDGE, p, sizeof(p));
}

//
// Dump bytes in 32-per-line groups. The device's certificate arrives chunked and
// is the thing we need captured in order to build the real handshake.
//
static void GipHexDump(const char* tag, const BYTE* d, int n) {
	char line[3 * 32 + 1];
	for (int off = 0; off < n; off += 32) {
		int m = (n - off > 32) ? 32 : (n - off);
		int c = 0;
		for (int i = 0; i < m; i++) {
			static const char* hx = "0123456789ABCDEF";
			line[c++] = hx[(d[off + i] >> 4) & 0xF];
			line[c++] = hx[d[off + i] & 0xF];
			line[c++] = ' ';
		}
		line[c] = 0;
		RM_DBG("RIFFMASTER:   %s[%03d] %s\r\n", tag, off, line);
	}
}

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
			RM_DBG("RIFFMASTER: GIP truncated pkt cmd=%02X len=%u (have %d)\r\n",
				hdr.command, hdr.packetLength, len - off);
			break;
		}

		const BYTE* payload = data + off + hdr.headerLength;
		g_gipPacketsSeen++;

		switch (hdr.command) {
		case GIP_CMD_ANNOUNCE: {
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
					RM_DBG("RIFFMASTER: GIP ANNOUNCE seq=%d VID=%04X PID=%04X\r\n",
						hdr.sequence,
						payload[8] | (payload[9] << 8),
						payload[10] | (payload[11] << 8));
				RM_DBG("RIFFMASTER: -> sending IDENTIFY%s\r\n",
					retryIdentify ? " (retry)" : "");
				g_gipIdentifySent = true;
				g_gipLastIdentifyTick = now;
				g_gipChunkTotal = 0;
				GipSend(g_gipExt.deviceHandle, GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0, 0);
			}
			break;
		}

		case GIP_CMD_IDENTIFY:
			// Chunked descriptor reply. ACK when the device asks us to, then power on
			// once the terminating zero-length chunk arrives.
			g_gipIdentifyReplySeen = true;
			RM_DBG("RIFFMASTER: GIP IDENTIFY chunk opts=%02X len=%u off=%u\r\n",
				hdr.options, hdr.packetLength, hdr.chunkOffset);

			if (hdr.options & GIP_OPT_ACKNOWLEDGE)
				GipSendAck(g_gipExt.deviceHandle, &hdr);

			if (hdr.packetLength == 0 && !g_gipPoweredOn) {
				g_gipPoweredOn = true;
				RM_DBG("RIFFMASTER: -> identify complete, sending init sequence\r\n");

				// Replay what the Windows host sent, in order, from the captured
				// enumeration (docs/gip_riffmaster.md section 5 stage 2). Previously we
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

				RM_DBG("RIFFMASTER: -> sent locale, POWER ON, LED\r\n");

				// Standard Microsoft gamepads stream input after IDENTIFY/POWER; unlike
				// the RiffMaster dongle they do not require the guitar RSA exchange.
				if (!g_gipReady) {
					g_gipReady = true;
					// Gamepads skip the guitar authentication sequence. Reaching this
					// point is therefore a successful session and may refill the safe
					// reconnect budget after a sleep/wake bounce.
					g_gipClaimAttempts = 0;
					GipRegisterWithXam();
				}
			}
			break;

		case GIP_CMD_STATUS:
			if (hdr.packetLength >= 1) {
				RM_DBG("RIFFMASTER: GIP STATUS 0x%02X (%s)\r\n",
					payload[0], (payload[0] & 0x80) ? "connected" : "DISCONNECTED");

				// The captured host starts auth right after the first connected
				// STATUS (docs/gip_riffmaster.md section 5: STATUS at +1.7379,
				// first auth packet at +1.7540). Match that ordering.
				// Fallback trigger. Deliberately NOT gated on g_gipPoweredOn -
				// that gate is what silently blocked the first attempt.
				if ((payload[0] & 0x80) && !g_gipAuthStarted) {
					RM_DBG("RIFFMASTER: (auth starting from STATUS path)\r\n");
					g_gipAuthStarted = true;
					g_gipAuthChunkTotal = 0;
					g_gipCertBytes = 0;
					g_gipAuthStage = 0;
					GipSendHostHello(g_gipExt.deviceHandle);
				}
			}
			break;

		case GIP_CMD_AUTHENTICATE:
			// Response to HOST_HELLO. Expect CLIENT_HELLO (0x02) then a chunked
			// CLIENT_CERTIFICATE (0x03) containing the device's RSA public key.
			if (hdr.options & GIP_OPT_CHUNK) {
				// Chunked: the inner header is only present on the first chunk.
				if (hdr.options & GIP_OPT_CHUNK_START)
					g_gipAuthChunkTotal = hdr.chunkOffset;
				// Certificate hex is NOT dumped any more. Dumping ~30 lines per run
				// saturated the xbWatson channel and silently dropped later output -
				// including two certificate chunks and the entire RSA self-test result.
				// The certificate is already captured in reference/riffmaster_cert.der.
				RM_DBG("RIFFMASTER: AUTH chunk opts=%02X len=%u off=%u total=%u\r\n",
					hdr.options, hdr.packetLength, hdr.chunkOffset, g_gipAuthChunkTotal);
				// Accumulate at the reported chunk offset rather than appending, so a
				// dropped or reordered chunk cannot silently shift the whole buffer.
				{
					uint32_t at = (hdr.options & GIP_OPT_CHUNK_START) ? 0 : hdr.chunkOffset;
					if (at + hdr.packetLength <= GIP_CERT_BUF_MAX) {
						memcpy(g_gipCertBuf + at, payload, hdr.packetLength);
						if ((int)(at + hdr.packetLength) > g_gipCertBytes)
							g_gipCertBytes = (int)(at + hdr.packetLength);
					}
				}

				if (hdr.options & GIP_OPT_ACKNOWLEDGE) {
					// Reuse the identify ACK path - same gip_pkt_acknowledge layout.
					uint32_t saved = g_gipChunkTotal;
					g_gipChunkTotal = g_gipAuthChunkTotal;
					GipSendAck(g_gipExt.deviceHandle, &hdr);
					g_gipChunkTotal = saved;
				}
				if (hdr.packetLength == 0) {
					RM_DBG("RIFFMASTER: *** AUTH stage %d complete, %d bytes ***\r\n",
						g_gipAuthStage, g_gipCertBytes);
					// A completed chunked reply advances the sequence.
					if (g_gipAuthStage == 1) {
						// CLIENT_HELLO payload: 10-byte header, then random[32].
						if (g_gipCertBytes >= 42)
							memcpy(g_gipClientRandom, g_gipCertBuf + 10, 32);
						// Received packet -> transcript gets [6, len).
						GipTranscriptAdd(&g_gipTranscript, g_gipCertBuf + 6, g_gipCertBytes - 6);
						g_gipAuthStage = 2;
						g_gipCertBytes = 0;
						GipSendAuthRequest(g_gipExt.deviceHandle,
							GIP_AUTH_CMD_CLIENT_CERT, GIP_AUTH_CERT_MAX_LEN);
					}
					else if (g_gipAuthStage == 2) {
						// Received packet -> transcript gets [6, len). Must happen
						// BEFORE HOST_SECRET, which is hashed after it.
						GipTranscriptAdd(&g_gipTranscript, g_gipCertBuf + 6,
							g_gipCertBytes - 6);
						RM_DBG("RIFFMASTER: *** CERTIFICATE RECEIVED - dumped above ***\r\n");
						g_gipAuthStage = 3;
						GipRsaSelfTest();
					}
					else if (g_gipAuthStage == 6) {
						// CLIENT_FINISH received. We do not verify it - the device is
						// the party being authenticated, and it has already accepted
						// everything we sent. Tell it the handshake is done.
						//
						// gip_auth_send_complete (refs/xone/auth/auth.c:489-497):
						//   { context = CONTROL(0x01), control = COMPLETE(0x00) }
						//   2 bytes, sent WITHOUT requesting an acknowledgement.
						static const BYTE done[2] = { 0x01, 0x00 };
						RM_DBG("RIFFMASTER: *** CLIENT_FINISH received (%d bytes) ***\r\n",
							g_gipCertBytes);
						RM_DBG("RIFFMASTER: -> sending AUTH COMPLETE\r\n");
						GipSend(g_gipExt.deviceHandle, GIP_CMD_AUTHENTICATE,
							GIP_OPT_INTERNAL, done, sizeof(done));
						g_gipAuthStage = 7;
						RM_LOG("RIFFMASTER: *** AUTH HANDSHAKE COMPLETE ***\r\n");
						// A connection that got all the way to auth is a REAL one, so
						// refresh the claim budget here rather than in teardown. See the
						// banner on GIP_CLAIM_MAX_ATTEMPTS: resetting on teardown let a
						// disconnect bounce-storm re-claim without limit.
						g_gipClaimAttempts = 0;
						GipRegisterWithXam();
					}
				}
			}
			else if (hdr.packetLength >= 6) {
				RM_DBG("RIFFMASTER: AUTH ctx=%02X opt=%02X err=%02X cmd=%02X len=%u (gip len=%u)\r\n",
					payload[0], payload[1], payload[2], payload[3],
					(payload[4] << 8) | payload[5], hdr.packetLength);
				GipHexDump("auth", payload, (int)hdr.packetLength);
				if (hdr.options & GIP_OPT_ACKNOWLEDGE)
					GipSendAck(g_gipExt.deviceHandle, &hdr);

				if (payload[2] != 0x00)
					RM_LOG("RIFFMASTER: !!! device reported auth error 0x%02X !!!\r\n", payload[2]);

				// Device acknowledged HOST_HELLO at the auth layer
				// (capture: 00 C1 00 01 00 00). Now request its hello.
				if (payload[3] == GIP_AUTH_CMD_HOST_HELLO && g_gipAuthStage == 0) {
					g_gipAuthStage = 1;
					g_gipCertBytes = 0;
					GipSendAuthRequest(g_gipExt.deviceHandle,
						GIP_AUTH_CMD_CLIENT_HELLO, GIP_AUTH_CLIENT_HELLO_LEN);
				}
				// The device answers each completed step with a short zero-length
				// packet. Drive the tail of the handshake off those, rather than
				// firing everything back to back and outrunning its RSA decrypt.
				else if (g_gipAuthStage == 4 && g_gipHostFinishReady) {
					g_gipAuthStage = 5;
					g_gipHostFinishReady = false;
					RM_DBG("RIFFMASTER: -> sending HOST_FINISH (50 bytes)\r\n");
					// HOST_FINISH also contributes to the transcript: [6, 6+36).
					GipTranscriptAdd(&g_gipTranscript, g_gipHostFinish + 6, 36);
					GipSend(g_gipExt.deviceHandle, GIP_CMD_AUTHENTICATE,
						GIP_OPT_INTERNAL | GIP_OPT_ACKNOWLEDGE,
						g_gipHostFinish, 50);
				}
				else if (g_gipAuthStage == 5) {
					// HOST_FINISH accepted - now ask for CLIENT_FINISH.
					// sizeof(gip_auth_pkt_client_finish) = 32 + 32 = 64, so the
					// request length is 68 (0x44), matching the capture.
					g_gipAuthStage = 6;
					g_gipCertBytes = 0;
					GipSendAuthRequest(g_gipExt.deviceHandle,
						GIP_AUTH_CMD_CLIENT_FINISH, 64);
				}
			}
			else {
				RM_DBG("RIFFMASTER: AUTH short pkt len=%u\r\n", hdr.packetLength);
			}
			break;

		case GIP_CMD_VIRTUAL_KEY:
			if (hdr.packetLength >= 2 && payload[1] == GIP_VKEY_GUIDE) {
				const bool down = payload[0] != 0;
				// Ignore any repeated DOWN packet while the physical button remains
				// held. Otherwise it can be interpreted as a second dashboard press,
				// immediately closing the Guide that the first one opened.
				if (down && !g_gipGuideDown)
					g_gipGuidePending = true;
				if (down && !g_gipGuideDown)
					g_gipGuideOverlayOpen = !g_gipGuideOverlayOpen;
				g_gipGuideDown = down;
				RM_DBG("RIFFMASTER: GIP GUIDE %s\r\n", payload[0] ? "DOWN" : "UP");
			}
			break;

		case GIP_CMD_INPUT:
			if (GipParseGamepadInput(payload, (int)hdr.packetLength, &g_gipState)) {
				g_gipInputsSeen++;
				// Rate-limited: these arrive at ~40 Hz and would flood the log.
				if (g_gipInputsSeen <= 3 || (g_gipInputsSeen % 400) == 0)
					RM_DBG("XBOXINPUT: GIP INPUT #%d btn=%04X\r\n",
						g_gipInputsSeen, g_gipState.buttons);
			}
			break;

		case GIP_CMD_ACKNOWLEDGE:
			// Drives the chunked send: one chunk per acknowledgement.
			if (g_gipTxActive) {
				GipTxSendNext(g_gipExt.deviceHandle);
			}
			else {
				RM_DBG("RIFFMASTER: GIP ACK seq=%d len=%u\r\n",
					hdr.sequence, hdr.packetLength);
			}
			break;

		default:
			RM_DBG("RIFFMASTER: GIP cmd=%02X opts=%02X seq=%d len=%u\r\n",
				hdr.command, hdr.options, hdr.sequence, hdr.packetLength);
			break;
		}

		off += total;
	}
}

// Minimal gamepad-only GIP path for additional controller sessions.  Authentication
// packets are intentionally not shared with the legacy RiffMaster path: official
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
			if (hdr.packetLength >= 2 && payload[1] == GIP_VKEY_GUIDE) {
				bool down = payload[0] != 0;
				if (down && !session->guideDown)
					session->guidePending = true;
				session->guideDown = down;
			}
			break;
		case GIP_CMD_INPUT:
			if (GipParseGamepadInput(payload, (int)hdr.packetLength, &session->state))
				session->inputsSeen++;
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

int32_t GipInterruptComplete(DWORD trbAddr, int32_t status) {
	HidControllerExtension* ext = (HidControllerExtension*)((BYTE*)trbAddr - 4);

	// Bail out before touching anything if the device is already gone. This callback
	// can still fire once after teardown with a completion that was already in flight;
	// parsing or re-arming at that point is a use-after-free.
	if (!ext || !ext->deviceHandle)
		return 0;

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
	// endpoint should not permanently kill a working guitar.
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
	if (productive) {
		g_gipReadErrors = 0;
		GipHandleTransfer(g_gipReadBuf, (int)ext->interruptTrb.length);
	}
	else if (++g_gipReadErrors >= GIP_MAX_CONSECUTIVE_READ_ERRORS) {
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
			RM_LOG("RIFFMASTER: read loop STOPPED after %d unproductive completions "
				"(last status 0x%08X) - disconnect guard fired\r\n",
				GIP_MAX_CONSECUTIVE_READ_ERRORS, status);
		}
		return status;
	}
	else if (status != 0 && g_gipPacketsSeen < 4) {
		RM_DBG("RIFFMASTER: GIP read status 0x%08X\r\n", status);
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
	int32_t queued = UsbdQueueAsyncTransfer(ext->deviceHandle, &ext->interruptTrb);
	if (queued != 0 && ++g_gipReadErrors >= GIP_MAX_CONSECUTIVE_READ_ERRORS) {
		ext->deviceHandle = 0;
		g_gipReadLoopStopped = true;
	}
	return queued;
}

//
// SET_CONFIGURATION completed - open the GIP interrupt IN endpoint and start reading.
// Extension base is recovered by subtracting controlTrb's offset (36).
//
int32_t GipSetConfigComplete(DWORD trbAddr, int32_t status) {
	XboxInputSetDiagStage(50);
	HidControllerExtension* ext = (HidControllerExtension*)((BYTE*)trbAddr - 36);

	RM_LOG("RIFFMASTER: SET_CONFIGURATION completed status=0x%08X\r\n", status);
	if (status != 0)
		return status;

	// Prefer the descriptor. It returned NULL on hardware for this device - the lookup
	// appears to depend on interface state our non-standard claim path never established -
	// so sweep every (type, direction, index) first and report what, if anything, it knows.
	usb_endpoint_descriptor* ep = 0;
	for (int xfer = 0; xfer <= 3 && !ep; xfer++) {
		for (int dir = 0; dir <= 1 && !ep; dir++) {
			for (int idx = 0; idx < 2; idx++) {
				usb_endpoint_descriptor* e =
					UsbdGetEndpointDescriptor(ext->deviceHandle, idx, xfer, dir);
				if (!e || e->bLength != 7 || e->bDescriptorType != 5)
					continue;
				RM_DBG("RIFFMASTER: descriptor sweep found EP %02X attr=%02X [q:%d/%d/%d]\r\n",
					e->bEndpointAddress, e->bmAttributes, xfer, dir, idx);
				// We want the interrupt IN endpoint specifically.
				if ((e->bEndpointAddress & 0x80) && (e->bmAttributes & 3) == USB_ENDPOINT_TYPE_INTERRUPT) {
					ep = e;
					break;
				}
			}
		}
	}

	BYTE     epAddr;
	uint16_t pkt;
	BYTE     interval;

	if (ep) {
		epAddr   = ep->bEndpointAddress;
		pkt      = swap_endianness_16(ep->wMaxPacketSize) & 0x7FF;
		interval = ep->bInterval;
		RM_DBG("RIFFMASTER: using descriptor values\r\n");
	}
	else {
		// Fall back to the values captured from this exact device on PC and confirmed
		// twice: Wireshark's dissected fields, and the raw configuration descriptor
		// bytes in docs/riffmaster_descriptors.bin. See docs/gip_riffmaster.md section 2.
		// This is a verified constant, not a guess - but it IS device-specific, which is
		// acceptable because we only claim this one VID/PID.
		// The Xbox One S controller observed on the target console exposes IN on
		// 0x82 (OUT is 0x02), not the RiffMaster guitar's 0x81 endpoint.
		epAddr   = 0x82;
		pkt      = 64;
		interval = 4;
		RM_DBG("RIFFMASTER: descriptor lookup failed - using VERIFIED capture values\r\n");
	}

	RM_DBG("RIFFMASTER: opening EP %02X maxPacket=%d interval=%d\r\n", epAddr, pkt, interval);

	NTSTATUS s = UsbdOpenEndpoint(ext->deviceHandle, USB_ENDPOINT_TYPE_INTERRUPT,
		epAddr, pkt, interval, (DWORD*)&ext->interruptTrb);
	if (NT_ERROR(s)) {
		RM_LOG("RIFFMASTER: UsbdOpenEndpoint FAILED 0x%08X\r\n", s);
		return s;
	}
	RM_LOG("RIFFMASTER: *** interrupt IN endpoint OPEN - starting GIP reads ***\r\n");
	XboxInputSetDiagStage(60);

	// Open the interrupt OUT endpoint too - without it we can never answer ANNOUNCE.
	// EP 0x02 OUT, INTERRUPT, 64, bInterval 4 (docs/gip_riffmaster.md section 2).
	{
		BYTE     outAddr = 0x02;
		uint16_t outPkt = 64;
		BYTE     outInterval = 4;
		usb_endpoint_descriptor* oe = UsbdGetEndpointDescriptor(
			ext->deviceHandle, 0, USB_ENDPOINT_TYPE_INTERRUPT, USB_DIRECTION_OUT);
		if (oe && oe->bLength == 7 && oe->bDescriptorType == 5) {
			outAddr = oe->bEndpointAddress;
			outPkt = swap_endianness_16(oe->wMaxPacketSize) & 0x7FF;
			outInterval = oe->bInterval;
		}
		NTSTATUS os = UsbdOpenEndpoint(ext->deviceHandle, USB_ENDPOINT_TYPE_INTERRUPT,
			outAddr, outPkt, outInterval, (DWORD*)&g_gipOutTrb);
		g_gipOutOpen = !NT_ERROR(os);
		RM_LOG("RIFFMASTER: interrupt OUT EP %02X -> 0x%08X %s\r\n",
			outAddr, os, g_gipOutOpen ? "OK" : "FAILED");
	}

	if (pkt > GIP_READ_BUF_SIZE)
		pkt = GIP_READ_BUF_SIZE;

#ifdef RIFFMASTER_NO_READ
	// L7-noread: claim the device and open all three endpoints exactly as normal, then
	// never queue a single interrupt read. Nothing ever completes, so GipInterruptComplete
	// can never run and the read loop cannot exist in any form.
	//
	// This is the split that should have come before any attempted fix: it separates
	// "having claimed the device and opened its endpoints" from "servicing it". The
	// guitar will not work - no reads means no input and no auth.
	RM_LOG("RIFFMASTER: interrupt reads NOT started (noread variant)\r\n");
	return 0;
#endif
	memset(g_gipReadBuf, 0, sizeof(g_gipReadBuf));
	ext->interruptTrb.savedEndpoint = ext->interruptTrb.endpoint;
	ext->interruptTrb.length = pkt;
	ext->interruptTrb.buffer = g_gipReadBuf;
	ext->interruptTrb.callback = (DWORD)GipInterruptComplete;
	ext->interruptTrb.flags = 1;
	return UsbdQueueAsyncTransfer(ext->deviceHandle, &ext->interruptTrb);
}

// Additional controllers use their own callback chain and never touch the legacy
// g_gipExt / g_gipReadBuf globals used by the proven single-controller path.
static int32_t GipSessionInterruptComplete(DWORD trbAddr, int32_t status) {
	HidControllerExtension* ext = (HidControllerExtension*)((BYTE*)trbAddr - 4);
	GipSessionSlot* session = GipSessionFromExtension(ext);
	if (!session || !session->reserved || !ext || !ext->deviceHandle)
		return 0;
	if (status == 0 && session->readBuf[0] != 0) {
		session->readErrors = 0;
		GipSessionHandleTransfer(session, session->readBuf, (int)ext->interruptTrb.length);
	}
	else if (++session->readErrors >= GIP_MAX_CONSECUTIVE_READ_ERRORS) {
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
	return UsbdQueueAsyncTransfer(ext->deviceHandle, &ext->interruptTrb);
}

static int32_t GipSessionSetConfigComplete(DWORD trbAddr, int32_t status) {
	HidControllerExtension* ext = (HidControllerExtension*)((BYTE*)trbAddr - 36);
	GipSessionSlot* session = GipSessionFromExtension(ext);
	if (!session || !session->reserved || status != 0 || !ext->deviceHandle)
		return status;
	NTSTATUS inStatus = UsbdOpenEndpoint(ext->deviceHandle, USB_ENDPOINT_TYPE_INTERRUPT,
		0x82, 64, 4, (DWORD*)&ext->interruptTrb);
	if (NT_ERROR(inStatus))
		return inStatus;
	NTSTATUS outStatus = UsbdOpenEndpoint(ext->deviceHandle, USB_ENDPOINT_TYPE_INTERRUPT,
		0x02, 64, 4, (DWORD*)&session->outTrb);
	session->outOpen = !NT_ERROR(outStatus);
	if (!session->outOpen)
		return outStatus;
	memset(session->readBuf, 0, sizeof(session->readBuf));
	ext->interruptTrb.savedEndpoint = ext->interruptTrb.endpoint;
	ext->interruptTrb.length = GIP_READ_BUF_SIZE;
	ext->interruptTrb.buffer = session->readBuf;
	ext->interruptTrb.callback = (DWORD)GipSessionInterruptComplete;
	ext->interruptTrb.flags = 1;
	return UsbdQueueAsyncTransfer(ext->deviceHandle, &ext->interruptTrb);
}

static int GipClaimAdditionalSession(deviceHandle* h, BYTE interfaceNumber) {
	GipSessionSlot* session = GipFindFreeSession();
	if (!session)
		return -1;
	memset(session, 0, sizeof(*session));
	session->reserved = true;
	session->userIndex = 0xFF;
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
	// This is the ONLY export that fires for the RiffMaster dongle, so identify the
	// device here rather than assuming. Nobody called UsbdGetDeviceDescriptor for it,
	// but the handle is live at this point, so we can ask ourselves.
	//
	// status is the whole point: 0 == a driver accepted the device, non-zero == the
	// add failed / nothing claimed it. Not logging it the first time was an oversight.
	usb_device_descriptor* dd = UsbdGetDeviceDescriptor ? UsbdGetDeviceDescriptor(h) : 0;
	usb_interface_descriptor* id = UsbdGetInterfaceDescriptor ? UsbdGetInterfaceDescriptor(h) : 0;

	RM_DBG("RIFFMASTER: ADDCOMPLETE handle=%p status=0x%08X (%s) driver=%p\r\n",
		h, status, (status == 0) ? "CLAIMED" : "not claimed",
		h ? h->driver : 0);

	if (dd)
		RM_DBG("RIFFMASTER:   dev VID=%04X PID=%04X class=%02X/%02X/%02X\r\n",
			swap_endianness_16(dd->idVendor), swap_endianness_16(dd->idProduct),
			dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol);
	else
		RM_DBG("RIFFMASTER:   dev descriptor NULL\r\n");

	if (id)
		RM_DBG("RIFFMASTER:   iface #%d alt=%d nEP=%d class=%02X/%02X/%02X%s\r\n",
			id->bInterfaceNumber, id->bAlternateSetting, id->bNumEndpoints,
			id->bInterfaceClass, id->bInterfaceSubClass, id->bInterfaceProtocol,
			(id->bInterfaceClass == 0xFF && id->bInterfaceSubClass == 0x47 &&
			 id->bInterfaceProtocol == 0xD0) ? "   <<< GIP" : "");
	else
		RM_DBG("RIFFMASTER:   iface descriptor NULL\r\n");

	// Once the proven primary path already owns one modern gamepad, every later
	// matching controller gets a distinct fixed session instead of overwriting the
	// primary extension and its asynchronous USB transfers.
	if (status != 0 && h && dd && id && g_gipExt.deviceHandle &&
		swap_endianness_16(dd->idVendor) == MICROSOFT_VENDOR_ID &&
		IsSupportedMicrosoftGamepadPid(swap_endianness_16(dd->idProduct)) &&
		id->bInterfaceClass == 0xFF && id->bInterfaceSubClass == 0x47 &&
		id->bInterfaceProtocol == 0xD0 && id->bInterfaceNumber == 0 &&
		id->bNumEndpoints == 2 && GipFindFreeSession()) {
		return GipClaimAdditionalSession(h, id->bInterfaceNumber);
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
	// RiffMaster dongle, only when the kernel has already given up on it.
	// ---------------------------------------------------------------------
	// Breadcrumb #1. Fires for EVERY device the core gives up on, ours or not, so a
	// replug that never re-enumerates is distinguishable from one we declined to claim.
	RM_TRACE("RIFFMASTER: TRACE ADDCOMPLETE h=%p status=0x%08X\r\n", h, status);
#ifdef RIFFMASTER_CLAIM_ONCE
	// L7-once: claim the dongle exactly ONE time per boot, ever. Every later arrival
	// is left unclaimed, i.e. treated exactly as a no-plugin boot treats it.
	//
	// Turning the guitar off makes the dongle bounce off and back onto USB ~6 times.
	// Each arrival currently re-enters the claim with the SAME static g_gipExt, and
	// re-queues TRBs the kernel may still own from the previous incarnation. This
	// build removes the storm entirely without touching teardown, so it separates
	// "re-claiming during the bounce" from "the single removal itself".
	static bool s_claimedOnce = false;
	if (s_claimedOnce && status != 0)
		return UsbdAddDeviceCompleteDetour.GetOriginal<decltype(&UsbdAddDeviceCompleteHook)>()(h, status);
#endif
	if (status != 0 && dd && id && h && g_gipClaimAttempts < GIP_CLAIM_MAX_ATTEMPTS) {
		uint16_t vid = swap_endianness_16(dd->idVendor);
		uint16_t pid = swap_endianness_16(dd->idProduct);

		if (vid == MICROSOFT_VENDOR_ID && IsSupportedMicrosoftGamepadPid(pid) &&
			id->bInterfaceClass == 0xFF && id->bInterfaceSubClass == 0x47 &&
			id->bInterfaceProtocol == 0xD0 &&
			id->bInterfaceNumber == 0 &&    // interface 0 = GIP data
			id->bNumEndpoints == 2) {       // interface 1 (audio) has 0 in alt 0 - skip it
			XboxInputSetDiagStage(20);

			g_gipClaimAttempts++;
#ifdef RIFFMASTER_CLAIM_ONCE
			s_claimedOnce = true;
#endif
			// Breadcrumb #2. Promoted from RM_DBG: in the `noread` run nothing at all
			// appeared in xbWatson after a replug, and because these were compiled out
			// there was no way to tell "the dongle never re-enumerated" from "it was
			// re-claimed silently". Under `trace` the claim is always visible.
			RM_TRACE("RIFFMASTER: TRACE CLAIM ATTEMPT %d handle=%p\r\n",
				g_gipClaimAttempts, h);
			RM_DBG("RIFFMASTER: *** CLAIM ATTEMPT %d on GIP dongle (handle %p) ***\r\n",
				g_gipClaimAttempts, h);

			// Statically allocated rather than new'd: we do not know the IRQL this
			// callback runs at, and a failed allocation here would be a hang.
			memset(&g_gipExt, 0, sizeof(g_gipExt));
			g_gipExt.deviceHandle = h;
			g_gipExt.interfaceNumber = id->bInterfaceNumber;
			g_gipExt.deviceType = 0;
			g_gipExt.interruptTrb.flags = 1;

			// -------------------------------------------------------------------
			// THE CLAIM. `claimonly` proved these two lines alone are sufficient to
			// freeze the console on removal - no endpoints, no transfers, no XAM,
			// no auth, and it still dies.
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
			// RIFFMASTER_NO_CLAIM tests the obvious alternative: do not claim at all.
			// Let the core record the device as unclaimed exactly as it does with no
			// plugin loaded - the configuration that is PROVEN to survive removal, by
			// every one of ladder levels 0-6 and by the no-plugin control - and drive
			// the endpoints on the live handle anyway. The handle is valid here either
			// way; the open question is only whether the core permits endpoint
			// operations on a device it considers unowned.
			// -------------------------------------------------------------------
#if defined(RIFFMASTER_NO_CLAIM_LATE)
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
#elif defined(RIFFMASTER_NO_CLAIM)
			int r = UsbdAddDeviceCompleteDetour.GetOriginal<decltype(&UsbdAddDeviceCompleteHook)>()(h, status);
			RM_LOG("RIFFMASTER: NOT claiming - passed original status 0x%08X through, "
				"driver stays %p\r\n", status, h->driver);
#elif defined(RIFFMASTER_KEEP_DRIVER)
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
			RM_LOG("RIFFMASTER: claimed WITHOUT touching driver - was %p, now %p\r\n",
				before, h->driver);
			XboxInputSetDiagStage(30);
#else
			h->driver = &g_gipExt;

			int r = UsbdAddDeviceCompleteDetour.GetOriginal<decltype(&UsbdAddDeviceCompleteHook)>()(h, 0);
#endif
			RM_DBG("RIFFMASTER: claim AddDeviceComplete(status=0) returned 0x%08X, driver now=%p\r\n",
				r, h->driver);

#ifdef RIFFMASTER_CLAIM_ONLY
			// L7-claimonly: take the device and stop. No default endpoint, no
			// SET_CONFIGURATION, no interrupt endpoints, no transfers ever.
			//
			// This is the last split available. `noread` proved the read loop is not
			// the cause but still had all three endpoints open; if this survives, the
			// fault is in OPENING endpoints on a device we claimed this way. If it
			// freezes, the bare claim is sufficient and the problem is that the core
			// believes a driver owns a device that has none.
			RM_LOG("RIFFMASTER: claimed and stopped (claimonly variant)\r\n");
			return r;
#endif
			NTSTATUS s = UsbdOpenDefaultEndpoint(h, (DWORD*)&g_gipExt.controlTrb);
			// RM_LOG, not RM_DBG: under NO_CLAIM this is the whole question - whether
			// the core will open an endpoint on a device it considers unowned.
			RM_LOG("RIFFMASTER: UsbdOpenDefaultEndpoint -> 0x%08X %s\r\n",
				s, NT_ERROR(s) ? "FAILED" : "OK");
			XboxInputSetDiagStage(40);
			if (NT_ERROR(s))
				return r;

			// Bring the device up. Per the captured enumeration
			// (docs/gip_riffmaster.md section 5) SET_CONFIGURATION is the last control
			// transfer; everything after it is GIP over the interrupt endpoints, and the
			// device then sends ANNOUNCE (0x02) unprompted.
			int32_t q = SendControlRequest(h, &g_gipExt.controlTrb,
				0x00,   // host->device, standard, device
				0x09,   // SET_CONFIGURATION
				1,      // bConfigurationValue
				0, 0, nullptr,
				(DWORD)GipSetConfigComplete);
			// NOT an NTSTATUS. This returns a handle-like value (observed 0xE1EBF3C0,
			// i.e. the device handle) on BOTH the claimed run, where SET_CONFIGURATION
			// then completed normally, and the unclaimed run, where it never completed.
			// So the queue call accepts the transfer either way, and the difference is
			// purely whether the core ever SERVICES it. Do not read this as success or
			// failure - it is only here to prove the call was reached and returned.
			RM_LOG("RIFFMASTER: SET_CONFIGURATION queued -> 0x%08X (not a status)\r\n", q);

#ifdef RIFFMASTER_NO_CLAIM_LATE
			// Only now tell the core the device was not claimed - after our endpoints
			// are open and the control transfer is already queued.
			r = UsbdAddDeviceCompleteDetour.GetOriginal<decltype(&UsbdAddDeviceCompleteHook)>()(h, status);
			RM_LOG("RIFFMASTER: deferred unclaim - reported 0x%08X after setup, driver=%p\r\n",
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
//   noreset  (no USB reset/patches)  - RM_LOG DbgPrints here  -> FROZE
//   giponly  (no HID detours/thread) - RM_LOG DbgPrints here  -> FROZE
//   nonotify (no XAM notify patches) - RM_LOG DbgPrints here  -> FROZE
//   fixremove(h->driver detached)    - RM_LOG DbgPrints here  -> FROZE
//   NO PLUGIN                        - nothing prints here    -> SURVIVES
//
// Every earlier hypothesis (bugcheck patches, USB reset, HID detours, mapping
// thread, notification patches, the dangling h->driver) was disproven by a build
// that removed it and froze anyway - and every one of those builds still logged
// from in here.
//
// All logging on this path is therefore RM_DBG, which compiles to nothing unless
// RIFFMASTER_VERBOSE is defined. If you need to trace teardown, set a flag here and
// print it later from a safe context - do not print from inside this call.
// ---------------------------------------------------------------------------
NTSTATUS UsbdRemoveDeviceCompleteHook(deviceHandle* h) {
	// Breadcrumb #3. If a freeze produces NO "REMOVE ENTER" line, the hang happens
	// BEFORE this function is ever reached - i.e. in whatever the USB core dispatches
	// to first on removal, walking the g_gipExt we handed it at claim time. That would
	// explain why `passive` (which does nothing here) froze identically, and it would
	// mean no amount of work inside this function can help.
	RM_TRACE("RIFFMASTER: TRACE REMOVE ENTER h=%p ours=%d\r\n",
		h, (h && h == g_gipExt.deviceHandle) ? 1 : 0);
#ifdef RIFFMASTER_PASSIVE_REMOVE
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
			if (g_gipSessions[i].reserved && g_gipSessions[i].ext.deviceHandle == h) {
				session = &g_gipSessions[i];
				break;
			}
		}
		if (session) {
			session->ready = false;
			session->guidePending = false;
			session->outOpen = false;
			session->ext.deviceHandle = 0;
			session->ext.cleanupDone = 1;
			h->driver = 0;
			if (session->userIndex != 0xFF) {
				int idx = (int)(session->deviceContext - 0x10000005);
				XamUserBindDeviceCallback(0xa7553952 + idx,
					session->deviceContext, 0, true, 0);
				session->userIndex = 0xFF;
				session->deviceContext = 0;
			}
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
	// freeze seen on guitar disconnect.
	//
	// Stop the read loop FIRST, then release our references.
	// ---------------------------------------------------------------------
	if (h && h == g_gipExt.deviceHandle) {
		RM_DBG("RIFFMASTER: *** device removed - tearing down GIP state ***\r\n");

		// 0. Stop presenting as a live controller IMMEDIATELY.
		//    Both XAM hooks gate on (g_gipUserIndex != 0xFF && g_gipAuthStage == 7).
		//    Clearing the stage first means that from this instruction onwards they
		//    fall through to the originals instead of answering "connected guitar"
		//    with stale state for a device that is being destroyed. XAM polls
		//    capabilities several times per 100 ms, so this window matters.
		g_gipAuthStage = 0;
		g_gipTxActive = false;
		g_gipHostFinishReady = false;

		// 1. Stop the re-arm loop and the send path. Null the handle before anything
		//    else so a completion that fires mid-teardown cannot re-arm.
		//    Keep a local copy - the close calls below still need it.
		deviceHandle* dead = g_gipExt.deviceHandle;
		g_gipExt.deviceHandle = 0;
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
		//    Set RIFFMASTER_CLOSE_ENDPOINTS_ON_REMOVE to restore the old behaviour.
#ifdef RIFFMASTER_CLOSE_ENDPOINTS_ON_REMOVE
		if (UsbdQueueCloseEndpoint) {
			UsbdQueueCloseEndpoint(dead, &g_gipExt.interruptTrb);
			UsbdQueueCloseEndpoint(dead, &g_gipOutTrb);
		}
		if (UsbdQueueCloseDefaultEndpoint)
			UsbdQueueCloseDefaultEndpoint(dead, (DWORD*)&g_gipExt.controlTrb);
#endif
		RM_DBG("RIFFMASTER: endpoints closed\r\n");

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
		//    This was MISSING: the guitar stayed registered after the device was
		//    gone, so XAM kept a controller bound to a dead device indefinitely.
		GipUnregisterFromXam();

		// 5. Reset the session so a replug starts clean rather than resuming
		//    half-initialised state.
			g_gipIdentifySent = false;
			g_gipIdentifyReplySeen = false;
			g_gipLastIdentifyTick = 0;
			g_gipPoweredOn = false;
		g_gipAuthStarted = true;
		g_gipReady = false;
		g_gipGuideDown = false;
		g_gipGuidePending = false;
		g_gipGuideOverlayOpen = false;
		g_gipChunkTotal = 0;
		g_gipAuthChunkTotal = 0;
		g_gipCertBytes = 0;
		// g_gipClaimAttempts is deliberately NOT reset here. Refilling the budget on
		// every teardown let a disconnect bounce-storm re-claim without limit, which is
		// what re-queues a TRB the kernel still owns. Only a connection that reaches
		// AUTH HANDSHAKE COMPLETE refills it. See GIP_CLAIM_MAX_ATTEMPTS.
		g_gipPacketsSeen = 0;
		g_gipInputsSeen = 0;
		g_gipSeq = 1;
		g_gipReadErrors = 0;
		g_gipReadLoopStopped = false;
		g_gipCapsLogged = 0;
		g_gipCaps2Logged = 0;
		memset(&g_gipState, 0, sizeof(g_gipState));

		RM_DBG("RIFFMASTER: teardown done, ready for replug\r\n");

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
		// froze with no endpoints, no transfers, no XAM and no auth. And `keepdriver`,
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
		// finished. The console survives an unplug, survives the guitar powering off,
		// and re-claims cleanly on replug (verified twice in one boot, handles
		// E1EBF3C0 then E1EBF3E0, full auth and XAM registration both times).
		//
		// KNOWN COST: the core never completes teardown of that device object, so a
		// handle pair is consumed per plug cycle. Bounded and slow, but real - see
		// docs/HOW_IT_WORKS.md and docs/KNOWN_ISSUES.md. Define RIFFMASTER_REMOVE_CALL_ORIGINAL to get the
		// old (freezing) behaviour back for testing.
		// ===================================================================
#ifndef RIFFMASTER_REMOVE_CALL_ORIGINAL
		RM_DBG("RIFFMASTER: skipping kernel removal path\r\n");
		return 0;
#endif
	}

	// h->driver has been detached above for our device, so the kernel takes the same
	// path it takes for any unclaimed device - the path that is known to survive.
	RM_TRACE("RIFFMASTER: TRACE REMOVE PRE-ORIG h=%p\r\n", h);
	NTSTATUS rr = UsbdRemoveDeviceCompleteDetour.GetOriginal<decltype(&UsbdRemoveDeviceCompleteHook)>()(h);
	RM_TRACE("RIFFMASTER: TRACE REMOVE POST-ORIG h=%p -> 0x%08X\r\n", h, rr);
	return rr;
#endif // RIFFMASTER_PASSIVE_REMOVE
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
#ifdef RIFFMASTER_VERBOSE
		{ (void*)UsbdGetDeviceDescriptor,    (void*)UsbdGetDeviceDescriptorHook,    &UsbdGetDeviceDescriptorDetour,    "UsbdGetDeviceDescriptor" },
		{ (void*)UsbdGetInterfaceDescriptor, (void*)UsbdGetInterfaceDescriptorHook, &UsbdGetInterfaceDescriptorDetour, "UsbdGetInterfaceDescriptor" },
		{ (void*)UsbdGetDeviceSpeed,         (void*)UsbdGetDeviceSpeedHook,         &UsbdGetDeviceSpeedDetour,         "UsbdGetDeviceSpeed" },
		{ (void*)UsbdOpenDefaultEndpoint,    (void*)UsbdOpenDefaultEndpointHook,    &UsbdOpenDefaultEndpointDetour,    "UsbdOpenDefaultEndpoint" },
#endif
	};

	for (int i = 0; i < (sizeof(probes) / sizeof(probes[0])); i++) {
		if (!probes[i].target) {
			RM_DBG("RIFFMASTER: PROBE SKIP %s - null pointer\r\n", probes[i].name);
			continue;
		}
		*probes[i].det = Detour(probes[i].target, probes[i].hook);
		probes[i].det->Install();
		RM_DBG("RIFFMASTER: PROBE installed on %s @ %p\r\n", probes[i].name, probes[i].target);
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
			RM_DBG("RIFFMASTER: DROP REASON = no valid HID descriptor (0x21) after interface descriptor\r\n");
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
			RM_DBG("RIFFMASTER: DROP REASON = all 4 controller slots in use\r\n");
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
	RM_DBG("RIFFMASTER: DROP REASON = interface is not HID (class/subclass/protocol != 03/00/00). "
		"Saw %02X/%02X/%02X. Device DID reach the hook.\r\n",
		interface_descriptor ? interface_descriptor->bInterfaceClass : 0xFF,
		interface_descriptor ? interface_descriptor->bInterfaceSubClass : 0xFF,
		interface_descriptor ? interface_descriptor->bInterfaceProtocol : 0xFF);
	return HidAddDeviceDetour.GetOriginal<decltype(&HidAddDeviceHook)>()(deviceHandle);
}

DWORD XamInputSetStateHook(DWORD user, DWORD flags, XINPUT_VIBRATION* vibration) {
	DWORD status = XamInputSetStateDetour.GetOriginal<decltype(&XamInputSetStateHook)>()(user, flags, vibration);

	if ((user & 0xFF) == 0xFF)
		user = 0;
	GipSessionSlot* multiSession = GipSessionFromUser((uint8_t)user);
	if (multiSession) {
		const BYTE left = vibration ? (BYTE)(vibration->wLeftMotorSpeed >> 8) : 0;
		const BYTE right = vibration ? (BYTE)(vibration->wRightMotorSpeed >> 8) : 0;
		GipSessionSendRumble(multiSession, left, right);
		return ERROR_SUCCESS;
	}

	// XAM passes the 16-bit 360 motor speeds in XINPUT_VIBRATION. Convert them
	// to the gamepad's 8-bit direct-motor values.
	if (g_gipReady && g_gipUserIndex != 0xFF && user == g_gipUserIndex) {
		const BYTE left = vibration ? (BYTE)(vibration->wLeftMotorSpeed >> 8) : 0;
		const BYTE right = vibration ? (BYTE)(vibration->wRightMotorSpeed >> 8) : 0;
		GipSendGamepadRumble(g_gipExt.deviceHandle, left, right);
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
	GipSessionSlot* multiSession = GipSessionFromUser((uint8_t)user);
	if (multiSession) {
		GipFillGuitarCaps(capabilities->Type, capabilities->SubType,
			capabilities->Flags, capabilities->Gamepad);
		capabilities->Vibration.wLeftMotorSpeed = 0;
		capabilities->Vibration.wRightMotorSpeed = 0;
		return ERROR_SUCCESS;
	}

	// ---- RiffMaster: report a GUITAR, not a gamepad ----------------------
	// This runs before hiddriver360's own path so real HID pads keep reporting
	// XINPUT_DEVSUBTYPE_GAMEPAD. rb1wiidrums replaced the SubType unconditionally,
	// which would have made every controller claim to be an instrument
	// (docs/rb1wii_analysis.md hunk 5).
	if (g_gipUserIndex != 0xFF && user == g_gipUserIndex && g_gipReady) {
		// Rate limited: the dash/game polls capabilities many times per second, and
		// logging every call floods xbdm and hangs the console.
		if (g_gipCapsLogged < 3) {
			g_gipCapsLogged++;
			RM_DBG("RIFFMASTER: XamInputGetCapabilitiesEx(user=%d) -> GUITAR\r\n", user);
		}
		GipFillGuitarCaps(capabilities->Type, capabilities->SubType,
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
// Guitar Hero works through the Ex path. Rock Band saw no device at all, which points
// at it using the plain non-Ex capabilities call. Both capability paths are covered.
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
	GipSessionSlot* multiSession = GipSessionFromUser((uint8_t)user);
	if (multiSession) {
		GipFillGuitarCaps(caps->Type, caps->SubType, caps->Flags, caps->Gamepad);
		caps->Vibration.wLeftMotorSpeed = 0;
		caps->Vibration.wRightMotorSpeed = 0;
		return ERROR_SUCCESS;
	}

	if (g_gipUserIndex != 0xFF && user == g_gipUserIndex && g_gipReady) {
		if (g_gipCaps2Logged < 3) {
			g_gipCaps2Logged++;
			RM_DBG("RIFFMASTER: XamInputGetCapabilities(user=%d flags=%d) -> GUITAR\r\n",
				user, flags);
		}
		GipFillGuitarCaps(caps->Type, caps->SubType, caps->Flags, caps->Gamepad);
		caps->Vibration.wLeftMotorSpeed = 0;
		caps->Vibration.wRightMotorSpeed = 0;
		return ERROR_SUCCESS;
	}
	return status;
}

NTSTATUS XInputdReadStateHook(DWORD dwDeviceContext, PDWORD pdwPacketNumber, PXINPUT_GAMEPAD pInputData, PBOOL unk) {
	GipSessionSlot* multiSession = GipSessionFromContext(dwDeviceContext);
	if (multiSession) {
		if (!pInputData)
			return ERROR_INVALID_PARAMETER;
		GipGamepadToXInput(&multiSession->state, pInputData);
		if (multiSession->guidePending && g_rmCfg.guideButton) {
			multiSession->guidePending = false;
			DWORD now = GetTickCount();
			if (now - multiSession->lastGuideTick >= (DWORD)g_rmCfg.guideCooldownMs) {
				multiSession->lastGuideTick = now;
				XamInputSendXenonButtonPress(multiSession->userIndex);
			}
		}
		if (pdwPacketNumber)
			*pdwPacketNumber = ++multiSession->packetNumber;
		if (unk)
			*unk = FALSE;
		return STATUS_SUCCESS;
	}
	// ---- RiffMaster: synthesize a 360 guitar report ----------------------
	// Checked before hiddriver360's own lookup: our state comes from the GIP parser,
	// not from its HID ButtonsReport.
	if (g_gipUserIndex != 0xFF && g_gipReady &&
		dwDeviceContext == g_gipDeviceContext) {
		if (!pInputData)
			return ERROR_INVALID_PARAMETER;

		GipGamepadToXInput(&g_gipState, pInputData);

		// Guide arrives as a separate GIP 0x07 packet, not in the gamepad
		// report. Retail 17559 masks the extended Guide bit for virtual devices,
		// so use XAM's supported one-shot Guide event.
		if (g_gipGuidePending && g_rmCfg.guideButton) {
			g_gipGuidePending = false;
			static DWORD lastGuide = 0;
			DWORD now = GetTickCount();
			if (now - lastGuide >= (DWORD)g_rmCfg.guideCooldownMs) {
				lastGuide = now;
				XamInputSendXenonButtonPress(g_gipUserIndex);
			}
		}

		if (pdwPacketNumber)
			*pdwPacketNumber = ++g_gipPacketNumber;
		if (unk)
			*unk = FALSE;
		return STATUS_SUCCESS;
	}

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
bool initFunctionPointers() {
	isDevkit = *(uint32_t*)(0x8010D334) == 0x00000000;
	HANDLE kernelHandle = GetModuleHandleA("xboxkrnl.exe");

	if (!kernelHandle) {
		DbgPrint("EINTIM: COULDNT GET KERNEL HANDLE!\n");
		return false;
	}

	HANDLE xamHandle = GetModuleHandleA("xam.xex");

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
	XexGetProcedureAddress(xamHandle, 746, &XamIsSysUiInvokedByXenonButton);
	XexGetProcedureAddress(xamHandle, 685, &XamInputGetCapabilitiesEx);
	XexGetProcedureAddress(xamHandle, 402, &XamInputSetState);
	XexGetProcedureAddress(xamHandle, 1183, &NotificationPatchPtr);

	// XamGetCurrentTitleId - xam ordinal 463 (0x1CF), no arguments, returns the title ID.
	// refs/xenia/src/xenia/kernel/xam/xam_table.inc:260, implementation at
	// refs/xenia/src/xenia/kernel/xam/xam_info.cc:225.
	// Optional: if it does not resolve we simply fall back to the compile-time subtype.
	XexGetProcedureAddress(xamHandle, 463, &XamGetCurrentTitleIdPtr);
	RM_LOG("RIFFMASTER: XamGetCurrentTitleId (463) %s\r\n",
		XamGetCurrentTitleIdPtr ? "resolved" : "DID NOT RESOLVE - using fixed subtype");

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
		//   - no plugin at all: guitar power-on floods STATUS_ACCESS_VIOLATION
		//     (0xC0000005) FirstChance and the console KEEPS RUNNING
		//   - killtest build (never claims, opens no endpoints): FREEZES on disconnect
		//   - the only thing killtest shares with us that a no-plugin boot lacks is
		//     these patches
		//
		// So: record the originals here, and restore the two BUGCHECK sites once the
		// reset is done (GipRestoreUsbBugchecks, called at the end of DllMain).
		// Only needed because of the USB driver reset below. If that is skipped, these
		// are not applied at all and kernel fault containment is never disturbed.
#ifndef RIFFMASTER_NO_USB_RESET
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
#if RIFFMASTER_LEVEL >= RM_LVL_NOPS
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
#ifndef RIFFMASTER_NO_NOTIFY_PATCH
		*(uint16_t*)0x816AB7A6 = 80; // Register custom notification type condition
		XNotifyTimerPtr = (uint16_t*)0x816ab7aa;
#endif
	}

#ifndef RIFFMASTER_NO_NOTIFY_PATCH
	*XNotifyTimerPtr = 1500;
#endif

	// Patches notification handling to work without JRPC2, Thanks crow!
	// 0x409A is a conditional branch (bne); 0x4800 makes it unconditional.
#ifndef RIFFMASTER_NO_NOTIFY_PATCH
	if (*(short*)((uintptr_t)(NotificationPatchPtr) + 48) == 0x409A) {
		*(short*)((uintptr_t)(NotificationPatchPtr) + 48) = 0x4800;
	}
#else
	DbgPrint("RIFFMASTER: XAM notification patches SKIPPED\r\n");
#endif

	return true;
}

BOOL APIENTRY DllMain(HANDLE Handle, DWORD Reason, PVOID Reserved) {
	if (Reason == DLL_PROCESS_ATTACH) {
		XboxInputLogReset();
		XboxInputSetDiagStage(1);
		// Fires before ANY check below, so "did our build load at all?" is answerable
		// even when the version/tray gate aborts the launch. Build stamp distinguishes
		// this xex from any other hiddriver360 build on the console.
		RM_LOG("RIFFMASTER: *** RiffMaster GIP driver loaded - built " __DATE__ " " __TIME__ " ***\r\n");
		RM_LOG("RIFFMASTER: kernel build %d, tray open = %d\r\n",
			XboxKrnlVersion->Build, IsTrayOpen() ? 1 : 0);

		if ((XboxKrnlVersion->Build != 17559 && XboxKrnlVersion->Build != 17489) || IsTrayOpen()) {
			RM_LOG("RIFFMASTER: ABORTING - unsupported kernel build or disc tray open\r\n");
			DbgPrint("EINTIM: Only 17559 and 17489 dashboards are currently supported or the disk tray is open. Aborting launch...\n");
			return FALSE;
		}

		RM_LOG("RIFFMASTER: *** BUILD LADDER LEVEL %d ***\r\n", RIFFMASTER_LEVEL);

		// Level 0 is the control: a plugin that loads into the same process, at the
		// same base address, and then does nothing at all. If a disconnect freezes
		// even this, the cause is DashLaunch/plugin residency itself and no amount of
		// work inside the driver will fix it. If it survives, we have a clean floor to
		// add subsystems onto — which is the thing the subtractive bisection lacked.
#if RIFFMASTER_LEVEL == RM_LVL_NULL
		RM_LOG("RIFFMASTER: level 0 - loaded and doing nothing. Disconnect the dongle now.\r\n");
		return TRUE;
#else
		// Optional user settings. Missing file is normal and not an error - it means
		// built-in defaults, and one gets written out with comments for next boot.
		// Read here, at load, and never again: everything downstream only reads the
		// parsed globals, so no hot path or raised-IRQL context ever touches the disk.
		bool cfgFound = RmCfgLoad(RM_CFG_PATH);
		RM_LOG("RIFFMASTER: config %s - tilt %d, SP tilt=%d click=%d, solo=%d, "
			"invertStrum=%d, default SubType 0x%02X, %d ini override(s)\r\n",
			cfgFound ? "loaded from " RM_CFG_PATH : "defaults (wrote " RM_CFG_PATH ")",
			g_rmCfg.tiltThreshold, g_rmCfg.starPowerTilt, g_rmCfg.starPowerClick,
			g_rmCfg.soloFlag, g_rmCfg.invertStrum, g_rmCfg.defaultSubType,
			g_rmCfg.overrideCount);

		DbgPrint("EINTIM: HELLO from xbox 360 HID controller driver version 0.6 beta\n");
		if (!initFunctionPointers())
			return FALSE;

		DbgPrint("EINTIM: Loading mappings!\r\n");
#ifndef RIFFMASTER_GIP_ONLY
		if (!LoadMappingsFromFile("HDD:\\hiddriver.json")) {
			DbgPrint("EINTIM: Failed to load mappings(JSON either doesn't exist yet or syntax error)!\r\n");
		}

#endif
		// HID detours: NOT needed for the RiffMaster. Our claim goes through
		// UsbdAddDeviceComplete, not the HID driver. These patch live code at
		// 0x800E4D68 / 0x800E4D28 - the HID device add/remove path, i.e. exactly
		// the code that runs when a USB device disappears. Prime remaining
		// suspect for the disconnect freeze.
#ifndef RIFFMASTER_GIP_ONLY
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
#if RIFFMASTER_LEVEL >= RM_LVL_FULL
		InstallUsbProbes();
		XboxInputSetDiagStage(10);
#endif

#if RIFFMASTER_LEVEL >= RM_LVL_XAMHOOKS
		XamInputGetCapabilitiesDetour = Detour(XamInputGetCapabilitiesEx, (void*)XamInputGetCapabilitiesExHook);
		XamInputSetStateDetour = Detour(XamInputSetState, (void*)XamInputSetStateHook);
		if (XamInputGetCapabilitiesPtr) {
			XamInputGetCapabilitiesDetour2 = Detour(XamInputGetCapabilitiesPtr, (void*)XamInputGetCapabilitiesHook);
			XamInputGetCapabilitiesDetour2.Install();
			RM_LOG("RIFFMASTER: hooked XamInputGetCapabilities (400) @ %p\r\n", XamInputGetCapabilitiesPtr);
		}
		else RM_LOG("RIFFMASTER: ordinal 400 did NOT resolve!\r\n");
		XInputdReadStateDetour = Detour(XInputdReadStatePtr, (void*)XInputdReadStateHook);

		XamInputSetStateDetour.Install();
		XamInputGetCapabilitiesDetour.Install();
		XInputdReadStateDetour.Install();
		XboxInputSetDiagStage(11);
		DbgPrint("EINTIM: Hooks installed\n");
#else
		RM_LOG("RIFFMASTER: XamInput/XInputd detours SKIPPED (level %d)\r\n", RIFFMASTER_LEVEL);
#endif

		// This is a dirty way of forcing the system to reenumerate USB devices so you don't need to replug the controllers
		//
		// RIFFMASTER_NO_USB_RESET skips it. This is the strongest remaining suspect for
		// the disconnect freeze: it is a full USB stack teardown and re-entry (the author
		// calls it "dirty", and it frees a physical page out from under the driver), it is
		// shared by the killtest build and this one, and it is absent from a no-plugin
		// boot - which is the exact configuration that survives a disconnect.
		//
		// We do not need it. Its only benefit is that devices already plugged in when the
		// plugin loads get re-enumerated; the guitar is powered on after boot anyway, so
		// our UsbdAddDeviceComplete detour sees it arrive normally.
		//
		// Skipping it also makes the bugcheck patches unnecessary, since those exist
		// solely to let this sequence run - which is why the earlier "skip the patches but
		// still do the reset" build hung at boot.
#ifndef RIFFMASTER_NO_USB_RESET
		DbgPrint("EINTIM: Resetting USB driver!\n");
		UsbdPowerDownNotification();

		// For some reason microsoft doesnt clean up this page by themselves in the shutdown notification, so ill do it for them, call me mr nice guy :)
		MmFreePhysicalMemory(0, *(DWORD*)UsbPhysicalPage);
		DbgPrint("EINTIM: USB driver shutdown complete.\n");

		UsbdDriverEntry();
		DbgPrint("EINTIM: USB driver reset complete.\n");

		// The USB driver reset is done, so the bugchecks are no longer in the way.
		// Put them back: with fault containment restored, a USB fault at runtime
		// (e.g. the guitar going to sleep) should raise a survivable exception the way
		// it does with no plugin loaded, instead of hanging the console.
		// OFF by default. Restoring the bugchecks did NOT fix the disconnect freeze, and
		// it coincided with Rock Band and Guitar Hero failing to launch at all - so it
		// is a suspected regression, not a neutral change. Only enable to re-test.
#ifdef RIFFMASTER_RESTORE_BUGCHECKS
		GipRestoreUsbBugchecks();
#endif
#else
		RM_LOG("RIFFMASTER: USB driver reset SKIPPED - power the guitar on AFTER boot\r\n");
#endif

		MakeThread((LPTHREAD_START_ROUTINE)XboxInputLogThread, nullptr);
		XboxInputSetDiagStage(12);

		// Start mapping manager thread.
		// Not needed for the RiffMaster: our mapping is fixed and known, so the JSON
		// mapping system and its background thread are dead weight (and the project's
		// release criteria say the plugin must need no config file).
#ifndef RIFFMASTER_GIP_ONLY
		MakeThread((LPTHREAD_START_ROUTINE)MappingManagerThreadProc, nullptr);
#endif
#endif // RIFFMASTER_LEVEL == RM_LVL_NULL
	}
	return TRUE;
}
