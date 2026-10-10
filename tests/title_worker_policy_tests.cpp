#include <assert.h>
#include <stdio.h>
#include "../source/title_worker_policy.h"

int main() {
    assert(!XboxInputTitleUiModuleAllowed(false, false, 0x4D530910u));
    assert(!XboxInputTitleUiModuleAllowed(true, false, 0));
    assert(!XboxInputTitleUiModuleAllowed(true, false, 0xFFFE07D2u));
    assert(!XboxInputTitleUiModuleAllowed(true, true, 0xFFFE07D1u)); // Spoofed title, emulator name
    assert(XboxInputTitleUiModuleAllowed(true, false, 0x4D530910u));
    assert(XboxInputTitleUiModuleAllowed(true, false, 0xFFFE07D1u));
    assert(XboxInputIsEmulatorModuleName(L"xbox.xex", 8));
    assert(XboxInputIsEmulatorModuleName(L"XeFu7.XEX", 9));
    assert(XboxInputIsEmulatorModuleName(L"xefu1_1.xex", 11));
    assert(XboxInputIsEmulatorModuleName(L"xefu2021c.xex", 13));
    assert(XboxInputIsEmulatorModuleName(L"HddX:\\Compatibility\\xefu.xex", 28));
    assert(!XboxInputIsEmulatorModuleName(L"default.xex", 11));
    assert(!XboxInputIsEmulatorModuleName(L"Aurora.xex", 10));
    assert(!XboxInputIsEmulatorModuleName(L"xefu.xex", 3));
    assert(!XboxInputIsEmulatorModuleName(L"xefu.xex", 257));
    const unsigned short countedName[] = { 'x','e','f','u','.','x','e','x' };
    assert(XboxInputIsEmulatorModuleName(countedName, 8));
    assert(!XboxInputTitleUiNativeModuleNameAllowed(countedName, 8));
    assert(!XboxInputTitleUiNativeModuleNameAllowed(L"xefu.xex", 8));
    assert(!XboxInputTitleUiNativeModuleNameAllowed(L"xbox.xex", 8));
    assert(!XboxInputTitleUiNativeModuleNameAllowed(L"XeFu7.XEX", 9));
    assert(!XboxInputTitleUiNativeModuleNameAllowed(L"HddX:\\Compatibility\\xefu.xex", 28));
    assert(!XboxInputTitleUiNativeModuleNameAllowed((const unsigned short*)0, 8));
    assert(!XboxInputTitleUiNativeModuleNameAllowed(L"Aurora.xex", 0));
    assert(!XboxInputTitleUiNativeModuleNameAllowed(L"Aurora.xex", 257));
    assert(XboxInputTitleUiNativeModuleNameAllowed(L"Aurora.xex", 10));
    assert(XboxInputTitleUiNativeModuleNameAllowed(L"default.xex", 11));
    assert(XboxInputTitleUiCleanupDelay(0) == 50);
    assert(XboxInputTitleUiCleanupDelay(8950) == 50);
    assert(XboxInputTitleUiCleanupDelay(8951) == 49);
    assert(XboxInputTitleUiCleanupDelay(8999) == 1);
    assert(XboxInputTitleUiCleanupDelay(9000) == 0);
    assert(XboxInputTitleUiCleanupDelay(10000) == 0);
    assert(XboxInputTitleUiCleanupDelay(0xFFFFFFFFu) == 0);
    puts("PASS: API-free early name policy; unknown/emulator contexts excluded; cleanup sleeps <=50ms until 9s");
    puts("PASS: counted Unicode emulator names and metadata gate; native names and spoofed title cases");
    // Classifier responses are mocked here; hardware must verify XAM's results.
    assert(!XboxInputTitleUiWorkerAllowed(0x4D530004u, true, true)); // Halo CE
    assert(!XboxInputTitleUiWorkerAllowed(0x4D530064u, true, true)); // Halo 2
    assert(!XboxInputTitleUiWorkerAllowed(0x5345002Bu, true, true)); // Sonic Heroes
    assert(!XboxInputTitleUiWorkerAllowed(0x12345678u, true, true)); // Any OG ID
    assert(!XboxInputTitleUiWorkerAllowed(0xFFFE07D2u, true, false)); // Launcher
    assert(!XboxInputTitleUiWorkerAllowed(0, true, false)); // Transition
    assert(!XboxInputTitleUiWorkerAllowed(0x4D530910u, false, false)); // Missing export
    assert(XboxInputTitleUiWorkerAllowed(0x4D530910u, true, false)); // Native title
    assert(XboxInputTitleUiWorkerAllowed(0xFFFE07D1u, true, false)); // Dashboard
    assert(XboxInputTitleUiWorkerAllowed(0xF0000001u, true, false)); // Homebrew
    puts("PASS: OG/launcher/transition excluded; native/dashboard allowed; missing classifier fails closed for UI only");
    return 0;
}
