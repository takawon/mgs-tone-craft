[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [string]$BuildDirectory = "",

    [ValidateRange(1, 32)]
    [int]$Jobs = 4,

    [switch]$SkipTests,

    # Optional focused regression run; the default still runs every test.
    [string]$TestRegex = "",

    [string[]]$Target = @(),

    # Inspect project targets without compiling/testing them. JUCE may compile
    # its configure-time juceaide helper during CMake configuration.
    [switch]$PlanOnly,

    # Save Ninja's dependency database as text for dependency audits.
    [switch]$DependencyReport,

    [switch]$BuildBenchmarks,

    # Diagnostic only: retain all normal validation and emit phase timings.
    [switch]$Timings,

    # One-time recovery only; preserve cache settings and rerun compiler detection.
    [switch]$RecoverDependencies,

    # Reuse source checkouts only; objects and binaries remain tree-local.
    [string]$DependencySourceRoot = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "native-build-encoding.ps1")
. (Join-Path $PSScriptRoot "build-evidence.ps1")
if ($RecoverDependencies -and ($PlanOnly -or $Target.Count -gt 0)) {
    throw "-RecoverDependencies requires a complete build, without -PlanOnly or -Target."
}
if ($Target.Count -gt 0 -and -not $SkipTests -and -not $PlanOnly) {
    throw "-Target requires -SkipTests (or -PlanOnly). Test validation must build all test prerequisites."
}


function Get-SourceState {
    # Include dirty and untracked inputs, including ignored local test fixtures.
    # Generated build trees and FetchContent caches are deliberately excluded.
    # NUL separators and explicit UTF-8 preserve Japanese filenames and avoid
    # Git's C-quoted path representation (and PowerShell's native decoding).
    $gitInfo = New-Object Diagnostics.ProcessStartInfo
    $gitInfo.FileName = $gitExecutable
    $gitInfo.Arguments = "-C `"$projectRoot`" ls-files --cached --others --exclude-standard -z"
    $gitInfo.UseShellExecute = $false
    $gitInfo.CreateNoWindow = $true
    $gitInfo.RedirectStandardOutput = $true
    $gitInfo.RedirectStandardError = $true
    $gitInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
    $gitProcess = [Diagnostics.Process]::Start($gitInfo)
    try {
        $gitPaths = $gitProcess.StandardOutput.ReadToEnd()
        $gitError = $gitProcess.StandardError.ReadToEnd()
        $gitProcess.WaitForExit()
        if ($gitProcess.ExitCode -ne 0) { throw "Cannot enumerate source inputs: $gitError" }
    } finally { $gitProcess.Dispose() }
    $paths = @($gitPaths.Split([char]0) | Where-Object { $_ })
    foreach ($directory in @("src", "tests", "tools", "assets", "third_party")) {
        $root = Join-Path $projectRoot $directory
        if (Test-Path -LiteralPath $root) {
            $paths += @(Get-ChildItem -LiteralPath $root -Recurse -File |
                Where-Object { $_.FullName -notmatch '[\\/](__pycache__|\.git)[\\/]' } |
                ForEach-Object { $_.FullName.Substring($projectRoot.Length + 1).Replace('\', '/') })
        }
    }
    $paths += @("SPECIFICATION.md", "VERSION", "CMakeLists.txt")
    $files = [ordered]@{}
    $writeTimes = [ordered]@{}
    foreach ($path in ($paths | Sort-Object -Unique)) {
        # Local notes/logs are not build inputs. Include every tracked file and
        # untracked source/configuration file, but not this audit's output.
        if ($path -match '^(docs/audits/|analysis/|build[^/]*/|dist/)' -or
            $path -in @("AGENTS.md", "HANDOFF.md", "ENGINE_DESIGN.md", "RELEASE_CHECKLIST.md")) { continue }
        $fullPath = Join-Path $projectRoot $path
        $fullPath = [IO.Path]::GetFullPath($fullPath)
        $isBuildOutput = $false
        foreach ($outputDirectory in @($resolvedBuildDirectory, $runtimeOutputDirectory)) {
            $outputPrefix = [IO.Path]::GetFullPath($outputDirectory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
            if ($fullPath.StartsWith($outputPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                $isBuildOutput = $true; break
            }
        }
        if ($isBuildOutput) { continue }
        if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
            $stream = [IO.File]::OpenRead($fullPath)
            $fileSha = [Security.Cryptography.SHA256]::Create()
            try {
                $files[$path] = [BitConverter]::ToString($fileSha.ComputeHash($stream)).Replace('-', '')
            } finally { $stream.Dispose(); $fileSha.Dispose() }
            $writeTimes[$path] = (Get-Item -LiteralPath $fullPath).LastWriteTimeUtc.Ticks.ToString()
        } else { $files[$path] = "MISSING" }
    }
    $serialized = $files | ConvertTo-Json -Compress -Depth 3
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($serialized))).Replace('-', '')
        $timeJson = $writeTimes | ConvertTo-Json -Compress -Depth 3
        $modificationDigest = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($timeJson))).Replace('-', '')
    } finally { $sha.Dispose() }
    return [ordered]@{ digest = $digest; modificationDigest = $modificationDigest; files = $files; writeTimes = $writeTimes }
}

function Enter-BuildLock([string]$Path) {
    # FileShare.None makes acquisition atomic; PID-only check/write races.
    # Keep the file after release to avoid unlink/recreate races between users.
    try {
        $handle = [IO.File]::Open($Path, [IO.FileMode]::OpenOrCreate,
            [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    } catch {
        throw "Another build owns '$Path'. Wait for it to finish."
    }
    try {
        # Honour a running old wrapper which only wrote a PID to this file.
        $reader = New-Object IO.StreamReader($handle, [Text.Encoding]::UTF8, $true, 1024, $true)
        try { $oldPidText = $reader.ReadToEnd() } finally { $reader.Dispose() }
        $oldBuildPid = 0
        [void][int]::TryParse($oldPidText, [ref]$oldBuildPid)
        if ($oldBuildPid -ne $PID -and $oldBuildPid -gt 0 -and
            (Get-Process -Id $oldBuildPid -ErrorAction SilentlyContinue)) {
            throw "Another MGSTC build is already running (PID $oldBuildPid)."
        }
        $bytes = [Text.Encoding]::UTF8.GetBytes("$PID")
        $handle.SetLength(0)
        $handle.Write($bytes, 0, $bytes.Length)
        $handle.Flush()
        return $handle
    } catch { $handle.Dispose(); throw }
}

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

function Resolve-Git([string]$VsRoot) {
    $found = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($found) {
        return $found.Source
    }

    # CMake FetchContent needs git to download JUCE and rpclib. A machine with
    # only Visual Studio installed still has one, but not on PATH.
    $candidates = @(
        (Join-Path $VsRoot ("Common7\IDE\CommonExtensions\Microsoft\" +
            "TeamFoundation\Team Explorer\Git\cmd")),
        (Join-Path $env:ProgramFiles "Git\cmd"),
        (Join-Path ${env:ProgramFiles(x86)} "Git\cmd")
    )
    foreach ($directory in $candidates) {
        $candidate = Join-Path $directory "git.exe"
        if (Test-Path -LiteralPath $candidate) {
            $env:PATH = "$directory;$env:PATH"
            return $candidate
        }
    }

    throw ("Git was not found. CMake needs it to download JUCE and rpclib. " +
        "Install Git for Windows or the Visual Studio Git component.")
}

Push-Location $projectRoot
$buildLocks = @()
$validation = $null
$validationPath = $null
$validationPassed = $false
$nativeEncodingState = $null
$buildTimer = [Diagnostics.Stopwatch]::StartNew()
$phaseRecords = New-Object 'System.Collections.Generic.List[object]'
$phaseName = "find_visual_studio"
$phaseStart = 0.0
function Set-BuildPhase([string]$Name) {
    if ($Timings) {
        $now = $buildTimer.Elapsed.TotalSeconds
        $phaseRecords.Add([ordered]@{ name = $script:phaseName; seconds = $now - $script:phaseStart })
        $script:phaseStart = $now
        $script:phaseName = $Name
    }
}
try {
    $vsDevCmd = Find-VsDevCmd
    Set-BuildPhase "import_visual_studio_environment"
    Import-VsEnvironment $vsDevCmd
    Set-BuildPhase "resolve_tools"
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

    $gitExecutable = Resolve-Git $vsRoot

    Write-Host "MSVC:  $($compiler.Source)"
    Write-Host "CMake: $cmakeExecutable"
    Write-Host "Ninja: $ninjaExecutable"
    Write-Host "Git:   $gitExecutable"

    Set-BuildPhase "directories_and_locks"
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

    $resolvedBuildDirectory = [IO.Path]::GetFullPath($resolvedBuildDirectory)
    $buildDirectoryPrefix = $resolvedBuildDirectory.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if ($resolvedBuildDirectory -eq $projectRoot -or
        $projectRoot.StartsWith($buildDirectoryPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        $resolvedBuildDirectory -match ('^' + [regex]::Escape($projectRoot) + '[\\/](src|tests|tools|assets|third_party|\.git|\.cursor|\.agents)([\\/]|$)')) {
        throw "Use a dedicated build directory, outside source/configuration directories."
    }
    # A custom tree owns its binaries as well as its objects. Otherwise an
    # up-to-date Debug tree can silently test a newer Release tree's exe.
    $runtimeOutputDirectory = $resolvedBuildDirectory

    Write-Host "Build tree: $resolvedBuildDirectory"
    Write-Host "Executables: $runtimeOutputDirectory"

    $buildLockPath = Join-Path $resolvedBuildDirectory ".mgstc-build.lock"
    if (-not (Test-Path -LiteralPath $resolvedBuildDirectory)) {
        New-Item -ItemType Directory -Path $resolvedBuildDirectory | Out-Null
    }
    if (-not (Test-Path -LiteralPath $runtimeOutputDirectory)) {
        New-Item -ItemType Directory -Path $runtimeOutputDirectory | Out-Null
    }
    # Serialize wrapper runs in this checkout, including shared dependency
    # source patching, and honour an old per-tree wrapper during transition.
    $projectBuildDirectory = Join-Path $projectRoot "build"
    if (-not (Test-Path -LiteralPath $projectBuildDirectory)) {
        New-Item -ItemType Directory -Path $projectBuildDirectory | Out-Null
    }
    foreach ($lockPath in (@((Join-Path $projectBuildDirectory ".mgstc-build.lock"), $buildLockPath) | Select-Object -Unique)) {
        $buildLocks += Enter-BuildLock $lockPath
    }

    Set-BuildPhase "native_output_encoding"
    $nativeEncodingState = Enter-NativeBuildEncoding $ninjaExecutable
    $encodingEvidence = [ordered]@{
        ninjaEncoding = $nativeEncodingState.ninjaEncoding
        codePage = $nativeEncodingState.codePage
        originalConsoleInput = $nativeEncodingState.consoleInput
        originalConsoleOutput = $nativeEncodingState.consoleOutput
    }
    Write-BuildEvidence (Join-Path $resolvedBuildDirectory ".mgstc-native-encoding.json") ($encodingEvidence | ConvertTo-Json)
    Write-Host "Native build encoding: $($nativeEncodingState.ninjaEncoding), CP $($nativeEncodingState.codePage)"
    Set-BuildPhase "source_hash_before"
    if (-not $PlanOnly) {
        $validationPath = Join-Path $resolvedBuildDirectory ".mgstc-validation.json"
        $sourceBefore = Get-SourceState
        Set-BuildPhase "initial_validation_evidence"
        $validation = [ordered]@{
            status = "InProgress"
            startedUtc = [DateTime]::UtcNow.ToString("O")
            head = (& $gitExecutable -C $projectRoot rev-parse HEAD)
            configuration = $Configuration
            jobs = $Jobs
            targets = $Target
            skipTests = [bool]$SkipTests
            testRegex = $TestRegex
            recoverDependencies = [bool]$RecoverDependencies
            nativeCodePage = $nativeEncodingState.codePage
            sourceBefore = $sourceBefore
        }
        Write-BuildEvidence $validationPath ($validation | ConvertTo-Json -Depth 6)
        Write-Host "Validation source SHA256: $($sourceBefore.digest)"
    }

    Set-BuildPhase "running_binary_guard"
    $outputRoot = [System.IO.Path]::GetFullPath($runtimeOutputDirectory)
    if (-not $outputRoot.EndsWith([IO.Path]::DirectorySeparatorChar)) {
        $outputRoot += [IO.Path]::DirectorySeparatorChar
    }
    $runningTargets = @(
        Get-Process -ErrorAction SilentlyContinue |
            Where-Object {
                # Path resolves process modules and can be expensive. Only
                # inspect it for names that the guard could actually reject.
                if ($_.ProcessName -notlike "mgstc*") {
                    return $false
                }
                if (-not $_.Path) {
                    return $false
                }
                try {
                    $processPath = [System.IO.Path]::GetFullPath($_.Path)
                } catch {
                    return $false
                }
                return $processPath.StartsWith(
                    $outputRoot,
                    [System.StringComparison]::OrdinalIgnoreCase)
            }
    )
    if ($runningTargets.Count -gt 0) {
        throw ("MGSTC binaries are still running; close them before building:`n  " +
            (($runningTargets | ForEach-Object { "PID $($_.Id): $($_.Path)" }) -join "`n  "))
    }

    Set-BuildPhase "cmake_configure_generate"
    $configureArguments = @()
    $recoveryArguments = @()
    if ($Timings) {
        $configureArguments += @("--profiling-format=google-trace", "--profiling-output=$(Join-Path $resolvedBuildDirectory '.mgstc-cmake-profile.json')")
    }
    if ($BuildBenchmarks) { $configureArguments += "-DMGSTC_BUILD_BENCHMARKS=ON" }
    if ($DependencySourceRoot) {
        $sourceRoot = if ([IO.Path]::IsPathRooted($DependencySourceRoot)) {
            [IO.Path]::GetFullPath($DependencySourceRoot)
        } else { [IO.Path]::GetFullPath((Join-Path $projectRoot $DependencySourceRoot)) }
        foreach ($dependency in @("juce", "rpclib")) {
            $sourceDirectory = Join-Path $sourceRoot "$dependency-src"
            if (-not (Test-Path -LiteralPath (Join-Path $sourceDirectory "CMakeLists.txt"))) {
                throw "Dependency source checkout is missing: $sourceDirectory"
            }
            $configureArguments += "-DFETCHCONTENT_SOURCE_DIR_$($dependency.ToUpperInvariant())=$sourceDirectory"
        }
    }
    if ($RecoverDependencies) {
        $cmakeVersion = @(& $cmakeExecutable --version)[0]
        if ($LASTEXITCODE -ne 0 -or $cmakeVersion -notmatch 'cmake version ([0-9]+\.[0-9]+)' -or
            [version]$matches[1] -lt [version]'3.24') {
            throw "-RecoverDependencies requires CMake 3.24 or newer."
        }
        $seed = New-DependencyRecoverySeed $resolvedBuildDirectory
        $recoveryArguments += @("--fresh", "-C", $seed)
    }
    & $cmakeExecutable @recoveryArguments `
        -S $projectRoot `
        -B $resolvedBuildDirectory `
        -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$ninjaExecutable" `
        "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$runtimeOutputDirectory" `
        "-DCMAKE_BUILD_TYPE=$Configuration" @configureArguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed with exit code $LASTEXITCODE."
    }

    Set-BuildPhase "optional_dependency_report"
    if ($DependencyReport) {
        & $ninjaExecutable -C $resolvedBuildDirectory -t deps |
            Set-Content -LiteralPath (Join-Path $resolvedBuildDirectory ".mgstc-ninja-deps.txt") -Encoding UTF8
        if ($LASTEXITCODE -ne 0) { throw "Ninja dependency inspection failed." }
    }
    if ($PlanOnly) {
        $planArguments = @("-C", $resolvedBuildDirectory, "-n", "-d", "explain") + $Target
        & $ninjaExecutable @planArguments
        if ($LASTEXITCODE -ne 0) { throw "Ninja build plan failed." }
        return
    }

    Set-BuildPhase "build_system"
    $buildArguments = @("--build", $resolvedBuildDirectory, "--parallel", "$Jobs")
    if ($RecoverDependencies) { $buildArguments += "--clean-first" }
    if ($Target.Count -gt 0) { $buildArguments += @("--target") + $Target }
    if ($Timings) { $buildArguments += @("--", "-d", "stats") }
    & $cmakeExecutable @buildArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }

    Set-BuildPhase "dependency_validation"
    $dependencyGuardArguments = @{
        BuildDirectory = $resolvedBuildDirectory
        ProjectRoot = $projectRoot
        NinjaExecutable = $ninjaExecutable
        CodePage = $nativeEncodingState.codePage
        Targets = $Target
    }
    & (Join-Path $PSScriptRoot "check-ninja-deps.ps1") @dependencyGuardArguments

    Set-BuildPhase "test_discovery"
    if (-not $SkipTests) {
        $testfile = Join-Path $resolvedBuildDirectory "CTestTestfile.cmake"
        if (-not (Test-Path -LiteralPath $testfile -PathType Leaf)) {
            throw "CTest test file was not generated: $testfile"
        }
        $testExecutables = @(
            Select-String `
                -LiteralPath $testfile `
                -Pattern '"([^"]+\.exe)"' |
                ForEach-Object { $_.Matches.Groups[1].Value } |
                Sort-Object -Unique
        )
        $missingTests = @(
            $testExecutables |
                Where-Object { -not (Test-Path -LiteralPath $_) }
        )
        if (-not $TestRegex -and $missingTests.Count -gt 0) {
            throw (
                "CTest executables missing after build:`n  " +
                ($missingTests -join "`n  ")
            )
        }

        Set-BuildPhase "test_execution"
        $testArguments = @("--test-dir", $resolvedBuildDirectory, "--output-on-failure")
        if ($TestRegex) {
            $testArguments += @("-R", $TestRegex, "--no-tests=error")
        }
        & $ctestExecutable @testArguments
        if ($LASTEXITCODE -ne 0) {
            throw "Tests failed with exit code $LASTEXITCODE."
        }
    }
    Set-BuildPhase "source_hash_after_and_compare"
    $sourceAfter = Get-SourceState
    $validation.sourceAfter = $sourceAfter
    if ($sourceBefore.digest -ne $sourceAfter.digest -or
        $sourceBefore.modificationDigest -ne $sourceAfter.modificationDigest) {
        throw "Sources changed during validation. Rebuild and rerun affected tests before claiming PASS."
    }
    $validationPassed = $true
    Write-Host "Validation sources unchanged. Evidence: $validationPath"
} finally {
    Set-BuildPhase "final_validation_evidence"
    try {
        $restoreError = $null
        try { Exit-NativeBuildEncoding $nativeEncodingState } catch {
            $validationPassed = $false
            $restoreError = $_
        }
        if ($validation) {
            $validation.status = if ($validationPassed) {
                if ($SkipTests) { "BuildPassedTestsNotRun" } else { "BuildAndSelectedTestsPassed" }
            } else { "FailedOrIncomplete" }
            $validation.finishedUtc = [DateTime]::UtcNow.ToString("O")
            $validation.elapsedSeconds = $buildTimer.Elapsed.TotalSeconds
            Write-BuildEvidence $validationPath ($validation | ConvertTo-Json -Depth 6)
        }
        if ($restoreError) { throw $restoreError }
    } finally {
        Set-BuildPhase "release_locks"
        foreach ($handle in $buildLocks) { $handle.SetLength(0); $handle.Dispose() }
        Pop-Location
        Set-BuildPhase "timing_output"
        if ($Timings -and $resolvedBuildDirectory -and (Test-Path -LiteralPath $resolvedBuildDirectory)) {
            $timingPath = Join-Path $resolvedBuildDirectory ".mgstc-timing-$PID.json"
            $timingEvidence = [ordered]@{
                elapsedSeconds = $buildTimer.Elapsed.TotalSeconds
                configuration = $Configuration
                skipTests = [bool]$SkipTests
                phases = @($phaseRecords.ToArray())
            }
            Write-BuildEvidence $timingPath ($timingEvidence | ConvertTo-Json -Depth 6)
            Write-Host "Phase timings: $timingPath"
        }
    }
}
