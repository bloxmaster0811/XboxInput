#ifndef XBOXINPUT_CONFIG_H
#define XBOXINPUT_CONFIG_H

#include <xtl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gamepad_mapping.h"

#define XBOXINPUT_CFG_FILENAME        "XboxInput.ini"
#define XBOXINPUT_RELOAD_FILENAME     "XboxInput.reload"
#define XBOXINPUT_LOG_FILENAME        "XboxInputGip.log"
#define XBOXINPUT_PROBE_FILENAME      "XboxInputCompatProbe.log"
#define XBOXINPUT_PRIVATE_DRIVE       "XboxInputCfg:"

static char g_xboxInputConfigPath[MAX_PATH] = "HDD:\\XboxInput.ini";
static char g_xboxInputReloadPath[MAX_PATH] = "HDD:\\XboxInput.reload";
static char g_xboxInputLogPath[MAX_PATH] = "HDD:\\XboxInputGip.log";
static char g_xboxInputProbePath[MAX_PATH] = "HDD:\\XboxInputCompatProbe.log";
static DWORD g_xboxInputPrivateMountStatus = (DWORD)-1;
static bool g_xboxInputUsingPrivateMount = false;

#define XBOXINPUT_CFG_PATH    g_xboxInputConfigPath
#define XBOXINPUT_RELOAD_PATH g_xboxInputReloadPath
#define XBOXINPUT_LOG_PATH    g_xboxInputLogPath
#define XBOXINPUT_PROBE_PATH  g_xboxInputProbePath

struct XboxInputConfig {
	bool guideButton;
	int guideCooldownMs;
	bool loadedFromFile;
};

static XboxInputConfig g_xboxInputConfig = { true, 1000, false };

static void XboxInputConfigTrim(char* text) {
	if (!text) return;
	char* first = text;
	while (*first == ' ' || *first == '\t') ++first;
	if (first != text) memmove(text, first, strlen(first) + 1);
	int length = (int)strlen(text);
	while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
		text[length - 1] == '\r' || text[length - 1] == '\n'))
		text[--length] = 0;
}

static DWORD XboxInputConfigNumber(const char* value) {
	if (!value || !*value) return 0;
	if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
		return (DWORD)strtoul(value + 2, 0, 16);
	return (DWORD)strtoul(value, 0, 10);
}

static bool XboxInputConfigBool(const char* value) {
	if (!value || !*value) return false;
	return strcmp(value, "1") == 0 || _stricmp(value, "true") == 0 ||
		_stricmp(value, "yes") == 0 || _stricmp(value, "on") == 0;
}

static int XboxInputConfigClamp(int value, int minimum, int maximum) {
	return value < minimum ? minimum : (value > maximum ? maximum : value);
}

static void XboxInputApplyConfigMapping(XboxInputMappingOptions* mapping,
	const char* key, const char* value) {
	if (!mapping || !key || !value) return;
	if      (_stricmp(key, "InvertLeftX") == 0)  mapping->invertLeftX = XboxInputConfigBool(value);
	else if (_stricmp(key, "InvertLeftY") == 0)  mapping->invertLeftY = XboxInputConfigBool(value);
	else if (_stricmp(key, "InvertRightX") == 0) mapping->invertRightX = XboxInputConfigBool(value);
	else if (_stricmp(key, "InvertRightY") == 0) mapping->invertRightY = XboxInputConfigBool(value);
	else if (_stricmp(key, "SwapSticks") == 0)   mapping->swapSticks = XboxInputConfigBool(value);
	else if (_stricmp(key, "SwapTriggers") == 0) mapping->swapTriggers = XboxInputConfigBool(value);
	else if (_stricmp(key, "LeftStickDeadzone") == 0) mapping->leftStickDeadzone = (uint16_t)XboxInputConfigClamp((int)XboxInputConfigNumber(value), 0, 32767);
	else if (_stricmp(key, "RightStickDeadzone") == 0) mapping->rightStickDeadzone = (uint16_t)XboxInputConfigClamp((int)XboxInputConfigNumber(value), 0, 32767);
	else if (_stricmp(key, "LeftTriggerDeadzone") == 0) mapping->leftTriggerDeadzone = (uint8_t)XboxInputConfigClamp((int)XboxInputConfigNumber(value), 0, 254);
	else if (_stricmp(key, "RightTriggerDeadzone") == 0) mapping->rightTriggerDeadzone = (uint8_t)XboxInputConfigClamp((int)XboxInputConfigNumber(value), 0, 254);
	else if (_stricmp(key, "RumblePercent") == 0) mapping->rumblePercent = (uint8_t)XboxInputConfigClamp((int)XboxInputConfigNumber(value), 0, 100);
	else XboxInputSetButtonMappingByName(mapping, key, value);
}

static bool XboxInputWriteDefaultConfig(const char* path) {
	FILE* file = fopen(path, "w");
	if (!file) return false;
	fputs(
		"; XboxInput.ini - optional XboxInput settings\r\n"
		"; Changes take effect after a hard reboot.\r\n"
		"; Booleans accept 1/0, true/false, yes/no, or on/off.\r\n\r\n"
		"[Guide]\r\n"
		"GuideButton = 1\r\n"
		"GuideCooldownMs = 1000\r\n\r\n"
		"[GamepadMapping]\r\n"
		"A = A\r\nB = B\r\nX = X\r\nY = Y\r\n"
		"DpadUp = DpadUp\r\nDpadDown = DpadDown\r\n"
		"DpadLeft = DpadLeft\r\nDpadRight = DpadRight\r\n"
		"Start = Start\r\nBack = Back\r\n"
		"LeftThumb = LeftThumb\r\nRightThumb = RightThumb\r\n"
		"LeftShoulder = LeftShoulder\r\nRightShoulder = RightShoulder\r\n"
		"InvertLeftX = 0\r\nInvertLeftY = 0\r\n"
		"InvertRightX = 0\r\nInvertRightY = 0\r\n"
		"SwapSticks = 0\r\nSwapTriggers = 0\r\n"
		"LeftStickDeadzone = 0\r\nRightStickDeadzone = 0\r\n"
		"LeftTriggerDeadzone = 0\r\nRightTriggerDeadzone = 0\r\n"
		"RumblePercent = 100\r\n",
		file);
	return fclose(file) == 0;
}

static bool XboxInputConfigFileExists(const char* path) {
	FILE* file = path ? fopen(path, "r") : 0;
	if (!file) return false;
	fclose(file);
	return true;
}

static bool XboxInputSetSiblingPath(char* destination, size_t capacity,
	const char* directory, const char* filename) {
	if (!destination || !directory || !directory[0] || !filename) return false;
	size_t directoryLength = strlen(directory);
	bool needsSlash = directory[directoryLength - 1] != '\\' &&
		directory[directoryLength - 1] != '/';
	size_t prefixLength = directoryLength + (needsSlash ? 1 : 0);
	if (prefixLength + strlen(filename) + 1 > capacity) return false;
	memcpy(destination, directory, directoryLength);
	if (needsSlash) destination[directoryLength] = '\\';
	strcpy(destination + prefixLength, filename);
	return true;
}

static bool XboxInputSetConfigPaths(const char* directory) {
	char config[MAX_PATH], reload[MAX_PATH], log[MAX_PATH], probe[MAX_PATH];
	if (!XboxInputSetSiblingPath(config, sizeof(config), directory, XBOXINPUT_CFG_FILENAME) ||
		!XboxInputSetSiblingPath(reload, sizeof(reload), directory, XBOXINPUT_RELOAD_FILENAME) ||
		!XboxInputSetSiblingPath(log, sizeof(log), directory, XBOXINPUT_LOG_FILENAME) ||
		!XboxInputSetSiblingPath(probe, sizeof(probe), directory, XBOXINPUT_PROBE_FILENAME))
		return false;
	strcpy(g_xboxInputConfigPath, config);
	strcpy(g_xboxInputReloadPath, reload);
	strcpy(g_xboxInputLogPath, log);
	strcpy(g_xboxInputProbePath, probe);
	return true;
}

static bool XboxInputGetModuleDirectory(PVOID moduleAddress, char* directory,
	size_t capacity) {
	if (!moduleAddress || !directory || capacity < 4) return false;
	PLDR_DATA_TABLE_ENTRY module = 0;
	XexPcToFileHeader(moduleAddress, &module);
	if (!module || !module->FullDllName.Buffer || module->FullDllName.Length == 0)
		return false;
	size_t count = module->FullDllName.Length / sizeof(WCHAR);
	if (count >= capacity) count = capacity - 1;
	for (size_t i = 0; i < count; ++i) {
		WCHAR character = module->FullDllName.Buffer[i];
		if (character > 0x7F) return false;
		directory[i] = (char)character;
	}
	directory[count] = 0;
	int separator = -1;
	for (int i = (int)count - 1; i >= 0; --i) {
		if (directory[i] == '\\' || directory[i] == '/') {
			separator = i;
			break;
		}
	}
	if (separator >= 0) directory[separator + 1] = 0;
	else {
		char* colon = strchr(directory, ':');
		if (!colon) return false;
		colon[1] = 0;
	}
	return directory[0] != 0;
}

static bool XboxInputMountNativeDirectory(const char* nativeDirectory) {
	if (!nativeDirectory || strncmp(nativeDirectory, "\\Device\\", 8) != 0)
		return false;
	CHAR linkBuffer[MAX_PATH] = { 0 };
	sprintf_s(linkBuffer, sizeof(linkBuffer),
		(KeGetCurrentProcessType() == PROC_SYSTEM) ? OBJ_SYS_STRING : OBJ_USR_STRING,
		XBOXINPUT_PRIVATE_DRIVE);
	ANSI_STRING linkName, deviceName;
	RtlInitAnsiString(&linkName, linkBuffer);
	RtlInitAnsiString(&deviceName, (PCHAR)nativeDirectory);
	ObDeleteSymbolicLink(&linkName);
	g_xboxInputPrivateMountStatus = (DWORD)ObCreateSymbolicLink(&linkName, &deviceName);
	g_xboxInputUsingPrivateMount = g_xboxInputPrivateMountStatus == 0;
	return g_xboxInputUsingPrivateMount;
}

static bool XboxInputSelectModuleDirectory(PVOID moduleAddress) {
	char moduleDirectory[MAX_PATH];
	if (!XboxInputGetModuleDirectory(moduleAddress, moduleDirectory,
		sizeof(moduleDirectory))) return false;
	if (strncmp(moduleDirectory, "\\Device\\", 8) == 0) {
		if (!XboxInputMountNativeDirectory(moduleDirectory)) return false;
		return XboxInputSetConfigPaths(XBOXINPUT_PRIVATE_DRIVE);
	}
	g_xboxInputPrivateMountStatus = 0;
	g_xboxInputUsingPrivateMount = false;
	return XboxInputSetConfigPaths(moduleDirectory);
}

static bool XboxInputSelectConfigPath(PVOID moduleAddress, bool* existed) {
	if (existed) *existed = false;
	if (XboxInputSelectModuleDirectory(moduleAddress)) {
		if (XboxInputConfigFileExists(g_xboxInputConfigPath)) {
			if (existed) *existed = true;
			return true;
		}
		if (XboxInputWriteDefaultConfig(g_xboxInputConfigPath)) return true;
	}
	static const char* fallbackRoots[] = {
		"HDD:", "Usb:", "Usb0:", "Usb1:", "Mu0:", "Mu1:", "UsbMu:"
	};
	for (DWORD i = 0; i < sizeof(fallbackRoots) / sizeof(fallbackRoots[0]); ++i) {
		if (!XboxInputSetConfigPaths(fallbackRoots[i])) continue;
		if (XboxInputConfigFileExists(g_xboxInputConfigPath)) {
			if (existed) *existed = true;
			return true;
		}
	}
	for (DWORD i = 0; i < sizeof(fallbackRoots) / sizeof(fallbackRoots[0]); ++i) {
		if (XboxInputSetConfigPaths(fallbackRoots[i]) &&
			XboxInputWriteDefaultConfig(g_xboxInputConfigPath)) return true;
	}
	XboxInputSetConfigPaths("HDD:");
	return false;
}

static bool XboxInputParseConfig(const char* path,
	XboxInputMappingOptions* mapping, bool includeGuide) {
	if (!path || !mapping) return false;
	FILE* file = fopen(path, "r");
	if (!file) return false;
	XboxInputSetDefaultMapping(mapping);
	char line[256], section[32];
	section[0] = 0;
	while (fgets(line, sizeof(line), file)) {
		XboxInputConfigTrim(line);
		if (!line[0] || line[0] == ';' || line[0] == '#') continue;
		if (line[0] == '[') {
			char* end = strchr(line, ']');
			if (!end) continue;
			*end = 0;
			strncpy(section, line + 1, sizeof(section) - 1);
			section[sizeof(section) - 1] = 0;
			continue;
		}
		char* equals = strchr(line, '=');
		if (!equals) continue;
		*equals = 0;
		char* key = line;
		char* value = equals + 1;
		XboxInputConfigTrim(key);
		XboxInputConfigTrim(value);
		if (_stricmp(section, "GamepadMapping") == 0)
			XboxInputApplyConfigMapping(mapping, key, value);
		else if (includeGuide && _stricmp(section, "Guide") == 0) {
			if (_stricmp(key, "GuideButton") == 0)
				g_xboxInputConfig.guideButton = XboxInputConfigBool(value);
			else if (_stricmp(key, "GuideCooldownMs") == 0)
				g_xboxInputConfig.guideCooldownMs = XboxInputConfigClamp(
					(int)XboxInputConfigNumber(value), 100, 10000);
		}
	}
	fclose(file);
	return true;
}

static bool XboxInputLoadConfig(const char* path) {
	bool loaded = XboxInputParseConfig(path, &g_xboxInputGamepadMapping, true);
	g_xboxInputConfig.loadedFromFile = loaded;
	return loaded;
}

static bool XboxInputLoadGamepadMapping(const char* path,
	XboxInputMappingOptions* mapping) {
	return XboxInputParseConfig(path, mapping, false);
}

#endif
