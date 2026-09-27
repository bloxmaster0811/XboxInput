#pragma once

#include <stdint.h>

// XDK-neutral output from a standard Xbox One/Series GIP command 0x20 payload.
// Keeping this boundary free of xtl.h lets the production parser run in normal
// host-side regression tests.
struct XboxInputGipGamepadState {
	uint16_t buttons;
	uint8_t leftTrigger;
	uint8_t rightTrigger;
	int16_t leftX;
	int16_t leftY;
	int16_t rightX;
	int16_t rightY;
};

bool XboxInputParseGipGamepadPayload(const uint8_t* payload, int length,
	XboxInputGipGamepadState* state);

// Guide is carried separately in GIP command 0x07 rather than command 0x20.
// Returns true only for a well-formed Guide virtual-key payload.
bool XboxInputApplyGipGuidePayload(const uint8_t* payload, int length,
	bool* guideDown, bool* guidePending);

// Builds the nine-byte Direct Motor Command payload shared by every session.
void XboxInputBuildGipRumblePayload(uint8_t leftMotor, uint8_t rightMotor,
	uint8_t payload[9]);
