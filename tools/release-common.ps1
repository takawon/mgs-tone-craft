# SPDX-License-Identifier: AGPL-3.0-only
# Shared, read-only release gates. No network mutation belongs in this file.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'distribution-common.ps1')
. (Join-Path $PSScriptRoot 'readme-release-gate.ps1')
. (Join-Path $PSScriptRoot 'release-content.ps1')

function Get-MgstcReleaseSha256([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Required file missing: $Path" }
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Get-MgstcReleaseTextDigest([string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Get-MgstcReleaseSourceHash([string]$Path, [byte[]]$Bytes) {
    # Match Git's text content across Windows checkouts and source archives.
    # Latin-1 is used only as a reversible byte mapping, not a text decoder.
    if ($Path -notmatch '\.(?:png|ico|wav|mgstc|scc|bin|mid)$') {
        $mapping=[Text.Encoding]::GetEncoding(28591)
        $Bytes=$mapping.GetBytes($mapping.GetString($Bytes).Replace("`r`n","`n"))
    }
    $sha=[Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Write-MgstcReleaseUtf8([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}
function Assert-MgstcReleaseNotes([string]$Text,[string]$Version) {
    if ($Text -notmatch ('\A# MGS Tone Craft ' + [regex]::Escape($Version) + ' Alpha\r?\n')) { throw 'Release Notes must start with the exact candidate version heading.' }
    Assert-MgstcReleaseText 'RELEASE_NOTES.md' $Text
}
function Get-MgstcPreparedReleaseContent {
    param([string]$ProjectRoot,[string]$Version,[string]$PreviousTag,[string]$PreviousCommit,[string]$CandidateCommit,[string]$ChangeAuditFile='',[string]$ReleaseNotesFile='',[switch]$Prototype)
    $candidate=Get-MgstcChangelogCandidate ([IO.File]::ReadAllText((Join-Path $ProjectRoot 'CHANGELOG.md'))) $Version
    if(-not $Prototype -and $candidate.text -match '(?im)^## .*\bUnreleased\b'){throw 'CHANGELOG candidate is Unreleased. Review and confirm its version heading before formal Prepare; a publication date is optional.'}
    $notes=New-MgstcReleaseContentNotes $candidate
    if($ReleaseNotesFile) {
        $path=Get-MgstcAbsolutePath $ReleaseNotesFile $ProjectRoot
        Assert-MgstcReleaseContentNotes ([IO.File]::ReadAllText($path)) $candidate
    }
    $binding=$null;$digest=$null
    if($ChangeAuditFile) {
        $path=Get-MgstcAbsolutePath $ChangeAuditFile $ProjectRoot
        $audit=Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
        $binding=Assert-MgstcReleaseChangeAudit $audit $candidate $PreviousTag $PreviousCommit
        $binding|Add-Member candidateCommit $CandidateCommit
        $binding|Add-Member notesSha256 (Get-MgstcReleaseTextDigest $notes)
        $digest=Assert-MgstcReleaseContentBinding $binding $Version $CandidateCommit $PreviousTag $PreviousCommit (Get-MgstcReleaseTextDigest $notes)
    } elseif(-not $Prototype) {throw 'Formal Prepare requires -ChangeAuditFile with the verified previous-public-release classification audit.'}
    [pscustomobject]@{candidate=$candidate;notes=$notes;binding=$binding;digest=$digest}
}
function Assert-MgstcReleaseContentReview($Review,[string]$Digest) {
    if(-not $Digest -or $null -eq $Review -or -not $Review.PSObject.Properties['releaseContentDigest'] -or $Review.releaseContentDigest -cne $Digest) {
        throw 'Release classification and Notes require review of this exact releaseContentDigest.'
    }
}
function Test-MgstcReleaseContentCandidate([string]$ProjectRoot,$Manifest,[string]$NotesText) {
    $candidate=Get-MgstcChangelogCandidate ([IO.File]::ReadAllText((Join-Path $ProjectRoot 'CHANGELOG.md'))) $Manifest.version
    if($candidate.text -match '(?im)^## .*\bUnreleased\b'){throw 'Formal release CHANGELOG remains Unreleased.'}
    Assert-MgstcReleaseContentNotes $NotesText $candidate
    if(-not $Manifest.PSObject.Properties['releaseContent']){throw 'Candidate has no reviewed CHANGELOG/Release Notes binding. Prepare a new candidate.'}
    $digest=Assert-MgstcReleaseContentBinding $Manifest.releaseContent $Manifest.version $Manifest.commit $Manifest.previousTag $Manifest.previousCommit $Manifest.notesSha256
    if($Manifest.releaseContent.changelogSectionSha256 -cne $candidate.sha256){throw 'CHANGELOG candidate section differs from the reviewed release content.'}
    Assert-MgstcReleaseContentReview $Manifest.review $digest
}
function Get-MgstcReleaseFilesDigest($Files) {
    $sorted = New-Object 'System.Collections.Generic.SortedDictionary[string,string]' ([StringComparer]::Ordinal)
    foreach ($file in $Files) { $sorted.Add([string]$file.path, ([string]$file.sha256).ToLowerInvariant()) }
    $lines = @($sorted.GetEnumerator() | ForEach-Object { $_.Key + "`t" + $_.Value })
    Get-MgstcReleaseTextDigest ($lines -join "`n")
}
function Get-MgstcReleaseAssetDigest($Assets) {
    $sorted = New-Object 'System.Collections.Generic.SortedDictionary[string,string]' ([StringComparer]::Ordinal)
    foreach ($asset in $Assets) {
        $key = ([string]$asset.name).ToLowerInvariant()
        $sorted.Add($key, $key + "`t" + ([long]$asset.size).ToString([Globalization.CultureInfo]::InvariantCulture) + "`t" + ([string]$asset.sha256).ToLowerInvariant())
    }
    Get-MgstcReleaseTextDigest (@($sorted.Values) -join "`n")
}
function Invoke-MgstcReleaseGit([string]$ProjectRoot, [string[]]$Arguments) {
    $result = @(& git -c core.quotepath=false -C $ProjectRoot @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "Git read failed: $($Arguments[0])" }
    $result
}
function Assert-MgstcReleasePath([string]$Path) {
    # Public source roots, rather than a list of private research filenames.
    if ($Path -match '[\x00-\x1f]|(^|/)\.\.?(/|$)|\\|^/' -or
        $Path -notmatch '^(src/|third_party/|assets/|cmake/|installer/|tools/|tests/(cpp/|powershell/|test_[^/]+\.py$)|docs/|\.github/workflows/|(?:README\.md|BUILDING\.md|CHANGELOG\.md|THIRD_PARTY\.md|LICENSE|VERSION|CMakeLists\.txt|build\.cmd|\.gitignore|\.gitattributes)$)') {
        throw "File is outside public source roots: $Path"
    }
    $segments=$Path.Split('/')
    for($i=0;$i -lt $segments.Count;$i++) {
        if ($segments[$i].StartsWith('.') -and $segments[$i] -notin @('.gitignore','.gitattributes','.clang-format','.clang-tidy','.editorconfig') -and
            -not ($i -eq 0 -and $segments[$i] -eq '.github' -and $Path.StartsWith('.github/workflows/'))) { throw "Hidden non-source path: $Path" }
    }
    if ($Path -match '(?i)(^|/)(?:node_modules|__pycache__|fixtures|audits|private|local)(/|$)|\.(?:pdb|obj|ilk|dmp|log|sqlite|db|ini|zip|7z|exe|dll)$') { throw "Non-source file in release tree: $Path" }
}
function Assert-MgstcReleaseText([string]$Path, [string]$Text) {
    # Generic credential and personal-home detection; no private case inventory.
    if ($Text -match '(?i)[A-Z]:[\\/]+Users[\\/]+[^\\/\s"'']+|-----BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY-----|\bgh[pousr]_[A-Za-z0-9]{30,}|\bgithub_pat_[A-Za-z0-9_]{40,}' -or
        (-not $Path.StartsWith('third_party/') -and $Text -match '/(?:Users|home)/[A-Za-z][^/\s"'']*/')) {
        throw "Personal path or credential material in public text: $Path"
    }
}
function Get-MgstcReleaseSourceState([string]$ProjectRoot) {
    $root = [IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\', '/')
    $head = [string](Invoke-MgstcReleaseGit $root @('rev-parse', 'HEAD'))
    $paths = @(Invoke-MgstcReleaseGit $root @('ls-files', '--cached', '--others', '--exclude-standard')) | Sort-Object -Unique
    $files = foreach ($path in $paths) {
        $path = [string]$path
        Assert-MgstcReleasePath $path
        $full = Join-Path $root $path
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { throw "Source file missing: $path" }
        $item = Get-Item -LiteralPath $full -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point in source: $path" }
        if ($item.Extension -notin @('.png', '.ico', '.wav', '.mgstc', '.scc', '.bin', '.mid')) {
            Assert-MgstcReleaseText $path ([IO.File]::ReadAllText($full))
        }
        [pscustomobject]@{ path = $path; sha256 = Get-MgstcReleaseSourceHash $path ([IO.File]::ReadAllBytes($full)) }
    }
    $dirty = @(Invoke-MgstcReleaseGit $root @('status', '--porcelain', '--untracked-files=normal'))
    [pscustomobject]@{ head = $head.Trim(); digest = Get-MgstcReleaseFilesDigest $files; files = @($files); dirty = $dirty }
}
function Resolve-MgstcReleaseFile([string]$ProjectRoot, [string]$RelativePath) {
    $root = [IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\', '/')
    $full = [IO.Path]::GetFullPath((Join-Path $root $RelativePath))
    if (-not $full.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw "Link escapes the source tree: $RelativePath" }
    $relative = $full.Substring($root.Length + 1).Replace('\', '/')
    $current = $root
    foreach ($part in $relative.Split('/')) {
        $match = @(Get-ChildItem -LiteralPath $current -Force | Where-Object { $_.Name -ceq $part })
        if ($match.Count -ne 1) { throw "Missing file or case mismatch: $RelativePath" }
        if ($match[0].Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point in document path: $RelativePath" }
        $current = $match[0].FullName
    }
    if (-not (Test-Path -LiteralPath $current -PathType Leaf)) { throw "Document target is not a file: $RelativePath" }
    $relative
}
function Get-MgstcMarkdownAnchors([string]$Text) {
    $seen = @{}
    foreach ($heading in [regex]::Matches($Text, '(?m)^#{1,6}\s+(.+?)\s*#*\r?$')) {
        $slug = ($heading.Groups[1].Value.ToLowerInvariant() -replace '[^\p{L}\p{N}\s_\-]', '' -replace '\s', '-')
        $base = $slug
        if ($seen.ContainsKey($base)) { $seen[$base]++; $slug += '-' + $seen[$base] } else { $seen[$base] = 0 }
        $slug
    }
}
function Test-MgstcReleaseDocuments {
    param([string]$ProjectRoot, [string]$Version, [string]$PreviousCommit = '', [switch]$RequireTracked, [switch]$RequirePublic)
    $root = [IO.Path]::GetFullPath($ProjectRoot)
    $definition=Get-Content -LiteralPath (Join-Path $root 'installer/distribution.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Test-MgstcManualInstallGuides $root $definition -SourcePaths
    $tracked = @(Invoke-MgstcReleaseGit $root @('ls-files'))
    $queue = New-Object 'System.Collections.Generic.Queue[string]'
    foreach ($entry in @('README.md','BUILDING.md','CHANGELOG.md','THIRD_PARTY.md','LICENSE','docs/USER_GUIDE.md','docs/screenshots/README.md')) { $queue.Enqueue($entry) }
    foreach ($entry in $definition.manualInstallFiles.PSObject.Properties) { $queue.Enqueue($entry.Value) }
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    $files = New-Object 'System.Collections.Generic.List[object]'
    $external = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    $links = 0
    while ($queue.Count -gt 0) {
        $relative = Resolve-MgstcReleaseFile $root $queue.Dequeue()
        if (-not $seen.Add($relative)) { continue }
        if ($RequireTracked -and $tracked -cnotcontains $relative) { throw "Untracked public document or image: $relative" }
        $full = Join-Path $root $relative
        $files.Add([pscustomobject]@{ path = $relative; sha256 = Get-MgstcReleaseSourceHash $relative ([IO.File]::ReadAllBytes($full)) })
        if ($relative -notmatch '\.(?:md|txt)$' -and $relative -ne 'LICENSE') { continue }
        $body = [IO.File]::ReadAllText($full)
        Assert-MgstcReleaseText $relative $body
        foreach ($match in [regex]::Matches($body, '!?\[[^\]\r\n]+\]\(([^)\r\n]+)\)')) {
            $target = $match.Groups[1].Value.Trim('<', '>')
            if ($target -match '^https?://') { [void]$external.Add($target); continue }
            if ($target -match '^[a-zA-Z][a-zA-Z0-9+.-]*:') { throw "Unsupported public link: $relative" }
            $parts = $target.Split('#', 2)
            $parent = Split-Path -Parent $relative
            if (-not $parent) { $parent='.' }
            $dest = if ($parts[0]) { Resolve-MgstcReleaseFile $root (Join-Path $parent ([uri]::UnescapeDataString($parts[0]))) } else { $relative }
            if ($RequireTracked -and $tracked -cnotcontains $dest) { throw "Untracked linked file: $dest" }
            $links++
            if ($parts.Count -eq 2 -and $parts[1]) {
                $anchors = @(Get-MgstcMarkdownAnchors ([IO.File]::ReadAllText((Join-Path $root $dest))))
                if ($anchors -cnotcontains [uri]::UnescapeDataString($parts[1])) { throw "Missing heading anchor: $relative -> $target" }
            }
            if ($dest -match '^(?:docs/|README\.md$|BUILDING\.md$|CHANGELOG\.md$|THIRD_PARTY\.md$|LICENSE$)') { $queue.Enqueue($dest) }
        }
    }
    $readme = [IO.File]::ReadAllText((Join-Path $root 'README.md'))
    $changelog = [IO.File]::ReadAllText((Join-Path $root 'CHANGELOG.md'))
    if ($changelog -notmatch ('(?m)^## \[' + [regex]::Escape($Version) + '\]')) { throw 'CHANGELOG version is missing.' }
    $warnings = @(Test-MgstcReadmeReleaseGuide -Text $readme -Version $Version)
    if ($RequirePublic -and $warnings.Count) { throw ($warnings -join ' ') }
    [pscustomobject]@{ digest = Get-MgstcReleaseFilesDigest $files; files = $files.ToArray(); linkCount = $links; externalLinks = @($external | ForEach-Object {$_}); warnings = $warnings }
}
function Test-MgstcReleaseReview($Review, [string]$SourceDigest, [string]$DocumentsDigest, [string]$PreviousCommit) {
    if ($null -eq $Review -or $Review.accepted -ne $true -or $Review.externalLinksAccepted -ne $true -or
        $Review.screenshotReview -ne 'accepted' -or [string]::IsNullOrWhiteSpace($Review.reviewer) -or
        $Review.sourceDigest -cne $SourceDigest -or $Review.documentsDigest -cne $DocumentsDigest -or $Review.previousCommit -cne $PreviousCommit) {
        throw 'A current human document, screenshot and external-link review is required.'
    }
    $when = [datetimeoffset]::Parse($Review.reviewedAt)
    if ($when -gt [datetimeoffset]::UtcNow.AddMinutes(5)) { throw 'Review timestamp is in the future.' }
    $categories = @('feature','operation','ui','io','compatibility','distribution','fix')
    foreach ($category in $categories) { if (@($Review.changeCategories) -notcontains $category) { throw "Review must classify $category changes (including none)." } }
}
function Test-MgstcReleaseTree {
    param([string]$ProjectRoot, [string]$Commit)
    if ($Commit -notmatch '^[0-9a-f]{40}$') { throw 'A full commit SHA is required.' }
    $temp = Join-Path ([IO.Path]::GetTempPath()) ('mgstc-release-tree-' + [guid]::NewGuid().ToString('N'))
    [void](New-Item -ItemType Directory -Path $temp)
    try {
        $archive = Join-Path $temp 'source.zip'
        [void](Invoke-MgstcReleaseGit $ProjectRoot @('archive','--format=zip',"--output=$archive",$Commit))
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [IO.Compression.ZipFile]::OpenRead($archive)
        try {
            $files = foreach ($entry in $zip.Entries) {
                if ($entry.FullName.EndsWith('/')) { continue }
                Assert-MgstcReleasePath $entry.FullName
                $stream = $entry.Open(); $memory = New-Object IO.MemoryStream
                try { $stream.CopyTo($memory); $bytes = $memory.ToArray() } finally { $stream.Dispose(); $memory.Dispose() }
                if ($entry.FullName -notmatch '\.(?:png|ico|wav|mgstc|scc|bin|mid)$') { Assert-MgstcReleaseText $entry.FullName ([Text.Encoding]::UTF8.GetString($bytes)) }
                $hash=Get-MgstcReleaseSourceHash $entry.FullName $bytes
                [pscustomobject]@{path=$entry.FullName;sha256=$hash}
            }
        } finally { $zip.Dispose() }
        [pscustomobject]@{ commit=$Commit; digest=Get-MgstcReleaseFilesDigest $files; files=@($files) }
    } finally {
        $resolved = [IO.Path]::GetFullPath($temp)
        if ((Split-Path -Parent $resolved) -ne ([IO.Path]::GetTempPath()).TrimEnd('\','/') -or (Split-Path -Leaf $resolved) -notlike 'mgstc-release-tree-*') { throw 'Unsafe temporary cleanup path.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
function Test-MgstcReleaseArchive($Manifest,[string]$CandidateDirectory,[string]$ProjectRoot) {
    . (Join-Path $ProjectRoot 'tools/distribution-common.ps1')
    $definition=Get-Content -LiteralPath (Join-Path $ProjectRoot 'installer/distribution.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ((Get-MgstcReleaseFilesDigest $Manifest.payloadFiles) -cne $Manifest.payloadDigest) { throw 'Candidate payload inventory digest mismatch.' }
    $expected=New-Object 'System.Collections.Generic.Dictionary[string,object]' ([StringComparer]::Ordinal)
    foreach($file in $Manifest.payloadFiles) {
        Assert-MgstcRelativePath $file.path
        if ($file.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or $file.bytes -le 0) { throw 'Invalid payload inventory entry.' }
        $expected.Add([string]$file.path,$file)
    }
    $required=@('mgstc.exe')+@($definition.manualInstallFiles.PSObject.Properties.Name)+@($definition.documents)+@($definition.documentation)+@($definition.licenses.PSObject.Properties.Name | ForEach-Object {'licenses/'+$_})+@($definition.dbFiles.PSObject.Properties.Name | ForEach-Object {'OPLL_PatchSlide_DB/'+$_})+@($definition.productBundleFiles | ForEach-Object {'MGS Tone Craft.vst3/'+$_})
    foreach($name in $required) {if(-not $expected.ContainsKey($name)){throw "Missing required ZIP payload: $name"}}
    foreach($name in $expected.Keys) {
        if ($required -ccontains $name) {continue}
        if ($name -notmatch '^docs/.+\.(?:md|txt|png|jpg|jpeg|svg|json)$' -or -not (Test-Path -LiteralPath (Join-Path $ProjectRoot $name) -PathType Leaf)) {throw "Unknown file in candidate ZIP: $name"}
        Assert-MgstcReleasePath $name
    }
    foreach($db in $definition.dbFiles.PSObject.Properties) {
        if ($expected['OPLL_PatchSlide_DB/'+$db.Name].sha256 -ine $db.Value -or $Manifest.dbFiles.($db.Name) -ine $db.Value) {throw 'Candidate uses an unapproved DB set.'}
    }
    $temp=Join-Path ([IO.Path]::GetTempPath()) ('mgstc-release-verify-'+[guid]::NewGuid().ToString('N'))
    [void](New-Item -ItemType Directory -Path $temp)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip=$null
    try {
        Assert-MgstcSetup (Join-Path $CandidateDirectory "MGSToneCraft-$($Manifest.version)-win64-setup.exe") "$($Manifest.version).0"
        $zip=[IO.Compression.ZipFile]::OpenRead((Join-Path $CandidateDirectory "MGSToneCraft-$($Manifest.version)-win64.zip"))
        $seen=New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        foreach($entry in $zip.Entries) {
            if($entry.FullName.EndsWith('/')){continue}
            $name=$entry.FullName
            Assert-MgstcRelativePath $name
            if(-not $expected.ContainsKey($name) -or -not $seen.Add($name)){throw 'Extra, duplicate or incorrectly cased ZIP entry.'}
            $stream=$entry.Open();$sha=[Security.Cryptography.SHA256]::Create()
            try {$hash=([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose();$stream.Dispose()}
            if($hash -ine $expected[$name].sha256 -or $entry.Length -ne $expected[$name].bytes){throw "ZIP payload bytes differ: $name"}
            $source=$null
            if(@($definition.manualInstallFiles.PSObject.Properties.Name) -ccontains $name){$source=Join-Path $ProjectRoot $definition.manualInstallFiles.$name}
            elseif($name.StartsWith('licenses/')) {
                $licenseName=$name.Substring(9)
                $relative=$definition.licenses.($licenseName)
                if(-not $relative.StartsWith('_deps/')){$source=Join-Path $ProjectRoot $relative}
            }
            elseif($name -notmatch '^(?:mgstc\.exe$|MGS Tone Craft\.vst3/|OPLL_PatchSlide_DB/)'){$source=Join-Path $ProjectRoot $name}
            if($source) {
                $reader=$entry.Open();$memory=New-Object IO.MemoryStream
                try{$reader.CopyTo($memory);$bytes=$memory.ToArray()}finally{$reader.Dispose();$memory.Dispose()}
                if(@($definition.manualInstallFiles.PSObject.Properties.Name) -ccontains $name){Assert-MgstcManualInstallText $bytes $name $definition}
                if((Get-MgstcReleaseSourceHash $name $bytes) -cne (Get-MgstcReleaseSourceHash $name ([IO.File]::ReadAllBytes($source)))){throw "ZIP documentation/license differs from candidate source: $name"}
                if($name -match '\.md$' -and -not $name.StartsWith('licenses/')) {
                    foreach($link in [regex]::Matches([Text.Encoding]::UTF8.GetString($bytes),'!?\[[^\]\r\n]+\]\(([^)\r\n]+)\)')) {
                        $target=$link.Groups[1].Value.Trim('<','>')
                        if($target -match '^https?://' -or $target.StartsWith('#')){continue}
                        $target=[uri]::UnescapeDataString($target.Split('#')[0])
                        $parent=Split-Path -Parent (Join-Path $temp $name)
                        $resolved=[IO.Path]::GetFullPath((Join-Path $parent $target))
                        if(-not $resolved.StartsWith($temp+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or -not $expected.ContainsKey($resolved.Substring($temp.Length+1).Replace('\','/'))){throw "Broken link inside manual ZIP: $name"}
                    }
                }
            }
            if($name -in @('mgstc.exe','MGS Tone Craft.vst3/Contents/x86_64-win/MGS Tone Craft.vst3','MGS Tone Craft.vst3/Contents/Resources/moduleinfo.json')) {
                $file=Join-Path $temp ([IO.Path]::GetFileName($name))
                $inputStream=$entry.Open();$outputStream=[IO.File]::Create($file)
                try{$inputStream.CopyTo($outputStream)}finally{$inputStream.Dispose();$outputStream.Dispose()}
                if($name.EndsWith('moduleinfo.json')){Assert-MgstcModule $file "$($Manifest.version).0"}else{Assert-MgstcProduct $file "$($Manifest.version).0"}
            }
        }
        if($seen.Count -ne $expected.Count){throw 'ZIP payload entries are missing.'}
    } finally {
        if($zip){$zip.Dispose()}
        $resolved=[IO.Path]::GetFullPath($temp)
        if((Split-Path -Parent $resolved) -ne ([IO.Path]::GetTempPath()).TrimEnd('\','/') -or (Split-Path -Leaf $resolved) -notlike 'mgstc-release-verify-*'){throw 'Unsafe release verification cleanup path.'}
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
function Test-MgstcReleaseCandidate {
    param([string]$CandidateDirectory, [switch]$RequirePublic)
    $dir = [IO.Path]::GetFullPath($CandidateDirectory)
    $m = Get-Content -LiteralPath (Join-Path $dir 'candidate.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($m.schemaVersion -ne 1 -or $m.state -ne 'Ready' -or $m.version -notmatch '^\d+\.\d+$' -or
        $m.tag -cne ('v' + $m.version) -or $m.commit -notmatch '^[0-9a-f]{40}$' -or $m.previousCommit -notmatch '^[0-9a-f]{40}$') { throw 'Invalid release candidate identity/state.' }
    $expected = @("MGSToneCraft-$($m.version)-win64-setup.exe", "MGSToneCraft-$($m.version)-win64.zip", 'SHA256SUMS.txt')
    if (@($m.assets).Count -ne 3 -or @($m.assets.name | Sort-Object -Unique).Count -ne 3) { throw 'Exactly three distinct release assets are required.' }
    foreach ($a in $m.assets) {
        if ($expected -cnotcontains $a.name -or $a.sha256 -notmatch '^[a-f0-9]{64}$') { throw 'Unexpected asset identity.' }
        $path = Join-Path $dir $a.name
        if ((Get-Item -LiteralPath $path).Length -ne $a.size -or (Get-MgstcReleaseSha256 $path) -cne $a.sha256) { throw "Asset hash/size mismatch: $($a.name)" }
    }
    if ((Get-MgstcReleaseAssetDigest $m.assets) -cne $m.assetsDigest) { throw 'Asset inventory digest mismatch.' }
    $sum = [IO.File]::ReadAllText((Join-Path $dir 'SHA256SUMS.txt'))
    $lines = @($sum -split '\r?\n' | Where-Object { $_ })
    if ($lines.Count -ne 2) { throw 'SHA256SUMS must describe exactly the two binary assets.' }
    foreach ($a in @($m.assets | Where-Object name -ne 'SHA256SUMS.txt')) {
        if ($lines -cnotcontains ($a.sha256 + '  ' + $a.name)) { throw 'SHA256SUMS content mismatch.' }
    }
    $notes = Join-Path $dir 'RELEASE_NOTES.md'
    if ((Get-MgstcReleaseSha256 $notes) -cne $m.notesSha256) { throw 'Release notes hash mismatch.' }
    Assert-MgstcReleaseNotes ([IO.File]::ReadAllText($notes)) $m.version
    if($m.PSObject.Properties['payloadFiles']) {
        Test-MgstcReleaseArchive $m $dir (Split-Path -Parent $PSScriptRoot)
    } elseif($RequirePublic) {throw 'Public candidates require the verified complete payload inventory.'}
    if ($RequirePublic) {
        if ($m.prototype -ne $false) { throw 'Prototype packages cannot be staged or published.' }
        $root = Split-Path -Parent $PSScriptRoot
        $state = Get-MgstcReleaseSourceState $root
        if ($state.head -cne $m.commit -or $state.digest -cne $m.sourceDigest -or $state.dirty.Count -gt 0) { throw 'Candidate source does not match a clean committed checkout.' }
        if (([IO.File]::ReadAllText((Join-Path $root 'VERSION'))).Trim() -cne $m.version) { throw 'Candidate VERSION mismatch.' }
        $tree = Test-MgstcReleaseTree $root $m.commit
        if ($tree.digest -cne $state.digest) { throw 'Tag source archive differs from the reviewed source.' }
        $docs = Test-MgstcReleaseDocuments -ProjectRoot $root -Version $m.version -PreviousCommit $m.previousCommit -RequireTracked -RequirePublic
        if ($docs.digest -cne $m.documentsDigest) { throw 'Candidate documentation digest mismatch.' }
        Test-MgstcReleaseReview $m.review $state.digest $docs.digest $m.previousCommit
        Test-MgstcReleaseContentCandidate $root $m ([IO.File]::ReadAllText($notes))
    }
    $m
}
