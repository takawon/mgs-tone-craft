[CmdletBinding()]
param(
    [ValidateSet("main", "scc", "opll")]
    [string]$Editor = "scc",
    [ValidateSet(
        "editor",
        "settings-view",
        "settings-midi",
        "settings-output",
        "library")]
    [string]$Target = "editor",
    [string]$OutputPath = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $projectRoot "build\mgstc.exe"

if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "JUCE executable was not found. Run tools\build.cmd first."
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $suffix = switch ($Target) {
        "settings-view" { "settings-view" }
        "settings-midi" { "settings-midi" }
        "settings-output" { "settings-output" }
        "library" { "library-manager" }
        default { "$Editor-editor" }
    }
    $OutputPath = Join-Path `
        $projectRoot `
        "build\ui-captures\$suffix.png"
} elseif (-not [IO.Path]::IsPathRooted($OutputPath)) {
    $OutputPath = Join-Path $projectRoot $OutputPath
}

$absoluteOutput = [IO.Path]::GetFullPath($OutputPath)
$outputDirectory = Split-Path -Parent $absoluteOutput
New-Item -ItemType Directory -Path $outputDirectory -Force |
    Out-Null

$arguments = @(
    "--editor=$Editor"
    "--capture-target=$Target"
    '"--capture-ui={0}"' -f $absoluteOutput
)
$process = Start-Process `
    -FilePath $executable `
    -ArgumentList $arguments `
    -WindowStyle Hidden `
    -Wait `
    -PassThru

if ($process.ExitCode -ne 0) {
    $failureStages = @{
        10 = "the JUCE content component was unavailable"
        11 = "JUCE returned an invalid component snapshot"
        12 = "the output directory could not be created"
        13 = "the output PNG stream could not be opened"
        14 = "the PNG encoder failed"
        15 = "the output path was not parsed as an absolute path"
    }
    $stage = $failureStages[$process.ExitCode]
    if ([string]::IsNullOrWhiteSpace($stage)) {
        $stage = "an unknown capture stage failed"
    }
    throw "JUCE UI capture failed: $stage (exit $($process.ExitCode))."
}
if (-not (Test-Path -LiteralPath $absoluteOutput -PathType Leaf)) {
    throw "JUCE UI capture did not create the expected PNG."
}

$result = Get-Item -LiteralPath $absoluteOutput
Write-Host "UI snapshot: $($result.FullName)"
Write-Host "Size: $($result.Length) bytes"
