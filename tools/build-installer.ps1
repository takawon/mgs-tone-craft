[CmdletBinding(DefaultParameterSetName='Inputs')]
param(
    [Parameter(Mandatory=$true,ParameterSetName='Inputs')][string]$DbSourceDirectory,
    [Parameter(Mandatory=$true,ParameterSetName='Prepared')][string]$PreparedStage,
    [string]$BuildDirectory='build', [string]$OutputDirectory='dist',
    [string]$IsccPath='', [switch]$ValidateOnly
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'distribution-common.ps1')
$root=Split-Path -Parent $PSScriptRoot
$output=Get-MgstcAbsolutePath $OutputDirectory $root
$ownedStage=$false
if($PSCmdlet.ParameterSetName -eq 'Inputs') {
    $material=Get-MgstcDistributionInput -ProjectRoot $root -DbSourceDirectory $DbSourceDirectory -BuildDirectory $BuildDirectory -IsccPath $IsccPath
    if($ValidateOnly) { Write-Output "Validated $($material.version) Release, x64 product VST3, formal $($material.definition.dbVersion), $($material.definition.candidateCount) candidates, licenses and pinned compiler.";return }
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    $PreparedStage=Join-Path $output ('.installer-stage-'+[guid]::NewGuid().ToString('N'))
    New-MgstcDistributionStage -Material $material -StageDirectory $PreparedStage | Out-Null
    $IsccPath=$material.compiler
    $ownedStage=$true
}
try {
    $report=Test-MgstcDistributionStage -StageDirectory $PreparedStage -ProjectRoot $root
    if($ValidateOnly) {
        $definition=Get-Content -LiteralPath (Join-Path $root 'installer/distribution.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        Get-MgstcCompiler $IsccPath $root $definition.compilerVersion $definition.compilerSha256 $definition.compilerSupportFiles | Out-Null
        Write-Output "Validated prepared $($report.version) distribution stage.";return
    }
    $artifact=Invoke-MgstcInstallerBuild -StageDirectory $PreparedStage -OutputDirectory $output -IsccPath $IsccPath -ProjectRoot $root
    $sidecar=[ordered]@{version=$report.version;binaryVersion=$report.binaryVersion;dbVersion=$report.dbVersion;candidateCount=$report.candidateCount;files=$report.files;setupSha256=$artifact.sha256}
    [IO.File]::WriteAllText(($artifact.path+'.payload.json'),($sidecar | ConvertTo-Json -Depth 8),(New-Object Text.UTF8Encoding($false)))
    Write-Output $artifact.path
} finally {
    if($ownedStage) {
        $resolved=[IO.Path]::GetFullPath($PreparedStage)
        if((Split-Path -Parent $resolved) -ne $output -or (Split-Path -Leaf $resolved) -notlike '.installer-stage-*') {throw 'Unsafe installer staging cleanup path.'}
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
