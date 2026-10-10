$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $projectRoot 'source'
$baseline = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'fixtures\ui_scope_baseline.json') -Raw | ConvertFrom-Json
if ($baseline.formatVersion -ne 1) { throw 'Unsupported UI scope fixture format.' }
function Get-XboxInputTextHash([string]$text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)))).Replace('-', '')
    }
    finally { $sha.Dispose() }
}
$main = [IO.File]::ReadAllText((Join-Path $sourceRoot 'main.cpp')).Replace("`r`n", "`n")
$mainStart = $main.IndexOf('// Deliberately API-free:')
$mainEnd = $main.IndexOf('// Lazily establish a title-owned watcher', $mainStart)
if ($mainStart -lt 0 -or $mainEnd -le $mainStart) {
    throw 'Expected UI section boundaries not found.'
}
if ((Get-XboxInputTextHash ($main.Remove($mainStart, $mainEnd - $mainStart))) -ne $baseline.outsideUiSha256) {
    throw 'Change outside the optional UI section and log identity: input/USB/rumble must remain unchanged.'
}
$helperStart = $main.IndexOf('static bool XboxInputReadTitleUiModuleName(')
$workerStart = $main.IndexOf('static DWORD XboxInputTitleUiWorker(', $helperStart)
$helper = $main.Substring($helperStart, $workerStart - $helperStart)
if ($helper -match 'KeGetCurrentProcessType\(|g_xboxInputHeaderField\(|XamGetCurrentTitleIdPtr|CreateThread\(|XboxInputQueueLog|XboxInputLogUiGate\(|GetTickCount\(|XNotify|Sleep\(|fopen\(' -or
    $helper -notmatch 'BaseDllName.Length > 512' -or
    $helper -notmatch 'XboxInputTitleUiNativeModuleNameAllowed\(name, count\)' -or
    $helper -notmatch 'BaseDllName.Length & 1') {
    throw 'Early name helper must use bounded counted-name reads only, no context/header/notification/thread/log/file calls.'
}
$ensureStart = $main.IndexOf('static void XboxInputEnsureTitleUiWorker()')
$ensureEnd = $main.IndexOf('// Lazily establish a title-owned watcher', $ensureStart)
$ensure = $main.Substring($ensureStart, $ensureEnd - $ensureStart)
$early = $ensure.IndexOf('if (!XboxInputReadTitleUiModuleName(&module, &namePrefix))')
foreach ($operation in @('KeGetCurrentProcessType(', 'g_xboxInputHeaderField(', 'XamGetCurrentTitleIdPtr)()', 'XboxInputLogUiGate(', 'InterlockedExchange(', 'CreateThread(')) {
    $index = $ensure.IndexOf($operation)
    if ($early -lt 0 -or $index -lt 0 -or $index -le $early) { throw "Name exclusion must precede $operation" }
}
$workerEnd = $main.IndexOf('static void XboxInputLogUiGate(', $workerStart)
$worker = $main.Substring($workerStart, $workerEnd - $workerStart)
$loop = $worker.IndexOf('for (;;)')
$earlyWorker = $worker.IndexOf('!XboxInputReadTitleUiModuleName(nullptr, nullptr)', $loop)
$titleWorker = $worker.IndexOf('XamGetCurrentTitleIdPtr)()', $loop)
if ($earlyWorker -lt 0 -or $titleWorker -le $earlyWorker -or
    $worker -notmatch 'if \(notificationActive && XboxInputReadTitleUiModuleName\(nullptr, nullptr\)\)' -or
    $worker -notmatch 'while \(XboxInputReadTitleUiModuleName\(nullptr, nullptr\)\)' -or
    $worker -notmatch 'XboxInputTitleUiCleanupDelay\(elapsed\)' -or
    $worker -match 'Sleep\(9000 - elapsed\)|fopen|XboxInputLoadGamepadMapping\(') {
    throw 'Worker and exit cleanup must recheck emulator exclusion; no nine-second unguarded sleep or file polling.'
}
# Preserve the complete validated active-notification body and its timing.
$activeStart = $worker.IndexOf("`t`tpendingRemoved |=")
$activeEnd = $worker.IndexOf("`t`tSleep(50);", $activeStart)
if ($activeStart -lt 0 -or $activeEnd -le $activeStart -or
    (Get-XboxInputTextHash ($worker.Substring($activeStart, $activeEnd-$activeStart))) -ne $baseline.nativeNotificationSha256) {
    throw 'Active native notification logic/timing changed outside the promised scope.'
}
foreach ($file in $baseline.files) {
    $target = Join-Path $sourceRoot $file.path
    if (-not (Test-Path -LiteralPath $target)) { throw "Missing source/dependency: $($file.path)" }
    if ($file.kind -eq 'binary') {
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash
    }
    elseif ($file.kind -eq 'text-lf') {
        $actual = Get-XboxInputTextHash ([IO.File]::ReadAllText($target).Replace("`r`n", "`n"))
    }
    else { throw "Unknown scope fixture encoding: $($file.kind)" }
    if ($actual -ne $file.sha256) {
        throw "Source/dependency changed outside UI gate scope: $($file.path)"
    }
}
Write-Host 'PASS: known emulator bypass precedes ALL setup helpers; worker/cleanup gated; active native notification body unchanged'
Write-Host 'PASS: all main.cpp code outside optional UI/identity and all other input/USB/rumble/dependency files match Test30'
