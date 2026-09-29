#include "controller_backend.h"
#include <string.h>

static const uint32_t kStandardGamepadCapabilities =
	XBOXINPUT_CAP_INPUT | XBOXINPUT_CAP_RUMBLE |
	XBOXINPUT_CAP_GUIDE | XBOXINPUT_CAP_LED;

static const XboxInputUsbInterfaceIdentity kWiredGipInterface = {
	0, 0, 2, 0xFF, 0x47, 0xD0
};

static const XboxInputUsbEndpointIdentity kLegacyWiredGipEndpoints = {
	0x81, 0x01, 0x03, 4, 64
};

static const XboxInputUsbEndpointIdentity kModernWiredGipEndpoints = {
	0x82, 0x02, 0x03, 4, 64
};

#define WIRED_GIP_PROFILE(pid, label, parserType, extraCaps, extraQuirks, endpoints) \
	{ 0x045E, pid, 0x0000, 0xFFFF, label, \
	  XBOXINPUT_TRANSPORT_GIP_WIRED, parserType, \
	  XBOXINPUT_INIT_GIP_STANDARD, \
	  kStandardGamepadCapabilities | (extraCaps), \
	  XBOXINPUT_QUIRK_GUIDE_SEPARATE | XBOXINPUT_QUIRK_IDENTIFY_ACK | (extraQuirks), \
	  kWiredGipInterface, endpoints }

// This is the single source of truth for devices the rejected-device claim
// path may take.  A FF/47/D0 signature alone is never sufficient.
static const XboxInputControllerProfile kControllerProfiles[] = {
	WIRED_GIP_PROFILE(0x02D1, "Xbox One", XBOXINPUT_PARSER_GIP_STANDARD, 0, 0,
		kLegacyWiredGipEndpoints),
	WIRED_GIP_PROFILE(0x02DD, "Xbox One (2015)", XBOXINPUT_PARSER_GIP_STANDARD, 0,
		XBOXINPUT_QUIRK_POWER_BEFORE_ANNOUNCE, kLegacyWiredGipEndpoints),
	WIRED_GIP_PROFILE(0x02E3, "Xbox One Elite", XBOXINPUT_PARSER_GIP_STANDARD, 0,
		XBOXINPUT_QUIRK_POWER_BEFORE_ANNOUNCE, kLegacyWiredGipEndpoints),
	WIRED_GIP_PROFILE(0x02EA, "Xbox One S", XBOXINPUT_PARSER_GIP_STANDARD, 0,
		XBOXINPUT_QUIRK_POWER_BEFORE_ANNOUNCE, kModernWiredGipEndpoints),
	WIRED_GIP_PROFILE(0x0B00, "Xbox Elite Series 2", XBOXINPUT_PARSER_GIP_STANDARD, 0,
		XBOXINPUT_QUIRK_POWER_BEFORE_ANNOUNCE, kModernWiredGipEndpoints),
	WIRED_GIP_PROFILE(0x0B12, "Xbox Series X|S", XBOXINPUT_PARSER_GIP_STANDARD,
		0,
		XBOXINPUT_QUIRK_POWER_BEFORE_ANNOUNCE, kModernWiredGipEndpoints),
	{
		0x24C6, 0x543A, 0x0000, 0xFFFF,
		"PowerA Xbox One Wired (1414134-01)",
		XBOXINPUT_TRANSPORT_GIP_WIRED,
		XBOXINPUT_PARSER_GIP_STANDARD,
		XBOXINPUT_INIT_GIP_POWERA_543A,
		kStandardGamepadCapabilities,
		XBOXINPUT_QUIRK_GUIDE_SEPARATE |
		XBOXINPUT_QUIRK_IDENTIFY_ACK |
		XBOXINPUT_QUIRK_POWER_BEFORE_ANNOUNCE |
		XBOXINPUT_QUIRK_POWERA_RUMBLE_KICK,
		{ 0, 0, 2, 0xFF, 0x47, 0xD0 },
		{ 0x82, 0x02, 0x03, 4, 64 }
	},
	{
		0x20D6, 0x4002, 0x0000, 0xFFFF,
		"PowerA Spectra Infinity Wired",
		XBOXINPUT_TRANSPORT_GIP_WIRED,
		XBOXINPUT_PARSER_GIP_STANDARD,
		XBOXINPUT_INIT_GIP_STANDARD,
		kStandardGamepadCapabilities,
		XBOXINPUT_QUIRK_GUIDE_SEPARATE |
		XBOXINPUT_QUIRK_IDENTIFY_ACK |
		XBOXINPUT_QUIRK_POWER_BEFORE_ANNOUNCE |
		XBOXINPUT_QUIRK_LED_AUTH_BEFORE_INPUT |
		XBOXINPUT_QUIRK_READ_BEFORE_INIT |
		XBOXINPUT_QUIRK_DISABLE_AUDIO_INTERFACE |
		XBOXINPUT_QUIRK_RUMBLE_SETUP,
		{ 0, 0, 2, 0xFF, 0x47, 0xD0 },
		{ 0x81, 0x01, 0x03, 4, 64 }
	},
};

#undef WIRED_GIP_PROFILE

const XboxInputControllerProfile* XboxInputFindProfileById(
	uint16_t vendorId, uint16_t productId, uint16_t revision) {
	for (DWORD i = 0; i < sizeof(kControllerProfiles) / sizeof(kControllerProfiles[0]); ++i) {
		const XboxInputControllerProfile* profile = &kControllerProfiles[i];
		if (profile->vendorId != vendorId || profile->productId != productId)
			continue;
		if (revision != 0 &&
			(revision < profile->minimumRevision || revision > profile->maximumRevision))
			continue;
		return profile;
	}
	return 0;
}

bool XboxInputProfileMatchesInterface(const XboxInputControllerProfile* profile,
	const XboxInputUsbInterfaceIdentity* interfaceIdentity) {
	if (!profile || !interfaceIdentity)
		return false;
	const XboxInputUsbInterfaceIdentity* expected = &profile->interfaceIdentity;
	return interfaceIdentity->number == expected->number &&
		interfaceIdentity->alternateSetting == expected->alternateSetting &&
		interfaceIdentity->endpointCount == expected->endpointCount &&
		interfaceIdentity->interfaceClass == expected->interfaceClass &&
		interfaceIdentity->interfaceSubClass == expected->interfaceSubClass &&
		interfaceIdentity->interfaceProtocol == expected->interfaceProtocol;
}

const XboxInputControllerProfile* XboxInputMatchProfile(
	uint16_t vendorId, uint16_t productId, uint16_t revision,
	const XboxInputUsbInterfaceIdentity* interfaceIdentity) {
	const XboxInputControllerProfile* profile = 0;
	return XboxInputDiagnoseProfileMatch(vendorId, productId, revision,
		interfaceIdentity, &profile) == XBOXINPUT_PROFILE_MATCHED ? profile : 0;
}

XboxInputProfileMatchResult XboxInputDiagnoseProfileMatch(
	uint16_t vendorId, uint16_t productId, uint16_t revision,
	const XboxInputUsbInterfaceIdentity* interfaceIdentity,
	const XboxInputControllerProfile** candidateProfile) {
	if (candidateProfile) *candidateProfile = 0;
	const XboxInputControllerProfile* profile =
		XboxInputFindProfileById(vendorId, productId, 0);
	if (!profile) return XBOXINPUT_PROFILE_UNKNOWN_VID_PID;
	if (candidateProfile) *candidateProfile = profile;
	if (revision != 0 && (revision < profile->minimumRevision ||
		revision > profile->maximumRevision))
		return XBOXINPUT_PROFILE_REVISION_OUT_OF_RANGE;
	if (!interfaceIdentity)
		return XBOXINPUT_PROFILE_MISSING_INTERFACE_DESCRIPTOR;
	const XboxInputUsbInterfaceIdentity* expected = &profile->interfaceIdentity;
	if (interfaceIdentity->number != expected->number)
		return XBOXINPUT_PROFILE_INTERFACE_NUMBER_MISMATCH;
	if (interfaceIdentity->alternateSetting != expected->alternateSetting)
		return XBOXINPUT_PROFILE_ALTERNATE_SETTING_MISMATCH;
	if (interfaceIdentity->endpointCount != expected->endpointCount)
		return XBOXINPUT_PROFILE_ENDPOINT_COUNT_MISMATCH;
	if (interfaceIdentity->interfaceClass != expected->interfaceClass)
		return XBOXINPUT_PROFILE_CLASS_MISMATCH;
	if (interfaceIdentity->interfaceSubClass != expected->interfaceSubClass)
		return XBOXINPUT_PROFILE_SUBCLASS_MISMATCH;
	if (interfaceIdentity->interfaceProtocol != expected->interfaceProtocol)
		return XBOXINPUT_PROFILE_PROTOCOL_MISMATCH;
	return XBOXINPUT_PROFILE_MATCHED;
}

const char* XboxInputTransportName(XboxInputTransportType transport) {
	switch (transport) {
	case XBOXINPUT_TRANSPORT_GIP_WIRED: return "gip-wired";
	case XBOXINPUT_TRANSPORT_GIP_WIRELESS: return "gip-wireless";
	case XBOXINPUT_TRANSPORT_HID: return "hid";
	case XBOXINPUT_TRANSPORT_XINPUT_NATIVE: return "xinput-native";
	default: return "none";
	}
}

const char* XboxInputParserName(XboxInputParserType parser) {
	switch (parser) {
	case XBOXINPUT_PARSER_GIP_STANDARD: return "gip-standard";
	case XBOXINPUT_PARSER_GIP_ELITE: return "gip-elite";
	case XBOXINPUT_PARSER_HID_DESCRIPTOR: return "hid-descriptor";
	case XBOXINPUT_PARSER_HID_FIXED: return "hid-fixed";
	default: return "none";
	}
}

const char* XboxInputInitProfileName(XboxInputInitProfile initProfile) {
	switch (initProfile) {
	case XBOXINPUT_INIT_GIP_STANDARD: return "gip-standard";
	case XBOXINPUT_INIT_GIP_POWERA_543A: return "gip-powera-543a";
	default: return "none";
	}
}

const char* XboxInputProfileMatchResultName(XboxInputProfileMatchResult result) {
	switch (result) {
	case XBOXINPUT_PROFILE_MATCHED: return "matched";
	case XBOXINPUT_PROFILE_MISSING_DEVICE_DESCRIPTOR: return "missing_device_descriptor";
	case XBOXINPUT_PROFILE_MISSING_INTERFACE_DESCRIPTOR: return "missing_interface_descriptor";
	case XBOXINPUT_PROFILE_UNKNOWN_VID_PID: return "unknown_vid_pid";
	case XBOXINPUT_PROFILE_REVISION_OUT_OF_RANGE: return "revision_out_of_range";
	case XBOXINPUT_PROFILE_INTERFACE_NUMBER_MISMATCH: return "interface_number_mismatch";
	case XBOXINPUT_PROFILE_ALTERNATE_SETTING_MISMATCH: return "alternate_setting_mismatch";
	case XBOXINPUT_PROFILE_ENDPOINT_COUNT_MISMATCH: return "endpoint_count_mismatch";
	case XBOXINPUT_PROFILE_CLASS_MISMATCH: return "interface_class_mismatch";
	case XBOXINPUT_PROFILE_SUBCLASS_MISMATCH: return "interface_subclass_mismatch";
	case XBOXINPUT_PROFILE_PROTOCOL_MISMATCH: return "interface_protocol_mismatch";
	default: return "unknown_result";
	}
}

void XboxInputResetNormalizedState(XboxInputNormalizedState* state) {
	if (state)
		memset(state, 0, sizeof(*state));
}

void XboxInputInitializeRuntime(XboxInputControllerRuntime* runtime,
	const XboxInputControllerProfile* profile) {
	if (!runtime)
		return;
	memset(runtime, 0, sizeof(*runtime));
	runtime->lifecycle = XBOXINPUT_SESSION_CLAIMING;
	runtime->profile = profile;
	runtime->playerIndex = 0xFF;
}

bool XboxInputRuntimeIsReady(const XboxInputControllerRuntime* runtime) {
	return runtime && runtime->lifecycle == XBOXINPUT_SESSION_READY &&
		runtime->playerIndex != 0xFF;
}

void XboxInputRuntimeSetReady(XboxInputControllerRuntime* runtime, bool ready) {
	if (!runtime)
		return;
	runtime->lifecycle = ready ? XBOXINPUT_SESSION_READY :
		XBOXINPUT_SESSION_INITIALIZING;
}

void XboxInputRetireRuntime(XboxInputControllerRuntime* runtime) {
	if (!runtime)
		return;
	runtime->lifecycle = XBOXINPUT_SESSION_RETIRED;
	runtime->playerIndex = 0xFF;
	runtime->deviceContext = 0;
	runtime->guideDown = false;
	runtime->guidePending = false;
	runtime->stopping = true;
	XboxInputResetNormalizedState(&runtime->state);
}
