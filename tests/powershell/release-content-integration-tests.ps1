# SPDX-License-Identifier: AGPL-3.0-only
# Isolated Notes/Prepare bindings; no Git, product build or network writes.
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repo 'tools/release-common.ps1')
$checks=0
function Check([bool]$Value,[string]$Name){if(-not $Value){throw $Name};$script:checks++}
function Rejected([scriptblock]$Action,[string]$Name){$failed=$false;try{& $Action|Out-Null}catch{$failed=$true};Check $failed $Name}
$temp=Join-Path ([IO.Path]::GetTempPath()) ('mgstc-content-integration-'+[guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $temp)
try {
    $log="## [0.365]`n`nCompared with public v0.297.`n`n### Added`n- A new candidate-search workflow.`n`n### Fixed`n- Repair an existing startup connection wait.`n`n### Notes`n- Preserve the user's SQLite library when updating.`n`n## [0.297] - 2026-09-19`n`n### Notes`n- Historical Alpha compatibility restrictions.`n"
    $logPath=Join-Path $temp 'CHANGELOG.md'
    [IO.File]::WriteAllText($logPath,$log,[Text.UTF8Encoding]::new($false))
    $candidate=Get-MgstcChangelogCandidate $log '0.365'
    $items=foreach($item in $candidate.items){
        $row=[ordered]@{id=$item.category;category=$item.category;sha256=$item.sha256;baselineState='present';evidence='Isolated baseline source proof fixture.'}
        if($item.category -eq 'Added'){$row.baselineState='absent'}
        if($item.category -eq 'Fixed'){$row.baselineBugConfirmed=$true}
        if($item.category -eq 'Notes'){$row.currentRelevant=$true}
        [pscustomobject]$row
    }
    $baseline='b'*40;$commit='a'*40
    $audit=[pscustomobject]@{version='0.365';previousTag='v0.297';previousCommit=$baseline;baselineVerified=$true;items=@($items)}
    $auditPath=Join-Path $temp 'audit.json'
    [IO.File]::WriteAllText($auditPath,($audit|ConvertTo-Json -Depth 9),[Text.UTF8Encoding]::new($false))
    $content=Get-MgstcPreparedReleaseContent -ProjectRoot $temp -Version '0.365' -PreviousTag 'v0.297' -PreviousCommit $baseline -CandidateCommit $commit -ChangeAuditFile $auditPath
    Check ($content.digest -match '^[a-f0-9]{64}$') 'canonical reviewed content binding'
    $review=[pscustomobject]@{releaseContentDigest=$content.digest}
    $m=[pscustomobject]@{version='0.365';commit=$commit;previousTag='v0.297';previousCommit=$baseline;notesSha256=$content.binding.notesSha256;releaseContent=$content.binding;review=$review}
    Test-MgstcReleaseContentCandidate $temp $m $content.notes
    Check $true 'formal committed-section and Notes agree'
    $before=Get-FileHash $logPath
    Rejected {Get-MgstcPreparedReleaseContent -ProjectRoot $temp -Version '0.365' -PreviousTag 'v0.297' -PreviousCommit $baseline -CandidateCommit $commit} 'formal audit required'
    Check ((Get-FileHash $logPath).Hash -ceq $before.Hash) 'readonly Prepare leaves CHANGELOG unchanged'
    $notesPath=Join-Path $temp 'notes.md'
    [IO.File]::WriteAllText($notesPath,$content.notes.Replace("`n","`r`n"),[Text.UTF8Encoding]::new($true))
    $copy=Get-MgstcPreparedReleaseContent -ProjectRoot $temp -Version '0.365' -PreviousTag 'v0.297' -PreviousCommit $baseline -CandidateCommit $commit -ChangeAuditFile $auditPath -ReleaseNotesFile $notesPath
    Check ($copy.digest -ceq $content.digest) 'BOM and CRLF normalize to identical approval'
    [IO.File]::WriteAllText($notesPath,$content.notes.Replace('### Added','### Fixed'),[Text.UTF8Encoding]::new($false))
    Rejected {Get-MgstcPreparedReleaseContent -ProjectRoot $temp -Version '0.365' -PreviousTag 'v0.297' -PreviousCommit $baseline -CandidateCommit $commit -ChangeAuditFile $auditPath -ReleaseNotesFile $notesPath} 'custom draft cannot recategorize Added as Fixed'
    Rejected {Test-MgstcReleaseContentCandidate $temp $m ($content.notes+'Changed notes')} 'Notes changed after approval'
    $m.commit='c'*40
    Rejected {Test-MgstcReleaseContentCandidate $temp $m $content.notes} 'candidate commit changed after approval'
    $m.commit=$commit;$m.review.releaseContentDigest='d'*64
    Rejected {Test-MgstcReleaseContentCandidate $temp $m $content.notes} 'missing or stale content review'
    $m.review.releaseContentDigest=$content.digest
    [IO.File]::WriteAllText($logPath,$log.Replace('Compared with public','Comparison with public'),[Text.UTF8Encoding]::new($false))
    $changed=Get-MgstcChangelogCandidate ([IO.File]::ReadAllText($logPath)) '0.365'
    Rejected {Test-MgstcReleaseContentCandidate $temp $m (New-MgstcReleaseContentNotes $changed)} 'CHANGELOG and Notes edited together still invalidate approved hashes'
    [IO.File]::WriteAllText($logPath,$log.Replace('## [0.365]','## [0.365] - Unreleased'),[Text.UTF8Encoding]::new($false))
    Rejected {Get-MgstcPreparedReleaseContent -ProjectRoot $temp -Version '0.365' -PreviousTag 'v0.297' -PreviousCommit $baseline -CandidateCommit $commit -ChangeAuditFile $auditPath} 'Unreleased cannot become formal candidate silently'
    $proto=Get-MgstcPreparedReleaseContent -ProjectRoot $temp -Version '0.365' -PreviousTag 'v0.297' -PreviousCommit $baseline -CandidateCommit $commit -Prototype
    Check ($null -eq $proto.binding) 'prototype without audit has no public approval binding'
    Rejected {Assert-MgstcReleaseContentReview $review $proto.digest} 'prototype cannot provide reviewed content approval'
    . (Join-Path $repo 'tools/release-github.ps1')
    function Test-MgstcReleaseCandidate {param($CandidateDirectory,[switch]$RequirePublic) throw 'Release content validation failed before remote operations'}
    foreach($action in @('Stage','Publish')){
        $message=''
        try{if($action -eq 'Stage'){Invoke-MgstcReleaseStage -CandidateDirectory $temp -Repository 'example/product'}else{Invoke-MgstcReleasePublish -CandidateDirectory $temp -Repository 'example/product'}}catch{$message=$_.Exception.Message}
        Check ($message -eq 'Release content validation failed before remote operations') "$action stops on content validation failure"
    }
    Write-Output "Release-content integration checks passed: $checks"
}finally{
    $resolved=[IO.Path]::GetFullPath($temp)
    if((Split-Path -Parent $resolved) -ne ([IO.Path]::GetTempPath()).TrimEnd('\','/') -or (Split-Path -Leaf $resolved) -notlike 'mgstc-content-integration-*'){throw 'Unsafe test cleanup path'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
