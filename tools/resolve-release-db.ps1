function Resolve-MgstcReleaseDatabase {
    [CmdletBinding()]
    param(
        [ValidateSet('LocalHandoff')][string]$Provider = 'LocalHandoff',
        [Parameter(Mandatory = $true)][string]$SourceDirectory,
        [Parameter(Mandatory = $true)]$Definition,
        [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot)
    )
    # Acquisition is intentionally separate from validation. A future provider
    # must return the same three local files; consumers never download data.
    if ([string]::IsNullOrWhiteSpace($SourceDirectory)) { throw 'A formal DB handoff directory is required.' }
    $directory = if ([IO.Path]::IsPathRooted($SourceDirectory)) {
        [IO.Path]::GetFullPath($SourceDirectory)
    } else { [IO.Path]::GetFullPath((Join-Path $ProjectRoot $SourceDirectory)) }
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) { throw 'The formal DB handoff directory does not exist.' }
    $runtime = [IO.Path]::GetFullPath((Join-Path $env:ProgramData 'OPLL_PatchSlide_DB')).TrimEnd('\', '/')
    if ($directory.TrimEnd('\', '/') -ieq $runtime -or $directory.StartsWith($runtime + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'ProgramData is an installation destination, not a formal DB acquisition source.'
    }
    $ancestor = Get-Item -LiteralPath $directory -Force
    while ($null -ne $ancestor) {
        if ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse points are not permitted in the DB handoff path.' }
        $ancestor = $ancestor.Parent
    }
    $files = [ordered]@{}
    foreach ($entry in $Definition.dbFiles.PSObject.Properties) {
        if ($entry.Name -notin @('patchslide.sqlite', 'patchslide_search.sqlite', 'manifest.json')) { throw 'Unsupported DB file in the distribution definition.' }
        $path = Join-Path $directory $entry.Name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Formal DB file missing: $($entry.Name)" }
        if ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse points are not permitted in the DB handoff.' }
        $files[$entry.Name] = $path
    }
    if ($files.Count -ne 3) { throw 'A complete three-file DB definition is required.' }
    foreach ($name in $files.Keys) {
        foreach ($suffix in @('-wal', '-shm', '-journal')) {
            if (Test-Path -LiteralPath ($files[$name] + $suffix)) { throw 'The formal DB handoff must not contain active SQLite sidecars.' }
        }
    }
    [pscustomobject]@{ provider = $Provider; directory = $directory; files = $files }
}
