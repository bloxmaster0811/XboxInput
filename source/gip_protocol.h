#ifndef XBOXINPUT_GIP_PROTOCOL_H
#define XBOXINPUT_GIP_PROTOCOL_H

#include <xtl.h>
#include <stdint.h>
#include <string.h>
#include "controller_backend.h"
#include "gip_gamepad_parser.h"
#include "gamepad_mapping.h"

// Xbox Game Input Protocol commands and framing used by wired gamepads.
#define GIP_CMD_ACKNOWLEDGE  0x01
#define GIP_CMD_ANNOUNCE     0x02
#define GIP_CMD_STATUS       0x03
#define GIP_CMD_IDENTIFY     0x04
#define GIP_CMD_POWER        0x05
#define GIP_CMD_AUTHENTICATE 0x06
#define GIP_CMD_VIRTUAL_KEY  0x07
#define GIP_CMD_RUMBLE       0x09
#define GIP_CMD_LED          0x0A
#define GIP_CMD_INPUT        0x20

#define GIP_OPT_ACKNOWLEDGE  0x10
#define GIP_OPT_INTERNAL     0x20
#define GIP_OPT_CHUNK_START  0x40
#define GIP_OPT_CHUNK        0x80
#define GIP_OPT_CLIENT_MASK  0x0F

#define GIP_VKEY_GUIDE       0x5B

struct GipHeader {
	uint8_t command;
	uint8_t options;
	uint8_t sequence;
	uint32_t packetLength;
	uint32_t chunkOffset;
	int headerLength;
};

static int GipDecodeVarint(const uint8_t* data, int len, uint32_t* value) {
	if (!data || !value || len <= 0)
		return 0;
	uint32_t decoded = 0;
	int i = 0;
	for (; i < 4 && i < len; ++i) {
		decoded |= (uint32_t)(data[i] & 0x7F) << (i * 7);
		if (!(data[i] & 0x80)) {
			*value = decoded;
			return i + 1;
		}
	}
	return 0;
}

static bool GipDecodeHeader(const uint8_t* data, int len, GipHeader* header) {
	if (!data || !header || len < 4)
		return false;
	header->command = data[0];
	header->options = data[1];
	header->sequence = data[2];
	header->packetLength = 0;
	header->chunkOffset = 0;
	header->headerLength = 0;

	int offset = 3;
	int consumed = GipDecodeVarint(data + offset, len - offset,
		&header->packetLength);
	if (consumed == 0)
		return false;
	offset += consumed;
	if (header->options & GIP_OPT_CHUNK) {
		consumed = GipDecodeVarint(data + offset, len - offset,
			&header->chunkOffset);
		if (consumed == 0)
			return false;
		offset += consumed;
	}
	if (offset > len)
		return false;
	header->headerLength = offset;
	return true;
}

typedef XboxInputNormalizedState GipGamepadState;

static bool GipParseGamepadInput(const BYTE* payload, int length,
	GipGamepadState* state) {
	if (!state)
		return false;
	XboxInputGipGamepadState parsed;
	if (!XboxInputParseGipGamepadPayload(payload, length, &parsed))
		return false;
	state->buttons = parsed.buttons;
	state->leftTrigger = parsed.leftTrigger;
	state->rightTrigger = parsed.rightTrigger;
	state->leftX = parsed.leftX;
	state->leftY = parsed.leftY;
	state->rightX = parsed.rightX;
	state->rightY = parsed.rightY;
	return true;
}

static void GipGamepadToXInput(const GipGamepadState* state,
	XINPUT_GAMEPAD* gamepad) {
	if (!state || !gamepad)
		return;
	XboxInputGipGamepadState input;
	input.buttons = state->buttons;
	input.leftTrigger = state->leftTrigger;
	input.rightTrigger = state->rightTrigger;
	input.leftX = state->leftX;
	input.leftY = state->leftY;
	input.rightX = state->rightX;
	input.rightY = state->rightY;
	XboxInputGipGamepadState mapped;
	XboxInputApplyGamepadMapping(&input, XboxInputGetActiveMapping(), &mapped);
	memset(gamepad, 0, sizeof(*gamepad));
	gamepad->wButtons = mapped.buttons;
	gamepad->bLeftTrigger = mapped.leftTrigger;
	gamepad->bRightTrigger = mapped.rightTrigger;
	gamepad->sThumbLX = mapped.leftX;
	gamepad->sThumbLY = mapped.leftY;
	gamepad->sThumbRX = mapped.rightX;
	gamepad->sThumbRY = mapped.rightY;
}

#endif
