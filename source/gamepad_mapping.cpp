#include "gamepad_mapping.h"
#include <string.h>

static const uint16_t kButtonMasks[XBOXINPUT_MAPPING_BUTTON_COUNT] = {
	0x0001, 0x0002, 0x0004, 0x0008,
	0x0010, 0x0020, 0x0040, 0x0080,
	0x0100, 0x0200, 0x1000, 0x2000, 0x4000, 0x8000,
};

static const char* const kButtonNames[XBOXINPUT_MAPPING_BUTTON_COUNT] = {
	"DpadUp", "DpadDown", "DpadLeft", "DpadRight",
	"Start", "Back", "LeftThumb", "RightThumb",
	"LeftShoulder", "RightShoulder", "A", "B", "X", "Y",
};

XboxInputMappingOptions g_xboxInputGamepadMapping;

static char AsciiLower(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static bool EqualName(const char* a, const char* b) {
	if (!a || !b) return false;
	while (*a && *b) {
		if (AsciiLower(*a++) != AsciiLower(*b++)) return false;
	}
	return *a == 0 && *b == 0;
}

static int FindButtonName(const char* name) {
	for (int i = 0; i < XBOXINPUT_MAPPING_BUTTON_COUNT; ++i)
		if (EqualName(name, kButtonNames[i])) return i;
	return -1;
}

void XboxInputSetDefaultMapping(XboxInputMappingOptions* mapping) {
	if (!mapping) return;
	memset(mapping, 0, sizeof(*mapping));
	for (int i = 0; i < XBOXINPUT_MAPPING_BUTTON_COUNT; ++i)
		mapping->buttonTargets[i] = kButtonMasks[i];
	mapping->rumblePercent = 100;
}

bool XboxInputSetButtonMappingByName(XboxInputMappingOptions* mapping,
	const char* sourceName, const char* targetName) {
	if (!mapping) return false;
	const int source = FindButtonName(sourceName);
	if (source < 0) return false;
	if (EqualName(targetName, "None")) {
		mapping->buttonTargets[source] = 0;
		return true;
	}
	const int target = FindButtonName(targetName);
	if (target < 0) return false;
	mapping->buttonTargets[source] = kButtonMasks[target];
	return true;
}

static int16_t InvertAxis(int16_t value) {
	return value == (int16_t)-32768 ? (int16_t)32767 : (int16_t)-value;
}

static int16_t ApplyStickDeadzone(int16_t value, uint16_t deadzone) {
	if (!deadzone) return value;
	int magnitude = value < 0 ? -(int)value : (int)value;
	if (magnitude <= (int)deadzone) return 0;
	return value;
}

static uint8_t ApplyTriggerDeadzone(uint8_t value, uint8_t deadzone) {
	if (!deadzone) return value;
	if (value <= deadzone) return 0;
	return (uint8_t)(((uint32_t)(value - deadzone) * 255u +
		(255u - deadzone) / 2u) / (255u - deadzone));
}

void XboxInputApplyGamepadMapping(const XboxInputGipGamepadState* input,
	const XboxInputMappingOptions* mapping, XboxInputGipGamepadState* output) {
	if (!input || !output) return;
	if (!mapping) {
		*output = *input;
		return;
	}

	*output = *input;
	uint16_t mappedButtons = 0;
	for (int i = 0; i < XBOXINPUT_MAPPING_BUTTON_COUNT; ++i)
		if (input->buttons & kButtonMasks[i])
			mappedButtons |= mapping->buttonTargets[i];
	output->buttons = mappedButtons;

	if (mapping->swapSticks) {
		int16_t x = output->leftX, y = output->leftY;
		output->leftX = output->rightX; output->leftY = output->rightY;
		output->rightX = x; output->rightY = y;
	}
	if (mapping->invertLeftX) output->leftX = InvertAxis(output->leftX);
	if (mapping->invertLeftY) output->leftY = InvertAxis(output->leftY);
	if (mapping->invertRightX) output->rightX = InvertAxis(output->rightX);
	if (mapping->invertRightY) output->rightY = InvertAxis(output->rightY);
	output->leftX = ApplyStickDeadzone(output->leftX, mapping->leftStickDeadzone);
	output->leftY = ApplyStickDeadzone(output->leftY, mapping->leftStickDeadzone);
	output->rightX = ApplyStickDeadzone(output->rightX, mapping->rightStickDeadzone);
	output->rightY = ApplyStickDeadzone(output->rightY, mapping->rightStickDeadzone);

	if (mapping->swapTriggers) {
		uint8_t trigger = output->leftTrigger;
		output->leftTrigger = output->rightTrigger;
		output->rightTrigger = trigger;
	}
	output->leftTrigger = ApplyTriggerDeadzone(output->leftTrigger,
		mapping->leftTriggerDeadzone);
	output->rightTrigger = ApplyTriggerDeadzone(output->rightTrigger,
		mapping->rightTriggerDeadzone);
}

uint8_t XboxInputScaleRumble(uint8_t motor, uint8_t percent) {
	if (percent > 100) percent = 100;
	return (uint8_t)(((uint32_t)motor * percent + 50u) / 100u);
}
