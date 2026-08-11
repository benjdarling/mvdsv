param(
    [string]$Exe = "build/Release/mvdsv.exe",
    [string]$Basedir = "K:/development/EvoBot/server",
    [string]$Output = "build/evobot-exec-e1m1",
    [int]$Runs = 1,
    [double]$Timeout = 180,
    [switch]$LoadNav
)

$arguments = @(
    "tools/run_evobot_e1m1_execution_test.py",
    "--exe", $Exe,
    "--basedir", $Basedir,
    "--output", $Output,
    "--runs", $Runs,
    "--timeout", $Timeout
)
if ($LoadNav) { $arguments += "--load-nav" }
& python @arguments
exit $LASTEXITCODE
