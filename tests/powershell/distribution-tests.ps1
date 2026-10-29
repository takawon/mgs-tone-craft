[CmdletBinding()]
param(
    [string]$StageDirectory='',
    [string]$SetupPath='',
    [string]$ProjectRoot='',
    [string]$ScratchDirectory=''
)
$ErrorActionPreference='Stop'
if(-not $ProjectRoot){$ProjectRoot=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)}
Set-StrictMode -Version Latest
. (Join-Path $ProjectRoot 'tools/distribution-common.ps1')
$count=0
function Expect-Failure([string]$CaseDescription,[scriptblock]$CaseAction) {
    $caseFailed=$false
    try { & $CaseAction | Out-Null } catch { $caseFailed=$true }
    if(-not $caseFailed){throw "Expected rejection: $CaseDescription"}
    $script:count++; Write-Host "PASS rejection: $CaseDescription"
}
function Open-TestZipForUpdate([string]$Path) {
    # Permit transient read-only observers of scratch ZIPs, while excluding
    # other writers. Production packaging still uses its existing access modes.
    # Retry incompatible sharing/lock violations only, for at most 2 seconds.
    for($attempt=0;$attempt -le 10;$attempt++) {
        $stream=$null
        try {
            $stream=[IO.FileStream]::new($Path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
            return [IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Update,$false)
        }
        catch {
            if($stream){$stream.Dispose()}
            $cause=$_.Exception.GetBaseException()
            if($cause -isnot [IO.IOException] -or ($cause.HResult -band 0xffff) -notin @(32,33) -or $attempt -eq 10){throw}
            Start-Sleep -Milliseconds 200
        }
    }
}
if(-not $ScratchDirectory){$ScratchDirectory=Join-Path $ProjectRoot 'build/distribution-test-runs'}
$scratchRoot=Get-MgstcAbsolutePath $ScratchDirectory $ProjectRoot
$scratch=Join-Path $scratchRoot ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch -Force | Out-Null
try {
    foreach($name in @('../escape','/absolute','C:/outside','a/../b','a\b')){Expect-Failure "unsafe relative path $name" {Assert-MgstcRelativePath $name}}
    $definition=Get-Content -LiteralPath (Join-Path $ProjectRoot 'installer/distribution.json') -Raw -Encoding UTF8 | ConvertFrom-Json

    Test-MgstcManualInstallGuides $ProjectRoot $definition -SourcePaths
    $count++;Write-Host 'PASS bilingual source guides: UTF-8 BOM, CRLF, sections and shared facts'
    foreach($guide in @('INSTALL.txt','INSTALL.en.txt')) {
        $good=[IO.File]::ReadAllBytes((Join-Path $ProjectRoot ('installer/'+$guide)))
        Expect-Failure "missing BOM $guide" {Assert-MgstcManualInstallText ([byte[]]$good[3..($good.Length-1)]) $guide $definition}
        Expect-Failure "invalid UTF-8 $guide" {Assert-MgstcManualInstallText ([byte[]](239,187,191,255)) $guide $definition}
        $text=[Text.Encoding]::UTF8.GetString($good)
        $wrong=$text.Replace('%LOCALAPPDATA%\MgsToneCraft\','%LOCALAPPDATA%\WrongProduct\')
        Expect-Failure "wrong library path $guide" {Assert-MgstcManualInstallText ([Text.Encoding]::UTF8.GetBytes($wrong)) $guide $definition}
        Expect-Failure "bare LF $guide" {Assert-MgstcManualInstallText ([Text.Encoding]::UTF8.GetBytes($text.Replace("`r`n","`n"))) $guide $definition}
        Expect-Failure "missing section $guide" {Assert-MgstcManualInstallText ([Text.Encoding]::UTF8.GetBytes($text.Replace('5. ','X. '))) $guide $definition}
    }
    $badDefinition=Get-Content -LiteralPath (Join-Path $ProjectRoot 'installer/distribution.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $badDefinition.manualInstallFiles.PSObject.Properties.Remove('INSTALL.en.txt')
    Expect-Failure 'English guide omitted from distribution definition' {Test-MgstcManualInstallGuides $ProjectRoot $badDefinition -SourcePaths}

    Assert-MgstcProductBundleInventory @($definition.productBundleFiles) $definition
    $count++;Write-Host 'PASS exact product bundle allowlist'
    foreach($name in @('Contents/Resources/tone_library.sqlite','Contents/settings-v1.ini','Contents/crash.dmp','Contents/unexpected.exe')) {
        $badNames=@($definition.productBundleFiles)+@($name)
        Expect-Failure "unknown bundle file $name" {Assert-MgstcProductBundleInventory $badNames $definition}
    }
    Expect-Failure 'missing product bundle binary' {Assert-MgstcProductBundleInventory @('Contents/Resources/moduleinfo.json') $definition}
    Expect-Failure 'runtime DB is not an acquisition source' {Resolve-MgstcReleaseDatabase -SourceDirectory (Join-Path $env:ProgramData 'OPLL_PatchSlide_DB') -Definition $definition -ProjectRoot $ProjectRoot}
    $empty=Join-Path $scratch 'empty-db';New-Item -ItemType Directory -Path $empty | Out-Null
    Expect-Failure 'incomplete formal DB set' {Resolve-MgstcReleaseDatabase -SourceDirectory $empty -Definition $definition -ProjectRoot $ProjectRoot}
    $fakeBuild=Join-Path $scratch 'invalid-build';New-Item -ItemType Directory -Path $fakeBuild | Out-Null
    $cachePath=Join-Path $fakeBuild 'CMakeCache.txt'
    [IO.File]::WriteAllText($cachePath,"CMAKE_BUILD_TYPE:STRING=Debug`nMGSTC_VST3_DIAG_MODE:STRING=OFF`n")
    Expect-Failure 'Debug build configuration' {Get-MgstcDistributionInput -ProjectRoot $ProjectRoot -DbSourceDirectory $empty -BuildDirectory $fakeBuild}
    [IO.File]::WriteAllText($cachePath,"CMAKE_BUILD_TYPE:STRING=Release`nMGSTC_VST3_DIAG_MODE:STRING=LATENCY0`n")
    Expect-Failure 'diagnostic VST3 build configuration' {Get-MgstcDistributionInput -ProjectRoot $ProjectRoot -DbSourceDirectory $empty -BuildDirectory $fakeBuild}
    $dummy=Join-Path $scratch 'dummy';[IO.File]::WriteAllText($dummy,'not a product')
    Expect-Failure 'non-PE product' {Assert-MgstcProduct $dummy '1.2.0'}
    Expect-Failure 'non-PE setup bootstrap' {Assert-MgstcSetup $dummy '1.2.0'}
    if($SetupPath) {
        $version=(Get-Content -LiteralPath (Join-Path $ProjectRoot 'VERSION') -Raw -Encoding UTF8).Trim()+'.0'
        Assert-MgstcSetup (Get-MgstcAbsolutePath $SetupPath $ProjectRoot) $version
        $count++;Write-Host 'PASS real pinned x86 setup bootstrap with product version/publisher checks'
        Expect-Failure 'setup bootstrap is not an x64 product binary' {Assert-MgstcProduct (Get-MgstcAbsolutePath $SetupPath $ProjectRoot) $version}
    } else {Write-Host 'Real setup validation requires -SetupPath; not claimed as passed.'}
    Expect-Failure 'unapproved compiler' {Get-MgstcCompiler $dummy $ProjectRoot $definition.compilerVersion $definition.compilerSha256 $definition.compilerSupportFiles}
    $fakeDb=[ordered]@{}
    foreach($name in $definition.dbFiles.PSObject.Properties.Name){$fakeDb[$name]=$dummy}
    Expect-Failure 'mixed/unapproved DB bytes' {Assert-MgstcDatabase $fakeDb $definition}
    $invalidModule=Join-Path $scratch 'moduleinfo.json'
    [IO.File]::WriteAllText($invalidModule,'{"Version":"1.2.0","Vendor":"Other","CID":"diagnostic"}')
    Expect-Failure 'diagnostic/foreign VST3 metadata' {Assert-MgstcModule $invalidModule '1.2.0'}
    if($StageDirectory) {
        $actualStage=Get-MgstcAbsolutePath $StageDirectory $ProjectRoot
        $actualReport=Test-MgstcDistributionStage -StageDirectory $actualStage -ProjectRoot $ProjectRoot
        $clone=Join-Path $scratch 'stage'
        Copy-Item -LiteralPath $actualStage -Destination $clone -Recurse
        Test-MgstcDistributionStage -StageDirectory $clone -ProjectRoot $ProjectRoot | Out-Null
        $count++;Write-Host 'PASS real common stage validation'
        $extra=Join-Path $clone 'app/settings-v1.ini';[IO.File]::WriteAllText($extra,'unapproved')
        Expect-Failure 'unexpected staged user settings' {Test-MgstcDistributionStage -StageDirectory $clone -ProjectRoot $ProjectRoot}
        Remove-Item -LiteralPath $extra
        foreach($relative in @('app/INSTALL.txt','app/INSTALL.en.txt','db/patchslide.sqlite','db/patchslide_search.sqlite','db/manifest.json','app/licenses/Inno-Setup-LICENSE.txt','vst3/Contents/x86_64-win/MGS Tone Craft.vst3')) {
            $path=Join-Path $clone $relative;$backup=Join-Path $scratch 'entry-backup'
            Move-Item -LiteralPath $path -Destination $backup
            try {
                Expect-Failure "missing $relative" {Test-MgstcDistributionStage -StageDirectory $clone -ProjectRoot $ProjectRoot}
                [IO.File]::WriteAllText($path,'wrong bytes')
                Expect-Failure "tampered $relative" {Test-MgstcDistributionStage -StageDirectory $clone -ProjectRoot $ProjectRoot}
                Remove-Item -LiteralPath $path
            } finally {Move-Item -LiteralPath $backup -Destination $path}
        }
        $payloadPath=Join-Path $clone 'payload.json';$original=[IO.File]::ReadAllText($payloadPath)
        $bad=$original | ConvertFrom-Json;$bad.version='99.999';$bad.binaryVersion='99.999.0'
        [IO.File]::WriteAllText($payloadPath,($bad|ConvertTo-Json -Depth 8))
        Expect-Failure 'wrong staged product version' {Test-MgstcDistributionStage -StageDirectory $clone -ProjectRoot $ProjectRoot}
        [IO.File]::WriteAllText($payloadPath,$original)
        $bad=$original | ConvertFrom-Json;$bad.installerOnlyFiles=@('app/mgstc.exe')
        [IO.File]::WriteAllText($payloadPath,($bad|ConvertTo-Json -Depth 8))
        Expect-Failure 'altered ZIP exclusion mapping' {Test-MgstcDistributionStage -StageDirectory $clone -ProjectRoot $ProjectRoot}
        [IO.File]::WriteAllText($payloadPath,$original)
        $zip=New-MgstcManualZip -StageDirectory $clone -OutputDirectory (Join-Path $scratch 'output') -ProjectRoot $ProjectRoot
        $count++;Write-Host 'PASS real ZIP creation, stream SHA and extraction SHA'
        Expect-Failure 'stale ZIP cannot be overwritten' {New-MgstcManualZip -StageDirectory $clone -OutputDirectory (Join-Path $scratch 'output') -ProjectRoot $ProjectRoot}
        $archive=Open-TestZipForUpdate $zip.path
        try {$entry=$archive.CreateEntry('settings-v1.ini');$writer=New-Object IO.StreamWriter($entry.Open());try{$writer.Write('not permitted')}finally{$writer.Dispose()}}finally{$archive.Dispose()}
        Expect-Failure 'unexpected ZIP user data' {Test-MgstcManualZip -ZipPath $zip.path -StageDirectory $clone -ProjectRoot $ProjectRoot}
        $archive=Open-TestZipForUpdate $zip.path
        try{$archive.GetEntry('settings-v1.ini').Delete();$archive.GetEntry('mgstc.exe').Delete()}finally{$archive.Dispose()}
        Expect-Failure 'missing ZIP product' {Test-MgstcManualZip -ZipPath $zip.path -StageDirectory $clone -ProjectRoot $ProjectRoot}
        if((Get-FileHash -LiteralPath (Join-Path $actualStage 'payload.json')).Hash -ne (Get-FileHash -LiteralPath (Join-Path $clone 'payload.json')).Hash){throw 'Original stage inventory was altered.'}
    } else {Write-Host 'Real-stage/ZIP cases require -StageDirectory; not claimed as passed.'}
    Write-Output "Distribution checks passed: $count"
} finally {
    $resolved=[IO.Path]::GetFullPath($scratch)
    if((Split-Path -Parent $resolved) -ne $scratchRoot -or (Split-Path -Leaf $resolved) -notmatch '^[a-f0-9]{32}$'){throw 'Unsafe distribution-test scratch cleanup path.'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
