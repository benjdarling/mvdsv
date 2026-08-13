[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [string[]]$Run,
    [string]$Output = "",
    [string]$NavObj = "",
    [int]$GridSize = 256,
    [string]$Cell = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$evoBotRoot = (Resolve-Path (Join-Path $repoRoot "..")).Path
if (-not $NavObj) {
    $NavObj = Join-Path $evoBotRoot "server\evosp\evobot\nav\debug\e1m1.obj"
}
$arguments = @(
    (Join-Path $PSScriptRoot "render_evobot_run.py")
) + $Run + @("--grid-size", $GridSize.ToString())
if (Test-Path -LiteralPath $NavObj -PathType Leaf) {
    $arguments += @("--nav-obj", $NavObj)
}
if ($Output) { $arguments += @("--output", $Output) }
if ($Cell) { $arguments += @("--cell", $Cell) }
& python @arguments
exit $LASTEXITCODE
