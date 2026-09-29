#pragma once

#include <xtl.h>
#include <stdint.h>

// Transport and device policy live here; USB execution stays in the transport
// implementation.  This keeps a profile declarative: selecting a profile can
// never execute arbitrary packets supplied by a configuration file.
enum XboxInputTransportType {
	XBOXINPUT_TRANSPORT_NONE = 0,
	XBOXINPUT_TRANSPORT_GIP_WIRED,
	XBOXINPUT_TRANSPORT_GIP_WIRELESS,
	XBOXINPUT_TRANSPORT_HID,
	XBOXINPUT_TRANSPORT_XINPUT_NATIVE,
};

enum XboxInputParserType {
	XBOXINPUT_PARSER_NONE = 0,
	XBOXINPUT_PARSER_GIP_STANDARD,
	XBOXINPUT_PARSER_GIP_ELITE,
	XBOXINPUT_PARSER_HID_DESCRIPTOR,
	XBOXINPUT_PARSER_HID_FIXED,
};

enum XboxInputInitProfile {
	XBOXINPUT_INIT_NONE = 0,
	XBOXINPUT_INIT_GIP_STANDARD,
	XBOXINPUT_INIT_GIP_POWERA_543A,
};

enum XboxInputCapability {
	XBOXINPUT_CAP_INPUT          = 1u << 0,
	XBOXINPUT_CAP_RUMBLE         = 1u << 1,
	XBOXINPUT_CAP_GUIDE          = 1u << 2,
	XBOXINPUT_CAP_LED            = 1u << 3,
	XBOXINPUT_CAP_SHARE          = 1u << 4,
	XBOXINPUT_CAP_PADDLES        = 1u << 5,
	XBOXINPUT_CAP_BATTERY        = 1u << 6,
	XBOXINPUT_CAP_TRIGGER_RUMBLE = 1u << 7,
	XBOXINPUT_CAP_AUDIO          = 1u << 8,
	XBOXINPUT_CAP_POWER_OFF      = 1u << 9,
};

enum XboxInputQuirk {
	XBOXINPUT_QUIRK_NONE                    = 0,
	XBOXINPUT_QUIRK_GUIDE_SEPARATE          = 1u << 0,
	XBOXINPUT_QUIRK_POWER_BEFORE_ANNOUNCE   = 1u << 1,
	XBOXINPUT_QUIRK_IDENTIFY_ACK             = 1u << 2,
	XBOXINPUT_QUIRK_NO_REANNOUNCE_ON_RESUME = 1u << 3,
	XBOXINPUT_QUIRK_POWERA_RUMBLE_KICK       = 1u << 4,
	XBOXINPUT_QUIRK_LED_AUTH_BEFORE_INPUT    = 1u << 5,
	XBOXINPUT_QUIRK_READ_BEFORE_INIT         = 1u << 6,
	XBOXINPUT_QUIRK_DISABLE_AUDIO_INTERFACE  = 1u << 7,
	XBOXINPUT_QUIRK_RUMBLE_SETUP             = 1u << 8,
};

struct XboxInputUsbInterfaceIdentity {
	uint8_t number;
	uint8_t alternateSetting;
	uint8_t endpointCount;
	uint8_t interfaceClass;
	uint8_t interfaceSubClass;
	uint8_t interfaceProtocol;
};

// The Xbox 360 USB descriptor helper does not reliably expose endpoint
// descriptors for devices claimed through the rejected-device path.  Endpoint
// topology is therefore part of the trusted built-in profile rather than a
// transport-wide fallback.  Early Xbox One controllers use EP1 while newer
// Xbox One S/Series controllers use EP2.
struct XboxInputUsbEndpointIdentity {
	uint8_t inputAddress;
	uint8_t outputAddress;
	uint8_t transferType;
	uint8_t interval;
	uint16_t maximumPacketSize;
};

struct XboxInputControllerProfile {
	uint16_t vendorId;
	uint16_t productId;
	uint16_t minimumRevision;
	uint16_t maximumRevision;
	const char* name;
	XboxInputTransportType transport;
	XboxInputParserType parser;
	XboxInputInitProfile initProfile;
	uint32_t capabilities;
	uint32_t quirks;
	XboxInputUsbInterfaceIdentity interfaceIdentity;
	XboxInputUsbEndpointIdentity endpointIdentity;
};

enum XboxInputProfileMatchResult {
	XBOXINPUT_PROFILE_MATCHED = 0,
	XBOXINPUT_PROFILE_MISSING_DEVICE_DESCRIPTOR,
	XBOXINPUT_PROFILE_MISSING_INTERFACE_DESCRIPTOR,
	XBOXINPUT_PROFILE_UNKNOWN_VID_PID,
	XBOXINPUT_PROFILE_REVISION_OUT_OF_RANGE,
	XBOXINPUT_PROFILE_INTERFACE_NUMBER_MISMATCH,
	XBOXINPUT_PROFILE_ALTERNATE_SETTING_MISMATCH,
	XBOXINPUT_PROFILE_ENDPOINT_COUNT_MISMATCH,
	XBOXINPUT_PROFILE_CLASS_MISMATCH,
	XBOXINPUT_PROFILE_SUBCLASS_MISMATCH,
	XBOXINPUT_PROFILE_PROTOCOL_MISMATCH,
};

// Common output of every future parser.  The field names intentionally match
// the proven GIP state during the first migration stage, making the type change
// layout- and behaviour-preserving.
struct XboxInputNormalizedState {
	WORD buttons;
	BYTE leftTrigger;
	BYTE rightTrigger;
	SHORT leftX;
	SHORT leftY;
	SHORT rightX;
	SHORT rightY;
	bool guide;
	bool share;
	uint32_t extraButtons;
};

enum XboxInputSessionLifecycle {
	XBOXINPUT_SESSION_FREE = 0,
	XBOXINPUT_SESSION_CLAIMING,
	XBOXINPUT_SESSION_INITIALIZING,
	XBOXINPUT_SESSION_READY,
	XBOXINPUT_SESSION_STOPPING,
	XBOXINPUT_SESSION_RETIRED,
};

struct XboxInputControllerRuntime {
	XboxInputSessionLifecycle lifecycle;
	const XboxInputControllerProfile* profile;
	XboxInputNormalizedState state;
	uint8_t playerIndex;
	uint32_t deviceContext;
	uint32_t packetNumber;
	bool guideDown;
	bool guidePending;
	uint32_t lastGuideTick;
	bool stopping;
};

const XboxInputControllerProfile* XboxInputFindProfileById(
	uint16_t vendorId, uint16_t productId, uint16_t revision);

const XboxInputControllerProfile* XboxInputMatchProfile(
	uint16_t vendorId, uint16_t productId, uint16_t revision,
	const XboxInputUsbInterfaceIdentity* interfaceIdentity);

XboxInputProfileMatchResult XboxInputDiagnoseProfileMatch(
	uint16_t vendorId, uint16_t productId, uint16_t revision,
	const XboxInputUsbInterfaceIdentity* interfaceIdentity,
	const XboxInputControllerProfile** candidateProfile);

bool XboxInputProfileMatchesInterface(const XboxInputControllerProfile* profile,
	const XboxInputUsbInterfaceIdentity* interfaceIdentity);

const char* XboxInputTransportName(XboxInputTransportType transport);
const char* XboxInputParserName(XboxInputParserType parser);
const char* XboxInputInitProfileName(XboxInputInitProfile initProfile);
const char* XboxInputProfileMatchResultName(XboxInputProfileMatchResult result);

void XboxInputResetNormalizedState(XboxInputNormalizedState* state);
void XboxInputInitializeRuntime(XboxInputControllerRuntime* runtime,
	const XboxInputControllerProfile* profile);
bool XboxInputRuntimeIsReady(const XboxInputControllerRuntime* runtime);
void XboxInputRuntimeSetReady(XboxInputControllerRuntime* runtime, bool ready);
void XboxInputRetireRuntime(XboxInputControllerRuntime* runtime);
