# SPDX-License-Identifier: AGPL-3.0-only
# Isolated release-content tests: no Git refs, product builds or network calls.
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repo 'tools/release-content.ps1')
$script:checks=0
function Assert-ContentTrue([bool]$Condition,[string]$Name) {
    $script:checks++
    if(-not $Condition){throw "ASSERTION FAILED: $Name"}
}
function Assert-ContentThrows([scriptblock]$Action,[string]$Name) {
    $script:checks++
    try { & $Action | Out-Null } catch { return }
    throw "ASSERTION FAILED: expected rejection: $Name"
}
function Copy-ContentObject($Value) { ConvertFrom-Json (ConvertTo-Json -InputObject $Value -Depth 20) }
$version='0.365';$tag='v0.297';$baseline='a'*40;$commit='b'*40
$source=@'
# Changelog

## [0.365] - Unreleased

Changes since the previous public release v0.297.

### Added

- New candidate browser with search and audition.

### Changed

- Existing library now supports updated organization controls.

### Fixed

- Existing context menus now open beneath their target control.

### Notes

- Preserve the user library when updating; keep a backup.

## [0.297] - 2026-09-19

### Notes

- Historical Alpha policy was different in this public version.
'@
$candidate=Get-MgstcChangelogCandidate $source $version
Assert-ContentTrue ($candidate.version -ceq $version -and $candidate.items.Count -eq 4) 'extract one candidate, exclude old public history'
Assert-ContentTrue ($candidate.sha256 -cmatch '^[a-f0-9]{64}$') 'candidate section SHA-256'
Assert-ContentTrue ($candidate.items[0].category -ceq 'Added' -and $candidate.items[2].category -ceq 'Fixed') 'retain classification order'
$notes=New-MgstcReleaseContentNotes $candidate
Assert-MgstcReleaseContentNotes $notes $candidate
Assert-ContentTrue ($notes.StartsWith("# MGS Tone Craft 0.365 Alpha`n") -and $notes -notmatch 'Historical Alpha') 'canonical Notes include current candidate only'
Assert-MgstcReleaseContentNotes ($notes.Replace("`n","`r`n")) $candidate
Assert-ContentTrue ($true) 'allow CRLF/LF normalization'
Assert-MgstcReleaseContentNotes (([string][char]0xfeff)+$notes) $candidate
Assert-ContentTrue ($true) 'allow a UTF-8 decoded BOM'
Assert-ContentThrows {Assert-MgstcReleaseContentNotes ($notes.Replace('### Added','### Fixed')) $candidate} 'Added recategorized as Fixed'
Assert-ContentThrows {Assert-MgstcReleaseContentNotes ($notes.Replace('Existing context menus now open beneath their target control.','')) $candidate} 'existing public bug fix omitted from Notes'
Assert-ContentThrows {Assert-MgstcReleaseContentNotes ($notes.Replace('Preserve the user library when updating; keep a backup.','')) $candidate} 'important compatibility warning omitted'
Assert-ContentThrows {Assert-MgstcReleaseContentNotes "# MGS Tone Craft 0.365 Alpha`n`n- a123456 Fix browser.`n" $candidate} 'raw commit-list Notes'
Assert-ContentThrows {Assert-MgstcReleaseContentNotes ($notes.Replace('0.365 Alpha','0.297 Alpha')) $candidate} 'Notes version mismatch'
Assert-ContentThrows {Assert-MgstcReleaseContentNotes ($notes.Replace('user library','user data')) $candidate} 'substantive Notes wording change requires regeneration'
Assert-ContentThrows {Get-MgstcChangelogCandidate $source '0.366'} 'old section cannot stand in for missing candidate'
Assert-ContentThrows {Get-MgstcChangelogCandidate ($source+"`n## [0.365]`n### Added`n- Duplicate candidate.`n") $version} 'duplicate candidate sections'
Assert-ContentThrows {Get-MgstcChangelogCandidate ($source.Replace('### Changed','### Added')) $version} 'duplicate category'
Assert-ContentThrows {Get-MgstcChangelogCandidate ($source.Replace('### Changed','### Internal')) $version} 'unknown classification'
Assert-ContentThrows {Get-MgstcChangelogCandidate ($source.Replace('- Existing library now supports updated organization controls.','')) $version} 'empty category'
Assert-ContentThrows {Get-MgstcChangelogCandidate ($source.Replace('## [0.365] - Unreleased','## [0.365] - tomorrow')) $version} 'malformed candidate header'
Assert-ContentThrows {Get-MgstcChangelogCandidate ($source.Replace('New candidate browser with search and audition.','a123456 Fix browser during development.')) $version} 'commit-list Changelog candidate'
Assert-ContentThrows {Get-MgstcChangelogCandidate "<!--`n## [0.365]`n### Added`n- Hidden feature. -->`n## [0.297]`n" $version} 'commented candidate cannot become public Notes'
Assert-ContentThrows {Get-MgstcChangelogCandidate ($source.Replace('Changes since the previous public release v0.297.',"abc1234 Fix temporary search crash`ndef5678 Adjust new feature")) $version} 'unclassified raw commit introduction'
Assert-ContentThrows {Get-MgstcChangelogCandidate ($source.Replace('Changes since the previous public release v0.297.','A development feature outside classification.')) $version} 'uncategorized user change cannot evade audit coverage'
Assert-ContentThrows {Get-MgstcChangelogCandidate ($source.Replace('### Added',"<!-- hidden category -->`n### Added")) $version} 'candidate comment content cannot escape item coverage'
Assert-ContentThrows {Get-MgstcChangelogCandidate ($source.Replace('Existing library now supports updated organization controls.','New candidate browser with search and audition.')) $version} 'same event duplicated under different classifications'
$undated=Get-MgstcChangelogCandidate ($source.Replace(' - Unreleased','')) $version
$dated=Get-MgstcChangelogCandidate ($source.Replace('Unreleased','2026-10-29')) $version
Assert-ContentTrue ($undated.items.Count -eq 4 -and $dated.items.Count -eq 4) 'no fabricated date required; dated historical format still supported'
$wrapped=Get-MgstcChangelogCandidate ($source.Replace('with search and audition.','with search' + "`n  and audition.")) $version
Assert-ContentTrue ($wrapped.items[0].text -match "`n") 'wrapped bullet text is audited as one item'
$auditItems=@(foreach($item in $candidate.items) {
    $record=[ordered]@{id=('change-'+$item.category);category=$item.category;sha256=$item.sha256;baselineState=$(if($item.category -ceq 'Added'){'absent'}elseif($item.category -ceq 'Notes'){'not-applicable'}else{'present'});evidence=@('PRIVATE_EVIDENCE: inspected prior public source and current product behavior.')}
    if($item.category -ceq 'Fixed'){$record.baselineBugConfirmed=$true}
    if($item.category -ceq 'Notes'){$record.currentRelevant=$true}
    [pscustomobject]$record
})
$audit=[pscustomobject]@{version=$version;previousTag=$tag;previousCommit=$baseline;baselineVerified=$true;items=$auditItems}
$summary=Assert-MgstcReleaseChangeAudit $audit $candidate $tag $baseline
Assert-ContentTrue ($summary.changelogSectionSha256 -ceq $candidate.sha256 -and $summary.classificationDigest -cmatch '^[a-f0-9]{64}$') 'valid complete published-baseline classification audit'
Assert-ContentTrue ((ConvertTo-Json $summary) -notmatch 'PRIVATE_EVIDENCE|change-Added|evidence|baselineBugConfirmed') 'public binding contains neither private evidence nor change IDs'
$bad=Copy-ContentObject $audit;$bad.items[2].baselineState='absent'
Assert-ContentThrows {Assert-MgstcReleaseChangeAudit $bad $candidate $tag $baseline} 'unpublished feature bug cannot be public Fixed'
$bad=Copy-ContentObject $audit;$bad.items[2].baselineBugConfirmed=$false
Assert-ContentThrows {Assert-MgstcReleaseChangeAudit $bad $candidate $tag $baseline} 'unconfirmed prior public bug cannot be Fixed'
$bad=Copy-ContentObject $audit;$bad.items[2].baselineBugConfirmed='true'
Assert-ContentThrows {Assert-MgstcReleaseChangeAudit $bad $candidate $tag $baseline} 'string is not confirmed boolean evidence'
$bad=Copy-ContentObject $audit;$bad.items[0].baselineState='present'
Assert-ContentThrows {Assert-MgstcReleaseChangeAudit $bad $candidate $tag $baseline} 'already public feature cannot be new Added'
$bad=Copy-ContentObject $audit;$bad.items[3].currentRelevant=$false
Assert-ContentThrows {Assert-MgstcReleaseChangeAudit $bad $candidate $tag $baseline} 'resolved interim restriction cannot remain a current Note'
$bad=Copy-ContentObject $audit;$bad.baselineVerified=$false
Assert-ContentThrows {Assert-MgstcReleaseChangeAudit $bad $candidate $tag $baseline} 'unknown public baseline cannot be finalized'
$bad=Copy-ContentObject $audit;$bad.baselineVerified=@($true)
Assert-ContentThrows {Assert-MgstcReleaseChangeAudit $bad $candidate $tag $baseline} 'array cannot impersonate a verified boolean'
$bad=Copy-ContentObject $audit;$bad.previousCommit='c'*40
Assert-ContentThrows {Assert-MgstcReleaseChangeAudit $bad $candidate $tag $baseline} 'audit for another baseline cannot be reused'
$bad=Copy-ContentObject $audit;$bad.items=@($bad.items|Where-Object category -ne 'Fixed')
Assert-ContentThrows {Assert-MgstcReleaseChangeAudit $bad $candidate $tag $baseline} 'missing existing public Fixed item'
$bad=Copy-ContentObject $audit;$bad.items[0].category='Fixed'
Assert-ContentThrows {Assert-MgstcReleaseChangeAudit $bad $candidate $tag $baseline} 'audit and Changelog category differ'
$bad=Copy-ContentObject $audit;$bad.items[1].id=$bad.items[0].id
Assert-ContentThrows {Assert-MgstcReleaseChangeAudit $bad $candidate $tag $baseline} 'audit IDs must be unique'
$bad=Copy-ContentObject $audit;$bad.items[1].evidence=@()
Assert-ContentThrows {Assert-MgstcReleaseChangeAudit $bad $candidate $tag $baseline} 'missing history evidence'
$changed=Get-MgstcChangelogCandidate ($source.Replace('Existing context menus','Previously published context menus')) $version
Assert-ContentThrows {Assert-MgstcReleaseChangeAudit $audit $changed $tag $baseline} 'changed Changelog invalidates audit mapping'
$reordered=Copy-ContentObject $audit;$reordered.items=@($reordered.items|Sort-Object id -Descending)
$same=Assert-MgstcReleaseChangeAudit $reordered $candidate $tag $baseline
Assert-ContentTrue ($same.classificationDigest -ceq $summary.classificationDigest) 'classification digest independent of audit row order'
$binding=[pscustomobject]@{version=$version;previousTag=$tag;previousCommit=$baseline;candidateCommit=$commit;changelogSectionSha256=$summary.changelogSectionSha256;classificationDigest=$summary.classificationDigest;notesSha256=(Get-MgstcReleaseContentHash $notes)}
$digest=Assert-MgstcReleaseContentBinding $binding $version $commit $tag $baseline $binding.notesSha256
Assert-ContentTrue ($digest -cmatch '^[a-f0-9]{64}$') 'candidate binding digest'
$bad=Copy-ContentObject $binding;$bad.candidateCommit='d'*40
Assert-ContentThrows {Assert-MgstcReleaseContentBinding $bad $version $commit $tag $baseline $binding.notesSha256} 'different candidate commit'
$bad=Copy-ContentObject $binding;$bad.notesSha256='e'*64
Assert-ContentThrows {Assert-MgstcReleaseContentBinding $bad $version $commit $tag $baseline $binding.notesSha256} 'different approved Notes hash'
$bad=Copy-ContentObject $binding;$bad.classificationDigest='not-a-hash'
Assert-ContentThrows {Assert-MgstcReleaseContentBinding $bad $version $commit $tag $baseline $binding.notesSha256} 'malformed classification seal'
$bad=Copy-ContentObject $binding;$bad.version=@($version)
Assert-ContentThrows {Assert-MgstcReleaseContentBinding $bad $version $commit $tag $baseline $binding.notesSha256} 'binding identities must be scalars'
$bad=Copy-ContentObject $binding;$bad|Add-Member evidence 'PRIVATE_EVIDENCE'
Assert-ContentThrows {Assert-MgstcReleaseContentBinding $bad $version $commit $tag $baseline $binding.notesSha256} 'private audit fields cannot enter public binding'
Write-Output "Release-content checks passed: $script:checks"
