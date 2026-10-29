# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param(
    [string]$CandidateDirectory='', [string]$Repository='',
    [switch]$Execute, [switch]$DryRun, [switch]$FromGitHubDraft,
    [string]$Tag='', [string]$Commit='', [string]$AssetsDigest='', [string]$NotesSha256='',
    [switch]$GitHubActions, [string]$Environment='mgstc-release'
)
$ErrorActionPreference='Stop'
if (-not $CandidateDirectory -or -not $Repository) { throw 'Usage: publish-release.ps1 -CandidateDirectory <candidate> -Repository <owner/repository> [-Execute]. Default is verification only.' }
if ($Execute -and $DryRun) { throw 'Execute and DryRun cannot be combined.' }
. (Join-Path $PSScriptRoot 'release-common.ps1')
. (Join-Path $PSScriptRoot 'release-github.ps1')
$directory=[IO.Path]::GetFullPath($CandidateDirectory)
if ($FromGitHubDraft) {
    Import-MgstcGitHubCandidate -CandidateDirectory $directory -Repository $Repository -Tag $Tag -Commit $Commit -AssetsDigest $AssetsDigest -NotesSha256 $NotesSha256 | Out-Null
}
Invoke-MgstcReleasePublish -CandidateDirectory $directory -Repository $Repository -Execute:$Execute -GitHubActions:$GitHubActions -Environment $Environment
