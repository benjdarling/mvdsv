[CmdletBinding()]
param(
    [string]$Map = "e1m1",
    [string]$BotName = "e1m1bot",
    [int]$Port = 27500,
    [string]$RconPassword = "evobotlocal",
    [string]$Mvdsv = "",
    [string]$BaseDir = "",
    [string]$Game = "evosp",
    [string]$ReadyToken = "",
    [string]$WaitForSpectator = "",
    [string]$FailureFile = "",
    [switch]$Deathmatch,
    [switch]$GenerateNav
)

$ErrorActionPreference = "Stop"

# The combined launcher starts this script in a separate console.  Preserve the
# actual child failure so its parent can report more than an otherwise opaque
# exit code 1 after that console closes.
trap {
    $failureText = $_.Exception.Message
    if ($_.InvocationInfo -and $_.InvocationInfo.PositionMessage) {
        $failureText += "`r`n" + $_.InvocationInfo.PositionMessage
    }
    if ($FailureFile) {
        try {
            $failurePath = [IO.Path]::GetFullPath($FailureFile)
            $failureParent = [IO.Path]::GetDirectoryName($failurePath)
            if ($failureParent) {
                [IO.Directory]::CreateDirectory($failureParent) | Out-Null
            }
            [IO.File]::WriteAllText($failurePath, $failureText)
        }
        catch {
            Write-Warning "Could not write launcher failure details to '$FailureFile'."
        }
    }
    Write-Error $failureText
    exit 1
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$evoBotRoot = (Resolve-Path (Join-Path $repoRoot "..")).Path

if (-not $Mvdsv) {
    [array]$buildCandidates = @(
        Join-Path $repoRoot "build\Release\mvdsv.exe"
        Join-Path $repoRoot "build\RelWithDebInfo\mvdsv.exe"
        # A second build tree permits compiling while the normal executable is
        # locked by a live observation session. Newest-build selection below
        # makes the next launcher restart pick up that binary automatically.
        Join-Path $repoRoot "build-live-fix\Release\mvdsv.exe"
        Join-Path $repoRoot "build-gaze-output-next\Release\mvdsv.exe"
        Join-Path $repoRoot "build-gaze-output\Release\mvdsv.exe"
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Sort-Object { (Get-Item -LiteralPath $_).LastWriteTimeUtc } -Descending
    if ($buildCandidates.Count) {
        $Mvdsv = $buildCandidates[0]
    }
    else {
        $Mvdsv = Join-Path $repoRoot "build\Release\mvdsv.exe"
    }
}
if (-not $BaseDir) {
    $BaseDir = Join-Path $evoBotRoot "server"
}

$Mvdsv = [IO.Path]::GetFullPath($Mvdsv)
$BaseDir = [IO.Path]::GetFullPath($BaseDir)
if (-not (Test-Path -LiteralPath $Mvdsv -PathType Leaf)) {
    throw "MVDSV was not found at '$Mvdsv'. Build Release or pass -Mvdsv <path>."
}
if (-not (Test-Path -LiteralPath $BaseDir -PathType Container)) {
    throw "Quake basedir was not found at '$BaseDir'. Pass -BaseDir <path>."
}
if ($RconPassword -notmatch '^[A-Za-z0-9_]+$') {
    throw "-RconPassword must contain only letters, digits, or underscores because MVDSV parses punctuation as command-line syntax."
}
if ($ReadyToken -and $ReadyToken -notmatch '^[A-Za-z0-9_]+$') {
    throw "-ReadyToken must contain only letters, digits, or underscores."
}
if ($WaitForSpectator -and $WaitForSpectator -notmatch '^[A-Za-z0-9_]+$') {
    throw "-WaitForSpectator must contain only letters, digits, or underscores."
}

$navCommand = if ($GenerateNav) { "evobot_nav_generate" } else { "evobot_nav_load" }
$deathmatchValue = if ($Deathmatch) { "1" } else { "0" }
$coopValue = if ($Deathmatch) { "0" } else { "1" }

function Quote-NativeArgument([string]$value) {
    if ($value -notmatch '[\s"]') { return $value }
    return '"' + ($value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

function Invoke-MvdsvRcon([string]$command, [int]$timeoutMilliseconds = 3000) {
    $request = [byte[]](255, 255, 255, 255) +
        [Text.Encoding]::ASCII.GetBytes("rcon $RconPassword $command") + [byte]0
    $chunks = New-Object Collections.Generic.List[string]
    $udp = New-Object Net.Sockets.UdpClient
    try {
        $udp.Client.ReceiveTimeout = $timeoutMilliseconds
        $udp.Connect("127.0.0.1", $Port)
        [void]$udp.Send($request, $request.Length)
        while ($true) {
            try {
                $remote = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
                $reply = $udp.Receive([ref]$remote)
                $offset = if ($reply.Length -ge 4 -and
                    $reply[0] -eq 255 -and $reply[1] -eq 255 -and
                    $reply[2] -eq 255 -and $reply[3] -eq 255) { 4 } else { 0 }
                if ($offset -lt $reply.Length -and
                    ($reply[$offset] -eq [byte][char]'n' -or
                     $reply[$offset] -eq [byte][char]'l')) { $offset++ }
                $chunks.Add([Text.Encoding]::GetEncoding(28591).GetString(
                    $reply, $offset, $reply.Length - $offset).TrimEnd([char]0))
                $udp.Client.ReceiveTimeout = 200
            }
            catch [Net.Sockets.SocketException] {
                if ($chunks.Count) { break }
                throw
            }
        }
    }
    finally {
        $udp.Dispose()
    }
    return ($chunks -join "").Replace("`r", "")
}

function Get-MvdsvStatus([int]$timeoutMilliseconds = 500, [int]$options = 0) {
    $statusCommand = if ($options) { "status $options`n" } else { "status`n" }
    $request = [byte[]](255, 255, 255, 255) +
        [Text.Encoding]::ASCII.GetBytes($statusCommand)
    $udp = New-Object Net.Sockets.UdpClient
    try {
        $udp.Client.ReceiveTimeout = $timeoutMilliseconds
        $udp.Connect("127.0.0.1", $Port)
        [void]$udp.Send($request, $request.Length)
        $remote = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
        $reply = $udp.Receive([ref]$remote)
        $offset = if ($reply.Length -ge 4 -and
            $reply[0] -eq 255 -and $reply[1] -eq 255 -and
            $reply[2] -eq 255 -and $reply[3] -eq 255) { 4 } else { 0 }
        if ($offset -lt $reply.Length -and
            ($reply[$offset] -eq [byte][char]'n' -or
             $reply[$offset] -eq [byte][char]'l')) { $offset++ }
        return [Text.Encoding]::GetEncoding(28591).GetString(
            $reply, $offset, $reply.Length - $offset).TrimEnd([char]0).Replace("`r", "")
    }
    finally {
        $udp.Dispose()
    }
}

function Wait-MvdsvReady([Diagnostics.Process]$process, [string]$startupToken) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    $badPasswordSince = $null
    $unidentifiedServerSince = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        $process.Refresh()
        if ($process.HasExited) {
            throw "MVDSV exited during startup with code $($process.ExitCode). UDP port $Port may already be in use."
        }
        try {
            $publicStatus = Get-MvdsvStatus 500
            if ($publicStatus -notmatch [regex]::Escape($startupToken)) {
                # MVDSV binds its UDP socket before it has necessarily consumed
                # every +set argument. A status packet in that small window can
                # contain the default hostname even though this is the process we
                # just started. Require a sustained mismatch before diagnosing a
                # foreign server; the pre-launch probe already found the port free.
                if (-not $unidentifiedServerSince) {
                    $unidentifiedServerSince = [DateTime]::UtcNow
                }
                if (([DateTime]::UtcNow -
                    $unidentifiedServerSince).TotalSeconds -ge 5) {
                    throw "UDP port $Port answered for five seconds without this launcher's identity. Close the other QuakeWorld server or pass another -Port value."
                }
                Start-Sleep -Milliseconds 100
                continue
            }
            $unidentifiedServerSince = $null
            $rconStatus = Invoke-MvdsvRcon "status" 500
            if ($rconStatus -match "Bad rcon_password") {
                if (-not $badPasswordSince) { $badPasswordSince = [DateTime]::UtcNow }
                if (([DateTime]::UtcNow - $badPasswordSince).TotalSeconds -ge 5) {
                    throw "The MVDSV instance started by this launcher rejected its configured RCON password for five seconds."
                }
                Start-Sleep -Milliseconds 100
                continue
            }
            return
        }
        catch [Net.Sockets.SocketException] {
            Start-Sleep -Milliseconds 150
        }
    }
    throw "MVDSV did not answer RCON on 127.0.0.1:$Port within 30 seconds."
}

try {
    $existingStatus = Get-MvdsvStatus 250
    if ($existingStatus) {
        throw "UDP port $Port is already serving a QuakeWorld server. Close it or pass another -Port value."
    }
}
catch [Net.Sockets.SocketException] {
    # No server answered, so the requested port is available for this launch.
}

# MVDSV treats '-' as a command-line switch delimiter even inside a +set value.
$startupToken = "evobot${PID}${Port}$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
if (-not $ReadyToken) {
    $ReadyToken = "evobotready${PID}${Port}$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
}

$arguments = @(
    "-d",
    "-noerrormsgbox",
    "-basedir", $BaseDir,
    "-game", $Game,
    "-progtype", "0",
    "-port", $Port.ToString(),
    "+set", "maxclients", "8",
    "+set", "maxspectators", "8",
    "+set", "sv_progsname", "spprogs",
    "+set", "sv_cheats", "1",
    "+set", "deathmatch", $deathmatchValue,
    "+set", "coop", $coopValue,
    "+set", "rcon_password", $RconPassword,
    "+set", "sv_crypt_rcon", "0",
    # The local telemetry recorder polls RCON while the launcher also performs
    # readiness/start commands. MVDSV validates each normal request against the
    # master password first, so its global limiter counts every request twice.
    # Keep abuse protection enabled, but leave ample room for this localhost-only
    # test workflow instead of intermittently rejecting a correct password.
    "+set", "sv_rconlim", "100",
    # Publish the identity only after the RCON settings have been processed.
    "+set", "hostname", $startupToken,
    "+map", $Map
)

Write-Host "Starting MVDSV on 127.0.0.1:$Port"
Write-Host "Executable: $Mvdsv"
Write-Host "Map: $Map  Bot: $BotName  Mode: $(if ($Deathmatch) { 'deathmatch' } else { 'single-player/coop' })"
Write-Host "Navigation: $(if ($GenerateNav) { 'generate' } else { 'load cached .botnav' })"
Write-Host "Close this window or press Ctrl+C to stop the server."
Write-Host ""

$argumentLine = ($arguments | ForEach-Object { Quote-NativeArgument $_ }) -join " "
$process = $null
$ready = $false
try {
    $process = Start-Process -FilePath $Mvdsv -WorkingDirectory $BaseDir `
        -ArgumentList $argumentLine -NoNewWindow -PassThru
    Wait-MvdsvReady $process $startupToken

    Write-Host "Loading navigation..."
    $navOutput = Invoke-MvdsvRcon "$navCommand $Map" 180000
    Write-Host $navOutput.Trim()
    if ($navOutput -match "(?i)failed|no navigation") {
        throw "Navigation setup failed. Use -GenerateNav if no cached .botnav exists."
    }

    Write-Host "Validating navigation..."
    $validationOutput = Invoke-MvdsvRcon "evobot_nav_reach_validate" 30000
    Write-Host $validationOutput.Trim()
    if ($validationOutput -match "(?i)failed" -or
        $validationOutput -notmatch "(?i)validation: ok") {
        throw "Navigation reachability validation failed."
    }
    if ($GenerateNav -or $navOutput -match "(?i)normalized") {
        $saveOutput = Invoke-MvdsvRcon "evobot_nav_save" 30000
        Write-Host $saveOutput.Trim()
        if ($saveOutput -notmatch "(?i)saved:") {
            throw "Navigation save failed."
        }
    }

    $addOutput = Invoke-MvdsvRcon "evobot_add $BotName"
    Write-Host $addOutput.Trim()
    if ($addOutput -notmatch "(?i)EvoBot added") {
        throw "EvoBot creation failed."
    }

    Start-Sleep -Milliseconds 250
    Write-Host "Selecting the map exit as the bot goal..."
    $routeOutput = ""
    for ($attempt = 0; $attempt -lt 3; $attempt++) {
        $routeOutput = Invoke-MvdsvRcon "evobot_nav_route_exit" 10000
        if ($routeOutput -match "(?i)result: (reachable|conditional|blocked)") {
            break
        }
        Start-Sleep -Milliseconds 250
    }
    Write-Host $routeOutput.Trim()
    if ($routeOutput -notmatch "(?i)result: (reachable|conditional|blocked)") {
        # This command emits a large diagnostic dump over connectionless UDP.
        # Missing/truncated text does not mean route selection failed; prepare is
        # the authoritative operation and can expand a blocked physical route
        # through its button/mover dependency plan.
        Write-Warning "The exit-route diagnostic reply was incomplete; continuing to executable plan preparation."
    }

    Write-Host "Planning the route to the exit..."
    $planOutput = ""
    for ($attempt = 0; $attempt -lt 3; $attempt++) {
        $planOutput = Invoke-MvdsvRcon "evobot_nav_plan_exit" 180000
        if ($planOutput -match "(?i)EvoBot plan status" -or
            $planOutput -match "(?i)result: (none|unreachable)") {
            break
        }
        Start-Sleep -Milliseconds 250
    }
    Write-Host $planOutput.Trim()
    if ($planOutput -match "(?i)result: (none|unreachable)") {
        throw "No executable exit plan could be created for the bot."
    }
    if ($planOutput -notmatch "(?i)EvoBot plan status") {
        Write-Warning "The exit-plan diagnostic reply was incomplete; executable preparation will verify the plan."
    }

    # Expand the dependency plan and build the opening physical route before a
    # spectator connects. This work scans the navigation graph and must not run
    # inside the first live server frame.
    Write-Host "Preparing executable route..."
    $prepareOutput = Invoke-MvdsvRcon "evobot_exec_prepare $BotName" 180000
    Write-Host $prepareOutput.Trim()
    if ($prepareOutput -notmatch "(?i)execution prepared") {
        throw "EvoBot execution could not be prepared."
    }

    # Publish readiness after the map, bot and plan exist but before execution.
    # The combined launcher can now connect ezQuake without losing the opening
    # section of the run.  Direct server-only launches leave WaitForSpectator empty
    # and start immediately as before.
    [void](Invoke-MvdsvRcon "hostname $ReadyToken" 3000)
    if ($WaitForSpectator) {
        Write-Host "Waiting for spectator '$WaitForSpectator' before starting execution..."
        $spectatorDeadline = [DateTime]::UtcNow.AddSeconds(60)
        $spectatorConnected = $false
        while ([DateTime]::UtcNow -lt $spectatorDeadline) {
            try {
                # MVDSV's legacy public `status` reply lists players but omits
                # spectators.  Bit 0 requests server info and bit 2 requests
                # spectators, so status 5 includes the hostname readiness token
                # and the EvoBotCam entry we are waiting for.
                $status = Get-MvdsvStatus 500 5
                $spectatorMarker = "\s\$WaitForSpectator"
                if ($status -match [regex]::Escape($spectatorMarker)) {
                    # Public status includes clients from cs_preconnected onward.
                    # The RCON status output marks those clients CONNECTING, so do
                    # not release the bot until ezQuake has fully spawned in-game.
                    $serverStatus = Invoke-MvdsvRcon "status" 1000
                    if ($serverStatus -notmatch '(?m)^CONNECTING\s*$') {
                        $spectatorConnected = $true
                        break
                    }
                }
            }
            catch [Net.Sockets.SocketException] {
                # Server is still starting a network frame; retry below.
            }
            Start-Sleep -Milliseconds 100
        }
        if (-not $spectatorConnected) {
            throw "Spectator '$WaitForSpectator' did not connect within 60 seconds."
        }
        Write-Host "Spectator is fully connected; starting EvoBot execution."
    }

    $startOutput = ""
    $executionStarted = $false
    # Connectionless UDP can lose a reply, and older/default MVDSV RCON limits
    # can reject a request while the spectator and recorder finish connecting.
    # Starting an already-running prepared executor is idempotent, so retry the
    # actual start command as well as independently checking its elapsed clock.
    for ($attempt = 0; $attempt -lt 20 -and -not $executionStarted; $attempt++) {
        try {
            $attemptOutput = Invoke-MvdsvRcon "evobot_exec_start $BotName" 1500
            if ($attemptOutput) {
                $startOutput = $attemptOutput
                if ($attempt -eq 0 -or $attemptOutput -match "(?i)execution started") {
                    Write-Host $attemptOutput.Trim()
                }
            }
            $executionStarted = $attemptOutput -match "(?i)execution started"
        }
        catch [Net.Sockets.SocketException] {
            # Confirm status below, then retry after a bounded delay.
        }
        if (-not $executionStarted) {
            Start-Sleep -Milliseconds 150
            try {
                $executionStatus = Invoke-MvdsvRcon `
                    "evobot_exec_status $BotName" 1000
                if ($executionStatus -match
                    '"total_elapsed":([0-9]+(?:\.[0-9]+)?)' -and
                    [double]$Matches[1] -gt 0) {
                    $executionStarted = $true
                    Write-Host "EvoBot execution confirmed by live status after its RCON acknowledgement was missed."
                }
            }
            catch [Net.Sockets.SocketException] {
                # Retry the start/status pair within the bounded window.
            }
        }
    }
    if (-not $executionStarted) {
        $startDetail = if ($startOutput) { $startOutput.Trim() } else {
            "<no RCON response>"
        }
        throw "EvoBot execution did not start. evobot_exec_start response: $startDetail"
    }

    Write-Host ""
    Write-Host "READY: connect ezQuake to 127.0.0.1:$Port and track $BotName"
    $ready = $true
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "MVDSV exited with code $($process.ExitCode)."
    }
}
finally {
    if ($process) {
        if (-not $ready -and -not $process.HasExited) {
            $process.Kill()
            $process.WaitForExit()
        }
        $process.Dispose()
    }
}
