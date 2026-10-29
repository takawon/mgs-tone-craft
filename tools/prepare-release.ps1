# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param(
    [string]$Version = '',
    [string]$CandidateCommit = '',
    [Parameter(Mandatory=$true)][string]$PreviousTag,
    [Parameter(Mandatory=$true)][string]$PreviousCommit,
    [Parameter(Mandatory=$true)][string]$DbSourceDirectory,
    [string]$BuildDirectory = 'build',
    [string]$OutputDirectory = '',
    [string]$IsccPath = '',
    [string]$ReviewFile = '',
    [string]$ReleaseNotesFile = '',
    [string]$ChangeAuditFile = '',
    [switch]$Prototype,
    [Alias('DryRun')][switch]$ValidateOnly,
    [switch]$UseValidatedBuild
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'release-common.ps1')
. (Join-Path $PSScriptRoot 'distribution-common.ps1')
$root=Split-Path -Parent $PSScriptRoot
$actualVersion=([IO.File]::ReadAllText((Join-Path $root 'VERSION'))).Trim()
if (-not $Version) { $Version=$actualVersion }
if ($Version -cne $actualVersion -or $Version -notmatch '^\d+\.\d+$') { throw 'Requested version must match VERSION.' }
if ($PreviousTag -notmatch '^v\d+\.\d+$' -or $PreviousCommit -notmatch '^[a-f0-9]{40}$') { throw 'Specify the published baseline tag and full actual commit SHA.' }
$before=Get-MgstcReleaseSourceState $root
if (-not $CandidateCommit) { $CandidateCommit=$before.head }
if ($CandidateCommit -cne $before.head -or $CandidateCommit -notmatch '^[a-f0-9]{40}$') { throw 'The candidate must be the current full HEAD SHA.' }
if (-not $Prototype -and $before.dirty.Count -gt 0) { throw 'A public candidate requires a clean committed checkout. Use -Prototype for local experiments.' }
$baseline=[string](Invoke-MgstcReleaseGit $root @('rev-parse',"$PreviousCommit^{commit}"))
if ($baseline.Trim() -cne $PreviousCommit) { throw 'Published baseline commit is not available locally.' }
[void](Invoke-MgstcReleaseGit $root @('merge-base','--is-ancestor',$PreviousCommit,$CandidateCommit))
$tree=Test-MgstcReleaseTree $root $CandidateCommit
if (-not $Prototype -and $tree.digest -cne $before.digest) { throw 'The tag source tree does not match the candidate files.' }
$docs=Test-MgstcReleaseDocuments -ProjectRoot $root -Version $Version -PreviousCommit $PreviousCommit -RequireTracked:(!$Prototype) -RequirePublic:(!$Prototype)
$releaseContent=Get-MgstcPreparedReleaseContent -ProjectRoot $root -Version $Version -PreviousTag $PreviousTag -PreviousCommit $PreviousCommit -CandidateCommit $CandidateCommit -ChangeAuditFile $ChangeAuditFile -ReleaseNotesFile $ReleaseNotesFile -Prototype:$Prototype
$review=$null
if ($ReviewFile) { $review=Get-Content -LiteralPath $ReviewFile -Raw -Encoding UTF8 | ConvertFrom-Json }
if (-not $Prototype -and (-not $ValidateOnly -or $null -ne $review)) { Test-MgstcReleaseReview $review $before.digest $docs.digest $PreviousCommit }
if (-not $Prototype -and (-not $ValidateOnly -or $null -ne $review)) { Assert-MgstcReleaseContentReview $review $releaseContent.digest }
$changes=@(Invoke-MgstcReleaseGit $root @('log','--format=%h %s',"$PreviousCommit..$CandidateCommit"))
$changeFiles=@(Invoke-MgstcReleaseGit $root @('diff','--name-only',$PreviousCommit,$CandidateCommit))
Write-Host "Candidate v$Version / $CandidateCommit; published baseline $PreviousTag / $PreviousCommit"
Write-Host "Source digest: $($before.digest)"
Write-Host "Documents digest: $($docs.digest); $($docs.linkCount) links checked"
Write-Host "Release content digest: $($releaseContent.digest); classification and canonical Notes require matching review"
if ($Prototype) { Write-Warning 'PROTOTYPE: this candidate cannot be staged or published.' }
foreach($warning in $docs.warnings) { Write-Warning $warning }
if ($ValidateOnly) {
    $inputInfo=Get-MgstcDistributionInput -ProjectRoot $root -DbSourceDirectory $DbSourceDirectory -BuildDirectory $BuildDirectory -IsccPath $IsccPath
    [pscustomobject]@{mode='ValidateOnly';version=$Version;commit=$CandidateCommit;sourceDigest=$before.digest;documentsDigest=$docs.digest;releaseContentDigest=$releaseContent.digest;previousTag=$PreviousTag;previousCommit=$PreviousCommit;prototype=[bool]$Prototype;reviewRequired=(-not $review);changes=$changes;changedFiles=$changeFiles}
    return
}
if (-not $OutputDirectory) { $OutputDirectory=Join-Path $root ('dist/release-candidates/' + $Version + '-' + [datetime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8)) }
if (-not [IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory=Join-Path $root $OutputDirectory }
$output=[IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a new output directory. Existing candidates are never overwritten.' }
[void](New-Item -ItemType Directory -Path $output)
$lock=$null
try {
    $lock=[IO.File]::Open((Join-Path $output '.prepare.lock'),[IO.FileMode]::CreateNew,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    if (-not $UseValidatedBuild) {
        $resolvedBuild=if([IO.Path]::IsPathRooted($BuildDirectory)){$BuildDirectory}else{Join-Path $root $BuildDirectory}
        $dependencyRoot=Join-Path $resolvedBuild '_deps'
        foreach($dependency in @('juce-src','rpclib-src')) {
            if (-not (Test-Path -LiteralPath (Join-Path $dependencyRoot "$dependency/CMakeLists.txt"))) { throw 'Offline preparation requires dependency sources already acquired by a regular build.' }
        }
        $testDefinition=Get-Content -LiteralPath (Join-Path $root 'installer/distribution.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        $testSource=Resolve-MgstcReleaseDatabase -SourceDirectory $DbSourceDirectory -Definition $testDefinition -ProjectRoot $root
        Assert-MgstcDatabase $testSource.files $testDefinition
        $testDb=Join-Path $output '.test-db'
        [void](New-Item -ItemType Directory -Path $testDb)
        foreach($name in $testSource.files.Keys) {Copy-Item -LiteralPath $testSource.files[$name] -Destination (Join-Path $testDb $name)}
        $previousTestDb=[Environment]::GetEnvironmentVariable('MGSTC_PATCHSLIDE_TEST_DB_DIR','Process')
        try {
            [Environment]::SetEnvironmentVariable('MGSTC_PATCHSLIDE_TEST_DB_DIR',$testDb,'Process')
            & (Join-Path $root 'tools/build.cmd') -Configuration Release -BuildDirectory $BuildDirectory -DependencySourceRoot $dependencyRoot
            if ($LASTEXITCODE -ne 0) { throw 'Release build or tests failed.' }
        } finally {[Environment]::SetEnvironmentVariable('MGSTC_PATCHSLIDE_TEST_DB_DIR',$previousTestDb,'Process')}
    }
    $build=if([IO.Path]::IsPathRooted($BuildDirectory)){$BuildDirectory}else{Join-Path $root $BuildDirectory}
    $evidence=Get-Content -LiteralPath (Join-Path $build '.mgstc-validation.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($evidence.status -ne 'BuildAndSelectedTestsPassed' -or $evidence.configuration -ne 'Release' -or $evidence.testRegex -or $evidence.sourceBefore.digest -cne $evidence.sourceAfter.digest) { throw 'Preparation requires a complete Release build and full regression run through tools/build.cmd.' }
    foreach($f in $evidence.sourceAfter.files.PSObject.Properties) {
        $full=Join-Path $root $f.Name
        if ((Get-MgstcReleaseSha256 $full) -ine $f.Value) { throw "Source changed since build: $($f.Name)" }
    }
    $inputInfo=Get-MgstcDistributionInput -ProjectRoot $root -DbSourceDirectory $DbSourceDirectory -BuildDirectory $BuildDirectory -IsccPath $IsccPath
    $stage=Join-Path $output 'payload'
    $payload=New-MgstcDistributionStage -Input $inputInfo -StageDirectory $stage
    $setup=Invoke-MgstcInstallerBuild -StageDirectory $stage -OutputDirectory $output -IsccPath $inputInfo.compiler
    $zip=New-MgstcManualZip -StageDirectory $stage -OutputDirectory $output
    Test-MgstcDistributionStage -StageDirectory $stage | Out-Null
    Test-MgstcManualZip -StageDirectory $stage -ZipPath (Join-Path $output "MGSToneCraft-$Version-win64.zip") | Out-Null
    $assets=@()
    foreach($name in @("MGSToneCraft-$Version-win64-setup.exe","MGSToneCraft-$Version-win64.zip")) {
        $path=Join-Path $output $name
        $assets += [pscustomobject]@{name=$name;size=(Get-Item -LiteralPath $path).Length;sha256=Get-MgstcReleaseSha256 $path}
    }
    $sum=(@($assets | ForEach-Object {$_.sha256+'  '+$_.name}) -join "`n")+"`n"
    Write-MgstcReleaseUtf8 (Join-Path $output 'SHA256SUMS.txt') $sum
    $assets += [pscustomobject]@{name='SHA256SUMS.txt';size=(Get-Item -LiteralPath (Join-Path $output 'SHA256SUMS.txt')).Length;sha256=Get-MgstcReleaseSha256 (Join-Path $output 'SHA256SUMS.txt')}
    $notes=$releaseContent.notes
    Assert-MgstcReleaseNotes $notes $Version
    Write-MgstcReleaseUtf8 (Join-Path $output 'RELEASE_NOTES.md') $notes
    $after=Get-MgstcReleaseSourceState $root
    if ($after.head -cne $before.head -or $after.digest -cne $before.digest) { throw 'Source changed during preparation.' }
    $definition=Get-Content -LiteralPath (Join-Path $root 'installer/distribution.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $payloadFiles=@(Get-MgstcZipInventory $payload)
    $manifest=[ordered]@{schemaVersion=1;state='Ready';version=$Version;tag='v'+$Version;commit=$CandidateCommit;previousTag=$PreviousTag;previousCommit=$PreviousCommit;prototype=[bool]$Prototype;sourceDigest=$before.digest;documentsDigest=$docs.digest;notesSha256=Get-MgstcReleaseSha256 (Join-Path $output 'RELEASE_NOTES.md');assets=$assets;assetsDigest=Get-MgstcReleaseAssetDigest $assets;payloadFiles=$payloadFiles;payloadDigest=Get-MgstcReleaseFilesDigest $payloadFiles;dbFiles=$definition.dbFiles;review=$review;releaseContent=$releaseContent.binding;build=[ordered]@{configuration='Release';tests='full';sourceDigest=$evidence.sourceAfter.digest};createdUtc=[datetimeoffset]::UtcNow.ToString('o')}
    Write-MgstcReleaseUtf8 (Join-Path $output 'candidate.json') ($manifest | ConvertTo-Json -Depth 20)
    Write-MgstcReleaseUtf8 (Join-Path $output 'changes.json') ([ordered]@{previousTag=$PreviousTag;previousCommit=$PreviousCommit;candidateCommit=$CandidateCommit;commits=$changes;files=$changeFiles;requiredCategories=@('feature','operation','ui','io','compatibility','distribution','fix');humanClassificationRequired=$true} | ConvertTo-Json -Depth 8)
    $checked=Test-MgstcReleaseCandidate -CandidateDirectory $output -RequirePublic:(!$Prototype)
    Write-Host "Prepared locally: $output"
    Write-Host "Assets digest: $($checked.assetsDigest). No tag, upload or release was created."
} catch {
    if (Test-Path -LiteralPath (Join-Path $output 'candidate.json')) { Remove-Item -LiteralPath (Join-Path $output 'candidate.json') -Force }
    Write-MgstcReleaseUtf8 (Join-Path $output 'FAILED.txt') 'Preparation failed. This directory is not a release candidate. Do not upload its files.'
    throw
} finally { if($lock){$lock.Dispose()} }
