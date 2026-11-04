param(
    [ValidateRange(1, 65535)]
    [int]$Port = 4096,
    [ValidateNotNullOrEmpty()]
    [string]$Username = "opencode",
    [switch]$PrepareOnly
)

$ErrorActionPreference = "Stop"
if ($Username -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$') { throw "Username is invalid" }

$appData = [Environment]::GetFolderPath([Environment+SpecialFolder]::ApplicationData)
$root = Join-Path $appData "gw2-agent-chat"
$connectionPath = Join-Path $root "connection.json"
$agentPath = Join-Path $PSScriptRoot "node_modules\opencode-ai\bin\opencode.exe"
if (-not (Test-Path -LiteralPath $agentPath -PathType Leaf)) { throw "Install the pinned OpenCode package with npm ci first" }
$version = (& $agentPath --version 2>$null | Select-Object -First 1).Trim()
if ($LASTEXITCODE -ne 0 -or $version -ne "1.19.5") { throw "The pinned OpenCode CLI 1.19.5 is required" }

$identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
$inherit = [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor [Security.AccessControl.InheritanceFlags]::ObjectInherit
$allow = [Security.AccessControl.AccessControlType]::Allow
$full = [Security.AccessControl.FileSystemRights]::FullControl
$propagation = [Security.AccessControl.PropagationFlags]::None

foreach ($path in @($appData, $root, $connectionPath)) {
    if (Test-Path -LiteralPath $path) {
        $item = Get-Item -LiteralPath $path -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Connection path cannot be a reparse point" }
        if ($path -eq $connectionPath -and $item.PSIsContainer) { throw "Connection path must be a file" }
    }
}

[IO.Directory]::CreateDirectory($root) | Out-Null
$directoryAcl = New-Object Security.AccessControl.DirectorySecurity
$directoryAcl.SetAccessRuleProtection($true, $false)
$directoryAcl.SetOwner($identity)
$directoryRule = [Security.AccessControl.FileSystemAccessRule]::new($identity, $full, $inherit, $propagation, $allow)
$directoryAcl.AddAccessRule($directoryRule)
Set-Acl -LiteralPath $root -AclObject $directoryAcl

$bytes = New-Object byte[] 32
$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
$previousPassword = $env:OPENCODE_SERVER_PASSWORD
$previousUsername = $env:OPENCODE_SERVER_USERNAME
$process = $null
try {
    $rng.GetBytes($bytes)
    $password = [Convert]::ToBase64String($bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')
    $env:OPENCODE_SERVER_PASSWORD = $password
    $env:OPENCODE_SERVER_USERNAME = $Username

    if (-not $PrepareOnly) {
        $process = Start-Process -FilePath $agentPath -ArgumentList @("serve", "--hostname", "127.0.0.1", "--port", "$Port") -WorkingDirectory $PSScriptRoot -PassThru -NoNewWindow
    }

    $temporaryPath = Join-Path $root (".connection-" + [Guid]::NewGuid().ToString("N") + ".tmp")
    try {
        $handle = [IO.File]::Open($temporaryPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $handle.Dispose()
        $fileAcl = New-Object Security.AccessControl.FileSecurity
        $fileAcl.SetAccessRuleProtection($true, $false)
        $fileAcl.SetOwner($identity)
        $fileRule = [Security.AccessControl.FileSystemAccessRule]::new($identity, $full, [Security.AccessControl.InheritanceFlags]::None, $propagation, $allow)
        $fileAcl.AddAccessRule($fileRule)
        Set-Acl -LiteralPath $temporaryPath -AclObject $fileAcl

        $connection = [ordered]@{ url = "http://127.0.0.1:$Port"; username = $Username; password = $password } | ConvertTo-Json -Compress
        [IO.File]::WriteAllText($temporaryPath, $connection, [Text.UTF8Encoding]::new($false))
        if (Test-Path -LiteralPath $connectionPath) {
            Set-Acl -LiteralPath $connectionPath -AclObject $fileAcl
            [IO.File]::Replace($temporaryPath, $connectionPath, $null)
        } else {
            [IO.File]::Move($temporaryPath, $connectionPath)
        }
    } finally {
        if (Test-Path -LiteralPath $temporaryPath) { [IO.File]::Delete($temporaryPath) }
    }
    if ($PrepareOnly) {
        [pscustomobject]@{ ProcessId = $null; ConnectionPath = $connectionPath }
    } else {
        [pscustomobject]@{ ProcessId = $process.Id; ConnectionPath = $connectionPath }
    }
} catch {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    throw
} finally {
    $rng.Dispose()
    [Array]::Clear($bytes, 0, $bytes.Length)
    Remove-Variable password, connection -ErrorAction SilentlyContinue
    if ($null -eq $previousPassword) { Remove-Item Env:OPENCODE_SERVER_PASSWORD -ErrorAction SilentlyContinue } else { $env:OPENCODE_SERVER_PASSWORD = $previousPassword }
    if ($null -eq $previousUsername) { Remove-Item Env:OPENCODE_SERVER_USERNAME -ErrorAction SilentlyContinue } else { $env:OPENCODE_SERVER_USERNAME = $previousUsername }
    Remove-Variable previousPassword, previousUsername -ErrorAction SilentlyContinue
}
