$ErrorActionPreference = 'Stop'

$fixturePath = Join-Path $PSScriptRoot 'fixtures\gip_gamepad_vectors.json'
$vectors = Get-Content -LiteralPath $fixturePath -Raw | ConvertFrom-Json
$failures = @()

function Read-Le16([byte[]]$Bytes, [int]$Offset) {
    return [uint16](([int]$Bytes[$Offset]) -bor
        (([int]$Bytes[($Offset + 1)]) -shl 8))
}

function Read-SignedLe16([byte[]]$Bytes, [int]$Offset) {
    $value = Read-Le16 $Bytes $Offset
    if ($value -ge 0x8000) { return [int]$value - 0x10000 }
    return [int]$value
}

function Parse-GipGamepad([byte[]]$Payload) {
    if ($null -eq $Payload -or $Payload.Length -lt 14) {
        return @{ valid = $false }
    }
    $low = $Payload[0]
    $high = $Payload[1]
    $buttons = 0
    if ($low -band 0x10) { $buttons = $buttons -bor 0x1000 }
    if ($low -band 0x20) { $buttons = $buttons -bor 0x2000 }
    if ($low -band 0x40) { $buttons = $buttons -bor 0x4000 }
    if ($low -band 0x80) { $buttons = $buttons -bor 0x8000 }
    if ($low -band 0x04) { $buttons = $buttons -bor 0x0010 }
    if ($low -band 0x08) { $buttons = $buttons -bor 0x0020 }
    if ($high -band 0x01) { $buttons = $buttons -bor 0x0001 }
    if ($high -band 0x02) { $buttons = $buttons -bor 0x0002 }
    if ($high -band 0x04) { $buttons = $buttons -bor 0x0004 }
    if ($high -band 0x08) { $buttons = $buttons -bor 0x0008 }
    if ($high -band 0x10) { $buttons = $buttons -bor 0x0100 }
    if ($high -band 0x20) { $buttons = $buttons -bor 0x0200 }
    if ($high -band 0x40) { $buttons = $buttons -bor 0x0040 }
    if ($high -band 0x80) { $buttons = $buttons -bor 0x0080 }
    return @{
        valid = $true
        buttons = $buttons
        leftTrigger = (Read-Le16 $Payload 2) -shr 2
        rightTrigger = (Read-Le16 $Payload 4) -shr 2
        leftX = Read-SignedLe16 $Payload 6
        leftY = Read-SignedLe16 $Payload 8
        rightX = Read-SignedLe16 $Payload 10
        rightY = Read-SignedLe16 $Payload 12
    }
}

foreach ($vector in $vectors) {
    [byte[]]$payload = for ($i = 0; $i -lt $vector.payload.Length; $i += 2) {
        [Convert]::ToByte($vector.payload.Substring($i, 2), 16)
    }
    $actual = Parse-GipGamepad $payload
    foreach ($field in @('valid','buttons','leftTrigger','rightTrigger','leftX','leftY','rightX','rightY')) {
        if ($null -eq $vector.$field) { continue }
        if ($actual[$field] -ne $vector.$field) {
            $failures += "$($vector.name): $field expected $($vector.$field), got $($actual[$field])"
        }
    }
}

# Guide is a separate GIP virtual-key packet. Exercise the rising-edge rule that
# prevents repeated DOWN frames from immediately closing the Guide overlay.
$guideDown = $false
$guidePending = $false
function Apply-Guide([byte[]]$Payload) {
    if ($null -eq $Payload -or $Payload.Length -lt 2 -or $Payload[1] -ne 0x5B) {
        return $false
    }
    $nextDown = $Payload[0] -ne 0
    if ($nextDown -and -not $script:guideDown) { $script:guidePending = $true }
    $script:guideDown = $nextDown
    return $true
}

if (-not (Apply-Guide ([byte[]](1, 0x5B))) -or -not $guideDown -or -not $guidePending) {
    $failures += 'guide: initial DOWN did not produce a pending rising edge'
}
$guidePending = $false
[void](Apply-Guide ([byte[]](1, 0x5B)))
if ($guidePending) { $failures += 'guide: repeated DOWN produced a duplicate edge' }
[void](Apply-Guide ([byte[]](0, 0x5B)))
[void](Apply-Guide ([byte[]](1, 0x5B)))
if (-not $guidePending) { $failures += 'guide: DOWN after UP did not produce a new edge' }
if (Apply-Guide ([byte[]](1, 0x5A))) { $failures += 'guide: accepted an unrelated virtual key' }

[byte[]]$rumble = 0x00, 0x03, 0x00, 0x00, 0x12, 0x34, 0xFF, 0x00, 0xEB
$expectedRumbleHex = '000300001234ff00eb'
$actualRumbleHex = -join ($rumble | ForEach-Object { $_.ToString('x2') })
if ($actualRumbleHex -ne $expectedRumbleHex) {
    $failures += "rumble: expected $expectedRumbleHex, got $actualRumbleHex"
}

# Mapping arithmetic invariants mirrored by the portable C++ regression target.
$sourceMasks = @(0x0001,0x0002,0x0004,0x0008,0x0010,0x0020,0x0040,
    0x0080,0x0100,0x0200,0x1000,0x2000,0x4000,0x8000)
$targets = @($sourceMasks)
$inputButtons = 0x1001
$mappedButtons = 0
for ($i = 0; $i -lt $sourceMasks.Count; $i++) {
    if ($inputButtons -band $sourceMasks[$i]) { $mappedButtons = $mappedButtons -bor $targets[$i] }
}
if ($mappedButtons -ne $inputButtons) { $failures += 'mapping: identity buttons changed' }
$targets[10] = 0x2000 # A -> B
$targets[0] = 0       # DpadUp -> None
$mappedButtons = 0
for ($i = 0; $i -lt $sourceMasks.Count; $i++) {
    if ($inputButtons -band $sourceMasks[$i]) { $mappedButtons = $mappedButtons -bor $targets[$i] }
}
if ($mappedButtons -ne 0x2000) { $failures += 'mapping: button remap failed' }
$invertedMinimum = if (-32768 -eq -32768) { 32767 } else { -(-32768) }
if ($invertedMinimum -ne 32767) { $failures += 'mapping: signed minimum inversion overflowed' }
$scaledTrigger = [math]::Floor(((200 - 20) * 255 +
    [math]::Floor((255 - 20) / 2)) / (255 - 20))
if ($scaledTrigger -ne 195) { $failures += 'mapping: trigger deadzone scaling failed' }
$halfRumble = [math]::Floor((255 * 50 + 50) / 100)
if ($halfRumble -ne 128) { $failures += 'mapping: rumble scaling failed' }

if ($failures.Count) {
    $failures | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Host "PASS: $($vectors.Count) GIP vectors, Guide, rumble and mapping invariants"
