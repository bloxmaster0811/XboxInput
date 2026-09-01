//
// riffmaster.ini - optional user settings.
//
// The plugin needs NO config file. Every setting below has a built-in default matching
// the behaviour verified on hardware, so a console with no riffmaster.ini behaves exactly
// as the tested build does. The file exists so people can tune the things that genuinely
// vary between guitars, consoles and game libraries - not as a required install step.
//
// It is written out with full documentation on first load if it does not exist, so users
// get a self-explaining file rather than having to read the source.
//
// WHAT IS DELIBERATELY NOT CONFIGURABLE: the fret colours. Rock Band and Guitar Hero
// expect green=A, red=B, yellow=Y, blue=X, orange=LB. Remapping those does not customise
// the guitar, it breaks the games - so it is not offered.
//
// WHERE THIS IS SAFE TO TOUCH: the file is read exactly once, from DllMain, at plugin
// load. Nothing here does file I/O from a USB completion, a XAM hook, or anything else at
// raised IRQL - those paths only read the already-parsed globals. Upstream already reads
// a file from DllMain (LoadMappingsFromFile), so this is a proven context for it.
//
#ifndef RIFFMASTER_CONFIG_H
#define RIFFMASTER_CONFIG_H

#include <xtl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define RM_CFG_PATH            "HDD:\\riffmaster.ini"
#define RM_CFG_MAX_OVERRIDES   32

struct RiffmasterConfig {
	// --- [Guitar] ---
	int  tiltThreshold;       // 0-255, tilt value at which star power engages
	int  tiltRelease;         // 0-255, value it must fall back below to disengage
	bool starPowerTilt;
	bool starPowerClick;
	bool soloFlag;
	bool invertStrum;
	bool invertWhammy;
	bool pickupSwitch;        // report a pickup value at all

	// --- [Joystick] ---
	bool joystickAsDpad;      // EXPERIMENTAL, off by default
	bool joystickDpadVertical;
	int  joystickDeadzone;    // 0-32767

	// --- [Guide] ---
	bool guideButton;
	int  guideCooldownMs;

	// --- [Subtypes] ---
	BYTE  defaultSubType;
	int   overrideCount;
	DWORD overrideId[RM_CFG_MAX_OVERRIDES];
	BYTE  overrideSub[RM_CFG_MAX_OVERRIDES];

	bool  loadedFromFile;
};

static RiffmasterConfig g_rmCfg = {
	0xD0,   // tiltThreshold  - RB4InstrumentMapper's value; verified to feel right
	0xC0,   // tiltRelease    - 16 below, so a guitar hovering at the edge cannot chatter
	true,   // starPowerTilt
	true,   // starPowerClick
	true,   // soloFlag
	false,  // invertStrum
	false,  // invertWhammy
	true,   // pickupSwitch
	false,  // joystickAsDpad - EXPERIMENTAL and untested, opt in
	false,  // joystickDpadVertical
	8192,   // joystickDeadzone - 25%, conservative because true stick range is unmeasured
	true,   // guideButton
	1000,   // guideCooldownMs
	0x06,   // defaultSubType - works in 20 of 21 titles tested
	0, { 0 }, { 0 },
	false
};

//
// Tiny, defensive INI parsing. No allocation, no exceptions, fixed buffers: this runs at
// plugin load inside someone else's process, and a malformed file must produce defaults
// rather than a hang.
//
static void RmCfgTrim(char* s) {
	if (!s) return;
	char* p = s;
	while (*p == ' ' || *p == '\t') p++;
	if (p != s) memmove(s, p, strlen(p) + 1);
	int n = (int)strlen(s);
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
	                 s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

static DWORD RmCfgNumber(const char* v, bool preferHex) {
	if (!v || !*v) return 0;
	if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
		return (DWORD)strtoul(v + 2, 0, 16);
	return (DWORD)strtoul(v, 0, preferHex ? 16 : 10);
}

static bool RmCfgBool(const char* v) {
	if (!v || !*v) return false;
	return (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y' ||
	        v[0] == 'o' || v[0] == 'O');   // 1 / true / yes / on
}

static int RmCfgClamp(int v, int lo, int hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

static void RmCfgWriteDefaults(const char* path) {
	FILE* f = fopen(path, "w");
	if (!f)
		return;   // read-only HDD or no write access: defaults still apply, just no file

	fputs(
	"; ============================================================================\r\n"
	";  riffmaster.ini - settings for riffmaster-rgh360\r\n"
	"; ============================================================================\r\n"
	";\r\n"
	";  This file is OPTIONAL. Every value below is already the built-in default, so\r\n"
	";  deleting the file changes nothing - the plugin just recreates it next boot.\r\n"
	";\r\n"
	";  CHANGES TAKE EFFECT AFTER A HARD REBOOT. The file is read once when the plugin\r\n"
	";  loads and never again.\r\n"
	";\r\n"
	";  Booleans accept: 1 / 0, true / false, yes / no, on / off\r\n"
	";  Lines starting with ; or # are comments.\r\n"
	";\r\n"
	";  NOT CONFIGURABLE ON PURPOSE: the fret colours. Rock Band and Guitar Hero\r\n"
	";  expect green=A, red=B, yellow=Y, blue=X, orange=LB. Remapping them would not\r\n"
	";  customise the guitar, it would break the games.\r\n"
	";\r\n"
	"\r\n"
	"; ----------------------------------------------------------------------------\r\n"
	"; [Guitar]  -  how the guitar's controls behave\r\n"
	"; ----------------------------------------------------------------------------\r\n"
	";\r\n"
	"; TiltThreshold = 0-255          (default 208)\r\n"
	";     How far you must tilt the neck up before star power engages.\r\n"
	";     LOWER  = more sensitive, less tilt needed.\r\n"
	";     HIGHER = less sensitive, more tilt needed.\r\n"
	";     Raise this if star power fires while you are just playing; lower it if it\r\n"
	";     will not fire even when the guitar is vertical. Resting tilt varies between\r\n"
	";     guitars and even with how you hold one, which is why this is adjustable.\r\n"
	";\r\n"
	"; TiltRelease = 0-255            (default 192)\r\n"
	";     Once engaged, tilt must fall back BELOW this before star power disengages.\r\n"
	";     The gap between it and TiltThreshold is hysteresis: it stops star power\r\n"
	";     flickering on and off when the guitar sits right at the threshold.\r\n"
	";     Keep it below TiltThreshold. Set it equal to TiltThreshold for no hysteresis.\r\n"
	";\r\n"
	"; StarPowerOnTilt = 1 or 0       (default 1)\r\n"
	";     1 = tilting the guitar activates star power.\r\n"
	";     0 = tilt is ignored. Useful with the joystick option below if you would\r\n"
	";         rather never swing the guitar, or if your tilt sensor is unreliable.\r\n"
	";\r\n"
	"; StarPowerOnJoystickClick = 1 or 0   (default 1)\r\n"
	";     1 = clicking the joystick (pressing it in) activates star power.\r\n"
	";     0 = the joystick click does nothing.\r\n"
	";     Both sources feed the same input, so a game cannot tell them apart, and\r\n"
	";     having both enabled is fine. A solo fret can never be mistaken for a click.\r\n"
	";\r\n"
	"; SoloFlag = 1 or 0              (default 1)\r\n"
	";     1 = the solo frets (the small row nearest the strum bar) tell the game they\r\n"
	";         are solo frets, matching real Xbox 360 guitar behaviour.\r\n"
	";     0 = they act as plain frets with no solo indication. Only worth trying if a\r\n"
	";         game behaves oddly when you use them.\r\n"
	";\r\n"
	"; InvertStrum = 1 or 0           (default 0)\r\n"
	";     1 = swap strum up and strum down.\r\n"
	";     0 = normal.\r\n"
	";\r\n"
	"; InvertWhammy = 1 or 0          (default 0)\r\n"
	";     1 = reverse the whammy bar, so resting reads as fully pressed.\r\n"
	";     0 = normal. Only useful if your whammy reads backwards.\r\n"
	";\r\n"
	"; PickupSwitch = 1 or 0          (default 1)\r\n"
	";     1 = report a pickup-switch position, as a real Rock Band guitar always does.\r\n"
	";     0 = report nothing for it.\r\n"
	";     The RiffMaster has no physical pickup switch, so this reports a constant\r\n"
	";     resting value - the same thing a real guitar with the switch parked sends.\r\n"
	";     Set 0 only if some game reacts badly to it.\r\n"
	"\r\n"
	"[Guitar]\r\n"
	"TiltThreshold = 208\r\n"
	"TiltRelease = 192\r\n"
	"StarPowerOnTilt = 1\r\n"
	"StarPowerOnJoystickClick = 1\r\n"
	"SoloFlag = 1\r\n"
	"InvertStrum = 0\r\n"
	"InvertWhammy = 0\r\n"
	"PickupSwitch = 1\r\n"
	"\r\n"
	"; ----------------------------------------------------------------------------\r\n"
	"; [Joystick]  -  the thumbstick on the guitar body\r\n"
	"; ----------------------------------------------------------------------------\r\n"
	";\r\n"
	"; The CLICK is handled in [Guitar] above (StarPowerOnJoystickClick). This section\r\n"
	"; is only about moving the stick.\r\n"
	";\r\n"
	"; *** EXPERIMENTAL AND UNTESTED. Off by default. ***\r\n"
	"; A real Xbox 360 guitar has no joystick, so there is nothing to be compatible\r\n"
	"; with, and the stick's true range and resting drift have never been measured on\r\n"
	"; hardware. If you enable this and the menus start moving on their own, raise\r\n"
	"; JoystickDeadzone or turn it off again.\r\n"
	";\r\n"
	"; JoystickAsDpad = 1 or 0        (default 0)\r\n"
	";     1 = pushing the stick left/right acts as D-pad left/right, for menus.\r\n"
	";     0 = the stick does nothing.\r\n"
	";\r\n"
	"; JoystickDpadVertical = 1 or 0  (default 0)\r\n"
	";     1 = also map stick up/down to D-pad up/down.\r\n"
	";     0 = vertical is ignored.\r\n"
	";     CAUTION: D-pad up/down is also how strumming is reported. With this on, a\r\n"
	";     drifting stick can register as a phantom strum and cost you notes mid-song.\r\n"
	";     Leave it off unless you only use the guitar for menus.\r\n"
	";\r\n"
	"; JoystickDeadzone = 0-32767     (default 8192)\r\n"
	";     How far the stick must move before it counts. Higher = less sensitive and\r\n"
	";     less prone to drift. 8192 is about a quarter of full deflection.\r\n"
	"\r\n"
	"[Joystick]\r\n"
	"JoystickAsDpad = 0\r\n"
	"JoystickDpadVertical = 0\r\n"
	"JoystickDeadzone = 8192\r\n"
	"\r\n"
	"; ----------------------------------------------------------------------------\r\n"
	"; [Guide]  -  the Xbox guide button\r\n"
	"; ----------------------------------------------------------------------------\r\n"
	";\r\n"
	"; GuideButton = 1 or 0           (default 1)\r\n"
	";     1 = the guitar's Xbox button opens the guide blade.\r\n"
	";     0 = it does nothing. Set this if the blade opens by accident while playing.\r\n"
	";\r\n"
	"; GuideCooldownMs = 100-10000    (default 1000)\r\n"
	";     Minimum milliseconds between guide presses. Stops one press being read as\r\n"
	";     many and opening/closing the blade repeatedly. Raise it if that happens.\r\n"
	"\r\n"
	"[Guide]\r\n"
	"GuideButton = 1\r\n"
	"GuideCooldownMs = 1000\r\n"
	"\r\n"
	"; ----------------------------------------------------------------------------\r\n"
	"; [Subtypes]  -  what kind of guitar each game is told it is\r\n"
	"; ----------------------------------------------------------------------------\r\n"
	";\r\n"
	"; The Xbox 360 has two guitar types, and a few games only accept one of them:\r\n"
	";     6 = Rock Band guitar\r\n"
	";     7 = Guitar Hero guitar\r\n"
	";\r\n"
	"; 6 works in 20 of the 21 titles tested. Guitar Hero III is the only exception and\r\n"
	"; is already handled automatically, so you should not need to change anything here.\r\n"
	";\r\n"
	"; It exists because title IDs differ between regions and re-releases. A PAL or\r\n"
	"; reissued disc can have a different ID than the one built in, miss the automatic\r\n"
	"; fix, and not work - and this is how you correct that without rebuilding.\r\n"
	";\r\n"
	"; Default = 6 or 7               (default 6)\r\n"
	";     What every game gets unless listed below. Leave at 6.\r\n"
	";\r\n"
	"; <titleid> = 6 or 7\r\n"
	";     Force one specific game. Entries here beat the built-in table, so you can\r\n"
	";     also use this to override a built-in entry that is wrong on your console.\r\n"
	";\r\n"
	";     To find a game's title ID: connect the guitar, launch the game with xbWatson\r\n"
	";     attached, and look for\r\n"
	";         RIFFMASTER: title id 0x415607F7 -> SubType 0x07 (Guitar Hero III)\r\n"
	";     then write the id without the 0x. Up to 32 entries.\r\n"
	";\r\n"
	";     Example - this exact line is already built in, and is shown only as a guide:\r\n"
	";         415607F7 = 7\r\n"
	"\r\n"
	"[Subtypes]\r\n"
	"Default = 6\r\n",
	f);
	fclose(f);
}

//
// Returns true if a file was found and parsed. A missing file is not an error - it means
// defaults, and one gets written for next time.
//
static bool RmCfgLoad(const char* path) {
	FILE* f = fopen(path, "r");
	if (!f) {
		RmCfgWriteDefaults(path);
		return false;
	}

	char line[256];
	char section[32];
	section[0] = 0;

	while (fgets(line, sizeof(line), f)) {
		RmCfgTrim(line);
		if (!line[0] || line[0] == ';' || line[0] == '#')
			continue;

		if (line[0] == '[') {
			char* end = strchr(line, ']');
			if (!end) continue;
			*end = 0;
			strncpy(section, line + 1, sizeof(section) - 1);
			section[sizeof(section) - 1] = 0;
			continue;
		}

		char* eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char* key = line;
		char* val = eq + 1;
		RmCfgTrim(key);
		RmCfgTrim(val);
		if (!key[0]) continue;

		if (_stricmp(section, "Guitar") == 0) {
			if      (_stricmp(key, "TiltThreshold") == 0)            g_rmCfg.tiltThreshold = RmCfgClamp((int)RmCfgNumber(val, false), 0, 255);
			else if (_stricmp(key, "TiltRelease") == 0)              g_rmCfg.tiltRelease   = RmCfgClamp((int)RmCfgNumber(val, false), 0, 255);
			else if (_stricmp(key, "StarPowerOnTilt") == 0)          g_rmCfg.starPowerTilt  = RmCfgBool(val);
			else if (_stricmp(key, "StarPowerOnJoystickClick") == 0) g_rmCfg.starPowerClick = RmCfgBool(val);
			else if (_stricmp(key, "SoloFlag") == 0)                 g_rmCfg.soloFlag       = RmCfgBool(val);
			else if (_stricmp(key, "InvertStrum") == 0)              g_rmCfg.invertStrum    = RmCfgBool(val);
			else if (_stricmp(key, "InvertWhammy") == 0)             g_rmCfg.invertWhammy   = RmCfgBool(val);
			else if (_stricmp(key, "PickupSwitch") == 0)             g_rmCfg.pickupSwitch   = RmCfgBool(val);
		}
		else if (_stricmp(section, "Joystick") == 0) {
			if      (_stricmp(key, "JoystickAsDpad") == 0)       g_rmCfg.joystickAsDpad       = RmCfgBool(val);
			else if (_stricmp(key, "JoystickDpadVertical") == 0) g_rmCfg.joystickDpadVertical = RmCfgBool(val);
			else if (_stricmp(key, "JoystickDeadzone") == 0)     g_rmCfg.joystickDeadzone     = RmCfgClamp((int)RmCfgNumber(val, false), 0, 32767);
		}
		else if (_stricmp(section, "Guide") == 0) {
			if      (_stricmp(key, "GuideButton") == 0)     g_rmCfg.guideButton     = RmCfgBool(val);
			else if (_stricmp(key, "GuideCooldownMs") == 0) g_rmCfg.guideCooldownMs = RmCfgClamp((int)RmCfgNumber(val, false), 100, 10000);
		}
		else if (_stricmp(section, "Subtypes") == 0) {
			if (_stricmp(key, "Default") == 0) {
				DWORD v = RmCfgNumber(val, false);
				if (v == 6 || v == 7) g_rmCfg.defaultSubType = (BYTE)v;
			}
			else if (g_rmCfg.overrideCount < RM_CFG_MAX_OVERRIDES) {
				// Key is a title ID. The log line prints it as hex and that is the
				// natural way to write it, so parse bare digits as hex here.
				DWORD id = RmCfgNumber(key, true);
				DWORD sub = RmCfgNumber(val, false);
				if (id != 0 && (sub == 6 || sub == 7)) {
					g_rmCfg.overrideId[g_rmCfg.overrideCount] = id;
					g_rmCfg.overrideSub[g_rmCfg.overrideCount] = (BYTE)sub;
					g_rmCfg.overrideCount++;
				}
			}
		}
	}

	fclose(f);

	// A release point at or above the engage point would defeat the hysteresis and let
	// star power latch on. Fix it rather than honouring a nonsensical pair.
	if (g_rmCfg.tiltRelease > g_rmCfg.tiltThreshold)
		g_rmCfg.tiltRelease = g_rmCfg.tiltThreshold;

	g_rmCfg.loadedFromFile = true;
	return true;
}

// Returns 0 if the user has no opinion about this title.
static BYTE RmCfgSubTypeOverride(DWORD titleId) {
	for (int i = 0; i < g_rmCfg.overrideCount; i++)
		if (g_rmCfg.overrideId[i] == titleId)
			return g_rmCfg.overrideSub[i];
	return 0;
}

#endif // RIFFMASTER_CONFIG_H
