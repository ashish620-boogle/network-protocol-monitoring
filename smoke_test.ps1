$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$serverBinary = Join-Path $projectRoot "server.exe"
$clientBinary = Join-Path $projectRoot "client.exe"

if (-not (Test-Path $serverBinary)) {
    throw "Missing server.exe. Build the project first with 'mingw32-make all'."
}

if (-not (Test-Path $clientBinary)) {
    throw "Missing client.exe. Build the project first with 'mingw32-make all'."
}

$logDir = Join-Path $projectRoot ".smoke-test"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$serverStdout = Join-Path $logDir "server.stdout.log"
$serverStderr = Join-Path $logDir "server.stderr.log"
$clientStdout = Join-Path $logDir "client.stdout.log"
$clientStderr = Join-Path $logDir "client.stderr.log"

Remove-Item -ErrorAction SilentlyContinue $serverStdout, $serverStderr, $clientStdout, $clientStderr

$serverProcess = $null
$clientProcess = $null

function Assert-LogContains {
    param(
        [string]$Path,
        [string]$Pattern,
        [string]$Message
    )

    if (-not (Select-String -Path $Path -Pattern $Pattern -Quiet)) {
        throw $Message
    }
}

function Show-Log {
    param(
        [string]$Title,
        [string]$Path
    )

    Write-Host ""
    Write-Host "== $Title =="
    if (Test-Path $Path) {
        Get-Content $Path
    }
}

try {
    Write-Host "Starting server..."
    $serverProcess = Start-Process -FilePath $serverBinary `
        -WorkingDirectory $projectRoot `
        -PassThru `
        -RedirectStandardOutput $serverStdout `
        -RedirectStandardError $serverStderr

    Start-Sleep -Seconds 1
    if ($serverProcess.HasExited) {
        throw "Server exited immediately. Check $serverStderr"
    }

    Write-Host "Starting client..."
    $clientProcess = Start-Process -FilePath $clientBinary `
        -ArgumentList "device-01", "127.0.0.1" `
        -WorkingDirectory $projectRoot `
        -PassThru `
        -RedirectStandardOutput $clientStdout `
        -RedirectStandardError $clientStderr

    Start-Sleep -Seconds 7

    Assert-LogContains -Path $serverStderr -Pattern "Starting device monitoring server" -Message "Server did not start correctly."
    Assert-LogContains -Path $serverStderr -Pattern "Listening TCP port 5000 and UDP port 5001" -Message "Server did not bind to the expected ports."
    Assert-LogContains -Path $serverStderr -Pattern "Accepted TCP client" -Message "Server did not accept a TCP client."
    Assert-LogContains -Path $serverStderr -Pattern "Device device-01 is online" -Message "Server did not mark the device online."
    Assert-LogContains -Path $serverStderr -Pattern "Received heartbeat from device-01 via .* seq=1" -Message "Server did not receive the TCP register packet."
    Assert-LogContains -Path $serverStderr -Pattern "Received heartbeat from device-01 via .* seq=2" -Message "Server did not receive the first heartbeat."

    Assert-LogContains -Path $clientStderr -Pattern "UDP heartbeat socket initialized" -Message "Client did not initialize UDP."
    Assert-LogContains -Path $clientStderr -Pattern "TCP connection established" -Message "Client did not connect over TCP."
    Assert-LogContains -Path $clientStderr -Pattern "Sent TCP register seq=1" -Message "Client did not send the register packet."
    Assert-LogContains -Path $clientStderr -Pattern "Received TCP ACK for register sequence 1" -Message "Client did not receive the register ACK."
    Assert-LogContains -Path $clientStderr -Pattern "Sent UDP heartbeat seq=2" -Message "Client did not send the UDP heartbeat."
    Assert-LogContains -Path $clientStderr -Pattern "Sent TCP heartbeat seq=2" -Message "Client did not send the TCP heartbeat."
    Assert-LogContains -Path $clientStderr -Pattern "Received TCP ACK for heartbeat sequence 2" -Message "Client did not receive the heartbeat ACK."

    Write-Host "Smoke test passed."
    Show-Log -Title "Server Log" -Path $serverStderr
    Show-Log -Title "Client Log" -Path $clientStderr
}
catch {
    Show-Log -Title "Server Log" -Path $serverStderr
    Show-Log -Title "Client Log" -Path $clientStderr
    throw
}
finally {
    if ($clientProcess -and -not $clientProcess.HasExited) {
        Stop-Process -Id $clientProcess.Id -Force
    }
    if ($serverProcess -and -not $serverProcess.HasExited) {
        Stop-Process -Id $serverProcess.Id -Force
    }
}
