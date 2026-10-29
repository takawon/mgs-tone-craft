# Shared GitHub transport and explicit Stage/Publish operations. No entry point
# in this file runs on import. Public scripts default to read-only verification.
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'release-content.ps1')

function Get-MgstcGitHubValue($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    if ($Object -is [Collections.IDictionary]) { return $Object[$Name] }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -ne $property) { return $property.Value }
    return $null
}

function Get-MgstcGitHubTextHash([string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-MgstcGitHubAssetDigest($Assets) {
    $lines = @($Assets | Sort-Object name | ForEach-Object {
        ([string]$_.name).ToLowerInvariant() + "`t" + ([long]$_.size).ToString([Globalization.CultureInfo]::InvariantCulture) + "`t" + ([string]$_.sha256).ToLowerInvariant()
    })
    return Get-MgstcGitHubTextHash ($lines -join "`n")
}

function Assert-MgstcGitHubRepository([string]$Repository) {
    if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'Repository must be an explicit owner/repository on github.com.' }
}

function Invoke-MgstcGitHubRequest {
    param([string]$Method='GET', [string]$Path, $Body, [string]$UploadPath='', [string]$DownloadPath='', [switch]$AllowNotFound, [scriptblock]$Transport)
    if ($Transport) { return & $Transport $Method $Path $Body $UploadPath $DownloadPath ([bool]$AllowNotFound) }
    if ($Path -notmatch '^/repos/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:/|$)' -and $Path -notmatch '^/orgs/[A-Za-z0-9_.-]+/teams/[A-Za-z0-9_.-]+/memberships/[A-Za-z0-9_.-]+$') { throw 'Unexpected GitHub API path.' }
    $token = if ($env:GH_TOKEN) { $env:GH_TOKEN } else { $env:GITHUB_TOKEN }
    if (-not $token) { throw 'Set GH_TOKEN (local) or GITHUB_TOKEN (Actions); token values are never recorded.' }
    $headers = @{ Authorization="Bearer $token"; Accept='application/vnd.github+json'; 'X-GitHub-Api-Version'='2022-11-28'; 'User-Agent'='MGSTC-release-tools' }
    $uri = 'https://api.github.com' + $Path
    $parameters = @{ Uri=$uri; Method=$Method; Headers=$headers; UseBasicParsing=$true; ErrorAction='Stop'; TimeoutSec=180 }
    if ($UploadPath) {
        $parameters.Uri = 'https://uploads.github.com' + $Path
        $parameters.InFile = $UploadPath
        $parameters.ContentType = 'application/octet-stream'
    } elseif ($DownloadPath) {
        $headers.Accept='application/octet-stream'
        $parameters.OutFile=$DownloadPath
    } elseif ($null -ne $Body) {
        $parameters.Body=[Text.Encoding]::UTF8.GetBytes(($Body | ConvertTo-Json -Depth 30 -Compress))
        $parameters.ContentType='application/json; charset=utf-8'
    }
    try {
        $response = Invoke-WebRequest @parameters
        if ($DownloadPath) { return }
        if ($response.Content) { return $response.Content | ConvertFrom-Json }
        return $null
    } catch {
        $status = 0
        $errorResponse=Get-MgstcGitHubValue $_.Exception 'Response'
        if ($null -ne $errorResponse) { $status=[int]$errorResponse.StatusCode }
        if ($AllowNotFound -and $status -eq 404) { return $null }
        # Do not emit response bodies, headers or authentication details.
        throw "GitHub $Method request failed (HTTP $status). No automatic destructive retry was attempted."
    }
}

function Get-MgstcGitHubTagCommit([string]$Repository, [string]$Tag, [scriptblock]$Transport) {
    $reference=Invoke-MgstcGitHubRequest -Path "/repos/$Repository/git/ref/tags/$([uri]::EscapeDataString($Tag))" -AllowNotFound -Transport $Transport
    if ($null -eq $reference) { return $null }
    $object=$reference.object
    for ($i=0; $i -lt 8; ++$i) {
        if ($object.type -eq 'commit') { return ([string]$object.sha).ToLowerInvariant() }
        if ($object.type -ne 'tag') { throw 'Tag does not resolve to a commit.' }
        $annotated=Invoke-MgstcGitHubRequest -Path "/repos/$Repository/git/tags/$($object.sha)" -Transport $Transport
        $object=$annotated.object
    }
    throw 'Annotated tag chain exceeds the supported bound.'
}

function Get-MgstcGitHubReleases([string]$Repository, [scriptblock]$Transport) {
    $all=@()
    for ($page=1; $page -le 100; ++$page) {
        $batch=@(Invoke-MgstcGitHubRequest -Path "/repos/$Repository/releases?per_page=100&page=$page" -Transport $Transport)
        $all += $batch
        if ($batch.Count -lt 100) { return $all }
    }
    throw 'Release list exceeds the supported pagination bound.'
}

function Assert-MgstcGitHubPreviousRelease($Manifest, [string]$Repository, [scriptblock]$Transport) {
    $published=@(Get-MgstcGitHubReleases $Repository $Transport | Where-Object { -not $_.draft } | Sort-Object { [DateTimeOffset]$_.published_at } -Descending)
    if (-not $published.Count -or $published[0].tag_name -cne $Manifest.previousTag) { throw 'The latest published Release changed; prepare and review the candidate again.' }
    $previous=Get-MgstcGitHubTagCommit $Repository $Manifest.previousTag $Transport
    if ($previous -cne $Manifest.previousCommit) { throw 'Previous published tag SHA differs from the reviewed candidate.' }
    $commit=Invoke-MgstcGitHubRequest -Path "/repos/$Repository/commits/$($Manifest.commit)" -Transport $Transport
    if ($commit.sha -ine $Manifest.commit) { throw 'Candidate commit is not available on GitHub.' }
}

function Assert-MgstcGitHubManifest($Manifest) {
    if ($Manifest.schemaVersion -ne 1 -or $Manifest.prototype -ne $false -or $Manifest.state -cne 'Ready') { throw 'Only a confirmed, committed candidate can be staged or published.' }
    if ($Manifest.version -notmatch '^\d+\.\d+(?:\.\d+)?$' -or $Manifest.tag -cne ('v'+$Manifest.version) -or $Manifest.commit -notmatch '^[a-f0-9]{40}$' -or $Manifest.previousCommit -notmatch '^[a-f0-9]{40}$') { throw 'Candidate version/tag/commit is invalid.' }
    foreach($hash in @($Manifest.sourceDigest,$Manifest.documentsDigest,$Manifest.notesSha256,$Manifest.assetsDigest)) {
        if ($hash -notmatch '^[a-f0-9]{64}$') { throw 'Candidate verification digest is invalid.' }
    }
    $expected=@("MGSToneCraft-$($Manifest.version)-win64-setup.exe","MGSToneCraft-$($Manifest.version)-win64.zip",'SHA256SUMS.txt')
    if (@($Manifest.assets).Count -ne 3 -or @($Manifest.assets | Select-Object -ExpandProperty name -Unique).Count -ne 3) { throw 'Exactly EXE, ZIP and SHA256SUMS assets are required.' }
    foreach($asset in $Manifest.assets) {
        if ($expected -cnotcontains $asset.name -or [long]$asset.size -le 0 -or $asset.sha256 -notmatch '^[a-f0-9]{64}$') { throw 'Unexpected or invalid candidate asset.' }
    }
    if ((Get-MgstcGitHubAssetDigest $Manifest.assets) -cne $Manifest.assetsDigest) { throw 'Candidate asset digest is inconsistent.' }
    $review=$Manifest.review
    if ($review.accepted -ne $true -or $review.externalLinksAccepted -ne $true -or $review.screenshotReview -cne 'accepted' -or -not $review.reviewer -or -not $review.reviewedAt -or $review.sourceDigest -cne $Manifest.sourceDigest -or $review.documentsDigest -cne $Manifest.documentsDigest -or $review.previousCommit -cne $Manifest.previousCommit) { throw 'Document and screenshot acceptance does not cover this candidate.' }
    $contentDigest=Assert-MgstcReleaseContentBinding (Get-MgstcGitHubValue $Manifest 'releaseContent') $Manifest.version $Manifest.commit $Manifest.previousTag $Manifest.previousCommit $Manifest.notesSha256
    if((Get-MgstcGitHubValue $review 'releaseContentDigest') -cne $contentDigest){throw 'CHANGELOG classification and Release Notes acceptance does not cover this candidate.'}
}

function ConvertTo-MgstcGitHubManifest($Manifest) {
    Assert-MgstcGitHubManifest $Manifest
    # Only these public provenance fields leave the development machine.
    $public=[ordered]@{}
    foreach($key in @('schemaVersion','state','version','tag','commit','previousTag','previousCommit','prototype','sourceDigest','documentsDigest','notesSha256','assetsDigest','assets','dbFiles','payloadFiles','payloadDigest','releaseContent')) { $public[$key]=Get-MgstcGitHubValue $Manifest $key }
    $review=[ordered]@{}
    foreach($key in @('accepted','reviewer','reviewedAt','documentsDigest','sourceDigest','previousCommit','externalLinksAccepted','changeCategories','screenshotReview','releaseContentDigest')) { $review[$key]=Get-MgstcGitHubValue $Manifest.review $key }
    $public.review=$review
    $json=$public | ConvertTo-Json -Depth 25 -Compress
    if ($json -match '(?i)[A-Z]:[\\/]|/Users/|/home/|gh[pousr]_[A-Za-z0-9]+|github_pat_') { throw 'Candidate provenance contains private path or credential material.' }
    return $public
}

function New-MgstcGitHubReleaseBody($Manifest, [string]$Notes) {
    $notes=$Notes.Replace("`r`n","`n").Replace("`r","`n").TrimEnd("`n")+"`n"
    if ((Get-MgstcGitHubTextHash $notes) -cne $Manifest.notesSha256 -or $notes.Contains('<!-- mgstc-release-candidate-v1')) { throw 'Release Notes bytes do not match the candidate.' }
    $json=ConvertTo-MgstcGitHubManifest $Manifest | ConvertTo-Json -Depth 25 -Compress
    $encoded=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($json))
    return $notes+"`n<!-- mgstc-release-candidate-v1`n"+$encoded+"`n-->"
}

function Read-MgstcGitHubReleaseBody([string]$Body) {
    $bodyLf=$Body.Replace("`r`n","`n").Replace("`r","`n")
    $match=[regex]::Match($bodyLf,'\A(?<notes>.*\n)\n<!-- mgstc-release-candidate-v1\n(?<data>[A-Za-z0-9+/=]+)\n-->\z',[Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $match.Success) { throw 'Draft has no unambiguous MGSTC candidate provenance.' }
    try { $manifest=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($match.Groups['data'].Value)) | ConvertFrom-Json }
    catch { throw 'Draft candidate provenance is malformed.' }
    Assert-MgstcGitHubManifest $manifest
    $notes=$match.Groups['notes'].Value
    if ((Get-MgstcGitHubTextHash $notes) -cne $manifest.notesSha256) { throw 'Draft Release Notes changed after candidate acceptance.' }
    return [pscustomobject]@{manifest=$manifest;notes=$notes}
}

function Get-MgstcGitHubDraft($Manifest, [string]$Repository, [scriptblock]$Transport) {
    $matches=@(Get-MgstcGitHubReleases $Repository $Transport | Where-Object { $_.tag_name -ceq $Manifest.tag })
    if ($matches.Count -gt 1) { throw 'Multiple Releases use this tag; resolve the conflict manually.' }
    if (-not $matches.Count) { return $null }
    $release=$matches[0]
    if (-not $release.draft) { throw 'This tag already has a published Release; it will not be changed.' }
    if (-not $release.prerelease) { throw 'Draft is not marked as an Alpha prerelease.' }
    return $release
}

function Assert-MgstcGitHubDraftIdentity($Release, $Manifest, [string]$Body) {
    if ($Release.tag_name -cne $Manifest.tag -or $Release.target_commitish -cne $Manifest.commit -or $Release.body.Replace("`r`n","`n") -cne $Body) { throw 'Existing draft commit or Release Notes differ from this exact candidate.' }
    $read=Read-MgstcGitHubReleaseBody $Release.body
    if ($read.manifest.assetsDigest -cne $Manifest.assetsDigest -or $read.manifest.sourceDigest -cne $Manifest.sourceDigest -or $read.manifest.documentsDigest -cne $Manifest.documentsDigest) { throw 'Existing draft provenance differs from the local candidate.' }
}

function Assert-MgstcGitHubAssets {
    param($Release,$Manifest,[string]$Repository,[scriptblock]$Transport,[switch]$AllowMissing)
    $assets=@(Invoke-MgstcGitHubRequest -Path "/repos/$Repository/releases/$($Release.id)/assets?per_page=100" -Transport $Transport)
    $names=@($Manifest.assets | Select-Object -ExpandProperty name)
    if ($assets.Count -gt 3 -or @($assets | Select-Object -ExpandProperty name -Unique).Count -ne $assets.Count) { throw 'Duplicate or unexpected remote assets.' }
    foreach($asset in $assets) {
        $expected=@($Manifest.assets | Where-Object { $_.name -ceq $asset.name })
        if ($names -cnotcontains $asset.name -or $expected.Count -ne 1 -or $asset.state -cne 'uploaded' -or [long]$asset.size -ne [long]$expected[0].size) { throw 'Remote asset set or size differs from the verified candidate.' }
        $temp=Join-Path ([IO.Path]::GetTempPath()) ('mgstc-release-asset-'+[Guid]::NewGuid().ToString('N'))
        try {
            Invoke-MgstcGitHubRequest -Path "/repos/$Repository/releases/assets/$($asset.id)" -DownloadPath $temp -Transport $Transport | Out-Null
            if ((Get-FileHash -LiteralPath $temp -Algorithm SHA256).Hash.ToLowerInvariant() -cne $expected[0].sha256) { throw 'Remote asset SHA256 differs from the verified candidate.' }
        } finally { if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force } }
    }
    if (-not $AllowMissing -and $assets.Count -ne 3) { throw 'The draft is missing one or more required assets.' }
    return $assets
}

function Enter-MgstcGitHubLock([string]$Repository,[string]$Tag) {
    Assert-MgstcGitHubRepository $Repository
    if ($Tag -notmatch '^v\d+\.\d+(?:\.\d+)?$') { throw 'A release tag is required for the operation lock.' }
    # Shared by every candidate directory for this host/user. Keep the lock
    # inode after closing so another process cannot lock a replacement file.
    $key=Get-MgstcGitHubTextHash ($Repository.ToLowerInvariant()+"`n"+$Tag.ToLowerInvariant())
    $lockPath=Join-Path ([IO.Path]::GetTempPath()) ('mgstc-release-operation-'+$key+'.lock')
    try { return [IO.File]::Open($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None) }
    catch { throw 'Cannot acquire the host/user repository-tag release lock; another operation may already own it.' }
}

function Confirm-MgstcGitHubOperation([string]$Action,$Manifest,[scriptblock]$ApprovalReader) {
    $phrase="$Action $($Manifest.tag) $($Manifest.commit) $($Manifest.assetsDigest) $($Manifest.notesSha256)"
    if ($env:GITHUB_ACTIONS -eq 'true' -and -not $ApprovalReader) { throw 'Local interactive approval is unavailable in Actions.' }
    Write-Host 'This approval applies only to this invocation and these exact candidate hashes.'
    if ($Action -eq 'STAGE') { Write-Host 'STAGE will create a publicly visible Git tag before creating the unpublished draft.' }
    $answer=if($ApprovalReader){ & $ApprovalReader $phrase }else{ Read-Host "Type exactly: $phrase" }
    if ([string]$answer -cne $phrase) { throw 'Explicit approval was not supplied; no remote write was made.' }
}

function Write-MgstcGitHubReceipt([string]$Path,$Value) {
    [IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 20),[Text.UTF8Encoding]::new($false))
}

function Invoke-MgstcReleaseStage {
    param([string]$CandidateDirectory,[string]$Repository,[switch]$Execute,[scriptblock]$Transport,[scriptblock]$CandidateValidator,[scriptblock]$ApprovalReader)
    Assert-MgstcGitHubRepository $Repository
    if (-not $CandidateValidator) { $CandidateValidator={ param($directory) Test-MgstcReleaseCandidate -CandidateDirectory $directory -RequirePublic } }
    $manifest=& $CandidateValidator $CandidateDirectory
    Assert-MgstcGitHubManifest $manifest
    $notes=[IO.File]::ReadAllText((Join-Path $CandidateDirectory 'RELEASE_NOTES.md'),[Text.Encoding]::UTF8)
    $body=New-MgstcGitHubReleaseBody $manifest $notes
    Write-Host ($manifest | Select-Object version,tag,commit,previousTag,previousCommit,assetsDigest,notesSha256,review | ConvertTo-Json -Depth 8)
    $lock=Enter-MgstcGitHubLock -Repository $Repository -Tag $manifest.tag
    try {
        Assert-MgstcGitHubPreviousRelease $manifest $Repository $Transport
        $draft=Get-MgstcGitHubDraft $manifest $Repository $Transport
        $tagSha=Get-MgstcGitHubTagCommit $Repository $manifest.tag $Transport
        if ($tagSha -and $tagSha -cne $manifest.commit) { throw 'Tag collision: the remote tag points to another commit.' }
        $intentPath=Join-Path $CandidateDirectory '.stage-intent.json'
        $intent=if(Test-Path -LiteralPath $intentPath){ Get-Content -LiteralPath $intentPath -Raw | ConvertFrom-Json }else{$null}
        $recoverable=$intent -and $intent.repository -ceq $Repository -and $intent.tag -ceq $manifest.tag -and $intent.commit -ceq $manifest.commit -and $intent.assetsDigest -ceq $manifest.assetsDigest -and $intent.notesSha256 -ceq $manifest.notesSha256 -and $intent.tagCreated -eq $true
        if ($draft) {
            if (-not $tagSha) { throw 'The draft tag was removed. It will not be recreated implicitly.' }
            Assert-MgstcGitHubDraftIdentity $draft $manifest $body
            $remote=@(Assert-MgstcGitHubAssets $draft $manifest $Repository $Transport -AllowMissing)
        } else {
            if ($tagSha -and -not $recoverable) { throw 'Existing tag without a matching draft or recorded interrupted stage; no mutation is allowed.' }
            $remote=@()
        }
        if (-not $Execute) { return [pscustomobject]@{status='StageValidatedNoWrites';tag=$manifest.tag;commit=$manifest.commit;assetsDigest=$manifest.assetsDigest} }
        Confirm-MgstcGitHubOperation 'STAGE' $manifest $ApprovalReader
        & $CandidateValidator $CandidateDirectory | Out-Null
        if (-not $tagSha) {
            Write-MgstcGitHubReceipt $intentPath @{repository=$Repository;tag=$manifest.tag;commit=$manifest.commit;assetsDigest=$manifest.assetsDigest;notesSha256=$manifest.notesSha256;tagCreated=$false}
            Invoke-MgstcGitHubRequest -Method POST -Path "/repos/$Repository/git/refs" -Body @{ref="refs/tags/$($manifest.tag)";sha=$manifest.commit} -Transport $Transport | Out-Null
            Write-MgstcGitHubReceipt $intentPath @{repository=$Repository;tag=$manifest.tag;commit=$manifest.commit;assetsDigest=$manifest.assetsDigest;notesSha256=$manifest.notesSha256;tagCreated=$true}
        }
        if ((Get-MgstcGitHubTagCommit $Repository $manifest.tag $Transport) -cne $manifest.commit) { throw 'Remote tag changed while staging.' }
        if (-not $draft) {
            & $CandidateValidator $CandidateDirectory | Out-Null
            $draft=Invoke-MgstcGitHubRequest -Method POST -Path "/repos/$Repository/releases" -Body @{tag_name=$manifest.tag;target_commitish=$manifest.commit;name="MGS Tone Craft $($manifest.version) Alpha";body=$body;draft=$true;prerelease=$true;make_latest='false';generate_release_notes=$false} -Transport $Transport
        }
        foreach($asset in $manifest.assets) {
            if (@($remote | Where-Object { $_.name -ceq $asset.name }).Count) { continue }
            & $CandidateValidator $CandidateDirectory | Out-Null
            Invoke-MgstcGitHubRequest -Method POST -Path "/repos/$Repository/releases/$($draft.id)/assets?name=$([uri]::EscapeDataString($asset.name))" -UploadPath (Join-Path $CandidateDirectory $asset.name) -Transport $Transport | Out-Null
        }
        $draft=Get-MgstcGitHubDraft $manifest $Repository $Transport
        Assert-MgstcGitHubDraftIdentity $draft $manifest $body
        Assert-MgstcGitHubAssets $draft $manifest $Repository $Transport | Out-Null
        if ((Get-MgstcGitHubTagCommit $Repository $manifest.tag $Transport) -cne $manifest.commit) { throw 'Remote tag changed after upload.' }
        & $CandidateValidator $CandidateDirectory | Out-Null
        $receipt=@{status='DraftVerified';repository=$Repository;tag=$manifest.tag;commit=$manifest.commit;assetsDigest=$manifest.assetsDigest;notesSha256=$manifest.notesSha256;url=$draft.html_url;assets=$manifest.assets;approved=$true}
        Write-MgstcGitHubReceipt (Join-Path $CandidateDirectory '.stage-receipt.json') $receipt
        return [pscustomobject]$receipt
    } finally { $lock.Dispose() }
}

function Assert-MgstcGitHubEnvironment([string]$Repository,[string]$Environment,[scriptblock]$Transport) {
    if ($Environment -cne 'mgstc-release') { throw 'The protected publication Environment must be mgstc-release.' }
    $environmentInfo=Invoke-MgstcGitHubRequest -Path "/repos/$Repository/environments/$([uri]::EscapeDataString($Environment))" -Transport $Transport
    $rules=@($environmentInfo.protection_rules | Where-Object { $_.type -ceq 'required_reviewers' -and @($_.reviewers).Count -gt 0 })
    if ($rules.Count -ne 1) { throw 'Environment has no configured Required reviewers; automatic publication is disabled.' }
    return [pscustomobject]@{environment=$environmentInfo;rule=$rules[0]}
}

function Assert-MgstcGitHubActionsApproval([string]$Repository,$Manifest,[string]$Environment,[scriptblock]$Transport) {
    if ($env:GITHUB_ACTIONS -cne 'true' -or $env:GITHUB_EVENT_NAME -cne 'workflow_dispatch' -or $env:GITHUB_REPOSITORY -cne $Repository -or $env:GITHUB_RUN_ID -notmatch '^\d+$' -or -not $env:GITHUB_EVENT_PATH) { throw 'Publication requires the explicit manual Actions run context.' }
    $event=Get-Content -LiteralPath $env:GITHUB_EVENT_PATH -Raw | ConvertFrom-Json
    if ($event.inputs.tag -cne $Manifest.tag -or $event.inputs.commit -cne $Manifest.commit -or $event.inputs.assets_digest -cne $Manifest.assetsDigest -or $event.inputs.notes_sha256 -cne $Manifest.notesSha256 -or $event.inputs.action -cne 'publish') { throw 'Workflow dispatch inputs do not authorize this exact candidate.' }
    $configured=Assert-MgstcGitHubEnvironment $Repository $Environment $Transport
    $run=Invoke-MgstcGitHubRequest -Path "/repos/$Repository/actions/runs/$($env:GITHUB_RUN_ID)" -Transport $Transport
    $repositoryInfo=Invoke-MgstcGitHubRequest -Path "/repos/$Repository" -Transport $Transport
    if ($run.event -cne 'workflow_dispatch' -or $run.head_branch -cne $repositoryInfo.default_branch) { throw 'Publication workflow must be manually dispatched from the default branch.' }
    $history=@(Invoke-MgstcGitHubRequest -Path "/repos/$Repository/actions/runs/$($env:GITHUB_RUN_ID)/approvals" -Transport $Transport)
    $matching=@($history | Where-Object { @($_.environments | Where-Object { $_.id -eq $configured.environment.id }).Count -gt 0 })
    if (@($matching | Where-Object state -eq 'rejected').Count) { throw 'This run has a rejected Environment review; start a new explicitly approved run.' }
    $approved=@($matching | Where-Object state -eq 'approved')
    if (-not $approved.Count) { throw 'No actual Environment approval was recorded for this run.' }
    $validReviewer=$false
    foreach($approval in $approved) {
        if ($configured.rule.prevent_self_review -and $approval.user.login -ieq $run.triggering_actor.login) { continue }
        # A configured User approval does not need organization membership
        # access merely because the Environment also lists a Team reviewer.
        if (@($configured.rule.reviewers | Where-Object { $_.type -eq 'User' -and $_.reviewer.login -ieq $approval.user.login }).Count) { $validReviewer=$true; break }
        foreach($required in $configured.rule.reviewers) {
            if ($required.type -eq 'User' -and $required.reviewer.login -ieq $approval.user.login) { $validReviewer=$true }
            elseif ($required.type -eq 'Team') {
                $organization=$Repository.Split('/')[0]
                $member=Invoke-MgstcGitHubRequest -Path "/orgs/$organization/teams/$($required.reviewer.slug)/memberships/$($approval.user.login)" -AllowNotFound -Transport $Transport
                if ($member -and $member.state -eq 'active') { $validReviewer=$true }
            }
        }
    }
    if (-not $validReviewer) { throw 'No configured Required reviewer approved this run (administrator bypass is not accepted).' }
}

function Import-MgstcGitHubCandidate {
    param([string]$Repository,[string]$Tag,[string]$Commit,[string]$AssetsDigest,[string]$NotesSha256,[string]$CandidateDirectory,[scriptblock]$Transport)
    Assert-MgstcGitHubRepository $Repository
    if ($Tag -notmatch '^v\d+\.\d+(?:\.\d+)?$' -or $Commit -notmatch '^[a-f0-9]{40}$' -or $AssetsDigest -notmatch '^[a-f0-9]{64}$' -or $NotesSha256 -notmatch '^[a-f0-9]{64}$') { throw 'Pin tag, full commit, assets digest and notes SHA before downloading a draft.' }
    if (Test-Path -LiteralPath $CandidateDirectory) { throw 'Draft download directory must be new; stale files will not be reused.' }
    $releases=@(Get-MgstcGitHubReleases $Repository $Transport | Where-Object { $_.tag_name -ceq $Tag })
    if ($releases.Count -ne 1 -or -not $releases[0].draft) { throw 'Expected one unpublished draft for this tag.' }
    $release=$releases[0]
    $parsed=Read-MgstcGitHubReleaseBody $release.body
    $manifest=$parsed.manifest
    if ($manifest.tag -cne $Tag -or $manifest.commit -cne $Commit -or $manifest.assetsDigest -cne $AssetsDigest -or $manifest.notesSha256 -cne $NotesSha256) { throw 'Draft does not match dispatch input hashes.' }
    if ((Get-MgstcGitHubTagCommit $Repository $Tag $Transport) -cne $Commit) { throw 'Draft tag SHA changed.' }
    Assert-MgstcGitHubPreviousRelease $manifest $Repository $Transport
    $assets=@(Assert-MgstcGitHubAssets $release $manifest $Repository $Transport)
    New-Item -ItemType Directory -Path $CandidateDirectory -ErrorAction Stop | Out-Null
    Write-MgstcGitHubReceipt (Join-Path $CandidateDirectory 'candidate.json') $manifest
    [IO.File]::WriteAllText((Join-Path $CandidateDirectory 'RELEASE_NOTES.md'),$parsed.notes,[Text.UTF8Encoding]::new($false))
    foreach($asset in $assets) { Invoke-MgstcGitHubRequest -Path "/repos/$Repository/releases/assets/$($asset.id)" -DownloadPath (Join-Path $CandidateDirectory $asset.name) -Transport $Transport | Out-Null }
    return $manifest
}

function Invoke-MgstcReleasePublish {
    param([string]$CandidateDirectory,[string]$Repository,[switch]$Execute,[switch]$GitHubActions,[string]$Environment='mgstc-release',[scriptblock]$Transport,[scriptblock]$CandidateValidator,[scriptblock]$ApprovalReader)
    Assert-MgstcGitHubRepository $Repository
    if (-not $CandidateValidator) { $CandidateValidator={ param($directory) Test-MgstcReleaseCandidate -CandidateDirectory $directory -RequirePublic } }
    $manifest=& $CandidateValidator $CandidateDirectory
    Assert-MgstcGitHubManifest $manifest
    $notes=[IO.File]::ReadAllText((Join-Path $CandidateDirectory 'RELEASE_NOTES.md'),[Text.Encoding]::UTF8)
    $body=New-MgstcGitHubReleaseBody $manifest $notes
    Write-Host ($manifest | Select-Object version,tag,commit,previousTag,previousCommit,assetsDigest,notesSha256,review | ConvertTo-Json -Depth 8)
    $lock=Enter-MgstcGitHubLock -Repository $Repository -Tag $manifest.tag
    try {
        Assert-MgstcGitHubPreviousRelease $manifest $Repository $Transport
        if ((Get-MgstcGitHubTagCommit $Repository $manifest.tag $Transport) -cne $manifest.commit) { throw 'Draft tag is missing or has changed.' }
        $draft=Get-MgstcGitHubDraft $manifest $Repository $Transport
        if (-not $draft) { throw 'Create and verify a draft before requesting publication.' }
        Assert-MgstcGitHubDraftIdentity $draft $manifest $body
        Assert-MgstcGitHubAssets $draft $manifest $Repository $Transport | Out-Null
        if ($GitHubActions) { Assert-MgstcGitHubEnvironment $Repository $Environment $Transport | Out-Null }
        if (-not $Execute) { return [pscustomobject]@{status='PublishValidatedNoWrites';tag=$manifest.tag;commit=$manifest.commit;assetsDigest=$manifest.assetsDigest;url=$draft.html_url} }
        if ($GitHubActions) { Assert-MgstcGitHubActionsApproval $Repository $manifest $Environment $Transport }
        else { Confirm-MgstcGitHubOperation 'PUBLISH' $manifest $ApprovalReader }
        & $CandidateValidator $CandidateDirectory | Out-Null
        Assert-MgstcGitHubPreviousRelease $manifest $Repository $Transport
        $latestDraft=Get-MgstcGitHubDraft $manifest $Repository $Transport
        Assert-MgstcGitHubDraftIdentity $latestDraft $manifest $body
        if ($latestDraft.id -ne $draft.id -or (Get-MgstcGitHubTagCommit $Repository $manifest.tag $Transport) -cne $manifest.commit) { throw 'Draft or tag changed during final approval.' }
        Assert-MgstcGitHubAssets $latestDraft $manifest $Repository $Transport | Out-Null
        & $CandidateValidator $CandidateDirectory | Out-Null
        if ((Get-MgstcGitHubTagCommit $Repository $manifest.tag $Transport) -cne $manifest.commit) { throw 'Tag changed immediately before publication.' }
        $published=Invoke-MgstcGitHubRequest -Method PATCH -Path "/repos/$Repository/releases/$($draft.id)" -Body @{draft=$false;prerelease=$true;make_latest='false';tag_name=$manifest.tag;target_commitish=$manifest.commit;body=$body} -Transport $Transport
        if ($published.draft -or -not $published.prerelease -or $published.tag_name -cne $manifest.tag) { throw 'Publication response was unexpected; inspect GitHub manually, do not retry a write.' }
        if ((Get-MgstcGitHubTagCommit $Repository $manifest.tag $Transport) -cne $manifest.commit) { throw 'Published tag SHA differs; inspect GitHub manually.' }
        Assert-MgstcGitHubAssets $published $manifest $Repository $Transport | Out-Null
        & $CandidateValidator $CandidateDirectory | Out-Null
        $receipt=@{status='PublishedAlpha';repository=$Repository;tag=$manifest.tag;commit=$manifest.commit;assetsDigest=$manifest.assetsDigest;notesSha256=$manifest.notesSha256;url=$published.html_url;assets=$manifest.assets;approved=$true;approvalMode=$(if($GitHubActions){'EnvironmentReview'}else{'InteractiveExactHashes'})}
        Write-MgstcGitHubReceipt (Join-Path $CandidateDirectory '.publish-receipt.json') $receipt
        return [pscustomobject]$receipt
    } finally { $lock.Dispose() }
}
