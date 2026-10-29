# SPDX-License-Identifier: AGPL-3.0-only
# Canonical release text and private classification-audit validation.
# These checks seal reviewed evidence; they do not infer product history.
function ConvertTo-MgstcReleaseContentText([string]$Text) {
    if ($null -eq $Text) { $Text='' }
    if ($Text.StartsWith([string][char]0xfeff,[StringComparison]::Ordinal)) { $Text=$Text.Substring(1) }
    $Text.Replace("`r`n","`n").Replace("`r","`n").TrimEnd([char]10)+"`n"
}
function Get-MgstcReleaseContentHash([string]$Text) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Get-MgstcReleaseContentProperty($Object,[string]$Name,[type]$RequiredType=$null) {
    if ($null -eq $Object -or -not $Object.PSObject.Properties[$Name]) { throw "Release content is missing required field: $Name" }
    $value=$Object.PSObject.Properties[$Name].Value
    if($RequiredType -and -not $RequiredType.IsInstanceOfType($value)){throw "Release-content field has an invalid scalar type: $Name"}
    $value
}
function Get-MgstcChangelogCandidate([string]$Text,[string]$Version) {
    if ($Version -cnotmatch '^\d+\.\d+(?:\.\d+)?$') { throw 'Invalid CHANGELOG candidate version.' }
    $normalized=ConvertTo-MgstcReleaseContentText $Text
    $lines=@($normalized.Split([char]10))
    # Preserve line positions while excluding invisible candidate headings.
    $structuralText=[regex]::Replace($normalized,'(?s)<!--.*?-->',{param($match) [regex]::Replace($match.Value,'[^\n]',' ')})
    if($structuralText.Contains('<!--')){throw 'Unterminated CHANGELOG HTML comment.'}
    $structural=@($structuralText.Split([char]10))
    $matching=New-Object 'System.Collections.Generic.List[int]'
    $fence=''
    for($i=0;$i -lt $lines.Count;$i++) {
        $trimmed=$structural[$i].Trim()
        if($fence) {
            if($trimmed -match ('^'+[regex]::Escape($fence[0])+'{'+$fence.Length+',}\s*$')){$fence=''}
            continue
        }
        if($trimmed -match '^(?<fence>`{3,}|~{3,})'){$fence=$Matches.fence;continue}
        if($structural[$i] -match ('^##\s+\['+[regex]::Escape($Version)+'\](?:\s|$)')){$matching.Add($i)}
    }
    if($matching.Count -ne 1){throw "CHANGELOG must contain exactly one candidate section [$Version]."}
    $start=$matching[0]
    if($lines[$start] -cnotmatch ('^## \['+[regex]::Escape($Version)+'\](?: - (?:\d{4}-\d{2}-\d{2}|Unreleased))?\s*$')){throw "Malformed CHANGELOG candidate heading [$Version]."}
    $end=$lines.Count
    for($i=$start+1;$i -lt $lines.Count;$i++) {
        if($structural[$i] -match '^##\s+\['){$end=$i;break}
    }
    $section=ConvertTo-MgstcReleaseContentText ($lines[$start..($end-1)] -join "`n")
    $body=($lines[($start+1)..($end-1)] -join "`n").Trim([char]10)
    $items=New-Object 'System.Collections.Generic.List[object]'
    $seenCategories=New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    $seenItems=New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    $category='';$itemLines=New-Object 'System.Collections.Generic.List[string]';$categoryCount=0
    function Add-MgstcChangelogItem {
        if($itemLines.Count -eq 0){return}
        $itemText=($itemLines.ToArray() -join "`n").TrimEnd()
        if($itemText -match '(?i)^(?:[0-9a-f]{7,40}\s+|(?:commit|merge commit)\s+[0-9a-f]{7,40}\b)'){throw 'Raw commit-list entries are not release change descriptions.'}
        $itemSha=Get-MgstcReleaseContentHash $itemText
        if(-not $seenItems.Add($itemSha)){throw 'Duplicate CHANGELOG change item; classify each user impact once.'}
        $items.Add([pscustomobject]@{category=$category;text=$itemText;sha256=$itemSha})
        $itemLines.Clear()
    }
    for($i=$start+1;$i -lt $end;$i++) {
        $line=$lines[$i]
        if($line -match '<!--|-->'){throw 'Candidate CHANGELOG content cannot hide classifications or prose inside HTML comments.'}
        if($line -match '(?i)^\s*(?:[-*]\s+)?(?:[0-9a-f]{7,40}\s+|(?:commit|merge commit)\s+[0-9a-f]{7,40}\b)'){throw 'Raw commit-list entries are not release change descriptions.'}
        if($line -cmatch '^### (Added|Changed|Fixed|Removed|Notes)\s*$') {
            Add-MgstcChangelogItem
            if($category -and $categoryCount -eq 0){throw "Empty CHANGELOG category: $category"}
            $category=$Matches[1]
            if(-not $seenCategories.Add($category)){throw "Duplicate CHANGELOG category: $category"}
            $categoryCount=0;continue
        }
        if($line -match '^\s*#|^\s*(`{3,}|~{3,})'){throw "Unsupported CHANGELOG structure at line $($i+1); use the five release categories and bullets."}
        if(-not $line.Trim()){continue}
        if($line -match '^[-*] (?<item>\S.*)$') {
            if(-not $category){throw 'CHANGELOG change items require a classification heading.'}
            Add-MgstcChangelogItem
            $itemLines.Add($Matches.item);$categoryCount++;continue
        }
        if($category) {
            if($itemLines.Count -eq 0 -or $line -notmatch '^\s{2,}\S'){throw "Malformed CHANGELOG change item at line $($i+1)."}
            $itemLines.Add($line.Trim());continue
        }
        # Only a baseline introduction may sit outside classified bullets.
        if($line -notmatch '^(?:比較元|前回(?:実)?公開版|(?i:Changes\s+(?:since|from)|Compared\s+with|Comparison\s+with)\b)'){throw "Unclassified CHANGELOG prose at line $($i+1); place user changes under a category."}
    }
    Add-MgstcChangelogItem
    if($category -and $categoryCount -eq 0){throw "Empty CHANGELOG category: $category"}
    if($items.Count -eq 0){throw 'CHANGELOG candidate has no classified change items.'}
    [pscustomobject]@{version=$Version;text=$section;body=$body;sha256=(Get-MgstcReleaseContentHash $section);items=$items.ToArray()}
}
function New-MgstcReleaseContentNotes($Candidate) {
    $version=[string](Get-MgstcReleaseContentProperty $Candidate 'version' ([string]))
    $body=[string](Get-MgstcReleaseContentProperty $Candidate 'body' ([string]))
    ConvertTo-MgstcReleaseContentText ("# MGS Tone Craft $version Alpha`n`n"+$body+"`n")
}
function Assert-MgstcReleaseContentNotes([string]$Text,$Candidate) {
    $expected=New-MgstcReleaseContentNotes $Candidate
    if((ConvertTo-MgstcReleaseContentText $Text) -cne $expected){throw 'Release Notes differ from the classified CHANGELOG candidate; regenerate and review both together.'}
}
function Assert-MgstcReleaseChangeAudit($Audit,$Candidate,[string]$PreviousTag,[string]$PreviousCommit) {
    $verified=Get-MgstcReleaseContentProperty $Audit 'baselineVerified' ([bool])
    if($verified -isnot [bool] -or -not $verified){throw 'Published baseline must be verified before classifying release changes.'}
    if($PreviousTag -cnotmatch '^v\d+\.\d+(?:\.\d+)?$' -or $PreviousCommit -cnotmatch '^[a-f0-9]{40}$'){throw 'Pin the published baseline tag and full commit SHA.'}
    $version=[string](Get-MgstcReleaseContentProperty $Candidate 'version')
    foreach($field in @('version','previousTag','previousCommit')) {
        if((Get-MgstcReleaseContentProperty $Audit $field ([string])) -isnot [string]){throw "Classification audit identity must be a string: $field"}
    }
    if((Get-MgstcReleaseContentProperty $Audit 'version') -cne $version -or
       (Get-MgstcReleaseContentProperty $Audit 'previousTag') -cne $PreviousTag -or
       (Get-MgstcReleaseContentProperty $Audit 'previousCommit') -cne $PreviousCommit){throw 'Classification audit does not cover this version and published baseline.'}
    $candidateItems=@(Get-MgstcReleaseContentProperty $Candidate 'items')
    $auditItems=@(Get-MgstcReleaseContentProperty $Audit 'items')
    if($auditItems.Count -ne $candidateItems.Count){throw 'Classification audit must cover every CHANGELOG item exactly once.'}
    $expected=New-Object 'System.Collections.Generic.Dictionary[string,object]' ([StringComparer]::Ordinal)
    foreach($item in $candidateItems){$expected.Add($item.sha256,$item)}
    $seen=New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    $ids=New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    $sealed=New-Object 'System.Collections.Generic.List[object]'
    foreach($item in $auditItems) {
        $id=Get-MgstcReleaseContentProperty $item 'id' ([string])
        $sha=Get-MgstcReleaseContentProperty $item 'sha256' ([string])
        $category=Get-MgstcReleaseContentProperty $item 'category' ([string])
        $state=Get-MgstcReleaseContentProperty $item 'baselineState' ([string])
        $evidence=@(Get-MgstcReleaseContentProperty $item 'evidence')
        if($id -isnot [string] -or [string]::IsNullOrWhiteSpace($id) -or -not $ids.Add($id)){throw 'Audit change IDs must be nonempty and unique.'}
        if($sha -isnot [string] -or -not $expected.ContainsKey($sha) -or -not $seen.Add($sha)){throw 'Unknown, duplicate or changed item in classification audit.'}
        if($category -isnot [string] -or $state -isnot [string]){throw 'Classification category and prior state must be strings.'}
        if($category -cne $expected[$sha].category){throw 'CHANGELOG classification does not match the reviewed item.'}
        if($evidence.Count -eq 0){throw 'Classification audit requires product-history evidence.'}
        foreach($entry in $evidence){if($entry -isnot [string] -or [string]::IsNullOrWhiteSpace($entry)){throw 'Classification audit requires nonempty textual evidence.'}}
        if($state -cnotin @('absent','present','not-applicable')){throw 'Unknown prior publication state cannot be classified.'}
        if($category -ceq 'Added' -and $state -cne 'absent'){throw 'Added changes must be absent from the prior public version.'}
        if($category -cin @('Changed','Fixed','Removed') -and $state -cne 'present'){throw 'Changed, Fixed and Removed require an existing public feature; initial-publication work belongs in Added.'}
        $bug=$null;$relevant=$null
        if($category -ceq 'Fixed') {
            $bug=Get-MgstcReleaseContentProperty $item 'baselineBugConfirmed' ([bool])
            if($bug -isnot [bool] -or -not $bug){throw 'Fixed requires a confirmed bug in the prior public version.'}
        }
        if($category -ceq 'Notes') {
            $relevant=Get-MgstcReleaseContentProperty $item 'currentRelevant' ([bool])
            if($relevant -isnot [bool] -or -not $relevant){throw 'Notes must describe a current user impact, not a resolved interim restriction.'}
        }
        $sealed.Add([ordered]@{id=$id;category=$category;sha256=$sha;baselineState=$state;evidence=$evidence;baselineBugConfirmed=$bug;currentRelevant=$relevant})
    }
    $record=[ordered]@{version=$version;previousTag=$PreviousTag;previousCommit=$PreviousCommit;baselineVerified=$true;items=@($sealed.ToArray()|Sort-Object {$_['sha256']})}
    $digest=Get-MgstcReleaseContentHash (ConvertTo-Json -InputObject $record -Depth 12 -Compress)
    [pscustomobject]@{version=$version;previousTag=$PreviousTag;previousCommit=$PreviousCommit;changelogSectionSha256=$Candidate.sha256;classificationDigest=$digest}
}
function Assert-MgstcReleaseContentBinding($Binding,[string]$Version,[string]$Commit,[string]$PreviousTag,[string]$PreviousCommit,[string]$NotesSha256) {
    $names=@('version','previousTag','previousCommit','candidateCommit','changelogSectionSha256','classificationDigest','notesSha256')
    if($null -eq $Binding -or @($Binding.PSObject.Properties).Count -ne $names.Count){throw 'Release-content binding must contain only its seven public fields.'}
    $values=New-Object 'System.Collections.Generic.List[string]'
    foreach($name in $names) {
        $value=Get-MgstcReleaseContentProperty $Binding $name ([string])
        if($value -isnot [string]){throw "Release-content binding field must be a string: $name"}
        if($name -cin @('changelogSectionSha256','classificationDigest','notesSha256') -and $value -cnotmatch '^[a-f0-9]{64}$'){throw "Invalid release-content SHA-256: $name"}
        $values.Add($name+'='+$value)
    }
    if($Version -cnotmatch '^\d+\.\d+(?:\.\d+)?$' -or $Commit -cnotmatch '^[a-f0-9]{40}$' -or
       $PreviousTag -cnotmatch '^v\d+\.\d+(?:\.\d+)?$' -or $PreviousCommit -cnotmatch '^[a-f0-9]{40}$' -or $NotesSha256 -cnotmatch '^[a-f0-9]{64}$'){throw 'Invalid expected release-content identity.'}
    if($Binding.version -cne $Version -or $Binding.candidateCommit -cne $Commit -or $Binding.previousTag -cne $PreviousTag -or
       $Binding.previousCommit -cne $PreviousCommit -or $Binding.notesSha256 -cne $NotesSha256){throw 'Release-content binding differs from the approved candidate identity.'}
    Get-MgstcReleaseContentHash (($values.ToArray() -join "`n")+"`n")
}
