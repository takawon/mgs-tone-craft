# SPDX-License-Identifier: AGPL-3.0-only
function Write-BuildEvidence([string]$Path, [string]$Json) {
    # Keep readers on a complete document and tolerate brief sharing violations.
    $temporary = "$Path.$PID.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText($temporary, $Json, (New-Object Text.UTF8Encoding($false)))
        for ($attempt = 0; ; $attempt++) {
            try {
                if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temporary, $Path, [NullString]::Value) }
                else { [IO.File]::Move($temporary, $Path) }
                return
            } catch [IO.IOException] {
                # PowerShell may wrap the IOException from a .NET method call.
                $nativeError = $_.Exception.GetBaseException().HResult -band 0xffff
                $canRetry = $nativeError -in @(32, 33) -or
                    ($nativeError -eq 1175 -and [IO.File]::Exists($temporary) -and [IO.File]::Exists($Path))
                if (-not $canRetry -or $attempt -ge 9) { throw }
                Start-Sleep -Milliseconds 100
            }
        }
    } finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

