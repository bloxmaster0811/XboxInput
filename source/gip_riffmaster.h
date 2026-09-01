//
// RiffMaster GIP input decoding + Xbox 360 guitar translation.
//
// Header-only so no .vcxproj change is needed.
//
// EVERY offset and bit in this file is traceable to:
//   docs/gip_riffmaster.md          - the GIP source format (verified from a PC capture,
//                                     cross-confirmed by RB4InstrumentMapper + PlasticBand)
//   docs/xbox360_guitar_mapping.md  - the Xbox 360 target format (PlasticBand)
// Nothing here is guessed. Where a value is uncertain it is called out in a comment.
//
// DELIBERATELY NO BITFIELDS. docs/BUILDING.md records that MSVC allocates bitfields MSB-first
// on PowerPC and LSB-first on x86, so a struct copied from an x86 reference compiles
// cleanly and decodes wrong with no warning. Masks and shifts are endian-agnostic and
// cannot silently invert.
//
#ifndef GIP_RIFFMASTER_H
#define GIP_RIFFMASTER_H

#include <xtl.h>
#include "riffmaster_config.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// GIP framing - refs/xone/bus/protocol.c:200-282
// ---------------------------------------------------------------------------
#define GIP_CMD_ACKNOWLEDGE 0x01
#define GIP_CMD_ANNOUNCE    0x02
#define GIP_CMD_STATUS      0x03
#define GIP_CMD_IDENTIFY    0x04
#define GIP_CMD_POWER       0x05
#define GIP_CMD_AUTHENTICATE 0x06
#define GIP_CMD_VIRTUAL_KEY 0x07
#define GIP_CMD_RUMBLE      0x09
#define GIP_CMD_LED         0x0A
#define GIP_CMD_INPUT       0x20

#define GIP_OPT_ACKNOWLEDGE 0x10
#define GIP_OPT_INTERNAL    0x20
#define GIP_OPT_CHUNK_START 0x40
#define GIP_OPT_CHUNK       0x80
#define GIP_OPT_CLIENT_MASK 0x0F

#define GIP_VKEY_GUIDE      0x5B   // protocol.c:24, the only accepted key

struct GipHeader {
	uint8_t  command;
	uint8_t  options;
	uint8_t  sequence;
	uint32_t packetLength;
	uint32_t chunkOffset;
	int      headerLength;   // bytes consumed; 0 => malformed
};

//
// Variable-length integer, refs/xone/bus/protocol.c:200-213.
// The length field is NOT a plain byte - a fixed 4-byte header parses input reports
// fine and then silently corrupts the chunked announce/identify packets.
//
static int GipDecodeVarint(const uint8_t* data, int len, uint32_t* val) {
	uint32_t v = 0;
	int i = 0;
	for (; i < 4 && i < len; i++) {
		v |= (uint32_t)(data[i] & 0x7F) << (i * 7);
		if (!(data[i] & 0x80))
			break;
	}
	*val = v;
	return i + 1;
}

//
// Returns false if the buffer is too short to hold the header it describes.
//
static bool GipDecodeHeader(const uint8_t* data, int len, GipHeader* h) {
	if (!data || !h || len < 4)
		return false;

	h->command      = data[0];
	h->options      = data[1];
	h->sequence     = data[2];
	h->packetLength = 0;
	h->chunkOffset  = 0;

	int i = 3;
	i += GipDecodeVarint(data + i, len - i, &h->packetLength);

	if (h->options & GIP_OPT_CHUNK) {
		if (i >= len)
			return false;
		i += GipDecodeVarint(data + i, len - i, &h->chunkOffset);
	}

	if (i > len)
		return false;

	h->headerLength = i;
	return true;
}

// ---------------------------------------------------------------------------
// RiffMaster 0x20 input report - docs/gip_riffmaster.md section 4
// Offsets below are PAYLOAD-relative (packet offset = payload + headerLength).
// ---------------------------------------------------------------------------
#define RM_OFF_BUTTONS_LO   0
#define RM_OFF_BUTTONS_HI   1
#define RM_OFF_TILT         2
#define RM_OFF_WHAMMY       3
#define RM_OFF_PICKUP       4
#define RM_OFF_UPPER_FRETS  5
#define RM_OFF_LOWER_FRETS  6
#define RM_OFF_JOY_X_LO     10
#define RM_OFF_JOY_Y_LO     12
#define RM_OFF_SYSTEM       14
#define RM_MIN_PAYLOAD      15   // everything we read lives below this

// Buttons word, little-endian u16 across payload bytes 0-1.
#define RM_BTN_SYNC        0x0001
#define RM_BTN_MENU        0x0004
#define RM_BTN_VIEW        0x0008
#define RM_BTN_GREEN_DUP   0x0010   // merged view; prefer the fret bytes
#define RM_BTN_RED_DUP     0x0020
#define RM_BTN_BLUE_DUP    0x0040
#define RM_BTN_YELLOW_DUP  0x0080
#define RM_BTN_STRUM_UP    0x0100
#define RM_BTN_STRUM_DOWN  0x0200
#define RM_BTN_DPAD_LEFT   0x0400
#define RM_BTN_DPAD_RIGHT  0x0800
#define RM_BTN_ORANGE_DUP  0x1000
#define RM_BTN_SOLO_OR_STICK 0x4000  // OVERLOADED - see RiffmasterState::joystickClick

// Fret bitfield, identical in the upper (payload 5) and lower (payload 6) bytes.
#define RM_FRET_GREEN   0x01
#define RM_FRET_RED     0x02
#define RM_FRET_YELLOW  0x04
#define RM_FRET_BLUE    0x08
#define RM_FRET_ORANGE  0x10

#define RM_SYS_SHARE    0x01

struct RiffmasterState {
	uint16_t buttons;
	uint8_t  upperFrets;
	uint8_t  lowerFrets;   // solo frets
	uint8_t  tilt;         // 0x00-0xFF, rest is orientation-dependent (0x08 and 0x66 both seen)
	uint8_t  whammy;       // 0x00 released -> 0xFF fully pressed
	uint8_t  pickup;       // 0x00,0x10,0x20,0x30,0x40 per references; UNVERIFIED on hardware
	int16_t  joyX;
	int16_t  joyY;
	uint8_t  system;       // bit0 = Share
	bool     guide;        // NOT in this report - arrives as GIP command 0x07
};

//
// Decode a 0x20 payload. Returns false if it is too short to trust.
// Multi-byte values are assembled byte-wise, so this is correct on a big-endian PPC
// target without any byteswap helper.
//
static bool RiffmasterParseInput(const uint8_t* p, int len, RiffmasterState* s) {
	if (!p || !s || len < RM_MIN_PAYLOAD)
		return false;

	s->buttons    = (uint16_t)(p[RM_OFF_BUTTONS_LO] | ((uint16_t)p[RM_OFF_BUTTONS_HI] << 8));
	s->tilt       = p[RM_OFF_TILT];
	s->whammy     = p[RM_OFF_WHAMMY];
	s->pickup     = p[RM_OFF_PICKUP];
	s->upperFrets = p[RM_OFF_UPPER_FRETS];
	s->lowerFrets = p[RM_OFF_LOWER_FRETS];
	s->joyX       = (int16_t)(p[RM_OFF_JOY_X_LO] | ((uint16_t)p[RM_OFF_JOY_X_LO + 1] << 8));
	s->joyY       = (int16_t)(p[RM_OFF_JOY_Y_LO] | ((uint16_t)p[RM_OFF_JOY_Y_LO + 1] << 8));
	s->system     = p[RM_OFF_SYSTEM];
	return true;
}

//
// Bit 0x4000 means EITHER "a solo fret is held" OR "the joystick was clicked".
// RB4InstrumentMapper disambiguates by trusting the lower-fret byte, not the flag
// (XboxRiffmasterInput.cs:22-23), and our capture confirms it: at 43.98 s the bit
// asserted with lowerFrets == 0 while the joystick axes moved.
//
static bool RiffmasterJoystickClick(const RiffmasterState* s) {
	return (s->buttons & RM_BTN_SOLO_OR_STICK) != 0 && s->lowerFrets == 0;
}

// ---------------------------------------------------------------------------
// Xbox 360 guitar translation - docs/xbox360_guitar_mapping.md
// ---------------------------------------------------------------------------

//
// Tilt on a real 360 guitar is reported as a BUTTON, not a continuous axis:
// "not tilted = 0, tilted = 32767". So we threshold rather than scale.
// 0xD0 is RB4InstrumentMapper's value (GuitarRPCS3Mapper.cs:44-84), whose own comment
// says it "should probably be configurable/calibratable". UNVERIFIED - tune in Phase 4.
//
#define RM_TILT_THRESHOLD 0xD0

//
// Pickup switch notch midpoints measured on real 360 hardware,
// refs/PlasticBand/Docs/Instruments/5-Fret Guitar/Rock Band/General Notes.md:30-36.
//
static uint8_t RiffmasterPickupTo360(uint8_t raw) {
	static const uint8_t notch[5] = { 0x17, 0x4B, 0x79, 0xAB, 0xE0 };
	int i = raw >> 4;              // GIP reports 0x00,0x10,0x20,0x30,0x40
	if (i < 0) i = 0;
	if (i > 4) i = 4;
	return notch[i];
}

//
// Whammy: byte -> full-range signed short, rest = -32768.
//
static int16_t RiffmasterWhammyTo360(uint8_t w) {
	return (int16_t)(((int32_t)w * 65535 / 255) - 32768);
}

//
// Fill a 360 XINPUT_GAMEPAD from guitar state.
//
// THE SOLO COLLAPSE, which is the whole reason this is a translation and not a
// passthrough: GIP gives independent upper/lower fret bytes; the 360 has only five
// fret bits plus one solo FLAG (LEFT_THUMB). Upper-green + solo-red simultaneously is
// not representable on the 360 - that is a hard limit of the target format.
//
static void RiffmasterToXInput(const RiffmasterState* s, XINPUT_GAMEPAD* g) {
	if (!s || !g)
		return;

	memset(g, 0, sizeof(XINPUT_GAMEPAD));

	const uint8_t frets = (uint8_t)(s->upperFrets | s->lowerFrets);

	if (frets & RM_FRET_GREEN)  g->wButtons |= XINPUT_GAMEPAD_A;
	if (frets & RM_FRET_RED)    g->wButtons |= XINPUT_GAMEPAD_B;
	if (frets & RM_FRET_YELLOW) g->wButtons |= XINPUT_GAMEPAD_Y;
	if (frets & RM_FRET_BLUE)   g->wButtons |= XINPUT_GAMEPAD_X;
	if (frets & RM_FRET_ORANGE) g->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;

	// Solo flag: trust the lower-fret BYTE, never bit 0x4000 (overloaded with stick click).
	if (s->lowerFrets != 0 && g_rmCfg.soloFlag)
		g->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;

	const bool strumUp   = (s->buttons & RM_BTN_STRUM_UP)   != 0;
	const bool strumDown = (s->buttons & RM_BTN_STRUM_DOWN) != 0;
	if (g_rmCfg.invertStrum) {
		if (strumDown) g->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
		if (strumUp)   g->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
	}
	else {
		if (strumUp)   g->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
		if (strumDown) g->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
	}
	if (s->buttons & RM_BTN_DPAD_LEFT)  g->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
	if (s->buttons & RM_BTN_DPAD_RIGHT) g->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
	if (s->buttons & RM_BTN_MENU)       g->wButtons |= XINPUT_GAMEPAD_START;
	if (s->buttons & RM_BTN_VIEW)       g->wButtons |= XINPUT_GAMEPAD_BACK;

	// Joystick as a D-pad. EXPERIMENTAL, off by default: the stick's true range and
	// resting drift were never measured (the protocol capture has only ~2 s of stick
	// movement), so the deadzone is a conservative guess rather than a derived value.
	// Vertical is a separate opt-in because D-pad up/down is also how strum is reported,
	// so a drifting stick could register as a phantom strum mid-song.
	if (g_rmCfg.joystickAsDpad) {
		const int dz = g_rmCfg.joystickDeadzone;
		if (s->joyX < -dz) g->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
		if (s->joyX >  dz) g->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
		if (g_rmCfg.joystickDpadVertical) {
			if (s->joyY >  dz) g->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
			if (s->joyY < -dz) g->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
		}
	}

	uint8_t whammy = s->whammy;
	if (g_rmCfg.invertWhammy)
		whammy = (uint8_t)(255 - whammy);
	g->sThumbRX = RiffmasterWhammyTo360(whammy);

	// Star power. A real 360 guitar has only one source for this - tilt, reported on the
	// right stick Y axis - so both sources here feed the same axis and a game cannot tell
	// them apart.
	//
	// The joystick click is a better trigger than tilt for anyone who would rather not
	// swing the guitar, and it is reliable: bit 0x4000 is overloaded between "solo fret
	// held" and "stick clicked", but RiffmasterJoystickClick() disambiguates on the
	// lower-fret byte, so a solo fret can never be mistaken for a click.
	//
	// Tilt uses hysteresis - engage at TiltThreshold, release only below TiltRelease -
	// so a guitar held right at the boundary cannot chatter star power on and off. The
	// latch is a function-local static: there is exactly one guitar, and threading it
	// through RiffmasterState would mean the parser owning presentation state.
	static bool s_spTiltLatched = false;
	if (g_rmCfg.starPowerTilt) {
		if (!s_spTiltLatched && s->tilt >= (uint8_t)g_rmCfg.tiltThreshold)
			s_spTiltLatched = true;
		else if (s_spTiltLatched && s->tilt < (uint8_t)g_rmCfg.tiltRelease)
			s_spTiltLatched = false;
	}
	else {
		s_spTiltLatched = false;
	}

	const bool spClick = g_rmCfg.starPowerClick && RiffmasterJoystickClick(s);
	g->sThumbRY = (s_spTiltLatched || spClick) ? 32767 : 0;

	g->bLeftTrigger = g_rmCfg.pickupSwitch ? RiffmasterPickupTo360(s->pickup) : 0;

	// Left stick is the auto-calibration sensor on a real 360 guitar, NOT a spare axis.
	// Feeding the RiffMaster joystick here could confuse a game's calibration routine,
	// which is why the stick goes to the D-pad above instead. The 360 guitar format has
	// no joystick at all.
	g->sThumbLX = 0;
	g->sThumbLY = 0;
}

// Standard Xbox One / Series wired-gamepad input (GIP command 0x20).  GIP
// fields are little-endian on the wire; assemble them bytewise for the 360's
// big-endian PPC target.
struct GipGamepadState {
	WORD buttons;
	BYTE leftTrigger, rightTrigger;
	SHORT leftX, leftY, rightX, rightY;
	bool guide;
};

static WORD GipReadLe16(const BYTE* p) {
	return (WORD)(p[0] | ((WORD)p[1] << 8));
}

static bool GipParseGamepadInput(const BYTE* p, int len, GipGamepadState* s) {
	if (!p || !s || len < 14) return false;
	const BYTE low = p[0], high = p[1];
	WORD buttons = 0;
	if (low & 0x10) buttons |= XINPUT_GAMEPAD_A;
	if (low & 0x20) buttons |= XINPUT_GAMEPAD_B;
	if (low & 0x40) buttons |= XINPUT_GAMEPAD_X;
	if (low & 0x80) buttons |= XINPUT_GAMEPAD_Y;
	if (low & 0x04) buttons |= XINPUT_GAMEPAD_START;
	if (low & 0x08) buttons |= XINPUT_GAMEPAD_BACK;
	if (high & 0x01) buttons |= XINPUT_GAMEPAD_DPAD_UP;
	if (high & 0x02) buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
	if (high & 0x04) buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
	if (high & 0x08) buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
	if (high & 0x10) buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
	if (high & 0x20) buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
	if (high & 0x40) buttons |= XINPUT_GAMEPAD_LEFT_THUMB;
	if (high & 0x80) buttons |= XINPUT_GAMEPAD_RIGHT_THUMB;
	s->buttons = buttons;
	s->leftTrigger = (BYTE)(GipReadLe16(p + 2) >> 2);
	s->rightTrigger = (BYTE)(GipReadLe16(p + 4) >> 2);
	s->leftX = (SHORT)GipReadLe16(p + 6);
	s->leftY = (SHORT)GipReadLe16(p + 8);
	s->rightX = (SHORT)GipReadLe16(p + 10);
	s->rightY = (SHORT)GipReadLe16(p + 12);
	return true;
}

static void GipGamepadToXInput(const GipGamepadState* s, XINPUT_GAMEPAD* g) {
	if (!s || !g) return;
	memset(g, 0, sizeof(XINPUT_GAMEPAD));
	g->wButtons = s->buttons;
	g->bLeftTrigger = s->leftTrigger;
	g->bRightTrigger = s->rightTrigger;
	g->sThumbLX = s->leftX;
	g->sThumbLY = s->leftY;
	g->sThumbRX = s->rightX;
	g->sThumbRY = s->rightY;
}

#endif // GIP_RIFFMASTER_H
