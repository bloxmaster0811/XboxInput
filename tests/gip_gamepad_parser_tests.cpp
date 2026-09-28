#include "../source/gip_gamepad_parser.h"
#include "../source/gamepad_mapping.h"
#include <assert.h>
#include <string.h>

static void TestButtons() {
	struct Case { uint8_t low, high; uint16_t expected; } cases[] = {
		{0x10,0,0x1000}, {0x20,0,0x2000}, {0x40,0,0x4000}, {0x80,0,0x8000},
		{0x04,0,0x0010}, {0x08,0,0x0020}, {0,0x01,0x0001}, {0,0x02,0x0002},
		{0,0x04,0x0004}, {0,0x08,0x0008}, {0,0x10,0x0100}, {0,0x20,0x0200},
		{0,0x40,0x0040}, {0,0x80,0x0080},
	};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		uint8_t payload[14] = {0};
		payload[0] = cases[i].low;
		payload[1] = cases[i].high;
		XboxInputGipGamepadState state = {0};
		assert(XboxInputParseGipGamepadPayload(payload, sizeof(payload), &state));
		assert(state.buttons == cases[i].expected);
	}
}

static void TestAnalogsAndValidation() {
	const uint8_t payload[14] = {
		0, 0, 0xFF, 0x03, 0x00, 0x02,
		0x00, 0x80, 0xFF, 0x7F, 0xFF, 0xFF, 0x34, 0x12
	};
	XboxInputGipGamepadState state = {0};
	assert(XboxInputParseGipGamepadPayload(payload, sizeof(payload), &state));
	assert(state.leftTrigger == 255 && state.rightTrigger == 128);
	assert(state.leftX == -32768 && state.leftY == 32767);
	assert(state.rightX == -1 && state.rightY == 0x1234);

	XboxInputGipGamepadState sentinel;
	memset(&sentinel, 0x5A, sizeof(sentinel));
	XboxInputGipGamepadState unchanged = sentinel;
	assert(!XboxInputParseGipGamepadPayload(payload, 13, &sentinel));
	assert(memcmp(&sentinel, &unchanged, sizeof(sentinel)) == 0);
}

static void TestGuideEdgeHandling() {
	bool down = false;
	bool pending = false;
	const uint8_t guideDown[] = {1, 0x5B};
	const uint8_t guideUp[] = {0, 0x5B};
	const uint8_t wrongKey[] = {1, 0x5A};

	assert(XboxInputApplyGipGuidePayload(guideDown, 2, &down, &pending));
	assert(down && pending);
	pending = false; // Simulate the XAM state hook consuming the press.
	assert(XboxInputApplyGipGuidePayload(guideDown, 2, &down, &pending));
	assert(down && !pending); // A repeated DOWN must not open/close Guide twice.
	assert(XboxInputApplyGipGuidePayload(guideUp, 2, &down, &pending));
	assert(!down && !pending);
	assert(XboxInputApplyGipGuidePayload(guideDown, 2, &down, &pending));
	assert(down && pending); // A new physical press produces a new edge.
	assert(!XboxInputApplyGipGuidePayload(wrongKey, 2, &down, &pending));
	assert(!XboxInputApplyGipGuidePayload(guideDown, 1, &down, &pending));
}

static void TestRumblePayload() {
	uint8_t payload[9] = {0};
	XboxInputBuildGipRumblePayload(0x12, 0x34, payload);
	const uint8_t expected[9] = {
		0x00, 0x03, 0x00, 0x00, 0x12, 0x34, 0xFF, 0x00, 0xEB
	};
	assert(memcmp(payload, expected, sizeof(expected)) == 0);

	XboxInputBuildGipRumblePayload(0, 0, payload);
	assert(payload[4] == 0 && payload[5] == 0);
	assert(payload[6] == 0xFF && payload[8] == 0xEB);
}

static void TestMappings() {
	XboxInputMappingOptions mapping;
	XboxInputSetDefaultMapping(&mapping);
	XboxInputGipGamepadState input = {
		(uint16_t)(0x1000 | 0x0001), 100, 200, -32768, 500, 2000, -3000
	};
	XboxInputGipGamepadState output = {0};
	XboxInputApplyGamepadMapping(&input, &mapping, &output);
	assert(memcmp(&input, &output, sizeof(input)) == 0);

	assert(XboxInputSetButtonMappingByName(&mapping, "A", "B"));
	assert(XboxInputSetButtonMappingByName(&mapping, "DpadUp", "None"));
	mapping.swapSticks = true;
	mapping.invertRightX = true;
	mapping.leftStickDeadzone = 2500;
	mapping.swapTriggers = true;
	mapping.leftTriggerDeadzone = 20;
	XboxInputApplyGamepadMapping(&input, &mapping, &output);
	assert(output.buttons == 0x2000);
	assert(output.leftX == 0 && output.leftY == -3000);
	assert(output.rightX == 32767 && output.rightY == 500);
	assert(output.leftTrigger == 195 && output.rightTrigger == 100);
	assert(!XboxInputSetButtonMappingByName(&mapping, "NotAButton", "A"));
	assert(!XboxInputSetButtonMappingByName(&mapping, "A", "NotAButton"));
	assert(XboxInputScaleRumble(255, 100) == 255);
	assert(XboxInputScaleRumble(255, 50) == 128);
	assert(XboxInputScaleRumble(123, 0) == 0);
}

int main() {
	TestButtons();
	TestAnalogsAndValidation();
	TestGuideEdgeHandling();
	TestRumblePayload();
	TestMappings();
	return 0;
}
