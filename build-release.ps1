[CmdletBinding()]
param(
    [string]$XexToolPath = $env:XEXTOOL_PATH,
    [string]$MsBuildPath
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceRoot = Join-Path $projectRoot 'source'
if (-not $MsBuildPath) {
    $MsBuildPath = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\MSBuild.exe'
}
$builtXex = Join-Path $sourceRoot 'Release Retail\riffmaster.xex'
$distDirectory = Join-Path $projectRoot 'dist'
$outputXex = Join-Path $distDirectory 'XboxInputGip-17559-retail.xex'

if (-not (Test-Path -LiteralPath $MsBuildPath)) {
    throw "MSBuild was not found: $MsBuildPath"
}
if (-not $XexToolPath) {
    throw 'Pass -XexToolPath <path-to-XexTool.exe>, or set XEXTOOL_PATH.'
}
if (-not (Test-Path -LiteralPath $XexToolPath)) {
    throw "XexTool was not found: $XexToolPath"
}

Push-Location $sourceRoot
try {
    & $MsBuildPath 'riffmaster.vcxproj' /t:Rebuild /p:Configuration='Release Retail' /p:Platform='Xbox 360' /v:minimal
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

if (-not (Test-Path -LiteralPath $builtXex)) {
    throw "Expected build output was not produced: $builtXex"
}

New-Item -ItemType Directory -Path $distDirectory -Force | Out-Null
& $XexToolPath -r a -m r $builtXex
if ($LASTEXITCODE -ne 0) {
    throw "XexTool failed with exit code $LASTEXITCODE"
}

Copy-Item -LiteralPath $builtXex -Destination $outputXex -Force
Write-Host "Built retail DashLaunch plugin: $outputXex"
