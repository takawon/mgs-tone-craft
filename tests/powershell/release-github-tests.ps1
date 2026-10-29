# SPDX-License-Identifier: AGPL-3.0-only
# Transport-only regression tests. They never contact GitHub or run product code.
[CmdletBinding()]
param([string]$WorkDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../tools/release-github.ps1')
$testRoot=if($WorkDirectory){[IO.Path]::GetFullPath($WorkDirectory)}else{Join-Path ([IO.Path]::GetTempPath()) ('mgstc-release-github-tests-'+[Guid]::NewGuid().ToString('N'))}
if(Test-Path -LiteralPath $testRoot){throw 'Test output directory must be new.'}
New-Item -ItemType Directory -Path $testRoot | Out-Null
$script:checks=0
function Assert-Test([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message};$script:checks++}
function Assert-Rejected([scriptblock]$Operation,[string]$Message){$rejected=$false;try{& $Operation | Out-Null}catch{$rejected=$true};Assert-Test $rejected $Message}
function New-TestCandidate([string]$Name){
    $directory=Join-Path $testRoot $Name
    New-Item -ItemType Directory -Path $directory | Out-Null
    $version='0.999'
    $binaryNames=@("MGSToneCraft-$version-win64-setup.exe","MGSToneCraft-$version-win64.zip")
    foreach($name in $binaryNames){[IO.File]::WriteAllText((Join-Path $directory $name),"Test payload: $name",[Text.UTF8Encoding]::new($false))}
    $assets=@(foreach($name in $binaryNames){$path=Join-Path $directory $name;[pscustomobject]@{name=$name;size=(Get-Item $path).Length;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}})
    $sum=(@($assets | ForEach-Object {$_.sha256+'  '+$_.name}) -join "`n")+"`n"
    [IO.File]::WriteAllText((Join-Path $directory 'SHA256SUMS.txt'),$sum,[Text.UTF8Encoding]::new($false))
    $path=Join-Path $directory 'SHA256SUMS.txt'
    $assets += [pscustomobject]@{name='SHA256SUMS.txt';size=(Get-Item $path).Length;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    $notes="MGS Tone Craft $version Alpha`n`nReviewed release notes.`n"
    [IO.File]::WriteAllText((Join-Path $directory 'RELEASE_NOTES.md'),$notes,[Text.UTF8Encoding]::new($false))
    $source=('b'*64);$docs=('c'*64);$previous=('d'*40)
    $manifest=[pscustomobject]@{schemaVersion=1;state='Ready';version=$version;tag="v$version";commit=('a'*40);previousTag='v0.998';previousCommit=$previous;prototype=$false;sourceDigest=$source;documentsDigest=$docs;notesSha256=(Get-MgstcGitHubTextHash $notes);assetsDigest=(Get-MgstcGitHubAssetDigest $assets);assets=$assets;dbFiles=@{manifest=('e'*64)};review=[pscustomobject]@{accepted=$true;reviewer='maintainer';reviewedAt='2026-01-01T00:00:00Z';documentsDigest=$docs;sourceDigest=$source;previousCommit=$previous;externalLinksAccepted=$true;screenshotReview='accepted';changeCategories=@('feature','operation','ui','io','compatibility','distribution','fix')}}
    $binding=[pscustomobject]@{version=$version;previousTag=$manifest.previousTag;previousCommit=$manifest.previousCommit;candidateCommit=$manifest.commit;changelogSectionSha256=('e'*64);classificationDigest=('f'*64);notesSha256=$manifest.notesSha256}
    $manifest|Add-Member releaseContent $binding
    $manifest.review|Add-Member releaseContentDigest (Assert-MgstcReleaseContentBinding $binding $version $manifest.commit $manifest.previousTag $manifest.previousCommit $manifest.notesSha256)
    Write-MgstcGitHubReceipt (Join-Path $directory 'candidate.json') $manifest
    $state=[pscustomobject]@{manifest=$manifest;directory=$directory;tagSha=$null;draft=$null;assets=@();bytes=@{};nextId=10;writes=[Collections.Generic.List[object]]::new();fail='';approved=$false;environmentConfigured=$true;validatorCalls=0;failValidationAt=0}
    $validator={param($path) $state.validatorCalls++;if($state.failValidationAt -gt 0 -and $state.validatorCalls -ge $state.failValidationAt){throw 'Source changed'};return $state.manifest}.GetNewClosure()
    $transport={
        param($method,$path,$body,$upload,$download,$allowMissing)
        if($state.fail -and $path -match $state.fail){throw 'Simulated API failure'}
        if($method -ne 'GET'){$state.writes.Add([pscustomobject]@{method=$method;path=$path;body=$body})}
        if($method -eq 'GET' -and $path -match '/releases\?'){
            $result=@([pscustomobject]@{id=1;tag_name=$state.manifest.previousTag;draft=$false;published_at='2025-12-01T00:00:00Z'})
            if($state.draft){$result += $state.draft};return $result
        }
        if($method -eq 'GET' -and $path -match '/git/ref/tags/(.+)$'){
            $tag=[uri]::UnescapeDataString($Matches[1]);$sha=if($tag -ceq $state.manifest.previousTag){$state.manifest.previousCommit}elseif($tag -ceq $state.manifest.tag){$state.tagSha}else{$null}
            if(-not $sha){if($allowMissing){return $null};throw '404'}
            return [pscustomobject]@{object=[pscustomobject]@{type='commit';sha=$sha}}
        }
        if($method -eq 'GET' -and $path -match '/commits/'){return [pscustomobject]@{sha=$state.manifest.commit}}
        if($method -eq 'POST' -and $path -match '/git/refs$'){
            if($state.tagSha){throw 'Ref already exists'};$state.tagSha=$body.sha;return [pscustomobject]@{ref=$body.ref}
        }
        if($method -eq 'POST' -and $path -match '/releases$'){
            if($state.draft){throw 'Draft already exists'}
            $state.draft=[pscustomobject]@{id=2;tag_name=$body.tag_name;target_commitish=$body.target_commitish;body=$body.body;draft=$true;prerelease=$true;html_url='https://github.com/example/product/releases/tag/test'}
            return $state.draft
        }
        if($method -eq 'GET' -and $path -match '/releases/2/assets\?'){return $state.assets}
        if($method -eq 'POST' -and $path -match '/releases/2/assets\?name=(.+)$'){
            $name=[uri]::UnescapeDataString($Matches[1]);$id=$state.nextId++;$bytes=[IO.File]::ReadAllBytes($upload)
            $asset=[pscustomobject]@{id=$id;name=$name;size=$bytes.Length;state='uploaded'}
            $state.assets += $asset;$state.bytes["$id"]=$bytes;return $asset
        }
        if($method -eq 'GET' -and $path -match '/releases/assets/(\d+)$'){
            [IO.File]::WriteAllBytes($download,$state.bytes[$Matches[1]]);return
        }
        if($method -eq 'GET' -and $path -match '/environments/'){
            $rules=if($state.environmentConfigured){@([pscustomobject]@{type='required_reviewers';prevent_self_review=$false;reviewers=@([pscustomobject]@{type='User';reviewer=[pscustomobject]@{login='maintainer'}})})}else{@()}
            return [pscustomobject]@{id=9;protection_rules=$rules}
        }
        if($method -eq 'GET' -and $path -match '/actions/runs/123/approvals$'){
            if($state.approved){return @([pscustomobject]@{state='approved';environments=@([pscustomobject]@{id=9});user=[pscustomobject]@{login='maintainer'}})};return @()
        }
        if($method -eq 'GET' -and $path -match '/actions/runs/123$'){return [pscustomobject]@{event='workflow_dispatch';head_branch='main';triggering_actor=[pscustomobject]@{login='maintainer'}}}
        if($method -eq 'GET' -and $path -match '^/repos/example/product$'){return [pscustomobject]@{default_branch='main'}}
        if($method -eq 'PATCH' -and $path -match '/releases/2$'){
            $state.draft.draft=$false;$state.draft.prerelease=$body.prerelease;return $state.draft
        }
        throw "Unexpected mocked API operation: $method $path"
    }.GetNewClosure()
    return [pscustomobject]@{directory=$directory;manifest=$manifest;state=$state;validator=$validator;transport=$transport}
}
function Invoke-TestStage($Fixture,[switch]$Execute,[scriptblock]$ApprovalReader={param($phrase)$phrase}){
    Invoke-MgstcReleaseStage -CandidateDirectory $Fixture.directory -Repository 'example/product' -Execute:$Execute -Transport $Fixture.transport -CandidateValidator $Fixture.validator -ApprovalReader $ApprovalReader
}
function Invoke-TestPublish($Fixture,[switch]$Execute,[switch]$GitHubActions,[scriptblock]$ApprovalReader={param($phrase)$phrase}){
    Invoke-MgstcReleasePublish -CandidateDirectory $Fixture.directory -Repository 'example/product' -Execute:$Execute -GitHubActions:$GitHubActions -Transport $Fixture.transport -CandidateValidator $Fixture.validator -ApprovalReader $ApprovalReader
}

$savedEnvironment=@{}
foreach($key in @('GITHUB_ACTIONS','GITHUB_EVENT_NAME','GITHUB_REPOSITORY','GITHUB_RUN_ID','GITHUB_EVENT_PATH')){$savedEnvironment[$key]=[Environment]::GetEnvironmentVariable($key)}
try{
    $fixture=New-TestCandidate 'read-only'
    $result=Invoke-TestStage $fixture
    Assert-Test ($result.status -eq 'StageValidatedNoWrites' -and $fixture.state.writes.Count -eq 0) 'Stage default performed a write.'
    Assert-Rejected {Invoke-TestStage $fixture -Execute -ApprovalReader {''}} 'Missing stage approval was accepted.'
    Assert-Test ($fixture.state.writes.Count -eq 0) 'Unapproved stage mutated GitHub.'
    $fixture.manifest.prototype=$true
    Assert-Rejected {Invoke-TestStage $fixture} 'Prototype candidate was accepted.'
    $fixture.manifest.prototype=$false
    $fixture.state.fail='/releases\?'
    Assert-Rejected {Invoke-TestStage $fixture -Execute} 'API failure did not stop Stage.'
    Assert-Test ($fixture.state.writes.Count -eq 0) 'Read API failure caused a write.'

    $fixture=New-TestCandidate 'stage-and-resume'
    $staged=Invoke-TestStage $fixture -Execute
    Assert-Test ($staged.status -eq 'DraftVerified' -and $fixture.state.tagSha -eq $fixture.manifest.commit -and $fixture.state.assets.Count -eq 3) 'Stage did not create the exact draft and three assets.'
    Assert-Test (@($fixture.state.writes | Where-Object method -eq POST).Count -eq 5) 'Unexpected Stage writes.'
    Assert-Test ($fixture.state.draft.draft -and $fixture.state.draft.prerelease) 'Stage published or created a stable release.'
    $writeCount=$fixture.state.writes.Count
    Invoke-TestStage $fixture -Execute | Out-Null
    Assert-Test ($fixture.state.writes.Count -eq $writeCount) 'Identical Stage retry rewrote remote content.'
    Assert-Rejected {Invoke-TestPublish $fixture -Execute -ApprovalReader {''}} 'Unapproved Publish was accepted.'
    Assert-Test (@($fixture.state.writes | Where-Object method -eq PATCH).Count -eq 0) 'Missing approval published a draft.'
    $published=Invoke-TestPublish $fixture -Execute
    Assert-Test ($published.status -eq 'PublishedAlpha' -and -not $fixture.state.draft.draft -and $fixture.state.draft.prerelease) 'Explicit Publish failed.'
    Assert-Test (($fixture.state.writes | Where-Object method -eq PATCH)[0].body.make_latest -ceq 'false') 'Alpha publication promoted latest.'
    Assert-Rejected {Invoke-TestStage $fixture -Execute} 'Published release was accepted for Stage modification.'
    Assert-Rejected {Invoke-TestPublish $fixture -Execute} 'Published release was accepted for mutation.'

    $fixture=New-TestCandidate 'collision'
    $fixture.state.tagSha=('f'*40)
    Assert-Rejected {Invoke-TestStage $fixture -Execute} 'Conflicting tag SHA was accepted.'
    $fixture.state.tagSha=$fixture.manifest.commit
    Assert-Rejected {Invoke-TestStage $fixture -Execute} 'Unowned existing tag without a draft was accepted.'
    Assert-Test ($fixture.state.writes.Count -eq 0) 'Tag collision caused a write.'

    $fixture=New-TestCandidate 'lock'
    $held=Enter-MgstcGitHubLock -Repository 'example/product' -Tag $fixture.manifest.tag
    try{
        Assert-Rejected {Invoke-TestStage $fixture -Execute} 'Concurrent operation was not rejected.'
        $otherDirectory=New-TestCandidate 'lock-other-directory'
        Assert-Rejected {Invoke-TestStage $otherDirectory -Execute} 'Same repository/tag in another candidate directory bypassed the lock.'
        Assert-Test ($otherDirectory.state.writes.Count -eq 0) 'Other-directory concurrent operation caused a write.'
        $otherTag=Enter-MgstcGitHubLock -Repository 'example/product' -Tag 'v0.997'
        try{Assert-Test ($null -ne $otherTag) 'A different tag was unnecessarily blocked by the lock.'}finally{$otherTag.Dispose()}
    }finally{$held.Dispose()}
    Assert-Test ($fixture.state.writes.Count -eq 0) 'Concurrent operation caused a write.'

    $fixture=New-TestCandidate 'partial-upload'
    $fixture.state.fail='assets\?name=SHA256SUMS'
    Assert-Rejected {Invoke-TestStage $fixture -Execute} 'Upload failure was ignored.'
    Assert-Test ($fixture.state.draft.draft -and $fixture.state.assets.Count -eq 2) 'Failed Stage did not retain its partial draft safely.'
    $fixture.state.fail=''
    Invoke-TestStage $fixture -Execute | Out-Null
    Assert-Test ($fixture.state.assets.Count -eq 3) 'Partial Stage did not resume missing assets.'

    $fixture=New-TestCandidate 'draft-creation-interrupted'
    $fixture.state.fail='/releases$'
    Assert-Rejected {Invoke-TestStage $fixture -Execute} 'Draft API failure was ignored.'
    Assert-Test ($fixture.state.tagSha -eq $fixture.manifest.commit -and $null -eq $fixture.state.draft) 'Tag creation interruption state is incorrect.'
    $fixture.state.fail=''
    Invoke-TestStage $fixture -Execute | Out-Null
    Assert-Test ($fixture.state.assets.Count -eq 3) 'Recorded exact-candidate tag ownership did not permit recovery.'

    $fixture=New-TestCandidate 'remote-content-drift'
    Invoke-TestStage $fixture -Execute | Out-Null
    $writeCount=$fixture.state.writes.Count
    $originalBody=$fixture.state.draft.body
    $fixture.state.draft.body+=' changed'
    Assert-Rejected {Invoke-TestPublish $fixture -Execute} 'Changed Release Notes were accepted.'
    $fixture.state.draft.body=$originalBody
    $assetId=[string]$fixture.state.assets[0].id
    $originalBytes=$fixture.state.bytes[$assetId]
    $fixture.state.bytes[$assetId]=[Text.Encoding]::UTF8.GetBytes('wrong asset')
    Assert-Rejected {Invoke-TestPublish $fixture -Execute} 'Changed asset bytes were accepted.'
    $fixture.state.bytes[$assetId]=$originalBytes
    $fixture.state.assets += [pscustomobject]@{id=99;name='extra.zip';size=1;state='uploaded'}
    Assert-Rejected {Invoke-TestPublish $fixture -Execute} 'Unexpected fourth asset was accepted.'
    Assert-Test ($fixture.state.writes.Count -eq $writeCount) 'Remote drift caused a mutation.'

    $fixture=New-TestCandidate 'approval-race'
    Invoke-TestStage $fixture -Execute | Out-Null
    $state=$fixture.state
    Assert-Rejected {Invoke-TestPublish $fixture -Execute -ApprovalReader {param($phrase)$state.draft.body+='changed';$phrase}.GetNewClosure()} 'Notes mutation during final approval was accepted.'
    Assert-Test (@($fixture.state.writes | Where-Object method -eq PATCH).Count -eq 0) 'Approval race published changed notes.'

    $fixture=New-TestCandidate 'source-race'
    Invoke-TestStage $fixture -Execute | Out-Null
    $fixture.state.failValidationAt=$fixture.state.validatorCalls+2
    Assert-Rejected {Invoke-TestPublish $fixture -Execute} 'Source change during final approval was accepted.'
    Assert-Test (@($fixture.state.writes | Where-Object method -eq PATCH).Count -eq 0) 'Source race published a candidate.'

    $fixture=New-TestCandidate 'actions'
    Invoke-TestStage $fixture -Execute | Out-Null
    $eventPath=Join-Path $fixture.directory 'event.json'
    Write-MgstcGitHubReceipt $eventPath @{inputs=@{tag=$fixture.manifest.tag;commit=$fixture.manifest.commit;assets_digest=$fixture.manifest.assetsDigest;notes_sha256=$fixture.manifest.notesSha256;action='publish'}}
    $env:GITHUB_ACTIONS='true';$env:GITHUB_EVENT_NAME='workflow_dispatch';$env:GITHUB_REPOSITORY='example/product';$env:GITHUB_RUN_ID='123';$env:GITHUB_EVENT_PATH=$eventPath
    $fixture.state.environmentConfigured=$false
    Assert-Rejected {Invoke-TestPublish $fixture -GitHubActions} 'Unprotected Environment was accepted.'
    $fixture.state.environmentConfigured=$true
    Assert-Rejected {Invoke-TestPublish $fixture -GitHubActions -Execute} 'No actual Environment approval was accepted.'
    Assert-Test (@($fixture.state.writes | Where-Object method -eq PATCH).Count -eq 0) 'Missing Environment approval published a draft.'
    $fixture.state.approved=$true
    $result=Invoke-TestPublish $fixture -GitHubActions -Execute
    Assert-Test ($result.status -eq 'PublishedAlpha' -and $result.approvalMode -eq 'EnvironmentReview') 'Actual configured reviewer approval did not permit publication.'

    $fixture=New-TestCandidate 'import'
    Invoke-TestStage $fixture -Execute | Out-Null
    $download=Join-Path $testRoot 'downloaded'
    $result=Import-MgstcGitHubCandidate -Repository 'example/product' -Tag $fixture.manifest.tag -Commit $fixture.manifest.commit -AssetsDigest $fixture.manifest.assetsDigest -NotesSha256 $fixture.manifest.notesSha256 -CandidateDirectory $download -Transport $fixture.transport
    Assert-Test ($result.commit -eq $fixture.manifest.commit -and (Get-FileHash (Join-Path $download 'RELEASE_NOTES.md')).Hash.ToLowerInvariant() -eq $fixture.manifest.notesSha256) 'Draft import did not preserve candidate and LF notes bytes.'
    foreach($asset in $fixture.manifest.assets){Assert-Test ((Get-FileHash (Join-Path $download $asset.name)).Hash.ToLowerInvariant() -eq $asset.sha256) 'Draft import changed asset bytes.'}
    Assert-Rejected {Import-MgstcGitHubCandidate -Repository 'example/product' -Tag $fixture.manifest.tag -Commit $fixture.manifest.commit -AssetsDigest $fixture.manifest.assetsDigest -NotesSha256 $fixture.manifest.notesSha256 -CandidateDirectory $download -Transport $fixture.transport} 'Existing import directory was reused.'

    $fixture=New-TestCandidate 'missing-content-review'
    $fixture.manifest.PSObject.Properties.Remove('releaseContent')
    Assert-Rejected {Invoke-TestStage $fixture -Execute} 'Candidate without classification binding was staged.'
    Assert-Test ($fixture.state.writes.Count -eq 0) 'Missing classification review wrote remote state.'
    $fixture=New-TestCandidate 'changed-content-after-stage'
    Invoke-TestStage $fixture -Execute | Out-Null
    $fixture.manifest.releaseContent.classificationDigest='0'*64
    Assert-Rejected {Invoke-TestPublish $fixture -Execute} 'Changed classification digest reused its approval.'
    Assert-Test (@($fixture.state.writes|Where-Object method -eq PATCH).Count -eq 0) 'Changed content approval published a draft.'
    $fixture=New-TestCandidate 'changed-draft-notes'
    Invoke-TestStage $fixture -Execute | Out-Null
    $fixture.state.draft.body+='Substantive unreviewed edit.'
    Assert-Rejected {Invoke-TestPublish $fixture -Execute} 'Edited draft Notes were published.'
    Assert-Test (@($fixture.state.writes|Where-Object method -eq PATCH).Count -eq 0) 'Edited Notes reached the publication API.'
    $fixture=New-TestCandidate 'private-audit-leak'
    $fixture.manifest.releaseContent|Add-Member evidence 'Private evidence must stay local'
    Assert-Rejected {ConvertTo-MgstcGitHubManifest $fixture.manifest} 'Private evidence entered public candidate metadata.'
    Write-Output "GitHub transport regression checks passed: $script:checks (no network operations)."
} finally {
    foreach($key in $savedEnvironment.Keys){[Environment]::SetEnvironmentVariable($key,$savedEnvironment[$key])}
}
