# Shared, local-only packaging operations. No GitHub, tag or publish commands.
. (Join-Path $PSScriptRoot 'resolve-release-db.ps1')

function Get-MgstcAbsolutePath([string]$Path, [string]$Root) {
    $absolute = if ([IO.Path]::IsPathRooted($Path)) { [IO.Path]::GetFullPath($Path) } else { [IO.Path]::GetFullPath((Join-Path $Root $Path)) }
    if ($absolute.Length -gt [IO.Path]::GetPathRoot($absolute).Length) { return $absolute.TrimEnd([char[]]'\/') }
    $absolute
}
function Assert-MgstcFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -eq 0) { throw "Required nonempty file missing: $Path" }
    if ((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse points cannot be packaged.' }
    $directory = (Get-Item -LiteralPath $Path -Force).Directory
    while ($null -ne $directory) {
        if ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse-point material directories cannot be packaged.' }
        $directory = $directory.Parent
    }
}
function Assert-MgstcRelativePath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path -match '[:\\]|(^|/)\.\.?(/|$)|^/|[\x00-\x1f]') { throw "Unsafe package path: $Path" }
}

function Assert-MgstcManualInstallText([byte[]]$Bytes, [string]$Name, $Definition) {
    if ($Bytes.Length -lt 3 -or $Bytes[0] -ne 239 -or $Bytes[1] -ne 187 -or $Bytes[2] -ne 191) { throw "Manual guide requires UTF-8 BOM: $Name" }
    $utf8 = New-Object Text.UTF8Encoding($false, $true)
    $body = $utf8.GetString($Bytes, 3, $Bytes.Length - 3)
    if ($body -match "(?<!`r)`n|`r(?!`n)" -or $body.Contains([string][char]0xfffd)) { throw "Invalid manual guide encoding/newlines: $Name" }
    foreach ($token in $Definition.manualInstallRequiredText) {
        if (-not $body.Contains($token)) { throw "Manual guide missing shared requirement '$token': $Name" }
    }
    $sections = @([regex]::Matches($body, '(?m)^([1-6])\. ') | ForEach-Object { $_.Groups[1].Value })
    if (($sections -join ',') -cne '1,2,3,4,5,6') { throw "Manual guide sections differ: $Name" }
    if (($Name -ceq 'INSTALL.txt' -and $body -notmatch '[\u3040-\u30ff\u4e00-\u9fff]') -or
        ($Name -ceq 'INSTALL.en.txt' -and $body -notmatch 'manual installation \(English\)')) { throw "Manual guide language mismatch: $Name" }
}
function Test-MgstcManualInstallGuides([string]$BaseDirectory, $Definition, [switch]$SourcePaths) {
    $names = @($Definition.manualInstallFiles.PSObject.Properties.Name | Sort-Object)
    if (($names -join ',') -cne 'INSTALL.en.txt,INSTALL.txt') { throw 'Both Japanese and English manual guides are required.' }
    foreach ($entry in $Definition.manualInstallFiles.PSObject.Properties) {
        Assert-MgstcRelativePath $entry.Value
        $relative = if ($SourcePaths) { $entry.Value } else { $entry.Name }
        $path = Join-Path $BaseDirectory $relative
        Assert-MgstcFile $path
        Assert-MgstcManualInstallText ([IO.File]::ReadAllBytes($path)) $entry.Name $Definition
    }
}

function Assert-MgstcX64([string]$Path) {
    Assert-MgstcFile $Path
    $stream = [IO.File]::OpenRead($Path); $reader = New-Object IO.BinaryReader($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'Product is not a PE file.' }
        $stream.Position = 0x3c; $offset = $reader.ReadInt32(); $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'Product must be Windows x64.' }
    } finally { $reader.Dispose(); $stream.Dispose() }
}
function Assert-MgstcProduct([string]$Path, [string]$BinaryVersion) {
    Assert-MgstcX64 $Path
    $info = (Get-Item -LiteralPath $Path).VersionInfo; $parsed = $null
    if (-not [version]::TryParse($info.ProductVersion, [ref]$parsed) -or $parsed.ToString(3) -ne ([version]$BinaryVersion).ToString(3) -or $parsed.Revision -gt 0) { throw "Product version mismatch: $Path" }
    if ($info.CompanyName.Trim() -ne 'Takawo.') { throw 'Unexpected product publisher.' }
    if ($info.IsDebug) { throw 'Debug product cannot be packaged.' }
}
function Assert-MgstcSetup([string]$Path, [string]$BinaryVersion) {
    Assert-MgstcFile $Path
    # The pinned Inno Setup 6.7.3 loader is an x86 bootstrap, including for
    # x64-only installations. Product EXE and VST3 checks remain strictly x64.
    $stream = [IO.File]::OpenRead($Path); $reader = New-Object IO.BinaryReader($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'Setup bootstrap is not a PE file.' }
        $stream.Position = 0x3c; $offset = $reader.ReadInt32(); $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x014C) { throw 'Unexpected Inno Setup bootstrap architecture.' }
    } finally { $reader.Dispose(); $stream.Dispose() }
    $info = (Get-Item -LiteralPath $Path).VersionInfo; $parsed = $null
    if (-not [version]::TryParse($info.ProductVersion, [ref]$parsed) -or $parsed.ToString(3) -ne ([version]$BinaryVersion).ToString(3) -or $parsed.Revision -gt 0) { throw 'Setup product version mismatch.' }
    if ($info.CompanyName.Trim() -ne 'Takawo.') { throw 'Unexpected setup publisher.' }
    if ($info.IsDebug) { throw 'Debug setup cannot be distributed.' }
}
function Assert-MgstcDatabase($Files, $Definition) {
    if (@($Definition.dbFiles.PSObject.Properties).Count -ne 3) { throw 'The approved DB set must contain three files.' }
    foreach ($entry in $Definition.dbFiles.PSObject.Properties) {
        $path = $Files[$entry.Name]; Assert-MgstcFile $path
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $entry.Value) { throw "Formal DB hash mismatch: $($entry.Name)" }
    }
    $manifest = Get-Content -LiteralPath $Files['manifest.json'] -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($manifest.source_binding.candidate_count -ne $Definition.candidateCount -or
        $manifest.physical_packaging.version -ne $Definition.dbVersion -or
        $manifest.source_db_sha256 -ne $Definition.dbFiles.'patchslide.sqlite' -or
        $manifest.search_db_sha256 -ne $Definition.dbFiles.'patchslide_search.sqlite') { throw 'DB manifest binding/count/version mismatch.' }
}
function Assert-MgstcProductBundleInventory([string[]]$Names, $Definition) {
    $expected=@($Definition.productBundleFiles)
    foreach($name in $expected){Assert-MgstcRelativePath $name}
    if(($Names.Count -ne $expected.Count) -or (($Names | Sort-Object) -join "`n") -cne (($expected | Sort-Object) -join "`n")) {throw 'Product VST3 bundle contains missing, unknown, diagnostic or user-data files.'}
}
function Get-MgstcCompiler([string]$IsccPath, [string]$Root, [string]$ExpectedVersion, [string]$ExpectedSha256, $SupportFiles) {
    if (-not $IsccPath) {
        $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
        if ($command) { $IsccPath = $command.Source }
        foreach ($candidate in @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe")) {
            if (-not $IsccPath -and (Test-Path -LiteralPath $candidate -PathType Leaf)) { $IsccPath = $candidate }
        }
    }
    if (-not $IsccPath) { throw "Inno Setup $ExpectedVersion is required; specify -IsccPath." }
    $compiler = Get-MgstcAbsolutePath $IsccPath $Root; Assert-MgstcFile $compiler
    # Official ISCC has no usable Windows FileVersion. Pin the actual compiler
    # bytes from the approved release instead of trusting a directory name.
    if ((Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash -ine $ExpectedSha256) { throw "Inno Setup compiler must match the approved $ExpectedVersion binary." }
    foreach ($entry in $SupportFiles.PSObject.Properties) {
        Assert-MgstcRelativePath $entry.Name
        $path = Join-Path (Split-Path -Parent $compiler) $entry.Name
        Assert-MgstcFile $path
        if ((Get-FileHash -LiteralPath $path).Hash -ine $entry.Value) { throw "Inno Setup $ExpectedVersion support file mismatch: $($entry.Name)" }
    }
    $compiler
}
function Get-MgstcDocumentationFiles([string]$Root, $Definition) {
    $queue = New-Object 'Collections.Generic.Queue[string]'
    foreach ($name in @($Definition.documents) + @($Definition.documentation)) { $queue.Enqueue($name) }
    $files = [ordered]@{}
    while ($queue.Count -gt 0) {
        $relative = $queue.Dequeue().Replace('\', '/'); Assert-MgstcRelativePath $relative
        if ($files.Contains($relative)) { continue }
        if ($relative -notin @($Definition.documents) -and $relative -notmatch '^docs/.+\.(md|png|jpg|jpeg|svg|json|txt)$') { throw "Unsupported public documentation target: $relative" }
        $path = Get-MgstcAbsolutePath $relative $Root; Assert-MgstcFile $path
        & git -C $Root check-ignore -q -- $relative
        if ($LASTEXITCODE -eq 0) { throw "Ignored files cannot be packaged as public documentation: $relative" }
        if ($LASTEXITCODE -notin @(0, 1)) { throw 'Unable to check documentation publication boundary.' }
        $files[$relative] = $path
        if ($relative -notmatch '\.md$') { continue }
        $text = Get-Content -LiteralPath $path -Raw -Encoding UTF8
        foreach ($match in [regex]::Matches($text, '!?\[[^\]\r\n]+\]\(([^)\r\n]+)\)')) {
            $target = $match.Groups[1].Value.Trim('<', '>')
            if ($target -match '^https?://' -or $target.StartsWith('#')) { continue }
            $target = [uri]::UnescapeDataString($target.Split('#')[0])
            $resolved = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $path) $target))
            $prefix = $Root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
            if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Documentation link escapes the source tree.' }
            $queue.Enqueue($resolved.Substring($prefix.Length).Replace('\', '/'))
        }
    }
    $files
}
function Get-MgstcDistributionInput {
    [CmdletBinding()]
    param(
        [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
        [Parameter(Mandatory = $true)][string]$DbSourceDirectory,
        [string]$BuildDirectory = 'build', [string]$IsccPath = ''
    )
    $root = [IO.Path]::GetFullPath($ProjectRoot); $build = Get-MgstcAbsolutePath $BuildDirectory $root
    $definitionPath = Join-Path $root 'installer/distribution.json'; Assert-MgstcFile $definitionPath
    $definition = Get-Content -LiteralPath $definitionPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw -Encoding UTF8).Trim()
    if ($version -notmatch '^\d+\.\d+$') { throw 'VERSION must contain major.minor.' }
    $binaryVersion = "$version.0"
    $cachePath = Join-Path $build 'CMakeCache.txt'; Assert-MgstcFile $cachePath
    $cache = Get-Content -LiteralPath $cachePath -Raw
    if ($cache -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release\r?$' -or $cache -notmatch '(?m)^MGSTC_VST3_DIAG_MODE:STRING=OFF\r?$') { throw 'A Release build with product VST3 diagnostics OFF is required.' }
    $exe = Join-Path $build 'mgstc.exe'; $bundle = Join-Path $build 'mgstc_vst3_artefacts/Release/VST3/MGS Tone Craft.vst3'
    $plugin = Join-Path $bundle 'Contents/x86_64-win/MGS Tone Craft.vst3'
    foreach ($path in @($exe, $plugin)) { Assert-MgstcProduct $path $binaryVersion }
    $bundleNames=@(Get-ChildItem -LiteralPath $bundle -Recurse -File -Force | ForEach-Object {$_.FullName.Substring($bundle.Length+1).Replace('\','/')})
    Assert-MgstcProductBundleInventory $bundleNames $definition
    $evidencePath = Join-Path $build '.mgstc-validation.json'; Assert-MgstcFile $evidencePath
    $evidence = Get-Content -LiteralPath $evidencePath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($evidence.configuration -ne 'Release' -or $evidence.status -ne 'BuildAndSelectedTestsPassed' -or @($evidence.targets).Count -ne 0 -or $evidence.sourceBefore.digest -ne $evidence.sourceAfter.digest) { throw 'A full Release build and successful tests through tools/build.cmd are required.' }
    if ($evidence.PSObject.Properties.Name -notcontains 'productArtifacts') { throw 'Build evidence has no product hashes. Run the current tools/build.cmd for Release.' }
    $productFiles = [ordered]@{'mgstc.exe'=$exe}
    foreach ($file in Get-ChildItem -LiteralPath $bundle -Recurse -File -Force) { $productFiles[$file.FullName.Substring($build.Length + 1).Replace('\','/')] = $file.FullName }
    if (@($evidence.productArtifacts.PSObject.Properties).Count -ne $productFiles.Count) { throw 'Validated product bundle inventory differs from the current build.' }
    foreach ($entry in $productFiles.GetEnumerator()) {
        $record = $evidence.productArtifacts.PSObject.Properties[$entry.Key]
        if ($null -eq $record -or $record.Value -ine (Get-FileHash -LiteralPath $entry.Value).Hash) { throw 'Product artifact changed after the formal Release build.' }
    }
    foreach ($entry in $evidence.sourceAfter.files.PSObject.Properties) {
        $source = Join-Path $root $entry.Name
        if (-not (Test-Path -LiteralPath $source -PathType Leaf) -or (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ine $entry.Value) { throw "Source changed after Release validation: $($entry.Name)" }
    }
    foreach ($path in @($exe, $plugin)) {
        if ((Get-Item -LiteralPath $path).LastWriteTimeUtc -gt [datetime]::Parse($evidence.finishedUtc).ToUniversalTime()) { throw 'Product binary changed after Release validation.' }
    }
    $modulePath = Join-Path $bundle 'Contents/Resources/moduleinfo.json'; Assert-MgstcFile $modulePath
    Assert-MgstcModule $modulePath $binaryVersion
    foreach ($file in Get-ChildItem -LiteralPath $bundle -Recurse -Force) {
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point in product VST3 bundle.' }
        if (-not $file.PSIsContainer -and $file.Extension -in @('.pdb', '.lib', '.exp', '.log')) { throw 'Development file in product VST3 bundle.' }
    }
    foreach ($entry in $definition.languageFiles.PSObject.Properties) {
        $path = Join-Path $root "installer/languages/$($entry.Name)"; Assert-MgstcFile $path
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $entry.Value) { throw "Pinned Inno Setup language hash mismatch: $($entry.Name)" }
    }
    $database = Resolve-MgstcReleaseDatabase -SourceDirectory $DbSourceDirectory -Definition $definition -ProjectRoot $root
    Assert-MgstcDatabase $database.files $definition
    $compiler = Get-MgstcCompiler $IsccPath $root $definition.compilerVersion $definition.compilerSha256 $definition.compilerSupportFiles
    $files = [ordered]@{ 'app/mgstc.exe' = $exe }
    $documents = Get-MgstcDocumentationFiles $root $definition
    foreach ($name in $documents.Keys) { $files["app/$name"] = $documents[$name] }
    Test-MgstcManualInstallGuides $root $definition -SourcePaths
    foreach ($entry in $definition.manualInstallFiles.PSObject.Properties) { $files['app/'+$entry.Name] = Join-Path $root $entry.Value }
    foreach ($entry in $definition.licenses.PSObject.Properties) {
        $base = if ($entry.Value.StartsWith('_deps/')) { $build } else { $root }
        $files["app/licenses/$($entry.Name)"] = Join-Path $base $entry.Value
    }
    foreach ($name in $database.files.Keys) { $files["db/$name"] = $database.files[$name] }
    foreach ($entry in $definition.installerOnlyFiles.PSObject.Properties) { $files[$entry.Name] = Join-Path $root $entry.Value }
    foreach ($file in Get-ChildItem -LiteralPath $bundle -Recurse -File -Force) { $files['vst3/' + $file.FullName.Substring($bundle.Length + 1).Replace('\', '/')] = $file.FullName }
    foreach ($entry in $files.GetEnumerator()) { Assert-MgstcRelativePath $entry.Key; Assert-MgstcFile $entry.Value }
    [pscustomobject]@{ root=$root; build=$build; version=$version; binaryVersion=$binaryVersion; definition=$definition; definitionSha256=(Get-FileHash -LiteralPath $definitionPath).Hash; compiler=$compiler; files=$files; sourceDigest=$evidence.sourceAfter.digest; buildHead=$evidence.head }
}
function Assert-MgstcModule([string]$Path, [string]$BinaryVersion) {
    $module = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    if ($module -notmatch ('"Version"\s*:\s*"' + [regex]::Escape($BinaryVersion) + '"') -or $module -notmatch 'ABCDEF019182FAEB4D6773634D677469' -or $module -notmatch 'ABCDEF011234ABCD4D6773634D677469') { throw 'Unexpected product VST3 module identity/version.' }
    $vendors = [regex]::Matches($module, '"Vendor"\s*:\s*"([^"]*)"')
    if ($vendors.Count -eq 0 -or @($vendors | Where-Object { $_.Groups[1].Value -ne 'Takawo.' }).Count -gt 0) { throw 'Unexpected VST3 manufacturer.' }
    $versions = [regex]::Matches($module, '"Version"\s*:\s*"([^"]*)"')
    if ($versions.Count -ne 3 -or @($versions | Where-Object { $_.Groups[1].Value -ne $BinaryVersion }).Count -gt 0 -or [regex]::Matches($module, '"CID"\s*:').Count -ne 2) { throw 'Unexpected VST3 class count/version.' }
}
function New-MgstcDistributionStage {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)][Alias('Input')]$Material, [Parameter(Mandatory=$true)][string]$StageDirectory)
    $stage = Get-MgstcAbsolutePath $StageDirectory $Material.root
    if (Test-Path -LiteralPath $stage) { throw 'Distribution stage must be new; existing outputs are never reused.' }
    New-Item -ItemType Directory -Path $stage | Out-Null
    $inventory = @()
    foreach ($entry in $Material.files.GetEnumerator() | Sort-Object Key) {
        $destination = Join-Path $stage $entry.Key
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        $sourceHash = (Get-FileHash -LiteralPath $entry.Value -Algorithm SHA256).Hash
        Copy-Item -LiteralPath $entry.Value -Destination $destination
        if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $sourceHash) { throw 'Material changed while staging.' }
        $inventory += [ordered]@{path=$entry.Key;sha256=$sourceHash;bytes=(Get-Item -LiteralPath $destination).Length}
    }
    $report = [ordered]@{schemaVersion=1;version=$Material.version;binaryVersion=$Material.binaryVersion;dbVersion=$Material.definition.dbVersion;candidateCount=$Material.definition.candidateCount;definitionSha256=$Material.definitionSha256;sourceDigest=$Material.sourceDigest;buildHead=$Material.buildHead;installerOnlyFiles=@($Material.definition.installerOnlyFiles.PSObject.Properties.Name);files=$inventory}
    [IO.File]::WriteAllText((Join-Path $stage 'payload.json'), ($report | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))
    Test-MgstcDistributionStage -StageDirectory $stage -ProjectRoot $Material.root | Out-Null
    $report
}
function Test-MgstcDistributionStage {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)][string]$StageDirectory, [string]$ProjectRoot=(Split-Path -Parent $PSScriptRoot))
    $root=[IO.Path]::GetFullPath($ProjectRoot); $stage=Get-MgstcAbsolutePath $StageDirectory $root
    Assert-MgstcFile (Join-Path $stage 'payload.json')
    $report=Get-Content -LiteralPath (Join-Path $stage 'payload.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $definitionPath=Join-Path $root 'installer/distribution.json'
    if ($report.schemaVersion -ne 1 -or $report.definitionSha256 -ine (Get-FileHash -LiteralPath $definitionPath).Hash) { throw 'Distribution definition changed after staging.' }
    $definition=Get-Content -LiteralPath $definitionPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $installerOnly=@($definition.installerOnlyFiles.PSObject.Properties.Name | Sort-Object)
    if((@($report.installerOnlyFiles | Sort-Object) -join "`n") -cne ($installerOnly -join "`n")){throw 'Installer-only material mapping differs from the common definition.'}
    $expected=[ordered]@{}
    foreach($entry in $report.files) {
        Assert-MgstcRelativePath $entry.path
        if($expected.Contains($entry.path)) {throw 'Duplicate stage inventory path.'}
        $path=Join-Path $stage $entry.path; Assert-MgstcFile $path
        if((Get-FileHash -LiteralPath $path).Hash -ine $entry.sha256 -or (Get-Item -LiteralPath $path).Length -ne $entry.bytes) {throw "Staged payload mismatch: $($entry.path)"}
        $expected[$entry.path]=$true
    }
    $bundleNames=@($report.files | Where-Object {$_.path.StartsWith('vst3/')} | ForEach-Object {$_.path.Substring(5)})
    Assert-MgstcProductBundleInventory $bundleNames $definition
    foreach($file in Get-ChildItem -LiteralPath $stage -Recurse -Force) {
        if($file.Attributes -band [IO.FileAttributes]::ReparsePoint) {throw 'Reparse point in distribution stage.'}
        if($file.PSIsContainer) {continue}
        $relative=$file.FullName.Substring($stage.Length+1).Replace('\','/')
        if($relative -ne 'payload.json' -and -not $expected.Contains($relative)) {throw "Unexpected staged file: $relative"}
    }
    foreach($name in @('app/mgstc.exe','vst3/Contents/x86_64-win/MGS Tone Craft.vst3','vst3/Contents/Resources/moduleinfo.json') + @($definition.manualInstallFiles.PSObject.Properties.Name | ForEach-Object {"app/$_"}) + @($definition.documents | ForEach-Object {"app/$_"}) + @($definition.documentation | ForEach-Object {"app/$_"}) + @($definition.licenses.PSObject.Properties.Name | ForEach-Object {"app/licenses/$_"}) + @($definition.installerOnlyFiles.PSObject.Properties.Name)) {
        if(-not $expected.Contains($name)) {throw "Required payload entry missing: $name"}
    }
    Test-MgstcManualInstallGuides (Join-Path $stage 'app') $definition
    if($report.version -notmatch '^\d+\.\d+$' -or $report.binaryVersion -ne "$($report.version).0") {throw 'Invalid staged product version.'}
    foreach($name in @('app/mgstc.exe','vst3/Contents/x86_64-win/MGS Tone Craft.vst3')) {Assert-MgstcProduct (Join-Path $stage $name) $report.binaryVersion}
    Assert-MgstcModule (Join-Path $stage 'vst3/Contents/Resources/moduleinfo.json') $report.binaryVersion
    $db=[ordered]@{};foreach($name in $definition.dbFiles.PSObject.Properties.Name){$db[$name]=Join-Path $stage "db/$name"}
    Assert-MgstcDatabase $db $definition
    if($report.dbVersion -ne $definition.dbVersion -or $report.candidateCount -ne $definition.candidateCount) {throw 'Staged DB identity mismatch.'}
    $appPrefix=(Join-Path $stage 'app')+[IO.Path]::DirectorySeparatorChar
    foreach($name in $expected.Keys | Where-Object {$_ -match '^app/.*\.md$' -and $_ -notmatch '^app/licenses/'}) {
        $path=Join-Path $stage $name
        foreach($match in [regex]::Matches((Get-Content -LiteralPath $path -Raw -Encoding UTF8),'!?\[[^\]\r\n]+\]\(([^)\r\n]+)\)')) {
            $target=$match.Groups[1].Value.Trim('<','>')
            if($target -match '^https?://' -or $target.StartsWith('#')){continue}
            $resolved=[IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $path) ([uri]::UnescapeDataString($target.Split('#')[0]))))
            if(-not $resolved.StartsWith($appPrefix,[StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path -LiteralPath $resolved -PathType Leaf)){throw "Broken packaged documentation link in $name"}
        }
    }
    $report
}
function Invoke-MgstcInstallerBuild {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)][string]$StageDirectory,[Parameter(Mandatory=$true)][string]$OutputDirectory,[string]$IsccPath='', [string]$ProjectRoot=(Split-Path -Parent $PSScriptRoot))
    $root=[IO.Path]::GetFullPath($ProjectRoot);$stage=Get-MgstcAbsolutePath $StageDirectory $root;$output=Get-MgstcAbsolutePath $OutputDirectory $root
    $report=Test-MgstcDistributionStage $stage $root
    $definition=Get-Content -LiteralPath (Join-Path $root 'installer/distribution.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $compiler=Get-MgstcCompiler $IsccPath $root $definition.compilerVersion $definition.compilerSha256 $definition.compilerSupportFiles
    $name="MGSToneCraft-$($report.version)-win64-setup.exe";$target=Join-Path $output $name
    if(Test-Path -LiteralPath $target){throw 'Installer output already exists; select a new candidate directory.'}
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    $temporary=Join-Path $output ('.iscc-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $temporary | Out-Null
    try {
        & $compiler "/DSourceRoot=$stage" "/DAppVersion=$($report.version)" "/DBinaryVersion=$($report.binaryVersion)" "/O$temporary" (Join-Path $stage 'recipe/MGSToneCraft.iss') | Out-Host
        if($LASTEXITCODE -ne 0){throw "ISCC failed ($LASTEXITCODE)."}
        $built=Join-Path $temporary $name;Assert-MgstcSetup $built $report.binaryVersion
        Test-MgstcDistributionStage $stage $root | Out-Null
        Move-Item -LiteralPath $built -Destination $target
    } finally {
        if((Split-Path -Parent ([IO.Path]::GetFullPath($temporary))) -ne $output -or (Split-Path -Leaf $temporary) -notlike '.iscc-*'){throw 'Unsafe compiler scratch path.'}
        Remove-Item -LiteralPath $temporary -Recurse -Force
    }
    [pscustomobject]@{path=$target;name=$name;bytes=(Get-Item -LiteralPath $target).Length;sha256=(Get-FileHash -LiteralPath $target).Hash}
}
function Get-MgstcZipInventory($Report, [string]$ProjectRoot=(Split-Path -Parent $PSScriptRoot)) {
    $definition=Get-Content -LiteralPath (Join-Path $ProjectRoot 'installer/distribution.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $bundleNames=@($Report.files | Where-Object {$_.path.StartsWith('vst3/')} | ForEach-Object {$_.path.Substring(5)})
    Assert-MgstcProductBundleInventory $bundleNames $definition
    foreach($entry in $Report.files) {
        if($entry.path -in @($Report.installerOnlyFiles)){continue}
        $name=if($entry.path.StartsWith('app/')){$entry.path.Substring(4)}elseif($entry.path.StartsWith('vst3/')){'MGS Tone Craft.vst3/'+$entry.path.Substring(5)}elseif($entry.path.StartsWith('db/')){'OPLL_PatchSlide_DB/'+$entry.path.Substring(3)}else{throw 'Unknown staged material group.'}
        [pscustomobject]@{path=$name;source=$entry.path;sha256=$entry.sha256;bytes=$entry.bytes}
    }
}
function Test-MgstcManualZip {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)][string]$ZipPath,[Parameter(Mandatory=$true)][string]$StageDirectory,[string]$ProjectRoot=(Split-Path -Parent $PSScriptRoot))
    $stage=Get-MgstcAbsolutePath $StageDirectory $ProjectRoot;$zip=Get-MgstcAbsolutePath $ZipPath $ProjectRoot
    $report=Test-MgstcDistributionStage $stage $ProjectRoot;Assert-MgstcFile $zip
    $expected=[ordered]@{};foreach($entry in Get-MgstcZipInventory $report $ProjectRoot){$expected[$entry.path]=$entry}
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive=[IO.Compression.ZipFile]::OpenRead($zip);$seen=[ordered]@{}
    try {
        foreach($entry in $archive.Entries){
            $name=$entry.FullName
            if($name.EndsWith('/')){throw 'Unexpected directory record in manual ZIP.'}
            Assert-MgstcRelativePath $name
            if($seen.Contains($name) -or -not $expected.Contains($name)){throw "Unexpected or duplicate ZIP entry: $name"}
            if($entry.Length -ne $expected[$name].bytes){throw "ZIP size mismatch: $name"}
            $stream=$entry.Open();$sha=[Security.Cryptography.SHA256]::Create()
            try{$actual=([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','')}finally{$stream.Dispose();$sha.Dispose()}
            if($actual -ine $expected[$name].sha256){throw "ZIP payload hash mismatch: $name"}
            $seen[$name]=$true
        }
    }finally{$archive.Dispose()}
    if($seen.Count -ne $expected.Count){throw 'Manual ZIP is missing required entries.'}
    # Also perform a real extraction, then compare every file to the fixed stage.
    $scratch=Join-Path ([IO.Path]::GetDirectoryName($zip)) ('.zip-check-'+[guid]::NewGuid().ToString('N'))
    try {
        [IO.Compression.ZipFile]::ExtractToDirectory($zip,$scratch)
        $definition=Get-Content -LiteralPath (Join-Path $ProjectRoot 'installer/distribution.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        Test-MgstcManualInstallGuides $scratch $definition
        foreach($entry in $expected.Values){$path=Join-Path $scratch $entry.path;Assert-MgstcFile $path;if((Get-FileHash -LiteralPath $path).Hash -ine $entry.sha256){throw 'Extracted ZIP does not match the common material.'}}
        if(@(Get-ChildItem -LiteralPath $scratch -Recurse -File -Force).Count -ne $expected.Count){throw 'Unexpected extracted file count.'}
    }finally{
        if(Test-Path -LiteralPath $scratch){
            if((Split-Path -Parent ([IO.Path]::GetFullPath($scratch))) -ne [IO.Path]::GetDirectoryName($zip) -or (Split-Path -Leaf $scratch) -notlike '.zip-check-*'){throw 'Unsafe ZIP scratch path.'}
            Remove-Item -LiteralPath $scratch -Recurse -Force
        }
    }
    [pscustomobject]@{path=$zip;files=@($expected.Values);bytes=(Get-Item -LiteralPath $zip).Length;sha256=(Get-FileHash -LiteralPath $zip).Hash}
}
function New-MgstcManualZip {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)][string]$StageDirectory,[Parameter(Mandatory=$true)][string]$OutputDirectory,[string]$ProjectRoot=(Split-Path -Parent $PSScriptRoot))
    $stage=Get-MgstcAbsolutePath $StageDirectory $ProjectRoot;$output=Get-MgstcAbsolutePath $OutputDirectory $ProjectRoot
    $report=Test-MgstcDistributionStage $stage $ProjectRoot;$zip=Join-Path $output "MGSToneCraft-$($report.version)-win64.zip"
    if(Test-Path -LiteralPath $zip){throw 'Manual ZIP output already exists; select a new candidate directory.'}
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive=[IO.Compression.ZipFile]::Open($zip,[IO.Compression.ZipArchiveMode]::Create)
    try{foreach($entry in Get-MgstcZipInventory $report $ProjectRoot){[IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,(Join-Path $stage $entry.source),$entry.path,[IO.Compression.CompressionLevel]::Optimal)|Out-Null}}finally{$archive.Dispose()}
    Test-MgstcManualZip -ZipPath $zip -StageDirectory $stage -ProjectRoot $ProjectRoot
}
