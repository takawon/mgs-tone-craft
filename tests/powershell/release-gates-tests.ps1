# Focused unit coverage for the read-only public release gates.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
. (Join-Path $repo 'tools\release-common.ps1')
$script:checks = 0
function Assert-True([bool]$Condition,[string]$Message) { $script:checks++; if (-not $Condition) { throw "ASSERTION FAILED: $Message" } }
function Assert-Throws([scriptblock]$Action,[string]$Message) { $script:checks++; try { & $Action } catch { return }; throw "ASSERTION FAILED: expected rejection: $Message" }
function Write-TestText([string]$Path,[string]$Text) { $parent=Split-Path -Parent $Path; if (-not (Test-Path -LiteralPath $parent)) { [void](New-Item -ItemType Directory -Path $parent -Force) }; [IO.File]::WriteAllText($Path,$Text,(New-Object Text.UTF8Encoding($false))) }
# Git is deliberately mocked: no repository, commit, index, or ref is created.
$script:releaseTrackedFiles=@()
function Invoke-MgstcReleaseGit([string]$ProjectRoot,[string[]]$Arguments) { if ($Arguments.Count -eq 1 -and $Arguments[0] -eq 'ls-files') { return $script:releaseTrackedFiles }; throw "Unexpected Git operation in unit test: $($Arguments -join ' ')" }
function New-DocumentFixture([string]$Root) {
 [void](New-Item -ItemType Directory -Path $Root -Force)
 $readme=@"
# MGS Tone Craft

Version 0.365

<!-- mgstc:download:start -->
## Download

[Releases](https://github.com/takawon/mgs-tone-craft/releases)
Installer EXE: MGSToneCraft-0.365-win64-setup.exe.
Manual ZIP: MGSToneCraft-0.365-win64.zip; INSTALL.txt and INSTALL.en.txt.
Source ZIP / tar.gz archives require a build.
[Guide](docs/USER_GUIDE.md#basics)
[Gallery](docs/screenshots/README.md)
[Changes](CHANGELOG.md)
![Main window](docs/screenshots/main.png)
<!-- mgstc:download:end -->

<!-- mgstc:whats-new:start -->
## What's New — 0.365

New shared editing and installation features for the candidate version.
<!-- mgstc:whats-new:end -->
"@
 Write-TestText (Join-Path $Root 'README.md') $readme; Write-TestText (Join-Path $Root 'BUILDING.md') "# Building`n"
 Write-TestText (Join-Path $Root 'CHANGELOG.md') "## [0.365] - 2026-10-28`n"; Write-TestText (Join-Path $Root 'THIRD_PARTY.md') "# Third party notices`n"
 Write-TestText (Join-Path $Root 'LICENSE') "AGPL-3.0-only`n"; Write-TestText (Join-Path $Root 'docs\USER_GUIDE.md') "# User guide`n`n## Basics`n"
 Write-TestText (Join-Path $Root 'docs\screenshots\README.md') "# Screenshots`n`n![Main](main.png)`n"; [IO.File]::WriteAllBytes((Join-Path $Root 'docs\screenshots\main.png'),[byte[]](1,2,3,4))
 [void](New-Item -ItemType Directory -Path (Join-Path $Root 'installer') -Force)
 foreach($name in @('distribution.json','INSTALL.txt','INSTALL.en.txt')) { Copy-Item -LiteralPath (Join-Path $repo ('installer/'+$name)) -Destination (Join-Path $Root ('installer/'+$name)) }
 $script:releaseTrackedFiles=@('installer/INSTALL.txt','installer/INSTALL.en.txt','README.md','BUILDING.md','CHANGELOG.md','THIRD_PARTY.md','LICENSE','docs/USER_GUIDE.md','docs/screenshots/README.md','docs/screenshots/main.png')
}
function New-CandidateFixture([string]$Root) {
 [void](New-Item -ItemType Directory -Path $Root -Force); $setupName='MGSToneCraft-0.365-win64-setup.exe'; $zipName='MGSToneCraft-0.365-win64.zip'
 # Opaque test bytes only; these are not build products or build evidence.
 [IO.File]::WriteAllBytes((Join-Path $Root $setupName),[byte[]](10,20,30)); [IO.File]::WriteAllBytes((Join-Path $Root $zipName),[byte[]](40,50,60,70))
 $setup=[pscustomobject]@{name=$setupName;size=(Get-Item (Join-Path $Root $setupName)).Length;sha256=(Get-MgstcReleaseSha256 (Join-Path $Root $setupName))}; $zip=[pscustomobject]@{name=$zipName;size=(Get-Item (Join-Path $Root $zipName)).Length;sha256=(Get-MgstcReleaseSha256 (Join-Path $Root $zipName))}
 $assets=@($setup,$zip); $sum=(($assets|ForEach-Object{$_.sha256+'  '+$_.name}) -join "`n")+"`n"; Write-TestText (Join-Path $Root 'SHA256SUMS.txt') $sum
 $assets += [pscustomobject]@{name='SHA256SUMS.txt';size=(Get-Item (Join-Path $Root 'SHA256SUMS.txt')).Length;sha256=(Get-MgstcReleaseSha256 (Join-Path $Root 'SHA256SUMS.txt'))}
 Write-TestText (Join-Path $Root 'RELEASE_NOTES.md') "# MGS Tone Craft 0.365 Alpha`nRelease notes fixture for 0.365.`n"
 $m=[ordered]@{schemaVersion=1;state='Ready';version='0.365';tag='v0.365';commit=('a'*40);previousCommit=('b'*40);prototype=$false;assets=$assets;assetsDigest=(Get-MgstcReleaseAssetDigest $assets);notesSha256=(Get-MgstcReleaseSha256 (Join-Path $Root 'RELEASE_NOTES.md'));sourceDigest=('c'*64);documentsDigest=('d'*64)}
 Write-TestText (Join-Path $Root 'candidate.json') (ConvertTo-Json -InputObject $m -Depth 8)
}
$tempRoot=Join-Path ([IO.Path]::GetTempPath()) ('mgstc-release-gates-tests-'+[guid]::NewGuid().ToString('N')); [void](New-Item -ItemType Directory -Path $tempRoot)
try {
 Assert-MgstcReleasePath 'tests/powershell/release-gates-tests.ps1'; Assert-MgstcReleasePath '.github/workflows/release.yml'
 Assert-Throws { Assert-MgstcReleasePath 'analysis/notes.md' } 'private source root'; Assert-Throws { Assert-MgstcReleasePath 'docs/.private/notes.md' } 'hidden path'; Assert-Throws { Assert-MgstcReleasePath 'docs/fixtures/sample.md' } 'fixture path'
 $personalPath='C:'+[char]92+'Users'+[char]92+'ExampleUser'+[char]92+'private.txt'; Assert-Throws { Assert-MgstcReleaseText 'README.md' $personalPath } 'personal path'
 Assert-Throws { Assert-MgstcReleaseText 'README.md' ('ghp_'+('x'*36)) } 'token-like text'; Assert-Throws { Assert-MgstcReleaseText 'README.md' ('github_pat_'+('x'*44)) } 'credential-like text'; Assert-MgstcReleaseText 'README.md' 'Generic public release documentation.'
 $docsRoot=Join-Path $tempRoot 'docs-fixture'; New-DocumentFixture $docsRoot
 $result=Test-MgstcReleaseDocuments -ProjectRoot $docsRoot -Version '0.365' -RequireTracked -RequirePublic
 Assert-True ($result.warnings.Count -eq 0) 'valid docs'; Assert-True ($result.linkCount -ge 5) 'recursive links and images'; Assert-True ($result.digest -match '^[a-f0-9]{64}$') 'docs SHA-256'
 $english=Join-Path $docsRoot 'installer/INSTALL.en.txt'; $englishBytes=[IO.File]::ReadAllBytes($english)
 Remove-Item -LiteralPath $english; Assert-Throws { Test-MgstcReleaseDocuments $docsRoot '0.365' } 'missing English guide'
 [IO.File]::WriteAllBytes($english,$englishBytes)
 $script:releaseTrackedFiles=@($script:releaseTrackedFiles|Where-Object{$_ -cne 'installer/INSTALL.en.txt'})
 Assert-Throws { Test-MgstcReleaseDocuments $docsRoot '0.365' -RequireTracked } 'untracked English guide'; $script:releaseTrackedFiles+='installer/INSTALL.en.txt'
 [IO.File]::AppendAllText($english,"Additional reviewed text.`r`n",[Text.UTF8Encoding]::new($false))
 Assert-True ((Test-MgstcReleaseDocuments $docsRoot '0.365').digest -cne $result.digest) 'guide changes invalidate document review digest'
 [IO.File]::WriteAllBytes($english,$englishBytes)
 $rp=Join-Path $docsRoot 'README.md'; $good=[IO.File]::ReadAllText($rp)
 Write-TestText $rp ($good.Replace('docs/USER_GUIDE.md#basics','docs/user_guide.md#basics')); Assert-Throws { Test-MgstcReleaseDocuments $docsRoot '0.365' } 'path case'
 Write-TestText $rp ($good.Replace('docs/USER_GUIDE.md#basics','docs/USER_GUIDE.md#missing')); Assert-Throws { Test-MgstcReleaseDocuments $docsRoot '0.365' } 'missing anchor'
 Write-TestText $rp ($good.Replace('docs/USER_GUIDE.md#basics','docs/missing.md')); Assert-Throws { Test-MgstcReleaseDocuments $docsRoot '0.365' } 'missing linked file'; Write-TestText $rp $good
 $gp=Join-Path $docsRoot 'docs\USER_GUIDE.md'; Write-TestText $gp ('# User guide'+"`n`n## Basics`n`n"+$personalPath)
 Assert-Throws { Test-MgstcReleaseDocuments $docsRoot '0.365' } 'personal data in linked document'; Write-TestText $gp "# User guide`n`n## Basics`n"
 $script:releaseTrackedFiles=@($script:releaseTrackedFiles|Where-Object{$_ -cne 'docs/USER_GUIDE.md'}); Assert-Throws { Test-MgstcReleaseDocuments $docsRoot '0.365' -RequireTracked } 'untracked reachable file'
 Assert-True ((Test-MgstcReleaseDocuments $docsRoot '0.365').digest -match '^[a-f0-9]{64}$') 'untracked prototype source allowed'; $script:releaseTrackedFiles+='docs/USER_GUIDE.md'
 Write-TestText $rp ($good.Replace('0.365','0.366')); Assert-Throws { Test-MgstcReleaseDocuments $docsRoot '0.365' -RequirePublic } 'README version mismatch'
 Write-TestText $rp ($good.Replace('## What' + [char]39 + 's New — 0.365','## What' + [char]39 + 's New — 0.364')); Assert-Throws { Test-MgstcReleaseDocuments $docsRoot '0.365' -RequirePublic } 'important changes heading version mismatch'
  Write-TestText $rp ($good.Replace('## What' + [char]39 + 's New — 0.365','## Highlights')); Assert-Throws { Test-MgstcReleaseDocuments $docsRoot '0.365' -RequirePublic } 'missing highlights'
 Write-TestText $rp ($good+"`nCurrent release: v0.297`n")
 Assert-Throws { Test-MgstcReleaseDocuments $docsRoot '0.365' -RequirePublic } 'stale latest despite current filenames'
 Assert-True ((Test-MgstcReleaseDocuments $docsRoot '0.365').warnings.Count -gt 0) 'prototype sees README failure as warnings'
 Write-TestText $rp ($good+"`n## Release history`nPast release v0.297`n")
 Assert-True ((Test-MgstcReleaseDocuments $docsRoot '0.365' -RequirePublic).warnings.Count -eq 0) 'old historical release allowed'
 Write-TestText $rp $good
 $script:releaseTrackedFiles=@($script:releaseTrackedFiles|Where-Object{$_ -cne 'docs/screenshots/main.png'})
 Assert-Throws { Test-MgstcReleaseDocuments $docsRoot '0.365' -RequireTracked -RequirePublic } 'untracked referenced image'
 $script:releaseTrackedFiles+='docs/screenshots/main.png'
 $review=[pscustomobject]@{accepted=$true;externalLinksAccepted=$true;screenshotReview='accepted';reviewer='Reviewer';reviewedAt=[datetimeoffset]::UtcNow.ToString('o');sourceDigest=('a'*64);documentsDigest=('b'*64);previousCommit=('c'*40);changeCategories=@('feature','operation','ui','io','compatibility','distribution','fix')}
 Test-MgstcReleaseReview $review ('a'*64) ('b'*64) ('c'*40); Assert-Throws { Test-MgstcReleaseReview $null ('a'*64) ('b'*64) ('c'*40) } 'missing review approval'
 $review.accepted=$false; Assert-Throws { Test-MgstcReleaseReview $review ('a'*64) ('b'*64) ('c'*40) } 'rejected review'; $review.accepted=$true
 Assert-Throws { Test-MgstcReleaseReview $review ('f'*64) ('b'*64) ('c'*40) } 'source digest binding'; Assert-Throws { Test-MgstcReleaseReview $review ('a'*64) ('f'*64) ('c'*40) } 'docs digest binding'; Assert-Throws { Test-MgstcReleaseReview $review ('a'*64) ('b'*64) ('f'*40) } 'baseline binding'
 $review.changeCategories=@('feature'); Assert-Throws { Test-MgstcReleaseReview $review ('a'*64) ('b'*64) ('c'*40) } 'missing categories'
 $cr=Join-Path $tempRoot 'candidate-fixture'; New-CandidateFixture $cr; $candidate=Test-MgstcReleaseCandidate $cr
 Assert-True ($candidate.state -eq 'Ready') 'valid candidate inventory'; $mp=Join-Path $cr 'candidate.json'; $valid=[IO.File]::ReadAllText($mp); $bad=$valid|ConvertFrom-Json; $bad.assets=@($bad.assets|Where-Object{$_.name -ne 'SHA256SUMS.txt'}); Write-TestText $mp (ConvertTo-Json $bad -Depth 8)
  Assert-Throws { Test-MgstcReleaseCandidate $cr } 'missing asset in manifest'; Write-TestText $mp $valid
  Remove-Item -LiteralPath (Join-Path $cr 'MGSToneCraft-0.365-win64.zip')
  Assert-Throws { Test-MgstcReleaseCandidate $cr } 'missing candidate asset file'; New-CandidateFixture $cr
 [IO.File]::WriteAllBytes((Join-Path $cr 'MGSToneCraft-0.365-win64.zip'),[byte[]](1,1,1,1)); Assert-Throws { Test-MgstcReleaseCandidate $cr } 'tampered asset'; New-CandidateFixture $cr
  $sp=Join-Path $cr 'SHA256SUMS.txt'; Write-TestText $sp ([IO.File]::ReadAllText($sp)+(('0'*64)+'  surplus.bin'+"`n"))
  $bad=[IO.File]::ReadAllText($mp)|ConvertFrom-Json; $sumAsset=$bad.assets|Where-Object{$_.name -eq 'SHA256SUMS.txt'}
  $sumAsset.size=(Get-Item $sp).Length; $sumAsset.sha256=Get-MgstcReleaseSha256 $sp; $bad.assetsDigest=Get-MgstcReleaseAssetDigest $bad.assets
  Write-TestText $mp (ConvertTo-Json $bad -Depth 8); Assert-Throws { Test-MgstcReleaseCandidate $cr } 'extra checksum row'
 New-CandidateFixture $cr; $bad=[IO.File]::ReadAllText($mp)|ConvertFrom-Json; $bad.prototype=$true; Write-TestText $mp (ConvertTo-Json $bad -Depth 8); Assert-Throws { Test-MgstcReleaseCandidate $cr -RequirePublic } 'prototype public rejection'
 New-CandidateFixture $cr; $bad=[IO.File]::ReadAllText($mp)|ConvertFrom-Json; $bad.notesSha256='0'*64; Write-TestText $mp (ConvertTo-Json $bad -Depth 8); Assert-Throws { Test-MgstcReleaseCandidate $cr } 'notes digest mismatch'
 New-CandidateFixture $cr; $notesPath=Join-Path $cr 'RELEASE_NOTES.md'; $wrongNotes=[IO.File]::ReadAllText($notesPath).Replace('0.365 Alpha','0.364 Alpha') + "Release notes also mention 0.365.`n"; Write-TestText $notesPath $wrongNotes
 $bad=[IO.File]::ReadAllText($mp)|ConvertFrom-Json; $bad.notesSha256=Get-MgstcReleaseSha256 $notesPath; Write-TestText $mp (ConvertTo-Json $bad -Depth 8)
 Assert-Throws { Test-MgstcReleaseCandidate $cr } 'notes heading mismatch despite matching body version'
 # Exercise the real candidate validator and public entry points with isolated
 # immutable-tree/state doubles. No Git repository or ref is created.
 & {
    . (Join-Path $repo 'tools/release-github.ps1')
    New-DocumentFixture $docsRoot
    $acceptedDocs=Test-MgstcReleaseDocuments $docsRoot '0.365' -RequireTracked -RequirePublic
    New-CandidateFixture $cr
    $file=Join-Path $cr 'candidate.json';$m=Get-Content $file -Raw | ConvertFrom-Json
    $m.documentsDigest=$acceptedDocs.digest
    $acceptedReview=[pscustomobject]@{accepted=$true;externalLinksAccepted=$true;screenshotReview='accepted';reviewer='Reviewer';reviewedAt=[datetimeoffset]::UtcNow.ToString('o');sourceDigest=$m.sourceDigest;documentsDigest=$m.documentsDigest;previousCommit=$m.previousCommit;changeCategories=@('feature','operation','ui','io','compatibility','distribution','fix')}
    $m|Add-Member review $acceptedReview
    $m|Add-Member payloadFiles @()
    $m|Add-Member payloadDigest ('e'*64)
    Write-TestText $file (ConvertTo-Json $m -Depth 10)
    $state=[pscustomobject]@{head=$m.commit;digest=$m.sourceDigest;dirty=@()}
    $treeDigest=$m.sourceDigest
    $notesGood=[IO.File]::ReadAllText((Join-Path $cr 'RELEASE_NOTES.md'))
    function Get-MgstcReleaseSourceState { param($ProjectRoot) $state }
    function Test-MgstcReleaseTree { param($ProjectRoot,$Commit) [pscustomobject]@{digest=$treeDigest} }
    function Test-MgstcReleaseArchive { param($Manifest,$CandidateDirectory,$ProjectRoot) }
    # This fixture isolates source/document identity; release-content bindings
    # are exercised separately with real CHANGELOG and audit text fixtures.
    function Test-MgstcReleaseContentCandidate { param($ProjectRoot,$Manifest,$NotesText) }
    $failDocument=$false
    function Test-MgstcReleaseDocuments {
        param($ProjectRoot,$Version,$PreviousCommit,[switch]$RequireTracked,[switch]$RequirePublic)
        if($failDocument){throw 'README.md:1 document gate failed for candidate v0.365'}
        $acceptedDocs
    }
    function Invoke-MgstcGitHubRequest { throw 'Unexpected network operation in source guard test' }
    Assert-True ((Test-MgstcReleaseCandidate $cr -RequirePublic).commit -ceq $m.commit) 'matching committed source and documents accepted'
    $state.dirty=@(' M README.md')
    foreach($action in @('Stage','Publish')) {
        $message=''
        try {
            if($action -eq 'Stage'){Invoke-MgstcReleaseStage -CandidateDirectory $cr -Repository 'example/product'|Out-Null}
            else{Invoke-MgstcReleasePublish -CandidateDirectory $cr -Repository 'example/product'|Out-Null}
        }catch{$message=$_.Exception.Message}
        Assert-True ($message -match 'clean committed checkout') "$action refuses working-tree-only README before network"
    }
    $state.dirty=@();$treeDigest='f'*64
    Assert-Throws {Test-MgstcReleaseCandidate $cr -RequirePublic} 'old README in candidate tree despite correct worktree'
    $treeDigest=$m.sourceDigest;$state.head='f'*40
    Assert-Throws {Test-MgstcReleaseCandidate $cr -RequirePublic} 'candidate commit changed after review'
    $state.head=$m.commit;$acceptedDocs.digest='f'*64
    Assert-Throws {Test-MgstcReleaseCandidate $cr -RequirePublic} 'README or image digest changed after review'
    $acceptedDocs.digest=$m.documentsDigest
    $failDocument=$true
    foreach($action in @('Stage','Publish')) {
        $message=''
        try {
            if($action -eq 'Stage'){Invoke-MgstcReleaseStage -CandidateDirectory $cr -Repository 'example/product'|Out-Null}
            else{Invoke-MgstcReleasePublish -CandidateDirectory $cr -Repository 'example/product'|Out-Null}
        }catch{$message=$_.Exception.Message}
        Assert-True ($message -match 'README.md:1 document gate failed') "$action refuses failed document gate before network"
    }
    $failDocument=$false
    Write-TestText (Join-Path $cr 'RELEASE_NOTES.md') ($notesGood+'Changed notes')
    Assert-Throws {Test-MgstcReleaseCandidate $cr -RequirePublic} 'notes bytes changed after review'
    Write-TestText (Join-Path $cr 'RELEASE_NOTES.md') $notesGood
    $m.prototype=$true;Write-TestText $file (ConvertTo-Json $m -Depth 10)
    Assert-Throws {Invoke-MgstcReleaseStage -CandidateDirectory $cr -Repository 'example/product'} 'prototype Stage blocked'
    Assert-Throws {Invoke-MgstcReleasePublish -CandidateDirectory $cr -Repository 'example/product'} 'prototype Publish blocked'
    $m.prototype=$false;$m.review=$null;Write-TestText $file (ConvertTo-Json $m -Depth 10)
    Assert-Throws {Invoke-MgstcReleaseStage -CandidateDirectory $cr -Repository 'example/product'} 'missing document approval Stage blocked'
    Assert-Throws {Invoke-MgstcReleasePublish -CandidateDirectory $cr -Repository 'example/product'} 'missing document approval Publish blocked'
 }
 # Static contract: ValidateOnly returns before candidate/output creation or build invocation.
$prepare=[IO.File]::ReadAllText((Join-Path $repo 'tools\prepare-release.ps1')); $vi=$prepare.IndexOf('ValidateOnly'); $ri=$prepare.IndexOf('return',$vi); $oi=$prepare.IndexOf('New-Item -ItemType Directory -Path'); $bi=$prepare.IndexOf('tools/build.cmd')
 Assert-True ($prepare -notmatch 'Invoke-RestMethod|Invoke-WebRequest|git push|git tag') 'no remote publication command'
 Write-Output "PASS: $script:checks release-gate assertions (unit fixtures only; no build or product binary validation)."
} finally {
 $resolved=[IO.Path]::GetFullPath($tempRoot); $base=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([char]92,[char]47)
 if ((Split-Path -Parent $resolved) -ne $base -or (Split-Path -Leaf $resolved) -notlike 'mgstc-release-gates-tests-*') { throw 'Unsafe test cleanup path.' }
 Remove-Item -LiteralPath $resolved -Recurse -Force
}
