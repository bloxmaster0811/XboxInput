[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$CompilerPath)
$ErrorActionPreference = 'Stop'
$testRoot = $PSScriptRoot
$sourceRoot = Join-Path (Split-Path -Parent $testRoot) 'source'
$outputRoot = Join-Path $testRoot 'out'
if (-not (Test-Path -LiteralPath $CompilerPath)) { throw 'Compiler not found.' }
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
# These POD-only tests need C++ compilation but no C++ standard-library runtime.
# Avoid Zig building an unused libc++ runtime; clang++/g++ also work.
$prefixArguments = @()
if ((Split-Path -Leaf $CompilerPath) -eq 'zig.exe') {
    $prefixArguments = @('cc', '-x', 'c++', '-fno-exceptions', '-fno-rtti')
}
$targets = @(
    @{ Name='title_worker_policy'; Sources=@((Join-Path $testRoot 'title_worker_policy_tests.cpp')) },
    @{ Name='output_transport_enabled'; Arguments=@('-m32', '-DXBOXINPUT_USB_DPC_SUBMIT=0', '-DXBOXINPUT_SUPPRESS_RUNTIME_RUMBLE=0', '-DXBOXINPUT_FORCE_ZERO_WIRE_RUMBLE=0', '-DXBOXINPUT_DETAILED_OUTPUT_TRACE=1'); Sources=@((Join-Path $testRoot 'gip_output_transport_tests.cpp')) },
    @{ Name='output_transport_suppressed'; Arguments=@('-m32', '-DXBOXINPUT_USB_DPC_SUBMIT=0', '-DXBOXINPUT_SUPPRESS_RUNTIME_RUMBLE=1', '-DXBOXINPUT_FORCE_ZERO_WIRE_RUMBLE=0', '-DXBOXINPUT_DETAILED_OUTPUT_TRACE=1'); Sources=@((Join-Path $testRoot 'gip_output_transport_tests.cpp')) },
    @{ Name='output_transport_zero_wire'; Arguments=@('-m32', '-DXBOXINPUT_USB_DPC_SUBMIT=0', '-DXBOXINPUT_FORCE_ZERO_WIRE_RUMBLE=1', '-DXBOXINPUT_DETAILED_OUTPUT_TRACE=1'); Sources=@((Join-Path $testRoot 'gip_output_transport_tests.cpp')) },
    @{ Name='output_transport_low_trace'; Arguments=@('-m32', '-DXBOXINPUT_USB_DPC_SUBMIT=0', '-DXBOXINPUT_FORCE_ZERO_WIRE_RUMBLE=1'); Sources=@((Join-Path $testRoot 'gip_output_transport_tests.cpp')) },
    @{ Name='output_transport_test25_control'; Arguments=@('-m32', '-DXBOXINPUT_FORCE_ZERO_WIRE_RUMBLE=1'); Sources=@((Join-Path $testRoot 'gip_output_transport_tests.cpp')) },
    @{ Name='output_transport_usb_dpc'; Arguments=@('-m32'); Sources=@((Join-Path $testRoot 'gip_output_transport_tests.cpp')) },
    @{ Name='output_transport_usb_dpc_detailed'; Arguments=@('-m32', '-DXBOXINPUT_DETAILED_OUTPUT_TRACE=1'); Sources=@((Join-Path $testRoot 'gip_output_transport_tests.cpp')) },
    @{ Name='output_queue'; Sources=@((Join-Path $testRoot 'gip_output_queue_tests.cpp')) },
    @{ Name='input_readiness'; Sources=@(
        (Join-Path $testRoot 'gip_input_readiness_tests.cpp'),
        (Join-Path $sourceRoot 'gip_gamepad_parser.cpp')
    ) },
    @{ Name='gamepad_parser'; Sources=@(
        (Join-Path $testRoot 'gip_gamepad_parser_tests.cpp'),
        (Join-Path $sourceRoot 'gip_gamepad_parser.cpp'),
        (Join-Path $sourceRoot 'gamepad_mapping.cpp')
    ) }
)
$taskTraceBaseline = $null
$taskActualBaseline = $null
foreach ($target in $targets) {
    $executable = Join-Path $outputRoot ($target.Name + '.exe')
    $targetArguments = @($target.Arguments)
    if ($target.Name -like 'output_transport*' -and (Split-Path -Leaf $CompilerPath) -eq 'zig.exe') {
        $targetArguments = @('-target', 'x86-windows-gnu') + @($target.Arguments | Where-Object { $_ -ne '-m32' })
    }
    & $CompilerPath @prefixArguments @targetArguments -std=c++11 -O2 -UNDEBUG -Wall -Wextra -Werror @($target.Sources) -o $executable
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $($target.Name)" }
    $taskTestOutput = @(& $executable)
    $taskTestOutput | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "Regression failed: $($target.Name)" }
    if ($target.Name -eq 'output_transport_zero_wire') {
        $taskTraceBaseline = @($taskTestOutput | Where-Object { $_ -match '^traffic_fixture ' })
    }
    if ($target.Name -eq 'output_transport_enabled') {
        $taskActualBaseline = @($taskTestOutput | Where-Object { $_ -match '^traffic_fixture ' })
    }
    if ($target.Name -eq 'output_transport_low_trace' -or $target.Name -eq 'output_transport_test25_control' -or $target.Name -like 'output_transport_usb_dpc*') {
        $taskReducedFixture = @($taskTestOutput | Where-Object { $_ -match '^traffic_fixture ' })
        $taskExpectedFixture = $taskTraceBaseline
        if ($target.Name -like 'output_transport_usb_dpc*') {
            $taskExpectedFixture = $taskActualBaseline
        }
        if ($taskExpectedFixture.Count -ne 1 -or $taskReducedFixture.Count -ne 1 -or
            $taskExpectedFixture[0] -ne $taskReducedFixture[0]) {
            throw 'Trace or DPC mode changed the deterministic USB bytes/timing/count/completions fixture.'
        }
        if (($taskReducedFixture[0] -replace 'hash=[0-9A-F]+ ', '') -ne
            ($taskTraceBaseline[0] -replace 'hash=[0-9A-F]+ ', '')) {
            throw 'Restoring motor strengths changed fixture scheduling or completion counts.'
        }
        Write-Host 'PASS: matched-strength direct/DPC bytes; Test25/Test26 submission times and transfer/completion counts unchanged'
    }
    Write-Host "PASS: native $($target.Name)"
}
