#include "controller_backend.h"
#include <string.h>

static const uint32_t kStandardGamepadCapabilities =
	XBOXINPUT_CAP_INPUT | XBOXINPUT_CAP_RUMBLE |
	XBOXINPUT_CAP_GUIDE | XBOXINPUT_CAP_LED;

static const XboxInputUsbInterfaceIdentity kWiredGipInterface = {
	0, 0, 2, 0xFF, 0x47, 0xD0
};

#define WIRED_GIP_PROFILE(pid, label, parserType, extraCaps, extraQuirks) \
	{ 0x045E, pid, 0x0000, 0xFFFF, label, \
	  XBOXINPUT_TRANSPORT_GIP_WIRED, parserType, \
	  XBOXINPUT_INIT_GIP_STANDARD, \
	  kStandardGamepadCapabilities | (extraCaps), \
	  XBOXINPUT_QUIRK_GUIDE_SEPARATE | XBOXINPUT_QUIRK_IDENTIFY_ACK | (extraQuirks), \
	  kWiredGipInterface }

// This is the single source of truth for devices the rejected-device claim
// path may take.  A FF/47/D0 signature alone is never sufficient.
static const XboxInputControllerProfile kControllerProfiles[] = {
	WIRED_GIP_PROFILE(0x02D1, "Xbox One", XBOXINPUT_PARSER_GIP_STANDARD, 0, 0),
	WIRED_GIP_PROFILE(0x02DD, "Xbox One (2015)", XBOXINPUT_PARSER_GIP_STANDARD, 0,
		XBOXINPUT_QUIRK_POWER_BEFORE_ANNOUNCE),
	WIRED_GIP_PROFILE(0x02E3, "Xbox One Elite", XBOXINPUT_PARSER_GIP_STANDARD, 0,
		XBOXINPUT_QUIRK_POWER_BEFORE_ANNOUNCE),
	WIRED_GIP_PROFILE(0x02EA, "Xbox One S", XBOXINPUT_PARSER_GIP_STANDARD, 0,
		XBOXINPUT_QUIRK_POWER_BEFORE_ANNOUNCE),
	WIRED_GIP_PROFILE(0x0B00, "Xbox Elite Series 2", XBOXINPUT_PARSER_GIP_STANDARD, 0,
		XBOXINPUT_QUIRK_POWER_BEFORE_ANNOUNCE),
	WIRED_GIP_PROFILE(0x0B12, "Xbox Series X|S", XBOXINPUT_PARSER_GIP_STANDARD,
		0,
		XBOXINPUT_QUIRK_POWER_BEFORE_ANNOUNCE),
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
		{ 0, 0, 2, 0xFF, 0x47, 0xD0 }
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
	const XboxInputControllerProfile* profile =
		XboxInputFindProfileById(vendorId, productId, revision);
	return XboxInputProfileMatchesInterface(profile, interfaceIdentity) ? profile : 0;
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

const char* XboxInputInitProfileName(XboxInputInitProfile initProfile) {
	switch (initProfile) {
	case XBOXINPUT_INIT_GIP_STANDARD: return "gip-standard";
	case XBOXINPUT_INIT_GIP_POWERA_543A: return "gip-powera-543a";
	default: return "none";
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
