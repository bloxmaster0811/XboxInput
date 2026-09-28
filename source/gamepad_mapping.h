#pragma once

#include <stdint.h>
#include "gip_gamepad_parser.h"

#define XBOXINPUT_MAPPING_BUTTON_COUNT 14

enum XboxInputMappingButton {
	XBOXINPUT_MAP_DPAD_UP = 0,
	XBOXINPUT_MAP_DPAD_DOWN,
	XBOXINPUT_MAP_DPAD_LEFT,
	XBOXINPUT_MAP_DPAD_RIGHT,
	XBOXINPUT_MAP_START,
	XBOXINPUT_MAP_BACK,
	XBOXINPUT_MAP_LEFT_THUMB,
	XBOXINPUT_MAP_RIGHT_THUMB,
	XBOXINPUT_MAP_LEFT_SHOULDER,
	XBOXINPUT_MAP_RIGHT_SHOULDER,
	XBOXINPUT_MAP_A,
	XBOXINPUT_MAP_B,
	XBOXINPUT_MAP_X,
	XBOXINPUT_MAP_Y,
};

struct XboxInputMappingOptions {
	uint16_t buttonTargets[XBOXINPUT_MAPPING_BUTTON_COUNT];
	bool invertLeftX;
	bool invertLeftY;
	bool invertRightX;
	bool invertRightY;
	bool swapSticks;
	bool swapTriggers;
	uint16_t leftStickDeadzone;
	uint16_t rightStickDeadzone;
	uint8_t leftTriggerDeadzone;
	uint8_t rightTriggerDeadzone;
	uint8_t rumblePercent;
};

extern XboxInputMappingOptions g_xboxInputGamepadMapping;

// Readers take one immutable snapshot pointer per operation. Reload publishes a
// completely parsed second buffer in one pointer exchange, so input and USB
// callbacks can never observe a partially updated mapping.
const XboxInputMappingOptions* XboxInputGetActiveMapping();
void XboxInputPublishMapping(const XboxInputMappingOptions* mapping);

void XboxInputSetDefaultMapping(XboxInputMappingOptions* mapping);
void XboxInputApplyGamepadMapping(const XboxInputGipGamepadState* input,
	const XboxInputMappingOptions* mapping, XboxInputGipGamepadState* output);
uint8_t XboxInputScaleRumble(uint8_t motor, uint8_t percent);

// Names are case-insensitive. "None" is accepted as an unbound target.
bool XboxInputSetButtonMappingByName(XboxInputMappingOptions* mapping,
	const char* sourceName, const char* targetName);
