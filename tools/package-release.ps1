[CmdletBinding(DefaultParameterSetName='Inputs')]
param(
    [Parameter(Mandatory=$true,ParameterSetName='Inputs')][string]$DbSourceDirectory,
    [Parameter(Mandatory=$true,ParameterSetName='Prepared')][string]$PreparedStage,
    [ValidateSet('Release')][string]$Configuration='Release',
    [string]$Version='', [string]$BuildDirectory='build',
    [string]$OutputDirectory='dist', [string]$IsccPath='', [switch]$ValidateOnly
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'distribution-common.ps1')
$root=Split-Path -Parent $PSScriptRoot
$output=Get-MgstcAbsolutePath $OutputDirectory $root
$ownedStage=$false
if($PSCmdlet.ParameterSetName -eq 'Inputs') {
    $material=Get-MgstcDistributionInput -ProjectRoot $root -DbSourceDirectory $DbSourceDirectory -BuildDirectory $BuildDirectory -IsccPath $IsccPath
    if($Version -and $Version -ne $material.version){throw 'Requested ZIP version differs from VERSION and validated products.'}
    if($ValidateOnly){Write-Output "Validated manual ZIP inputs for $($material.version).";return}
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    $PreparedStage=Join-Path $output ('.zip-stage-'+[guid]::NewGuid().ToString('N'))
    New-MgstcDistributionStage -Material $material -StageDirectory $PreparedStage | Out-Null
    $ownedStage=$true
}
try {
    $report=Test-MgstcDistributionStage -StageDirectory $PreparedStage -ProjectRoot $root
    if($Version -and $Version -ne $report.version){throw 'Requested ZIP version differs from the common stage.'}
    if($ValidateOnly){Write-Output "Validated prepared manual ZIP inputs for $($report.version).";return}
    $artifact=New-MgstcManualZip -StageDirectory $PreparedStage -OutputDirectory $output -ProjectRoot $root
    [IO.File]::WriteAllText(($artifact.path+'.payload.json'),($artifact | Select-Object files,bytes,sha256 | ConvertTo-Json -Depth 8),(New-Object Text.UTF8Encoding($false)))
    Write-Output $artifact.path
} finally {
    if($ownedStage) {
        $resolved=[IO.Path]::GetFullPath($PreparedStage)
        if((Split-Path -Parent $resolved) -ne $output -or (Split-Path -Leaf $resolved) -notlike '.zip-stage-*'){throw 'Unsafe ZIP staging cleanup path.'}
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
