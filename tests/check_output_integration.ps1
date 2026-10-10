$ErrorActionPreference = 'Stop'
$sourceRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'source'
$main = Get-Content -LiteralPath (Join-Path $sourceRoot 'main.cpp') -Raw
$transport = Get-Content -LiteralPath (Join-Path $sourceRoot 'gip_output_transport.inl') -Raw
$policy = Get-Content -LiteralPath (Join-Path $sourceRoot 'gip_output_policy.h') -Raw
if ($policy -notmatch '#define XBOXINPUT_SUPPRESS_RUNTIME_RUMBLE 0' -or
    $policy -notmatch '#define XBOXINPUT_FORCE_ZERO_WIRE_RUMBLE 0' -or
    $transport -notmatch 'Prepare\(now, XboxInputAllowRuntimeRumble\(XboxInputRuntimeIsReady\(&session->runtime\)\)\)') {
    throw 'Test26 must restore actual runtime motor strengths without suppressing output.'
}
if ($main -notmatch '#include "gip_output_policy.h"' -or
    $main -notmatch 'diagnostics=og-test31-early-emulator-bypass titleUiWorkerStartup=early-name-gated workerAndCleanupGate=1 liveConfigReload=disabled' -or
    $main -notmatch 'runtime_rumble_requests' -or
    $main -notmatch 'wireMotors=' -or
    $transport -notmatch 'session->output.RequestRumble\(left, right\)') {
    throw 'Test26 policy, identity, request retention or wire telemetry missing.'
}
$policyCall = $transport.IndexOf('XboxInputApplyRuntimeOutputPolicy(&session->output.active)')
$prepareTrb = $transport.IndexOf('session->outTrb.buffer =')
if ($policyCall -lt 0 -or $policyCall -gt $prepareTrb -or
    [regex]::Matches($transport, 'XboxInputApplyRuntimeOutputPolicy\(').Count -ne 1 -or
    $policy -notmatch 'if \(packet->rumble && packet->length == 13\)') {
    throw 'Wire policy must apply only to runtime packets before submission.'
}
if ($transport -notmatch '\+\+session->runtimeRumbleRequests' -or
    $transport -match '#if XBOXINPUT_SUPPRESS_RUNTIME_RUMBLE \|\| XBOXINPUT_FORCE_ZERO_WIRE_RUMBLE') {
    throw 'Test26 must retain the same bounded request telemetry independently of motor-strength policy.'
}
Write-Host 'PASS: Test26 restores actual motor strengths while retaining startup, logical state and request telemetry'
if ($policy -notmatch '#define XBOXINPUT_DETAILED_OUTPUT_TRACE 0' -or
    $transport -notmatch 'if \(!XboxInputPublishOutputTrace\(phase, a, b, c\)\) return;' -or
    $main -notmatch 'detailedOutputTrace=%u') {
    throw 'Test26 must retain reduced successful per-transfer tracing.'
}
Write-Host 'PASS: reduced trace policy and build identity present'
if ($main -match 'UsbdQueueAsyncTransfer\([^;]*[Oo]utTrb' -or
    $main -match 'SendInterruptRequest\([^;]*[Oo]utTrb' -or
    $main -match '(g_gipOutTrb|session->outTrb)\.(buffer|length|callback|flags)\s*=') {
    throw 'A GIP output path bypasses the single-owner transport.'
}
if ([regex]::Matches($transport, 'UsbdQueueAsyncTransfer\(').Count -ne 1) {
    throw 'Expected exactly one transport submission site.'
}
$rumbleStart = $transport.IndexOf('static int GipSessionSendRumble(')
$rumbleEnd = $transport.IndexOf('static int32_t GipOutputComplete(', $rumbleStart)
$rumble = $transport.Substring($rumbleStart, $rumbleEnd - $rumbleStart)
if ($rumble -match 'Sleep\(|UsbdQueueAsyncTransfer\(|SendInterruptRequest\(') {
    throw 'XAM rumble producer blocks or submits directly.'
}
$callbackStart = $transport.IndexOf('static int32_t GipOutputComplete(')
$callbackEnd = $transport.IndexOf('static void GipSubmitOutputOnUsbDpc(', $callbackStart)
$callback = $transport.Substring($callbackStart, $callbackEnd - $callbackStart)
if ($callback -match 'Sleep\(|UsbdQueueAsyncTransfer\(|GipOutputLog\(|GipEnqueue|KfAcquireSpinLock\(|KfReleaseSpinLock\(') {
    throw 'Completion callback does more than publish completion.'
}
if ($callback -notmatch 'outputCompletionStatus' -or $callback -notmatch 'outputCompletionPending') {
    throw 'Completion callback must publish through the session mailbox.'
}
$queueCall = $transport.IndexOf('UsbdQueueAsyncTransfer(handle, &session->outTrb)')
$lastLock = $transport.LastIndexOf('KfAcquireSpinLock(&session->outputLock)', $queueCall)
$lastUnlock = $transport.LastIndexOf('KfReleaseSpinLock(&session->outputLock, irql)', $queueCall)
if ($queueCall -lt 0 -or $lastLock -lt 0 -or $lastUnlock -lt $lastLock) {
    throw 'Output submission must release outputLock before entering the USB stack.'
}
$setStateStart = $main.IndexOf('DWORD XamInputSetStateHook(')
$setStateEnd = $main.IndexOf('DWORD XamInputGetCapabilitiesExHook(', $setStateStart)
$setState = $main.Substring($setStateStart, $setStateEnd - $setStateStart)
if ($setState.IndexOf('GipControllerFromUser(') -gt $setState.IndexOf('GetOriginal<decltype(&XamInputSetStateHook)>()')) {
    throw 'Virtual controller rumble must be routed before the stock XAM path.'
}
if ([regex]::Matches($main, 'session->output.Initialize\(\)').Count -ne 2) {
    throw 'Both primary and additional sessions must initialize output ownership.'
}
if ($callback -match 'fopen|fprintf|fflush|fclose|DbgPrint|malloc|new ') {
    throw 'Completion diagnostics must not allocate, format, write files or debug-print.'
}
$traceStart = $transport.IndexOf('static void GipOutputTraceLog(')
$traceEnd = $transport.IndexOf('static void GipStopOutput(', $traceStart)
$trace = $transport.Substring($traceStart, $traceEnd - $traceStart)
if ($trace -notmatch 'XboxInputQueueLogEventEx' -or
    $trace -match 'fopen|fprintf|fflush|fclose|DbgPrint|Sleep\(|malloc|new |KfAcquireSpinLock') {
    throw 'Trace helper must only publish a bounded event.'
}
if ($callback.IndexOf('const DWORD transfer = session->outputTraceTransfer') -gt
    $callback.IndexOf('InterlockedCompareExchange(&session->outputCompletionPending, 1, 0)')) {
    throw 'Callback must capture transfer identity before publishing completion.'
}
if ($main -notmatch 'GipLogOutputDiagnostics\(GetTickCount\(\)\)' -or
    $main -notmatch 'case XBOXINPUT_LOG_OUTPUT_TRACE:') {
    throw 'Independent logger heartbeat or trace formatter missing.'
}
Write-Host 'PASS: single GIP OUT submission site, unlocked USB submit, nonblocking XAM producer, bounded callback trace, pre-publication identity, independent heartbeat'

$dpcStart = $transport.IndexOf('static void GipSubmitOutputOnUsbDpc(')
$workerStart = $transport.IndexOf('static DWORD WINAPI GipOutputWorker(', $dpcStart)
$workerEnd = $transport.IndexOf('static void GipLogOutputDiagnostics(', $workerStart)
$dpc = $transport.Substring($dpcStart, $workerStart - $dpcStart)
$worker = $transport.Substring($workerStart, $workerEnd - $workerStart)
if ($policy -notmatch '#define XBOXINPUT_USB_DPC_SUBMIT 1' -or
    $policy -notmatch 'XBOXINPUT_USB_DPC_PROCESSOR = 2' -or
    $main -notmatch 'XexGetProcedureAddress\(kernelHandle, 886, &KeCallAndWaitForDpcRoutinePtr\)' -or
    $main -notmatch 'if \(!KeCallAndWaitForDpcRoutinePtr\) missing \|= 0x00010000' -or
    $main -notmatch 'dpc_handoff_failed' -or
    $worker -notmatch 'KeCallAndWaitForDpcRoutinePtr\(GipSubmitOutputOnUsbDpc, session,' -or
    $worker -notmatch 'if \(handoffStatus < 0\)' -or
    $worker -notmatch 'session->output.AbortUnsubmitted\(\)' -or
    $worker -match 'int32_t token = UsbdQueueAsyncTransfer' -or
    $dpc -notmatch 'int32_t token = UsbdQueueAsyncTransfer' -or
    $dpc -notmatch '!session->runtime.stopping' -or
    $dpc -match 'Sleep\(|fopen|fprintf|fflush|fclose|malloc|new |KeCallAndWaitForDpcRoutinePtr\(') {
    throw 'DPC submission, context revalidation or fail-closed handoff integration missing.'
}
$handoff = $worker.IndexOf('KeCallAndWaitForDpcRoutinePtr(GipSubmitOutputOnUsbDpc, session,')
if ($worker.LastIndexOf('KfReleaseSpinLock(&session->outputLock, irql)', $handoff) -lt
    $worker.LastIndexOf('KfAcquireSpinLock(&session->outputLock)', $handoff)) {
    throw 'The worker must release its lock before waiting for the DPC routine.'
}
Write-Host 'PASS: ordinal-886 processor-2 handoff, unlocked passive wait, bounded DPC submit, removal revalidation and fail-closed error path'

if ([regex]::Matches($main, 'GipSessionCanRegisterFromInput\(').Count -ne 2) {
    throw 'Both primary and additional INPUT paths must use tested readiness policy.'
}
$setupStart = $transport.IndexOf('static void GipEstablishStoppedRumble(')
$setupEnd = $transport.IndexOf('static void GipBeginStartup(', $setupStart)
$setup = $transport.Substring($setupStart, $setupEnd - $setupStart)
if ($setup -notmatch 'inputReadiness\.CompleteStartup\(') {
    throw 'Successful startup setup must publish readiness independent of IDENTIFY.'
}
if ($main -match 'startupComplete\s*=' -or $main -match '->startupComplete') {
    throw 'A main.cpp path bypasses the tested readiness policy.'
}
if ($main -notmatch '(?s)if \(stage == GIP_PRE_READ_INIT_POWER\)\s*GipBeginStartup\(session\);') {
    throw 'Restarting startup must clear the registration gate.'
}
Write-Host 'PASS: both INPUT paths use readiness policy; completed setup opens gate and restarted setup clears it'
