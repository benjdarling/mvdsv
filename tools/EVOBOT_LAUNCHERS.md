# EvoBot local launchers

Double-click `run-and-spectate-evobot.bat` to start the Release MVDSV build on
e1m1, load its cached navigation, add `e1m1bot`, execute its exit route, launch
ezQuake as a spectator, and automatically track the bot in first person.
It also records the bot's telemetry and creates a labelled top-down PNG in
`build-live-fix\spectated-runs`. Pass `-NoRunRecording` to disable this.

The separate launchers are useful when MVDSV is already running:

- `run-evobot-mvdsv.bat` starts only the server and bot.
- `spectate-evobot-ezquake.bat` starts only the spectator client.

Arguments are forwarded to the PowerShell scripts. Examples:

```powershell
tools\run-and-spectate-evobot.bat -Map e2m1 -BotName routebot
tools\run-and-spectate-evobot.bat -Map dm2 -BotName dmbot -Deathmatch
tools\run-and-spectate-evobot.bat -Map e1m1 -GenerateNav -Windowed
tools\spectate-evobot-ezquake.bat -BotName e1m1bot -Port 27500
tools\render-evobot-run.bat build-live-fix\spectated-runs\e1m1-YYYYMMDD-HHMMSS.jsonl
```

Run-map cells use letters from west to east and numbers from north to south.
Each cell is 256 Quake units. Red crosses mark measured server/command hitches;
cyan ticks show sampled gaze direction. To make a close-up after identifying a
cell, run:

```powershell
tools\render-evobot-run.bat <run.jsonl> -Cell E12
```

Defaults match this checkout:

- MVDSV: newest available optimized build from `build\Release` or
  `build\RelWithDebInfo`
- ezQuake: `..\ezquake\build-msbuild-x64-vs2022\Release\ezquake.exe`
- basedir: `..\server`
- server: `127.0.0.1:27500`

Override them with `-Mvdsv`, `-EzQuake`, `-BaseDir`, and `-Port`. The combined
launcher leaves MVDSV in a separate console; close that console to stop it.
