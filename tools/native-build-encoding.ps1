# SPDX-License-Identifier: AGPL-3.0-only
# CMake AUTO reads console input CP; cl writes using console output CP.
function Enter-NativeBuildEncoding([string]$NinjaExecutable) {
    if (-not ('Mgstc.NativeCodePage' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Mgstc {
    public static class NativeCodePage {
        [DllImport("kernel32.dll")] public static extern uint GetACP();
        [DllImport("kernel32.dll")] public static extern uint GetConsoleCP();
        [DllImport("kernel32.dll")] public static extern uint GetConsoleOutputCP();
    }
}
'@
    }
    $description = @(& $NinjaExecutable -t wincodepage)
    if ($LASTEXITCODE -ne 0) { throw "Cannot determine Ninja's build-file encoding." }
    $encodingName = (($description | Select-String '^Build file encoding: (ANSI|UTF-8)$').Matches.Groups[1].Value)
    if (-not $encodingName) { throw "Ninja did not report a supported Windows encoding." }
    $codePage = if ($encodingName -eq 'UTF-8') { 65001 } else { [int][Mgstc.NativeCodePage]::GetACP() }
    $state = [ordered]@{
        input = [Console]::InputEncoding
        output = [Console]::OutputEncoding
        pipeline = $script:OutputEncoding
        consoleInput = [int][Mgstc.NativeCodePage]::GetConsoleCP()
        consoleOutput = [int][Mgstc.NativeCodePage]::GetConsoleOutputCP()
        codePage = $codePage
        ninjaEncoding = $encodingName
    }
    try {
        # No console: CMake AUTO and native tools fall back to ACP. .NET
        # console setters would fail, so do not call them in this case.
        if ($state.consoleInput -eq 0 -and $state.consoleOutput -eq 0) {
            if ($codePage -ne [int][Mgstc.NativeCodePage]::GetACP()) {
                throw "No console is available and Ninja's encoding differs from ACP."
            }
        } elseif ($state.consoleInput -eq 0 -or $state.consoleOutput -eq 0) {
            throw "Incomplete console code-page state; refusing native configuration."
        } else {
            $encoding = [Text.Encoding]::GetEncoding($codePage)
            [Console]::InputEncoding = $encoding
            [Console]::OutputEncoding = $encoding
            if ([Mgstc.NativeCodePage]::GetConsoleCP() -ne $codePage -or
                [Mgstc.NativeCodePage]::GetConsoleOutputCP() -ne $codePage) {
                throw "Cannot align console input/output with Ninja's encoding."
            }
        }
        $script:OutputEncoding = [Text.Encoding]::GetEncoding($codePage)
        return $state
    } catch {
        Exit-NativeBuildEncoding $state
        throw
    }
}

function Exit-NativeBuildEncoding($State) {
    if (-not $State) { return }
    try {
        if ($State.consoleInput -ne 0) { [Console]::InputEncoding = $State.input }
    } finally {
        try {
            if ($State.consoleOutput -ne 0) { [Console]::OutputEncoding = $State.output }
        } finally { $script:OutputEncoding = $State.pipeline }
    }
}

function New-DependencyRecoverySeed([string]$Directory) {
    $cache = Join-Path $Directory 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cache)) {
        throw "-RecoverDependencies requires an existing CMake cache."
    }
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfff')
    Copy-Item -LiteralPath $cache -Destination (Join-Path $Directory ".mgstc-cache-before-recovery-$stamp.txt")
    $entries = New-Object 'System.Collections.Generic.List[string]'
    $entries.Add('# Preserve typed user cache settings; compiler detection is intentionally rerun.')
    foreach ($line in [IO.File]::ReadAllLines($cache)) {
        if ($line -match '^([A-Za-z_][A-Za-z0-9_.+-]*):(BOOL|STRING|PATH|FILEPATH|UNINITIALIZED)=(.*)$') {
            $key = $matches[1]; $kind = $matches[2]; $value = $matches[3]
            if ($kind -eq 'UNINITIALIZED') { $kind = 'STRING' }
            $equals = '=='
            while ($value.Contains("]$equals]")) { $equals += '=' }
            $entries.Add("set($key [$equals[$value]$equals] CACHE $kind [==[Preserved recovery setting]==] FORCE)")
        }
    }
    $seed = Join-Path $Directory '.mgstc-recovery-initial-cache.cmake'
    [IO.File]::WriteAllLines($seed, $entries, (New-Object Text.UTF8Encoding($false)))
    return $seed
}
