# SPDX-License-Identifier: AGPL-3.0-only
# Read-only Markdown checks. Factual accuracy and image review remain human work.
function Test-MgstcReadmeReleaseGuide([string]$Text, [string]$Version) {
    $issues = New-Object 'System.Collections.Generic.List[string]'
    $lines = @($Text.Replace("`r`n", "`n").Replace("`r", "`n").Split("`n"))
    $visible = New-Object 'System.Collections.Generic.List[string]'
    $markdown = New-Object 'System.Collections.Generic.List[string]'
    $contexts = New-Object 'System.Collections.Generic.List[string]'
    $historical = New-Object 'System.Collections.Generic.List[bool]'
    $headings = New-Object 'System.Collections.Generic.List[object]'
    $markers = @{}
    foreach ($key in @('download:start','download:end','whats-new:start','whats-new:end')) { $markers[$key] = New-Object 'System.Collections.Generic.List[int]' }
    $stack = New-Object 'System.Collections.Generic.List[object]'
    $heading = '(overview)'; $history = $false; $fence = ''; $comment = $false
    $versionPattern = '(?<![\d.])v?(?<version>\d+\.\d+(?:\.\d+)?)(?![\d.])'
    $currentWords = '(?:最新|現行|現在の|current\s+(?:published\s+)?(?:release|version)|latest\s+(?:published\s+)?(?:release|version))'
    function Add-MgstcReadmeIssue([int]$Index, [string]$Reason) {
        $line = [Math]::Min([Math]::Max($Index, 0), $lines.Count - 1)
        $snippet = $lines[$line].Trim()
        $issues.Add(('README.md:{0} [{1}] {2}; expected v{3}; text: {4}' -f ($line + 1), $contexts[$line], $Reason, $Version, $snippet))
    }
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i].Normalize([Text.NormalizationForm]::FormKC).Replace([string][char]0x2019, "'")
        $trimmed = $line.Trim()
        if ($fence) {
            if ($trimmed -match ('^' + [regex]::Escape($fence[0]) + '{' + $fence.Length + ',}\s*$')) { $fence = '' }
            $visible.Add(''); $markdown.Add(''); $contexts.Add($heading); $historical.Add($history); continue
        }
        if (-not $comment -and $trimmed -match '^(?<fence>`{3,}|~{3,})') {
            $fence = $Matches.fence; $visible.Add(''); $markdown.Add(''); $contexts.Add($heading); $historical.Add($history); continue
        }
        if (-not $comment -and $trimmed -match '^<!-- mgstc:(download|whats-new):(start|end) -->$') {
            $markers[$Matches[1]+':'+$Matches[2]].Add($i)
        }
        # HTML comments do not display as release claims. Marker lines are still
        # counted above; marker-like code examples do not satisfy the structure.
        $rendered = ''
        $remaining = $line
        while ($remaining.Length -gt 0) {
            if ($comment) {
                $end = $remaining.IndexOf('-->')
                if ($end -lt 0) { $remaining = ''; break }
                $remaining = $remaining.Substring($end + 3); $comment = $false
            } else {
                $start = $remaining.IndexOf('<!--')
                if ($start -lt 0) { $rendered += $remaining; $remaining = ''; break }
                $rendered += $remaining.Substring(0, $start)
                $remaining = $remaining.Substring($start + 4); $comment = $true
            }
        }
        # Keep link labels; URLs can contain historical versions without making
        # the surrounding product description a current-version assertion.
        $plain = [regex]::Replace($rendered, '!?\[([^\]]+)\]\([^)]+\)', '$1')
        $plain = [regex]::Replace($plain, 'https?://\S+', '')
        $plain = $plain.Replace('`','').Replace('**','').Replace('__','')
        if ($plain -match '^\s{0,3}(?<level>#{1,6})\s+(?<title>.+?)\s*#*\s*$') {
            $level = $Matches.level.Length; $title = $Matches.title
            while ($stack.Count -gt 0 -and $stack[$stack.Count - 1].level -ge $level) { $stack.RemoveAt($stack.Count - 1) }
            $history = $stack.Count -gt 0 -and $stack[$stack.Count - 1].history
            if ($title -match '^(?:過去のリリース(?:履歴)?|リリース履歴|変更履歴|参考リンク|Release history|Past releases|Changelog(?: references?| quotes?)?|References)\s*$') { $history = $true }
            if ($title -match ($currentWords + "|What's\s+New|ダウンロード|インストール|Download|Installation")) { $history = $false }
            $heading = $title
            $stack.Add([pscustomobject]@{ level=$level; history=$history })
            $headings.Add([pscustomobject]@{ line=$i; level=$level; title=$title; history=$history })
        }
        $visible.Add($plain); $markdown.Add($rendered); $contexts.Add($heading); $historical.Add($history)
    }
    if ($fence -or $comment) { Add-MgstcReadmeIssue ($lines.Count - 1) 'Unterminated Markdown fence or HTML comment prevents reliable validation' }
    $bounds = @{}
    foreach ($name in @('download','whats-new')) {
        $start = $markers[$name+':start']; $end = $markers[$name+':end']
        if ($start.Count -ne 1 -or $end.Count -ne 1) { Add-MgstcReadmeIssue 0 "Require exactly one paired $name marker region"; continue }
        if ($start[0] -ge $end[0]) { Add-MgstcReadmeIssue $start[0] "Invalid $name marker order"; continue }
        $bounds[$name] = [pscustomobject]@{ start=$start[0]; end=$end[0] }
    }
    if ($bounds.ContainsKey('download') -and $bounds.ContainsKey('whats-new')) {
        if ($bounds.download.end -ge $bounds.'whats-new'.start) { Add-MgstcReadmeIssue $bounds.download.start 'Download and important-changes regions overlap or are out of order' }
    }
    $newHeadings = @($headings | Where-Object { $_.title -match "(?i)What'?s\s+New|新着情報|最新の変更" })
    if ($newHeadings.Count -eq 0) { Add-MgstcReadmeIssue 0 "Require one unambiguous What's New heading, found none" }
    if ($newHeadings.Count -gt 1) {
        foreach ($duplicate in $newHeadings) { Add-MgstcReadmeIssue $duplicate.line "Require one unambiguous What's New heading, found $($newHeadings.Count)" }
    }
    foreach ($name in @('download','whats-new')) {
        if (-not $bounds.ContainsKey($name)) { continue }
        $region = $bounds[$name]
        $inside = @($headings | Where-Object { $_.line -gt $region.start -and $_.line -lt $region.end -and $_.level -le 2 })
        $pattern = if ($name -eq 'download') { 'ダウンロード|導入|Download|Install' } else { "What'?s\s+New|新着情報|最新の変更" }
        if ($inside.Count -ne 1 -or $inside[0].level -ne 2 -or $inside[0].title -notmatch $pattern -or $inside[0].history) {
            Add-MgstcReadmeIssue $region.start "The $name region must contain its single current H2 heading"; continue
        }
        $target = $inside[0]
        $preceding = @($visible[($region.start+1)..($target.line)] | Where-Object { $_.Trim() })
        if ($preceding.Count -ne 1) { Add-MgstcReadmeIssue $region.start "The $name region must start with its heading" }
        if ($name -eq 'download') {
            $block = $markdown[($target.line+1)..($region.end-1)] -join "`n"
            $navigable = [regex]::Replace($block, '(?s)(`+).*?\1', '')
            if ($navigable -notmatch '(?<!!)(?<!\\)\[[^\]]+\]\(https://github\.com/takawon/mgs-tone-craft/releases/?\)') { Add-MgstcReadmeIssue $target.line 'Download guidance needs the version-independent GitHub Releases-list Markdown link' }
            foreach ($requirement in @(
                @{ pattern='(?i)インストーラー|installer|setup'; description='installer option' },
                @{ pattern='(?i)\bEXE\b|\.exe'; description='EXE distribution' },
                @{ pattern='(?i)手動(?:導入)?\s*ZIP|manual\s+(?:install(?:ation)?\s+)?ZIP'; description='manual installation ZIP' },
                @{ pattern='(?i)ソース|source'; description='source archive distinction' },
                @{ pattern='(?i)tar\.gz'; description='source tar.gz archive' },
                @{ pattern='(?i)\bINSTALL\.txt\b'; description='Japanese INSTALL.txt guide' },
                @{ pattern='(?i)\bINSTALL\.en\.txt\b'; description='English INSTALL.en.txt guide' }
            )) { if ($block -notmatch $requirement.pattern) { Add-MgstcReadmeIssue $target.line ('Download guidance is missing '+$requirement.description) } }
        } else {
            $versions = @([regex]::Matches($target.title, $versionPattern, 'IgnoreCase') | ForEach-Object { $_.Groups['version'].Value })
            if ($versions.Count -ne 1 -or $versions[0] -cne $Version) { Add-MgstcReadmeIssue $target.line "What's New heading must identify only the candidate version" }
            $body = @($visible[($target.line+1)..($region.end-1)] | Where-Object { $_.Trim() -and $_ -notmatch '^\s*#' }) -join ''
            if (($body -replace '\s','').Length -lt 20) { Add-MgstcReadmeIssue $target.line "What's New requires substantive change descriptions, followed by factual human review" }
        }
    }
    $tableCurrentColumns = @(); $tableHeading = ''
    for ($i = 0; $i -lt $visible.Count; $i++) {
        if (-not $visible[$i].Trim()) { $tableCurrentColumns = @(); continue }
        $line = $visible[$i]
        # A clearly quoted record in a history section is historical. Ordinary
        # "Current release" prose does not become safe merely under that heading.
        if ($historical[$i] -and $line -match '^\s*>') { continue }
        $lineVersions = @([regex]::Matches($line, $versionPattern, 'IgnoreCase') | ForEach-Object { $_.Groups['version'].Value })
        $tableClaim = $false
        if ($line.Trim().StartsWith('|') -and $line.Trim().EndsWith('|')) {
            $cells = @($line.Trim().Trim('|').Split('|') | ForEach-Object { $_.Trim() })
            $newColumns = @(for ($column = 0; $column -lt $cells.Count; $column++) { if ($cells[$column] -match $currentWords) { $column } })
            if ($newColumns.Count -gt 0) {
                $tableCurrentColumns = @($newColumns) + @(for ($column = 0; $column -lt $cells.Count; $column++) { if ($cells[$column] -match '^(?:Version|Published version|バージョン|版)$') { $column } })
                $tableHeading = $contexts[$i]
            }
            elseif ($tableCurrentColumns.Count -gt 0 -and $tableHeading -ceq $contexts[$i]) {
                $tableClaim = $true
                foreach ($column in $tableCurrentColumns) {
                    if ($column -ge $cells.Count) { continue }
                    foreach ($match in [regex]::Matches($cells[$column], $versionPattern, 'IgnoreCase')) {
                        if ($match.Groups['version'].Value -cne $Version) { Add-MgstcReadmeIssue $i 'Current-release table column identifies another version' }
                    }
                }
            }
        } else { $tableCurrentColumns = @() }
        $continuedClaim = $false
        if ($i -gt 0 -and $contexts[$i-1] -ceq $contexts[$i] -and $visible[$i-1] -match $currentWords -and
            -not [regex]::IsMatch($visible[$i-1], $versionPattern, 'IgnoreCase') -and $lineVersions.Count -gt 0) {
            $continuedClaim = $true
            foreach ($declared in $lineVersions) { if ($declared -cne $Version) { Add-MgstcReadmeIssue $i 'Continued current-release assertion identifies another version' } }
        }
        $hasCurrentClaim = $line -match $currentWords
        if ($historical[$i] -and -not $hasCurrentClaim -and -not $tableClaim -and -not $continuedClaim) { continue }
        $comparison = $line -match '(?i)compared\s+(?:with|to)|comparison\s+(?:with|to)|比較元|比較対象|前回(?:公開)?版(?:との比較|との差分)'
        # Current assertions are checked throughout visible current prose,
        # including text preceding or following the explicitly marked regions.
        $claimWords = $currentWords
        if (-not $comparison -and -not $historical[$i]) { $claimWords += '|\bversion\b|\bver\.|バージョン|開発版' }
        $claimPattern = '(?i)(?:' + $claimWords + ')[^\r\n\d]{0,48}?' + $versionPattern
        foreach ($match in [regex]::Matches($line, $claimPattern)) {
            if ($match.Groups['version'].Value -cne $Version) { Add-MgstcReadmeIssue $i 'Current-version assertion differs from the candidate version' }
        }
        $reverseClaim = '(?i)' + $versionPattern + '[\s|:()—-]*(?:は|が|is\s+)?' + $currentWords
        foreach ($match in [regex]::Matches($line, $reverseClaim)) {
            if ($match.Groups['version'].Value -cne $Version) { Add-MgstcReadmeIssue $i 'Current-release label follows a different version' }
        }
        if ($line -match '^\s{0,3}#{1,6}\s+' -and $line -match ($currentWords + "|What's\s+New|ダウンロード|インストール|導入|Download|Installation")) {
            foreach ($declared in $lineVersions) { if ($declared -cne $Version) { Add-MgstcReadmeIssue $i 'Current guidance heading identifies another version' } }
        }
        if ($line -match $currentWords -and $lineVersions.Count -eq 1 -and $lineVersions[0] -cne $Version) {
            Add-MgstcReadmeIssue $i 'Current-release row or heading identifies another version'
        }
        $isolated = $line.Trim() -replace '^#{1,6}\s*',''
        if ($isolated -match ('(?i)^(?:(?:MGS\s+Tone\s+Craft|MGSTC)\s*[-:()—]*\s*)?' + $versionPattern + '\s*\)?\s*$')) {
            if ($Matches.version -cne $Version) { Add-MgstcReadmeIssue $i 'Standalone product/version label differs from the candidate version' }
        }
        if ($isolated -match ('(?i)^(?:MGS\s+Tone\s+Craft|MGSTC)[\s:()—-]*' + $versionPattern)) {
            if ($Matches.version -cne $Version) { Add-MgstcReadmeIssue $i 'Product heading or opening label identifies another version' }
        }
        if ($line -match '^\s*#\s+' -and $line -match '(?i)MGS\s+Tone\s+Craft|\bMGSTC\b') {
            foreach ($declared in $lineVersions) { if ($declared -cne $Version) { Add-MgstcReadmeIssue $i 'Product overview heading identifies another version' } }
        }
        if ($line -match '(?i)未公開|近日公開予定|公開予定|次回公開候補|次回配布候補|not\s+(?:yet\s+)?released(?:\s+yet)?|unreleased|coming\s+soon|planned\s+release') { Add-MgstcReadmeIssue $i 'Current guidance contains an unreleased/future-publication assertion' }
        $oldTargetPattern = '(?i)' + $versionPattern + '\s*(?:で(?:追加|変更|導入)|の(?:新機能|変更|更新)|(?:release\s+)?(?:changes|highlights))'
        foreach ($match in [regex]::Matches($(if ($comparison) { '' } else { $line }), $oldTargetPattern)) {
            if ($match.Groups['version'].Value -cne $Version) { Add-MgstcReadmeIssue $i 'Change description explicitly targets another version; move historical material to history' }
        }
        foreach ($match in [regex]::Matches($(if ($comparison) { '' } else { $line }), '(?i)(?:release\s+notes|highlights|changes)\s+(?:for|in)\s+' + $versionPattern)) {
            if ($match.Groups['version'].Value -cne $Version) { Add-MgstcReadmeIssue $i 'Change-summary target differs from the candidate version' }
        }
        foreach ($match in [regex]::Matches($markdown[$i], '(?i)https://github\.com/takawon/mgs-tone-craft/releases/download/v(?<version>\d+\.\d+(?:\.\d+)?)/')) {
            if ($match.Groups['version'].Value -cne $Version) { Add-MgstcReadmeIssue $i 'Current download link targets another release version; retain old links only in history' }
        }
        if ($line -match '(?i)%APPDATA%[\\/]MgsToneCraft|%LOCALAPPDATA%[\\/](?!MgsToneCraft(?:[\\/]|\s|$))[^\s]+') { Add-MgstcReadmeIssue $i 'User-library location must use %LOCALAPPDATA%\MgsToneCraft\' }
        if ($line -match '(?i)%ProgramData%[\\/](?!OPLL_PatchSlide_DB(?:[\\/]|\s|$))[^\s]+' -and $line -match '(?i)PatchSlide') { Add-MgstcReadmeIssue $i 'Shared PatchSlide DB location must use %ProgramData%\OPLL_PatchSlide_DB\' }
    }
    $issues.ToArray()
}
