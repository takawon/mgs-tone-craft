[CmdletBinding()]
param(
    [string]$Configuration = "Release",
    [string]$Version = "",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot "dist"
}

$exePath = Join-Path $projectRoot "build\mgstc.exe"
if (-not (Test-Path -LiteralPath $exePath)) {
    throw "build\mgstc.exe was not found. Build Release first."
}

if (-not $Version) {
    $versionFile = Join-Path $projectRoot "VERSION"
    if (Test-Path -LiteralPath $versionFile) {
        $Version = (Get-Content -LiteralPath $versionFile -TotalCount 1).Trim()
    }
}
if (-not $Version) {
    $specPath = Join-Path $projectRoot "SPECIFICATION.md"
    if (Test-Path -LiteralPath $specPath) {
        foreach ($line in Get-Content -LiteralPath $specPath -TotalCount 8) {
            if ($line -match ':\s*([0-9]+(?:\.[0-9]+)?)\s*$' -and $line -match '0\.|1\.') {
                $Version = $Matches[1]
                break
            }
        }
    }
}
if (-not $Version) {
    $Version = "alpha"
}

$stageRoot = Join-Path $env:TEMP ("mgstc-dist-" + [Guid]::NewGuid().ToString("N"))
$stageDir = Join-Path $stageRoot "MGSToneCraft"
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

Copy-Item -LiteralPath $exePath -Destination (Join-Path $stageDir "mgstc.exe")
Copy-Item -LiteralPath (Join-Path $projectRoot "LICENSE") `
    -Destination (Join-Path $stageDir "LICENSE")
Copy-Item -LiteralPath (Join-Path $projectRoot "README.md") `
    -Destination (Join-Path $stageDir "README.md")
Copy-Item -LiteralPath (Join-Path $projectRoot "THIRD_PARTY.md") `
    -Destination (Join-Path $stageDir "THIRD_PARTY.md")

$licensesDir = Join-Path $stageDir "licenses"
New-Item -ItemType Directory -Path $licensesDir -Force | Out-Null
foreach ($name in @("emu2149", "emu2212", "emu2413")) {
    $src = Join-Path $projectRoot "third_party\$name\LICENSE"
    Copy-Item -LiteralPath $src `
        -Destination (Join-Path $licensesDir "$name-LICENSE.txt")
}
$juceBuildTree = Join-Path $env:LOCALAPPDATA `
    ("MgsToneCraft\cmake-build-" + $Configuration.ToLowerInvariant())
$asioLicense = Join-Path $juceBuildTree `
    "_deps\juce-src\modules\juce_audio_devices\native\asio\LICENSE.txt"
if (-not (Test-Path -LiteralPath $asioLicense)) {
    throw "JUCE ASIO license was not found. Build $Configuration before packaging."
}
Copy-Item -LiteralPath $asioLicense `
    -Destination (Join-Path $licensesDir "Steinberg-ASIO-SDK-LICENSE.txt")

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$zipName = "MGSToneCraft-$Version-win64.zip"
$zipPath = Join-Path $OutputDirectory $zipName
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

Compress-Archive -Path (Join-Path $stageRoot "*") `
    -DestinationPath $zipPath `
    -CompressionLevel Optimal
Remove-Item -LiteralPath $stageRoot -Recurse -Force

Write-Output $zipPath
