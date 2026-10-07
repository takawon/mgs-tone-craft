# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$ProjectRoot,
    [Parameter(Mandatory = $true)][string]$NinjaExecutable,
    [Parameter(Mandatory = $true)][int]$CodePage,
    [string[]]$Targets = @()
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'build-evidence.ps1')

function Invoke-NinjaDependencyQuery {
    param([string]$Executable, [string]$Directory, [string[]]$Arguments, [int]$OutputCodePage)
    # Read native bytes: the PowerShell 5.1 pipeline decoder can corrupt paths.
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $Executable
    $info.WorkingDirectory = $Directory
    foreach ($argument in $Arguments) {
        if ($argument.Contains('"') -or $argument.EndsWith('\')) {
            throw "Unsupported Ninja query argument: $argument"
        }
    }
    $info.Arguments = (($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' ')
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $info
    $outputBytes = New-Object System.IO.MemoryStream
    $errorBytes = New-Object System.IO.MemoryStream
    try {
        if (-not $process.Start()) { throw 'Unable to start Ninja dependency query.' }
        $outputCopy = $process.StandardOutput.BaseStream.CopyToAsync($outputBytes)
        $errorCopy = $process.StandardError.BaseStream.CopyToAsync($errorBytes)
        $process.WaitForExit()
        [System.Threading.Tasks.Task]::WaitAll(@($outputCopy, $errorCopy))
        $encoding = [System.Text.Encoding]::GetEncoding($OutputCodePage,
            [System.Text.EncoderFallback]::ExceptionFallback,
            [System.Text.DecoderFallback]::ExceptionFallback)
        $outputText = $encoding.GetString($outputBytes.ToArray())
        $errorText = $encoding.GetString($errorBytes.ToArray())
        if ($process.ExitCode -ne 0) {
            throw "Ninja dependency query failed ($($process.ExitCode)): $errorText"
        }
        return $outputText
    } finally {
        $outputBytes.Dispose()
        $errorBytes.Dispose()
        $process.Dispose()
    }
}

function ConvertTo-NinjaDependencyPath {
    param([string]$Path, [string]$BaseDirectory)
    $nativePath = $Path.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
    if (-not [System.IO.Path]::IsPathRooted($nativePath)) {
        $nativePath = Join-Path $BaseDirectory $nativePath
    }
    return [System.IO.Path]::GetFullPath($nativePath).TrimEnd('\', '/').ToUpperInvariant()
}

function ConvertFrom-NinjaDependencies {
    param([string]$Text, [string]$Directory)
    $records = @{}
    $current = $null
    foreach ($line in ($Text -split '\r?\n')) {
        if ($line -match '^(?<object>.+): #deps (?<count>\d+), deps mtime \d+ \((?<status>[^)]+)\)$') {
            $key = ConvertTo-NinjaDependencyPath $Matches.object $Directory
            if ($records.ContainsKey($key)) { throw "Duplicate Ninja dependency record: $key" }
            $current = [ordered]@{
                count = [int]$Matches.count
                status = $Matches.status
                headers = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
                entries = 0
            }
            $records[$key] = $current
        } elseif ($null -ne $current -and $line.StartsWith('    ')) {
            $header = ConvertTo-NinjaDependencyPath $line.Substring(4) $Directory
            [void]$current.headers.Add($header)
            $current.entries++
        } elseif (-not [string]::IsNullOrWhiteSpace($line)) {
            throw "Unrecognized Ninja dependency output: $line"
        } else {
            $current = $null
        }
    }
    return $records
}

function Select-NinjaDependencyRepresentatives {
    param([object[]]$Representatives, [string[]]$SelectedTargets, [string]$Commands, [string]$Directory)
    if ($SelectedTargets.Count -eq 0 -or $SelectedTargets -contains 'all') { return $Representatives }
    $normalizedCommands = $Commands.Replace('\', '/')
    return @($Representatives | Where-Object {
        $relativeObject = ([string]$_.object).Replace('\', '/')
        $absoluteObject = (ConvertTo-NinjaDependencyPath $_.object $Directory).Replace('\', '/')
        $objectExpression = '(?:' + [regex]::Escape($relativeObject) + '|' + [regex]::Escape($absoluteObject) + ')'
        $normalizedCommands -match ('(?i)(?:/Fo"?|["\s])' + $objectExpression + '(?:["\s]|$)')
    })
}

function Test-NinjaDependencyRepresentatives {
    param([object[]]$Representatives, [hashtable]$Records, [string]$Directory, [string]$Root)
    return @(
        foreach ($representative in $Representatives) {
            $object = ConvertTo-NinjaDependencyPath $representative.object $Directory
            $header = ConvertTo-NinjaDependencyPath $representative.header $Root
            $record = $Records[$object]
            $errorText = $null
            if (-not [System.IO.File]::Exists($object)) {
                $errorText = 'Required representative object is missing.'
            } elseif ($null -eq $record) {
                $errorText = 'Ninja header dependency record is missing.'
            } elseif ($record.status -ne 'VALID') {
                $errorText = "Ninja header dependency record is $($record.status)."
            } elseif ($record.count -eq 0) {
                $errorText = 'Ninja header dependency record has zero entries.'
            } elseif ($record.count -ne $record.entries) {
                $errorText = 'Ninja header dependency count does not match its entries.'
            } elseif (-not $record.headers.Contains($header)) {
                $errorText = 'Expected representative header is missing from Ninja dependencies.'
            }
            [ordered]@{
                target = $representative.target
                object = $representative.object
                expectedHeader = $representative.header
                dependencyCount = $(if ($null -eq $record) { $null } else { $record.count })
                dependencyStatus = $(if ($null -eq $record) { $null } else { $record.status })
                passed = ($null -eq $errorText)
                error = $errorText
            }
        }
    )
}

$resolvedDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$resolvedRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
$evidencePath = Join-Path $resolvedDirectory '.mgstc-deps-validation.json'
$evidence = [ordered]@{
    status = 'Failed'
    checkedUtc = [DateTime]::UtcNow.ToString('o')
    buildDirectory = $resolvedDirectory
    projectRoot = $resolvedRoot
    codePage = $CodePage
    targets = @($Targets)
    representatives = @()
    error = $null
}
try {
    $metadataPath = Join-Path $resolvedDirectory '.mgstc-deps-representatives.json'
    if (-not [System.IO.File]::Exists($metadataPath)) { throw "Missing dependency guard metadata: $metadataPath" }
    $parsedRepresentatives = Get-Content -LiteralPath $metadataPath -Raw -Encoding UTF8 | ConvertFrom-Json
    # Windows PowerShell 5.1 emits a JSON array as one pipeline object. Enumerate
    # it explicitly so each target/object/header remains a scalar record.
    $representatives = @(foreach ($representative in $parsedRepresentatives) { $representative })
    if ($representatives.Count -eq 0) { throw 'Dependency guard metadata has no representatives.' }
    foreach ($representative in $representatives) {
        if ([string]::IsNullOrWhiteSpace($representative.target) -or
            [string]::IsNullOrWhiteSpace($representative.object) -or
            [string]::IsNullOrWhiteSpace($representative.header)) {
            throw 'Dependency guard metadata requires target, object and header for every representative.'
        }
    }
    $commands = ''
    if ($Targets.Count -gt 0 -and $Targets -notcontains 'all') {
        $commands = Invoke-NinjaDependencyQuery $NinjaExecutable $resolvedDirectory (@('-t', 'commands') + $Targets) $CodePage
    }
    $required = @(Select-NinjaDependencyRepresentatives $representatives $Targets $commands $resolvedDirectory)
    if ($required.Count -eq 0) {
        $evidence.status = 'NotApplicable'
        Write-Host 'Ninja header dependency guard: selected target graph has no representative translation units.'
    } else {
        $requiredObjects = @($required | ForEach-Object { [string]$_.object } | Select-Object -Unique)
        $depsText = Invoke-NinjaDependencyQuery $NinjaExecutable $resolvedDirectory (@('-t', 'deps') + $requiredObjects) $CodePage
        $records = ConvertFrom-NinjaDependencies $depsText $resolvedDirectory
        $checked = @(Test-NinjaDependencyRepresentatives $required $records $resolvedDirectory $resolvedRoot)
        $evidence.representatives = $checked
        $failed = @($checked | Where-Object { -not $_.passed })
        if ($failed.Count -gt 0) {
            throw (($failed | ForEach-Object { "$($_.target): $($_.object): $($_.error)" }) -join [Environment]::NewLine)
        }
        $evidence.status = 'Passed'
        Write-Host "Ninja header dependency guard: $($checked.Count) representative records passed."
    }
} catch {
    $evidence.error = $_.Exception.Message
    throw
} finally {
    Write-BuildEvidence $evidencePath ($evidence | ConvertTo-Json -Depth 8)
}
