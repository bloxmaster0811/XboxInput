#ifndef XBOXINPUT_TITLE_WORKER_POLICY_H
#define XBOXINPUT_TITLE_WORKER_POLICY_H

static bool XboxInputTitleUiModuleAllowed(bool metadataAvailable,
    bool emulatorName, unsigned int executableTitle) {
    return metadataAvailable && !emulatorName && executableTitle != 0 &&
        executableTitle != 0xFFFE07D2u;
}

static unsigned int XboxInputModuleLower(unsigned int ch) {
    return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}

// Counted Unicode name, no terminator required and no allocation or loader call.
template <typename CharT>
static bool XboxInputIsEmulatorModuleName(const CharT* name, unsigned int count) {
    if (!name || count == 0 || count > 256) return false;
    unsigned int start = 0;
    for (unsigned int i = 0; i < count; ++i)
        if (name[i] == '/' || name[i] == '\\') start = i + 1;
    const unsigned int length = count - start;
    if (length < 8) return false;
    const CharT* leaf = name + start;
    if (leaf[length - 4] != '.' ||
        XboxInputModuleLower(leaf[length - 3]) != 'x' ||
        XboxInputModuleLower(leaf[length - 2]) != 'e' ||
        XboxInputModuleLower(leaf[length - 1]) != 'x') return false;
    const bool xbox = length == 8 &&
        XboxInputModuleLower(leaf[0]) == 'x' &&
        XboxInputModuleLower(leaf[1]) == 'b' &&
        XboxInputModuleLower(leaf[2]) == 'o' &&
        XboxInputModuleLower(leaf[3]) == 'x';
    const bool xefu = XboxInputModuleLower(leaf[0]) == 'x' &&
        XboxInputModuleLower(leaf[1]) == 'e' &&
        XboxInputModuleLower(leaf[2]) == 'f' &&
        XboxInputModuleLower(leaf[3]) == 'u';
    return xbox || xefu;
}

template <typename CharT>
static bool XboxInputTitleUiNativeModuleNameAllowed(const CharT* name,
    unsigned int count) {
    return name && count != 0 && count <= 256 &&
        !XboxInputIsEmulatorModuleName(name, count);
}

// Exit cleanup checks the module between sleeps; never sleep through the whole
// title handover before attempting to restore notification options.
static unsigned int XboxInputTitleUiCleanupDelay(unsigned int elapsed) {
    if (elapsed >= 9000) return 0;
    const unsigned int remaining = 9000 - elapsed;
    return remaining < 50 ? remaining : 50;
}

// XAM classifies OG game IDs; also exclude the backward-compatibility launcher
// and an unknown/transitional title. No title-owned thread may start there.
// If the classifier is unavailable, disable only this optional UI facility.
static bool XboxInputTitleUiWorkerAllowed(unsigned int title,
    bool classifierAvailable, bool xbox1Title) {
    return classifierAvailable && title != 0 && title != 0xFFFE07D2u &&
        !xbox1Title;
}

#endif
