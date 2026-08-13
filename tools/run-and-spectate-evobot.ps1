[CmdletBinding()]
param(
    [string]$Map = "e1m1",
    [string]$BotName = "e1m1bot",
    [int]$Port = 27500,
    [string]$RconPassword = "evobotlocal",
    [string]$Mvdsv = "",
    [string]$EzQuake = "",
    [string]$BaseDir = "",
    [string]$Game = "evosp",
    [switch]$Deathmatch,
    [switch]$GenerateNav,
    [switch]$Windowed,
    [switch]$NoRunRecording
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$evoBotRoot = (Resolve-Path (Join-Path $repoRoot "..")).Path
$serverScript = Join-Path $PSScriptRoot "run-evobot-mvdsv.ps1"
$spectatorScript = Join-Path $PSScriptRoot "spectate-evobot-ezquake.ps1"
Write-Host "EvoBot combined launcher v20260813.4"
Write-Host "Launcher script: $($MyInvocation.MyCommand.Path)"

function Quote-ProcessArgument([string]$value) {
    return '"' + ($value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

function Get-UdpListenerProcess([int]$serverPort) {
    $pattern = '^\s*UDP\s+\S+:' + [regex]::Escape($serverPort.ToString()) +
        '\s+\S+\s+(\d+)\s*$'
    foreach ($line in (& netstat -ano -p udp 2>$null)) {
        if ($line -match $pattern) {
            return Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue
        }
    }
    return $null
}

function Confirm-LocalServerReplacement([int]$serverPort) {
    $listener = Get-UdpListenerProcess $serverPort
    if (-not $listener) { return }
    $listenerPath = $listener.Path
    $localBuildRoots = @("build-gaze-output-next", "build", "build-live-fix", "build-gaze-output") | ForEach-Object {
        [IO.Path]::GetFullPath((Join-Path $repoRoot $_)) +
            [IO.Path]::DirectorySeparatorChar
    }
    $isLocalMvdsv = $listener.ProcessName -ieq "mvdsv" -and $listenerPath -and
        ($localBuildRoots | Where-Object {
            [IO.Path]::GetFullPath($listenerPath).StartsWith(
                $_, [StringComparison]::OrdinalIgnoreCase)
        })
    if (-not $isLocalMvdsv) {
        throw "UDP port $serverPort is owned by PID $($listener.Id) ('$($listener.ProcessName)'), not this repository's MVDSV. Close it or use -Port."
    }

    Write-Host "A previous local MVDSV is still using UDP port ${serverPort}:"
    Write-Host "  PID $($listener.Id): $listenerPath"
    $answer = Read-Host "Stop that server and replace it? [Y/n]"
    if ($answer -and $answer -notmatch '^(?i)y(es)?$') {
        throw "Launch cancelled; the existing MVDSV was left running."
    }
    Stop-Process -Id $listener.Id -Force
    $listener.WaitForExit()
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ((Get-UdpListenerProcess $serverPort) -and
        [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    if (Get-UdpListenerProcess $serverPort) {
        throw "UDP port $serverPort remained occupied after stopping PID $($listener.Id)."
    }
    Write-Host "Stopped previous local MVDSV PID $($listener.Id)."
}

function Wait-Mvdsv([string]$address, [int]$serverPort, [string]$readyToken,
    [int]$timeoutSeconds, [Diagnostics.Process]$bootstrapProcess,
    [string]$failureFile) {
    $deadline = [DateTime]::UtcNow.AddSeconds($timeoutSeconds)
    $request = [byte[]](255, 255, 255, 255) +
        [Text.Encoding]::ASCII.GetBytes("status`n")
    while ([DateTime]::UtcNow -lt $deadline) {
        $bootstrapProcess.Refresh()
        if ($bootstrapProcess.HasExited) {
            $detail = ""
            if ($failureFile -and (Test-Path -LiteralPath $failureFile -PathType Leaf)) {
                $detail = (Get-Content -LiteralPath $failureFile -Raw).Trim()
            }
            $message = "The MVDSV bootstrap process exited with code $($bootstrapProcess.ExitCode) before the bot became ready."
            if ($detail) {
                $message += "`r`n`r`nChild launcher error:`r`n$detail"
            }
            if ($failureFile) {
                $message += "`r`n`r`nFailure log: $failureFile"
            }
            throw $message
        }
        $udp = $null
        try {
            $udp = New-Object Net.Sockets.UdpClient
            $udp.Client.ReceiveTimeout = 500
            $udp.Connect($address, $serverPort)
            [void]$udp.Send($request, $request.Length)
            $remote = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
            $reply = $udp.Receive([ref]$remote)
            $text = [Text.Encoding]::GetEncoding(28591).GetString($reply)
            if ($text -match [regex]::Escape($readyToken)) {
                return
            }
            Start-Sleep -Milliseconds 250
        }
        catch {
            Start-Sleep -Milliseconds 250
        }
        finally {
            if ($udp) { $udp.Dispose() }
        }
    }
    throw "The EvoBot did not become ready on 127.0.0.1:$serverPort within $timeoutSeconds seconds. Check the server window."
}

$readyToken = "evobotready${PID}${Port}$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$spectatorName = "EvoBotCam"
$launcherLogRoot = Join-Path $repoRoot "build-live-fix\launcher-logs"
[IO.Directory]::CreateDirectory($launcherLogRoot) | Out-Null
$failureFile = Join-Path $launcherLogRoot (
    "bootstrap-{0}-{1}.txt" -f (Get-Date -Format "yyyyMMdd-HHmmss"), $PID)

Confirm-LocalServerReplacement $Port

$serverArguments = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $serverScript,
    "-Map", $Map,
    "-BotName", $BotName,
    "-Port", $Port.ToString(),
    "-RconPassword", $RconPassword,
    "-Game", $Game,
    "-ReadyToken", $readyToken,
    "-WaitForSpectator", $spectatorName,
    "-FailureFile", $failureFile
)
if ($Mvdsv) { $serverArguments += @("-Mvdsv", $Mvdsv) }
if ($BaseDir) { $serverArguments += @("-BaseDir", $BaseDir) }
if ($Deathmatch) { $serverArguments += "-Deathmatch" }
if ($GenerateNav) { $serverArguments += "-GenerateNav" }

$argumentLine = ($serverArguments | ForEach-Object { Quote-ProcessArgument $_ }) -join " "
$serverProcess = Start-Process -FilePath "powershell.exe" -ArgumentList $argumentLine `
    -WorkingDirectory $PSScriptRoot -PassThru
Write-Host "MVDSV console started (PID $($serverProcess.Id)); waiting for the server..."
Wait-Mvdsv "127.0.0.1" $Port $readyToken 300 $serverProcess $failureFile
Write-Host "MVDSV and '$BotName' are ready."

if (-not $NoRunRecording) {
    $recordingStamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $recordingRoot = Join-Path $repoRoot "build-live-fix\spectated-runs"
    $recordingPath = Join-Path $recordingRoot "$Map-$recordingStamp.jsonl"
    $effectiveBaseDir = if ($BaseDir) { $BaseDir } else {
        Join-Path $evoBotRoot "server"
    }
    $navObj = Join-Path $effectiveBaseDir "$Game\evobot\nav\debug\$Map.obj"
    $recorderArguments = @(
        (Join-Path $PSScriptRoot "record_evobot_run.py"),
        "--port", $Port.ToString(),
        "--password", $RconPassword,
        "--bot", $BotName,
        "--output", $recordingPath,
        "--nav-obj", $navObj
    )
    $recorderLine = ($recorderArguments | ForEach-Object {
        Quote-ProcessArgument $_
    }) -join " "
    $recorderProcess = Start-Process -FilePath "python.exe" `
        -ArgumentList $recorderLine -WorkingDirectory $repoRoot `
        -WindowStyle Hidden -PassThru
    Write-Host "Recording run telemetry; the labelled map will be written beside:"
    Write-Host "  $recordingPath"
}

$spectatorParameters = @{
    BotName = $BotName
    SpectatorName = $spectatorName
    Server = "127.0.0.1"
    Port = $Port
}
if ($EzQuake) { $spectatorParameters.EzQuake = $EzQuake }
if ($BaseDir) { $spectatorParameters.BaseDir = $BaseDir }
if ($Windowed) { $spectatorParameters.Windowed = $true }
& $spectatorScript @spectatorParameters

Write-Host ""
Write-Host "The MVDSV console remains open separately. Close it to stop the server."
