# SPDX-License-Identifier: AGPL-3.0-only
# Explicit local draft generation. Never edits source documents or Git refs.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$OutputFile,[string]$Version='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'release-content.ps1')
$root=Split-Path -Parent $PSScriptRoot
$actual=([IO.File]::ReadAllText((Join-Path $root 'VERSION'))).Trim()
if(-not $Version){$Version=$actual}
if($Version -cne $actual){throw 'Notes version must match VERSION.'}
$candidate=Get-MgstcChangelogCandidate ([IO.File]::ReadAllText((Join-Path $root 'CHANGELOG.md'))) $Version
$notes=New-MgstcReleaseContentNotes $candidate
Assert-MgstcReleaseContentNotes $notes $candidate
$path=if([IO.Path]::IsPathRooted($OutputFile)){[IO.Path]::GetFullPath($OutputFile)}else{[IO.Path]::GetFullPath((Join-Path $root $OutputFile))}
if(Test-Path -LiteralPath $path){throw 'Draft output already exists. Select a new file; source documents are never overwritten.'}
[void](New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force)
$stream=[IO.File]::Open($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
try{$bytes=[Text.UTF8Encoding]::new($false).GetBytes($notes);$stream.Write($bytes,0,$bytes.Length)}finally{$stream.Dispose()}
Write-Output $path
