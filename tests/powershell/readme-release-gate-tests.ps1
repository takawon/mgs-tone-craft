# SPDX-License-Identifier: AGPL-3.0-only
# Read-only focused tests for the README release-context gate.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$gate = Join-Path $repo 'tools/readme-release-gate.ps1'
if (-not (Test-Path -LiteralPath $gate -PathType Leaf)) { throw 'README gate implementation is missing.' }
. $gate
$script:checks = 0
function Assert-Case([string]$Name, [string]$Text, [bool]$ShouldReject, [string]$ExpectedText = '') {
    $issues = @(Test-MgstcReadmeReleaseGuide -Text $Text -Version '0.365')
    $rejected = $issues.Count -gt 0
    $script:checks++
    if ($rejected -ne $ShouldReject) {
        throw "ASSERTION FAILED: $Name; expected rejected=$ShouldReject, got rejected=$rejected. Issues: $($issues -join ' | ')"
    }
    if ($ShouldReject) {
        $joined = $issues -join "`n"
        foreach ($needle in @('README.md:', '0.365', $ExpectedText)) {
            if ($needle -and $joined -notmatch [regex]::Escape($needle)) {
                throw "ASSERTION FAILED: $Name diagnostic must include '$needle': $joined"
            }
        }
    }
    Write-Host ("PASS {0}: {1}" -f $(if ($ShouldReject) { 'reject' } else { 'accept' }), $Name)
}
function New-Readme([string]$Download = '', [string]$WhatsNew = '', [string]$Intro = '', [string]$History = '') {
    @"
# MGS Tone Craft

$Intro

<!-- mgstc:download:start -->
## ダウンロードと導入

$Download
<!-- mgstc:download:end -->

## 主な機能

MGS Tone Craft combines OPLL, SCC and PSG sound layers for MGSC.

<!-- mgstc:whats-new:start -->
## What's New — v0.365

$WhatsNew
<!-- mgstc:whats-new:end -->

$History
"@
}
$download = @'
配布物と最新版は[GitHub Releases](https://github.com/takawon/mgs-tone-craft/releases)一覧で確認してください。

| 配布物 | 内容 |
|---|---|
| Windows インストーラー EXE | Standalone と任意の VST3 をセットアップ |
| 手動導入 ZIP | Standalone、VST3 bundle、PatchSlide DB。一式を展開して手順に従います |
| ソース ZIP / tar.gz | GitHub のソースアーカイブ。BUILDING.md に従ってビルドします |

ZIPの導入手順: `INSTALL.txt`（日本語）と `INSTALL.en.txt`（English）。
'@
$whatsNew = @'
OPLL、SCC、PSGの音色を共通時間軸で編集する複合音色制作を追加し、試聴とPatchSlide探索を改善しました。
詳細と操作例は公開操作ガイドを確認してください。
'@
$valid = New-Readme $download $whatsNew
Assert-Case 'version-neutral release-list download and meaningful current highlights' $valid $false
Assert-Case 'historical versions and comparison baseline are allowed' (New-Readme $download ($whatsNew + "`n前回公開版v0.297からの変更です。") '' "## 過去のリリース`n`n- v0.297 (2026-10-01): 以前の公開版。`n- Windows ZIP v0.285 は旧配布物です。") $false
Assert-Case 'historical release links and quoted old-version statement remain allowed' (New-Readme $download $whatsNew '' "## Release history`n`n[Release v0.297](https://github.com/takawon/mgs-tone-craft/releases/tag/v0.297)`n`n> Historical release note: Current release was v0.297 at that time.") $false
Assert-Case "comparative old baseline in current What's New remains allowed" (New-Readme $download ($whatsNew + "`nCompared with version v0.297, this version adds editing tools.")) $false

Assert-Case 'stale latest published v0.297 despite current asset filename' (New-Readme ($download + "`n最新公開版 v0.297。MGSToneCraft-0.365-win64-setup.exe を使います。") $whatsNew) $true 'v0.297'
Assert-Case 'English stale Current release despite current ZIP filename' (New-Readme ($download + "`nCurrent release: v0.297; download MGSToneCraft-0.365-win64.zip.") $whatsNew) $true 'v0.297'
Assert-Case 'Current release claim under ordinary Release history heading is still live' (New-Readme $download $whatsNew '' "## Release history`n`nCurrent release: v0.297") $true 'v0.297'
Assert-Case 'Current release column and stale following table row' (New-Readme ($download + "`n| Current release | Version |`n|---|---|`n| Download now | v0.297 |") $whatsNew) $true 'v0.297'
Assert-Case 'latest claim split across lines with Alpha suffix' (New-Readme $download $whatsNew "Latest published release is`nv0.297 Alpha.") $true 'v0.297'
Assert-Case 'product H1 has Alpha suffix after stale version' ($valid.Replace('# MGS Tone Craft', '# MGS Tone Craft Alpha v0.297')) $true 'v0.297'
Assert-Case 'product H1 has stale version before Alpha suffix' ($valid.Replace('# MGS Tone Craft', '# v0.297 Alpha - MGS Tone Craft')) $true 'v0.297'
Assert-Case 'stale latest declaration with full-width spacing' (New-Readme ($download + "`n最新公開版　Ｖ ０．２９７") $whatsNew) $true '0.365'
Assert-Case 'stale root overview declaration outside markers' (New-Readme $download $whatsNew '現行版 v0.297 を紹介します。') $true 'v0.297'
Assert-Case 'stale latest heading outside marked current sections' (New-Readme $download $whatsNew '' "## 最新公開版 v0.297`n`n過去のファイル名を説明します。") $true 'v0.297'
Assert-Case 'candidate called unreleased in current download guide' (New-Readme ($download + "`n開発版0.365は未公開です。") $whatsNew) $true '未公開'
Assert-Case 'candidate called not released yet in English' (New-Readme ($download + "`nVersion 0.365 is not released yet.") $whatsNew) $true 'not released yet'
Assert-Case 'old leading current version' (New-Readme $download $whatsNew 'Version 0.297 is the current version.') $true '0.297'
Assert-Case 'old release-only heading' (New-Readme $download $whatsNew '' "## What's New — v0.297`n`nHistorical feature notes.") $true '0.297'
Assert-Case 'duplicate current latest heading nested in history remains contradictory' (New-Readme $download $whatsNew '' "## 過去のリリース`n`n### 最新公開版 v0.297`n`n過去の変更です。") $true 'v0.297'

Assert-Case 'missing exact Releases list link' (New-Readme ($download.Replace('https://github.com/takawon/mgs-tone-craft/releases','https://github.com/takawon/mgs-tone-craft/releases/latest')) $whatsNew) $true 'releases'
Assert-Case 'fixed version asset URLs alone are not primary download guide' (New-Readme ($download.Replace('[GitHub Releases](https://github.com/takawon/mgs-tone-craft/releases)','[v0.365](https://github.com/takawon/mgs-tone-craft/releases/download/v0.365/MGSToneCraft-0.365-win64.zip)')) $whatsNew) $true 'Releases'
Assert-Case 'Releases-list link in inline code is not an actionable Markdown link' (New-Readme ($download.Replace('[GitHub Releases](https://github.com/takawon/mgs-tone-craft/releases)','`https://github.com/takawon/mgs-tone-craft/releases`')) $whatsNew) $true 'Releases'
Assert-Case 'Releases-list image link does not satisfy a download link' (New-Readme ($download.Replace('[GitHub Releases](https://github.com/takawon/mgs-tone-craft/releases)','![GitHub Releases](https://github.com/takawon/mgs-tone-craft/releases)')) $whatsNew) $true 'Releases'
Assert-Case 'old fixed download URL is rejected despite version-independent Releases list' (New-Readme ($download + "`n[Download installer](https://github.com/takawon/mgs-tone-craft/releases/download/v0.297/MGSToneCraft-0.297-win64-setup.exe)") $whatsNew) $true 'v0.297'
Assert-Case 'missing EXE installer format' (New-Readme ($download.Replace('Windows インストーラー EXE','Windows application')) $whatsNew) $true 'EXE'
Assert-Case 'missing manual ZIP format' (New-Readme ($download.Replace('手動導入 ZIP','展開版')) $whatsNew) $true 'ZIP'
Assert-Case 'missing GitHub source tar.gz archive' (New-Readme ($download.Replace('ZIP / tar.gz','ZIP')) $whatsNew) $true 'tar.gz'
Assert-Case 'missing Japanese manual guide' (New-Readme ($download.Replace('`INSTALL.txt`（日本語）と ','')) $whatsNew) $true 'INSTALL.txt'
Assert-Case 'missing English manual guide' (New-Readme ($download.Replace('`INSTALL.en.txt`（English）','')) $whatsNew) $true 'INSTALL.en.txt'

foreach ($marker in @('<!-- mgstc:download:start -->','<!-- mgstc:download:end -->')) {
    Assert-Case "missing marker $marker" ($valid.Replace($marker,'')) $true 'marker'
    Assert-Case "duplicate marker $marker" ($valid.Replace($marker, "$marker`n$marker")) $true 'marker'
}
Assert-Case 'reversed download marker pair' ($valid.Replace('<!-- mgstc:download:start -->', '<!-- mgstc:download:TEMP -->').Replace('<!-- mgstc:download:end -->','<!-- mgstc:download:start -->').Replace('<!-- mgstc:download:TEMP -->','<!-- mgstc:download:end -->')) $true 'marker'
foreach ($marker in @('<!-- mgstc:whats-new:start -->','<!-- mgstc:whats-new:end -->')) {
    Assert-Case "missing marker $marker" ($valid.Replace($marker,'')) $true 'marker'
    Assert-Case "duplicate marker $marker" ($valid.Replace($marker, "$marker`n$marker")) $true 'marker'
}
Assert-Case "duplicate What's New section" ($valid + "`n## What's New — v0.365`n`nSecond conflicting section.") $true 'What'
Assert-Case "What's New section for old target version" ($valid.Replace("## What's New — v0.365","## What's New — v0.297")) $true '0.297'
Assert-Case 'empty important changes body' (New-Readme $download '') $true '0.365'
Assert-Case 'candidate heading but stale old-version highlights' (New-Readme $download 'Change highlights for v0.297 are stale.') $true 'v0.297'
$userMismatch = $download + [char]10 + 'User library: ' + [char]37 + 'LOCALAPPDATA%\OldProduct\'
Assert-Case 'wrong user library location' (New-Readme $userMismatch $whatsNew) $true 'LOCALAPPDATA'
$dbMismatch = $download + [char]10 + 'PatchSlide shared database: ' + [char]37 + 'ProgramData%\Old_DB\'
Assert-Case 'wrong shared DB destination' (New-Readme $dbMismatch $whatsNew) $true 'ProgramData'

Write-Output "README context-gate checks passed: $script:checks"
