[CmdletBinding()]
param(
    [string]$BotName = "e1m1bot",
    [string]$Server = "127.0.0.1",
    [int]$Port = 27500,
    [string]$SpectatorName = "EvoBotCam",
    [string]$EzQuake = "",
    [string]$BaseDir = "",
    [switch]$Windowed
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$evoBotRoot = (Resolve-Path (Join-Path $repoRoot "..")).Path

if (-not $EzQuake) {
    $EzQuake = Join-Path $evoBotRoot "ezquake\build-msbuild-x64-vs2022\Release\ezquake.exe"
}
if (-not $BaseDir) {
    $BaseDir = Join-Path $evoBotRoot "server"
}

$EzQuake = [IO.Path]::GetFullPath($EzQuake)
$BaseDir = [IO.Path]::GetFullPath($BaseDir)
if (-not (Test-Path -LiteralPath $EzQuake -PathType Leaf)) {
    throw "ezQuake was not found at '$EzQuake'. Build Release or pass -EzQuake <path>."
}
if (-not (Test-Path -LiteralPath $BaseDir -PathType Container)) {
    throw "Quake basedir was not found at '$BaseDir'. Pass -BaseDir <path>."
}

# f_spawn runs after the connection reaches active play, when `track` can find
# the bot. Issuing track directly on the command line is too early.
$arguments = @(
    "-basedir", $BaseDir,
    "+spectator", "1",
    "+name", $SpectatorName,
    # A tracked bot is a remote player from ezQuake's point of view. Predicting
    # its last networked usercmd makes a real direction change (most visibly the
    # opening corner) overshoot between packets and then correct, which looks like
    # a brief pause despite uninterrupted full-speed server input. Interpolate the
    # high-rate local snapshots for this diagnostic spectator instead.
        "+cl_predict_players", "0",
    "+cl_nolerp", "0",
    "+rate", "100000",
    "+alias", "f_spawn", "track $BotName",
    "+connect", "${Server}:$Port"
)
if ($Windowed) {
    $arguments = @("-window") + $arguments
}

function Quote-NativeArgument([string]$value) {
    if ($value -notmatch '[\s"]') { return $value }
    return '"' + ($value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}
$argumentLine = ($arguments | ForEach-Object { Quote-NativeArgument $_ }) -join " "

Write-Host "Launching ezQuake as '$SpectatorName'."
Write-Host "Connecting to ${Server}:$Port and tracking '$BotName'."
$process = Start-Process -FilePath $EzQuake -WorkingDirectory $BaseDir `
    -ArgumentList $argumentLine -PassThru
Write-Host "ezQuake started (PID $($process.Id))."
