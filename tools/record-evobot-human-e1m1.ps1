param(
    [string]$Exe = "build/Release/mvdsv.exe",
    [string]$Basedir = "K:/development/EvoBot/server",
    [string]$EzQuake = "K:/development/EvoBot/ezquake/build-msbuild-x64-vs2022/Release/ezquake.exe",
    [string]$Output = "build/evobot-human-e1m1",
    [string]$PlayerName = "evobot-human",
    [string]$Label = "",
    [int]$Port = 27720,
    [double]$Timeout = 600,
    [switch]$NoLaunchClient
)

$arguments = @(
    "tools/record_evobot_human_e1m1.py",
    "--exe", $Exe,
    "--basedir", $Basedir,
    "--ezquake", $EzQuake,
    "--output", $Output,
    "--player-name", $PlayerName,
    "--port", $Port,
    "--timeout", $Timeout
)
if ($Label) { $arguments += @("--label", $Label) }
if (-not $NoLaunchClient) { $arguments += "--launch-client" }
& python @arguments
exit $LASTEXITCODE
