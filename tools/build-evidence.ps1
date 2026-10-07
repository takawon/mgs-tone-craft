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
                $nativeError = $_.Exception.HResult -band 0xffff
                if ($nativeError -notin @(32, 33) -or $attempt -ge 9) { throw }
                Start-Sleep -Milliseconds 100
            }
        }
    } finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

