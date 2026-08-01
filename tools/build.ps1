[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [string]$BuildDirectory = "",

    [ValidateRange(1, 32)]
    [int]$Jobs = 4,

    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot

function Normalize-ProcessPath {
    $environment = [Environment]::GetEnvironmentVariables(
        [EnvironmentVariableTarget]::Process)
    $pathKeys = @(
        $environment.Keys |
            Where-Object { [string]$_ -ieq "Path" }
    )

    if ($pathKeys.Count -le 1) {
        return
    }

    $pathValue = [string]$environment["Path"]
    foreach ($key in $pathKeys) {
        if ([string]$key -ceq "PATH") {
            [Environment]::SetEnvironmentVariable(
                "PATH",
                $null,
                [EnvironmentVariableTarget]::Process)
        }
    }
    [Environment]::SetEnvironmentVariable(
        "Path",
        $pathValue,
        [EnvironmentVariableTarget]::Process)
}

function Find-VsDevCmd {
    $installerRoot = ${env:ProgramFiles(x86)}
    if ($installerRoot) {
        $vswhere = Join-Path $installerRoot `
            "Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path -LiteralPath $vswhere) {
            $installation = & $vswhere `
                -latest `
                -products * `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                -property installationPath
            if ($LASTEXITCODE -eq 0 -and $installation) {
                $candidate = Join-Path $installation.Trim() `
                    "Common7\Tools\VsDevCmd.bat"
                if (Test-Path -LiteralPath $candidate) {
                    return $candidate
                }
            }
        }
    }

    $visualStudioRoot = Join-Path $env:ProgramFiles "Microsoft Visual Studio"
    if (Test-Path -LiteralPath $visualStudioRoot) {
        $versions = Get-ChildItem `
            -LiteralPath $visualStudioRoot `
            -Directory `
            -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending
        foreach ($version in $versions) {
            $editions = Get-ChildItem `
                -LiteralPath $version.FullName `
                -Directory `
                -ErrorAction SilentlyContinue |
                Sort-Object Name
            foreach ($edition in $editions) {
                $candidate = Join-Path $edition.FullName `
                    "Common7\Tools\VsDevCmd.bat"
                if (Test-Path -LiteralPath $candidate) {
                    return $candidate
                }
            }
        }
    }

    throw "Visual Studio C++ development environment was not found."
}

function Import-VsEnvironment([string]$VsDevCmd) {
    Normalize-ProcessPath
    $command = "`"$VsDevCmd`" -no_logo -arch=x64 && set"
    $variables = & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "VsDevCmd.bat failed with exit code $LASTEXITCODE."
    }

    foreach ($line in $variables) {
        if ($line -match "^([^=]+)=(.*)$") {
            [Environment]::SetEnvironmentVariable(
                $matches[1],
                $matches[2],
                [EnvironmentVariableTarget]::Process)
        }
    }
    Normalize-ProcessPath
}

Push-Location $projectRoot
try {
    $vsDevCmd = Find-VsDevCmd
    Import-VsEnvironment $vsDevCmd
    # VsDevCmd may set this from the installed UI language. Ninja parses MSVC
    # include diagnostics most reliably in English.
    $env:VSLANG = "1033"

    $vsRoot = Split-Path -Parent (
        Split-Path -Parent (
            Split-Path -Parent $vsDevCmd))
    $cmakeBin = Join-Path $vsRoot `
        "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
    $cmakeExecutable = Join-Path $cmakeBin "cmake.exe"
    $ctestExecutable = Join-Path $cmakeBin "ctest.exe"
    $ninjaExecutable = Join-Path $vsRoot `
        "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

    foreach ($tool in @(
        $cmakeExecutable,
        $ctestExecutable,
        $ninjaExecutable
    )) {
        if (-not (Test-Path -LiteralPath $tool)) {
            throw "Required Visual Studio build tool '$tool' was not found."
        }
    }
    $compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
    if (-not $compiler) {
        throw "MSVC compiler 'cl.exe' was not found after VsDevCmd."
    }

    Write-Host "MSVC:  $($compiler.Source)"
    Write-Host "CMake: $cmakeExecutable"
    Write-Host "Ninja: $ninjaExecutable"

    $runtimeOutputDirectory = Join-Path $projectRoot "build"
    $resolvedBuildDirectory = if (
        -not [string]::IsNullOrWhiteSpace($BuildDirectory)
    ) {
        if ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
            $BuildDirectory
        } else {
            Join-Path $projectRoot $BuildDirectory
        }
    } elseif ($projectRoot -match "[^\x00-\x7F]") {
        if (-not $env:LOCALAPPDATA) {
            throw "LOCALAPPDATA is required for the ASCII build path."
        }
        Join-Path $env:LOCALAPPDATA `
            "MgsToneCraft\cmake-build-$($Configuration.ToLowerInvariant())"
    } else {
        $runtimeOutputDirectory
    }

    Write-Host "Build tree: $resolvedBuildDirectory"
    Write-Host "Executables: $runtimeOutputDirectory"

    # CMake can misdecode localized MSVC /showIncludes output, leaving Ninja
    # unaware that a public header changed.  Detect project-header updates
    # independently and clean only in that case so stale ABI layouts can
    # never be linked into an otherwise incremental build.
    $headerDependencyStamp = Join-Path `
        $resolvedBuildDirectory ".mgstc-header-deps.stamp"
    $headerStampTime = if (
        Test-Path -LiteralPath $headerDependencyStamp -PathType Leaf
    ) {
        (Get-Item -LiteralPath $headerDependencyStamp).LastWriteTimeUtc
    } else {
        [DateTime]::MinValue
    }
    $projectHeadersChanged = @(
        Get-ChildItem `
            -LiteralPath (Join-Path $projectRoot "src") `
            -Recurse `
            -File `
            -ErrorAction Stop |
            Where-Object {
                $_.Extension -in @(".h", ".hpp", ".inl") -and
                $_.LastWriteTimeUtc -gt $headerStampTime
            }
    ).Count -gt 0

    & $cmakeExecutable `
        -S $projectRoot `
        -B $resolvedBuildDirectory `
        -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$ninjaExecutable" `
        "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$runtimeOutputDirectory" `
        "-DCMAKE_BUILD_TYPE=$Configuration"
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed with exit code $LASTEXITCODE."
    }

    $targetExecutables = @("mgstc.exe") | ForEach-Object {
        [System.IO.Path]::GetFullPath(
            (Join-Path $runtimeOutputDirectory $_))
    }
    $runningTargets = @(
        Get-Process -Name "mgstc" `
            -ErrorAction SilentlyContinue |
            Where-Object {
                $processPath = if ($_.Path) {
                    [System.IO.Path]::GetFullPath($_.Path)
                }
                $_.Path -and @(
                    $targetExecutables |
                        Where-Object {
                            [string]::Equals(
                                $processPath,
                                $_,
                                [System.StringComparison]::OrdinalIgnoreCase)
                        }
                ).Count -gt 0
            }
    )
    if ($runningTargets.Count -gt 0) {
        Write-Host "Stopping the running build target before relinking:"
        foreach ($process in $runningTargets) {
            Write-Host "  PID $($process.Id): $($process.Path)"
            Stop-Process -Id $process.Id -Force
            Wait-Process -Id $process.Id -ErrorAction SilentlyContinue
        }
    }

    if ($projectHeadersChanged) {
        Write-Host `
            "Project header change detected: cleaning stale Ninja objects."
        & $cmakeExecutable `
            --build $resolvedBuildDirectory `
            --target clean
        if ($LASTEXITCODE -ne 0) {
            throw "Clean step failed with exit code $LASTEXITCODE."
        }
    }

    & $cmakeExecutable `
        --build $resolvedBuildDirectory `
        --parallel $Jobs
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }

    if (-not $SkipTests) {
        & $ctestExecutable `
            --test-dir $resolvedBuildDirectory `
            --output-on-failure
        if ($LASTEXITCODE -ne 0) {
            throw "Tests failed with exit code $LASTEXITCODE."
        }
    }

    [IO.File]::WriteAllText(
        $headerDependencyStamp,
        [DateTime]::UtcNow.ToString("O"))
} finally {
    Pop-Location
}
