$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $projectRoot 'source'
$main = Get-Content -LiteralPath (Join-Path $sourceRoot 'main.cpp') -Raw
$start = $main.IndexOf('static void XboxInputEnsureTitleUiWorker()')
$end = $main.IndexOf('HRESULT XamInputGetStateHook(', $start)
$ensure = $main.Substring($start, $end - $start)
$gate = $ensure.IndexOf('if (!XboxInputTitleUiWorkerAllowed(title, classifierAvailable, xbox1Title))')
$moduleGate = $ensure.IndexOf('if (!XboxInputTitleUiModuleAllowed(metadataAvailable, emulatorName, executableTitle))')
$titleCall = $ensure.IndexOf('DWORD title = ((xam_get_current_title_id_t)XamGetCurrentTitleIdPtr)();')
if ($moduleGate -lt 0 -or $titleCall -lt 0 -or $moduleGate -gt $titleCall -or
    $main -notmatch 'XexGetProcedureAddress\(kernelHandle, 403, \(PVOID\*\)&g_xboxInputExecutableModule\)' -or
    $main -notmatch 'XexGetProcedureAddress\(kernelHandle, 299, \(PVOID\*\)&g_xboxInputHeaderField\)' -or
    $main -notmatch 'event=title_ui_gate' -or
    $main -match 'if \(!g_xboxInput(ExecutableModule|HeaderField)\) missing') {
    throw 'Executable-module gate must precede XAM title lookup and remain optional for controller operation.'
}
if ($gate -lt 0 -or $gate -gt $ensure.IndexOf('InterlockedExchange(') -or
    $gate -gt $ensure.IndexOf('CreateThread(') -or
    $main -notmatch 'XexGetProcedureAddress\(xamHandle, 447, &g_xboxInputIsXbox1TitleId\)' -or
    $main -match 'if \(!g_xboxInputIsXbox1TitleId\) missing' -or
    $ensure -notmatch 'KeGetCurrentProcessType\(\) != PROC_USER') {
    throw 'Original Xbox guard must precede worker mutation/creation, with an optional resolved classifier and process guard.'
}
$hookStart = $main.IndexOf('HRESULT XamInputGetStateHook(')
$hookEnd = $main.IndexOf('NTSTATUS XInputdReadStateHook(', $hookStart)
$hook = $main.Substring($hookStart, $hookEnd - $hookStart)
if ($hook -notmatch 'XboxInputEnsureTitleUiWorker\(\);' -or
    $hook -notmatch 'GetOriginal<decltype\(&XamInputGetStateHook\)>\(\)\(user, deviceContext, state\)') {
    throw 'Native title-worker startup or stock input forwarding was lost.'
}
# The self-contained scope fixture below verifies these input/output files,
# dependencies and all main.cpp code outside the optional UI section.
Write-Host 'PASS: OG guard precedes thread startup/state mutation; native startup and stock forwarding retained'
Write-Host 'PASS: executable-module guard precedes XAM lookup; optional metadata exports and bounded decision telemetry present'
& (Join-Path $PSScriptRoot 'check_early_ui_bypass.ps1')
if (-not $?) { throw 'Early worker/cleanup bypass verification failed.' }
