# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param([string]$CandidateDirectory='', [string]$Repository='', [switch]$Execute, [switch]$DryRun)
$ErrorActionPreference='Stop'
if (-not $CandidateDirectory -or -not $Repository) { throw 'Usage: stage-release.ps1 -CandidateDirectory <prepared candidate> -Repository <owner/repository> [-Execute]. Default is verification only.' }
if ($Execute -and $DryRun) { throw 'Execute and DryRun cannot be combined.' }
. (Join-Path $PSScriptRoot 'release-common.ps1')
. (Join-Path $PSScriptRoot 'release-github.ps1')
Invoke-MgstcReleaseStage -CandidateDirectory ([IO.Path]::GetFullPath($CandidateDirectory)) -Repository $Repository -Execute:$Execute
