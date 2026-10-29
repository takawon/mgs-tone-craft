[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('InstallDb', 'RemoveDb', 'GetUserPath', 'RemoveUserData')]
    [string]$Mode,
    [string]$Source = '',
    [string]$UserPath = '',
    [string]$DbDirectory = '',
    [Parameter(Mandatory = $true)]
    [string]$ResultFile,
    [string]$MessagesFile = ''
)

# Runs only from Setup/Uninstall, never from an application or audio thread.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$dbHashes = [ordered]@{
    'patchslide.sqlite' = '67e3de24979e2c3fe0817653fa8fce6386ee91099b9a615d4231a7a32512d0a0'
    'patchslide_search.sqlite' = 'a7b43f1121e2355f2023488f078cdbe7c8f952d028b6602291cc4a6616e47fa1'
    'manifest.json' = 'd912cf65a8f61fe4a1a3d5c07746525401038521cf48b3bf3cce51b7d6c7eb93'
}
$utf8 = New-Object Text.UTF8Encoding($false)
$script:installerMessages = $null

function Get-InstallerMessage([string]$Key, [string[]]$Arguments = @()) {
    if ($null -eq $script:installerMessages) {
        $script:installerMessages = @{}
        if ($MessagesFile) {
            foreach ($line in [IO.File]::ReadAllLines($MessagesFile, [Text.Encoding]::UTF8)) {
                $separator = $line.IndexOf('=')
                if ($separator -gt 0) {
                    $script:installerMessages[$line.Substring(0, $separator)] = $line.Substring($separator + 1)
                }
            }
        }
    }
    if ($script:installerMessages.ContainsKey($Key)) {
        $text = $script:installerMessages[$Key]
        for ($index = 0; $index -lt $Arguments.Count; ++$index) {
            $text = $text.Replace(('%' + ($index + 1)), $Arguments[$index])
        }
        return $text
    }
    if ($Arguments.Count -gt 0) { return ($Key + ': ' + ($Arguments -join ' | ')) }
    return $Key
}

function Initialize-Identity {
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Principal;
public static class MgstcInstallerIdentity {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr OpenProcess(uint access, bool inherit, int id);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool GetTokenInformation(IntPtr token, int informationClass,
        IntPtr information, int length, out int needed);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);
    public static string OwnerSid(int id) {
        IntPtr process = OpenProcess(0x1000, false, id);
        if (process == IntPtr.Zero) throw new Win32Exception();
        IntPtr token = IntPtr.Zero;
        IntPtr buffer = IntPtr.Zero;
        try {
            if (!OpenProcessToken(process, 8, out token)) throw new Win32Exception();
            int needed;
            GetTokenInformation(token, 1, IntPtr.Zero, 0, out needed);
            buffer = Marshal.AllocHGlobal(needed);
            if (!GetTokenInformation(token, 1, buffer, needed, out needed))
                throw new Win32Exception();
            return new SecurityIdentifier(Marshal.ReadIntPtr(buffer)).Value;
        } finally {
            if (buffer != IntPtr.Zero) Marshal.FreeHGlobal(buffer);
            if (token != IntPtr.Zero) CloseHandle(token);
            CloseHandle(process);
        }
    }
}
'@
}

function Assert-PlainPath([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw (Get-InstallerMessage 'HelperReparsePoint' @($cursor))
            }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
}

function Get-InteractiveUserPath {
    # The elevated account can differ from the desktop user. Never use its APPDATA.
    Initialize-Identity
    $session = (Get-Process -Id $PID).SessionId
    $owners = @(Get-Process -Name explorer -ErrorAction SilentlyContinue |
        Where-Object { $_.SessionId -eq $session } |
        ForEach-Object {
            [MgstcInstallerIdentity]::OwnerSid($_.Id)
        } | Sort-Object -Unique)
    if ($owners.Count -ne 1) {
        throw (Get-InstallerMessage 'HelperDesktopUserUnknown')
    }
    $sid = $owners[0]
    $profile = Get-ItemPropertyValue -LiteralPath (
        "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\$sid") -Name ProfileImagePath
    $appData = Get-ItemPropertyValue -LiteralPath (
        "Registry::HKEY_USERS\$sid\Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Folders") -Name 'Local AppData'
    # Shell Folders provides the resolved path. Fail closed on expansion/redirects
    # outside the identified profile instead of guessing with administrator env vars.
    if ($profile.Contains('%') -or $appData.Contains('%') -or
        -not [IO.Path]::IsPathRooted($profile) -or -not [IO.Path]::IsPathRooted($appData)) {
        throw (Get-InstallerMessage 'HelperProfilePathUnresolved')
    }
    $profile = [IO.Path]::GetFullPath($profile).TrimEnd('\') + '\'
    $appData = [IO.Path]::GetFullPath($appData)
    if (-not $appData.StartsWith($profile, [StringComparison]::OrdinalIgnoreCase)) {
        throw (Get-InstallerMessage 'HelperAppDataRedirected')
    }
    $result = Join-Path $appData 'MgsToneCraft'
    Assert-PlainPath $result
    return $result
}

function Open-ExclusiveFiles([string]$Directory, [string[]]$Names) {
    $handles = New-Object 'System.Collections.Generic.List[System.IO.FileStream]'
    try {
        foreach ($name in $Names) {
            $path = Join-Path $Directory $name
            Assert-PlainPath $path
            if (Test-Path -LiteralPath $path) {
                # Share DELETE permits our same-volume rename while denying all
                # new reads/writes; an already-open SQLite connection conflicts.
                $handles.Add([IO.File]::Open($path, [IO.FileMode]::Open,
                    [IO.FileAccess]::ReadWrite, [IO.FileShare]::Delete))
            }
        }
    } catch {
        foreach ($handle in $handles) { $handle.Dispose() }
        throw (Get-InstallerMessage 'HelperFilesInUse' @($_.Exception.Message))
    }
    return ,$handles
}

function Assert-NoDbSidecars([string]$Directory) {
    foreach ($name in @('patchslide.sqlite', 'patchslide_search.sqlite')) {
        foreach ($suffix in @('-wal', '-shm', '-journal')) {
            if (Test-Path -LiteralPath (Join-Path $Directory ($name + $suffix))) {
                throw (Get-InstallerMessage 'HelperSqliteSidecar' @($name + $suffix))
            }
        }
    }
}

function Grant-DbReadAccess([string]$Path, [bool]$IsDirectory) {
    $acl = Get-Acl -LiteralPath $Path
    $users = New-Object Security.Principal.SecurityIdentifier('S-1-5-32-545')
    if ($IsDirectory) {
        $rule = New-Object Security.AccessControl.FileSystemAccessRule($users,
            [Security.AccessControl.FileSystemRights]::ReadAndExecute,
            ([Security.AccessControl.InheritanceFlags]::ContainerInherit -bor [Security.AccessControl.InheritanceFlags]::ObjectInherit),
            [Security.AccessControl.PropagationFlags]::None, [Security.AccessControl.AccessControlType]::Allow)
    } else {
        $rule = New-Object Security.AccessControl.FileSystemAccessRule($users,
            [Security.AccessControl.FileSystemRights]::ReadAndExecute,
            [Security.AccessControl.AccessControlType]::Allow)
    }
    $acl.AddAccessRule($rule)
    Set-Acl -LiteralPath $Path -AclObject $acl
}

function Install-SharedDb([string]$Directory) {
    if (-not $Source) { throw (Get-InstallerMessage 'HelperSourceRequired') }
    Assert-PlainPath $Source
    foreach ($name in $dbHashes.Keys) {
        $path = Join-Path $Source $name
        Assert-PlainPath $path
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $dbHashes[$name]) {
            throw (Get-InstallerMessage 'HelperPackagedHashMismatch' @($name))
        }
    }
    Assert-PlainPath $Directory
    [IO.Directory]::CreateDirectory($Directory) | Out-Null
    Grant-DbReadAccess $Directory $true
    $marker = Join-Path $Directory '.mgstc-db-transaction.json'
    $transaction = Join-Path $Directory ('.mgstc-installer-' + [Guid]::NewGuid().ToString('N'))
    $oldDirectory = Join-Path $transaction 'old'
    $newDirectory = Join-Path $transaction 'new'
    $lock = $null
    $oldHandles = $null
    $newHandles = $null
    $movedOld = New-Object 'System.Collections.Generic.List[string]'
    $movedNew = New-Object 'System.Collections.Generic.List[string]'
    $canCleanup = $false
    try {
        try {
            $lock = [IO.File]::Open($marker, [IO.FileMode]::CreateNew,
                [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        } catch {
            throw (Get-InstallerMessage 'HelperDatabaseOperationPending' @($marker))
        }
        $record = $utf8.GetBytes((@{ operation = 'InstallDb'; backup = $transaction } | ConvertTo-Json))
        $lock.Write($record, 0, $record.Length)
        $lock.Flush($true)
        [IO.Directory]::CreateDirectory($oldDirectory) | Out-Null
        [IO.Directory]::CreateDirectory($newDirectory) | Out-Null
        foreach ($name in $dbHashes.Keys) {
            $staged = Join-Path $newDirectory $name
            [IO.File]::Copy((Join-Path $Source $name), $staged, $false)
            if ((Get-FileHash -LiteralPath $staged -Algorithm SHA256).Hash -ne $dbHashes[$name]) {
                throw (Get-InstallerMessage 'HelperStagedHashMismatch' @($name))
            }
            Grant-DbReadAccess $staged $false
        }
        Assert-NoDbSidecars $Directory
        $oldHandles = Open-ExclusiveFiles $Directory @($dbHashes.Keys)
        $newHandles = Open-ExclusiveFiles $newDirectory @($dbHashes.Keys)
        Assert-NoDbSidecars $Directory
        try {
            foreach ($handle in $oldHandles) {
                $name = [IO.Path]::GetFileName($handle.Name)
                [IO.File]::Move($handle.Name, (Join-Path $oldDirectory $name))
                $movedOld.Add($name)
            }
            foreach ($name in $dbHashes.Keys) {
                [IO.File]::Move((Join-Path $newDirectory $name), (Join-Path $Directory $name))
                $movedNew.Add($name)
            }
            $canCleanup = $true
        } catch {
            $originalError = $_.Exception.Message
            try {
                foreach ($name in $movedNew) { [IO.File]::Delete((Join-Path $Directory $name)) }
                foreach ($name in $movedOld) {
                    [IO.File]::Move((Join-Path $oldDirectory $name), (Join-Path $Directory $name))
                }
                $canCleanup = $true
            } catch {
                throw (Get-InstallerMessage 'HelperRollbackFailed' @($transaction, $marker, $originalError, $_.Exception.Message))
            }
            throw (Get-InstallerMessage 'HelperReplacementRestored' @($originalError))
        }
    } finally {
        if ($newHandles) { foreach ($handle in $newHandles) { $handle.Dispose() } }
        if ($oldHandles) { foreach ($handle in $oldHandles) { $handle.Dispose() } }
        if ($lock) {
            # Pre-mutation failures also leave the original files untouched.
            if ($movedOld.Count -eq 0 -and $movedNew.Count -eq 0) { $canCleanup = $true }
            $lock.Dispose()
            if ($canCleanup) {
                # Only our exact files and empty, known transaction directories.
                foreach ($subdir in @($oldDirectory, $newDirectory)) {
                    foreach ($name in $dbHashes.Keys) {
                        $temporaryFile = Join-Path $subdir $name
                        if (Test-Path -LiteralPath $temporaryFile) { [IO.File]::Delete($temporaryFile) }
                    }
                    if (Test-Path -LiteralPath $subdir) { [IO.Directory]::Delete($subdir, $false) }
                }
                if (Test-Path -LiteralPath $transaction) { [IO.Directory]::Delete($transaction, $false) }
                [IO.File]::Delete($marker)
            }
        }
    }
}

function Remove-KnownFiles([string]$Directory, [string[]]$Names) {
    Assert-PlainPath $Directory
    if (-not (Test-Path -LiteralPath $Directory)) { return }
    $handles = Open-ExclusiveFiles $Directory $Names
    try {
        foreach ($handle in $handles) {
            [IO.File]::Delete($handle.Name)
        }
    } finally {
        foreach ($handle in $handles) { $handle.Dispose() }
    }
    # Unknown files are retained; only remove the directory if now empty.
    if (@(Get-ChildItem -LiteralPath $Directory -Force).Count -eq 0) {
        [IO.Directory]::Delete($Directory, $false)
    }
}

try {
    if (-not $DbDirectory) {
        $DbDirectory = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'OPLL_PatchSlide_DB'
    }
    switch ($Mode) {
        'GetUserPath' {
            $message = Get-InteractiveUserPath
        }
        'InstallDb' {
            Install-SharedDb ([IO.Path]::GetFullPath($DbDirectory))
            $message = Get-InstallerMessage 'HelperDatabaseInstalled'
        }
        'RemoveDb' {
            Assert-PlainPath $DbDirectory
            if (Test-Path -LiteralPath $DbDirectory) {
                $marker = Join-Path $DbDirectory '.mgstc-db-transaction.json'
                $operationLock = $null
                try {
                    try {
                        $operationLock = [IO.File]::Open($marker, [IO.FileMode]::CreateNew,
                            [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
                    } catch {
                        throw (Get-InstallerMessage 'HelperDatabaseUninstallPending')
                    }
                    Assert-NoDbSidecars $DbDirectory
                    Remove-KnownFiles $DbDirectory @($dbHashes.Keys)
                } finally {
                    if ($operationLock) {
                        $operationLock.Dispose()
                        [IO.File]::Delete($marker)
                    }
                }
                if (@(Get-ChildItem -LiteralPath $DbDirectory -Force).Count -eq 0) {
                    [IO.Directory]::Delete($DbDirectory, $false)
                }
            }
            $message = Get-InstallerMessage 'HelperDatabaseRemoved'
        }
        'RemoveUserData' {
            $actual = Get-InteractiveUserPath
            if (-not $UserPath -or -not [string]::Equals([IO.Path]::GetFullPath($UserPath), $actual,
                [StringComparison]::OrdinalIgnoreCase)) {
                throw (Get-InstallerMessage 'HelperUserPathMismatch')
            }
            Remove-KnownFiles $actual @('tone_library.sqlite', 'tone_library.sqlite-wal',
                'tone_library.sqlite-shm', 'tone_library.sqlite-journal', 'settings-v1.ini')
            $message = Get-InstallerMessage 'HelperUserDataRemoved'
        }
    }
    [IO.File]::WriteAllText($ResultFile, $message, $utf8)
    exit 0
} catch {
    $message = $_.Exception.Message
    try { [IO.File]::WriteAllText($ResultFile, $message, $utf8) } catch { }
    Write-Error $message -ErrorAction Continue
    exit 1
}
