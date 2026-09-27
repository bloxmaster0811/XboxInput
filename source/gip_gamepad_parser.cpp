#include "gip_gamepad_parser.h"

// Xbox 360 XInput button values, kept local so this parser remains portable.
enum XboxInputGamepadButton {
	XBOXINPUT_DPAD_UP        = 0x0001,
	XBOXINPUT_DPAD_DOWN      = 0x0002,
	XBOXINPUT_DPAD_LEFT      = 0x0004,
	XBOXINPUT_DPAD_RIGHT     = 0x0008,
	XBOXINPUT_START          = 0x0010,
	XBOXINPUT_BACK           = 0x0020,
	XBOXINPUT_LEFT_THUMB     = 0x0040,
	XBOXINPUT_RIGHT_THUMB    = 0x0080,
	XBOXINPUT_LEFT_SHOULDER  = 0x0100,
	XBOXINPUT_RIGHT_SHOULDER = 0x0200,
	XBOXINPUT_A              = 0x1000,
	XBOXINPUT_B              = 0x2000,
	XBOXINPUT_X              = 0x4000,
	XBOXINPUT_Y              = 0x8000,
};

static uint16_t ReadLe16(const uint8_t* p) {
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

bool XboxInputParseGipGamepadPayload(const uint8_t* p, int length,
	XboxInputGipGamepadState* state) {
	if (!p || !state || length < 14)
		return false;

	const uint8_t low = p[0];
	const uint8_t high = p[1];
	uint16_t buttons = 0;
	if (low & 0x10) buttons |= XBOXINPUT_A;
	if (low & 0x20) buttons |= XBOXINPUT_B;
	if (low & 0x40) buttons |= XBOXINPUT_X;
	if (low & 0x80) buttons |= XBOXINPUT_Y;
	if (low & 0x04) buttons |= XBOXINPUT_START;
	if (low & 0x08) buttons |= XBOXINPUT_BACK;
	if (high & 0x01) buttons |= XBOXINPUT_DPAD_UP;
	if (high & 0x02) buttons |= XBOXINPUT_DPAD_DOWN;
	if (high & 0x04) buttons |= XBOXINPUT_DPAD_LEFT;
	if (high & 0x08) buttons |= XBOXINPUT_DPAD_RIGHT;
	if (high & 0x10) buttons |= XBOXINPUT_LEFT_SHOULDER;
	if (high & 0x20) buttons |= XBOXINPUT_RIGHT_SHOULDER;
	if (high & 0x40) buttons |= XBOXINPUT_LEFT_THUMB;
	if (high & 0x80) buttons |= XBOXINPUT_RIGHT_THUMB;

	state->buttons = buttons;
	state->leftTrigger = (uint8_t)(ReadLe16(p + 2) >> 2);
	state->rightTrigger = (uint8_t)(ReadLe16(p + 4) >> 2);
	state->leftX = (int16_t)ReadLe16(p + 6);
	state->leftY = (int16_t)ReadLe16(p + 8);
	state->rightX = (int16_t)ReadLe16(p + 10);
	state->rightY = (int16_t)ReadLe16(p + 12);
	return true;
}

bool XboxInputApplyGipGuidePayload(const uint8_t* payload, int length,
	bool* guideDown, bool* guidePending) {
	if (!payload || length < 2 || !guideDown || !guidePending ||
		payload[1] != 0x5B)
		return false;
	const bool down = payload[0] != 0;
	if (down && !*guideDown)
		*guidePending = true;
	*guideDown = down;
	return true;
}

void XboxInputBuildGipRumblePayload(uint8_t leftMotor, uint8_t rightMotor,
	uint8_t payload[9]) {
	if (!payload)
		return;
	payload[0] = 0x00; // reserved
	payload[1] = 0x03; // left and right grip motors
	payload[2] = 0x00; // left trigger motor (no Xbox 360 equivalent)
	payload[3] = 0x00; // right trigger motor
	payload[4] = leftMotor;
	payload[5] = rightMotor;
	payload[6] = 0xFF; // duration
	payload[7] = 0x00; // delay
	payload[8] = 0xEB; // repeat continuously until the next motor command
}
