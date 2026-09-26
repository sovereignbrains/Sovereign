<#
.SYNOPSIS
  End-to-end check of the ETW flight recorder (src/service/autologger.h) - needs admin.

.DESCRIPTION
  1. sovereign-core --install: Windows must parse the AutoLogger registry key
     (logman query autosession\Sovereign-Core) and the session must already run.
  2. The installed service (real SCM, real GoCore) serves ping, box_ping,
     box_start/box_stop, then stops.
  3. The recorder's own file, decoded: the service's lifecycle and commands are
     there, sing-box's info lines (they name domains) are not - the default
     keyword set leaves them out.
  4. sovereign-core --uninstall: session stopped, registry key gone, the .etl kept.
  An existing SovereignCore service is uninstalled first. -KeepInstalled skips 4.

.EXAMPLE
  pwsh tests/smoke/autologger_smoke.ps1 -Exe build/ci/src/service/sovereign-core.exe
#>
param([Parameter(Mandatory)][string]$Exe, [switch]$KeepInstalled)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sovtrace = Join-Path $repo 'tools/trace/sovtrace.ps1'
$failures = [System.Collections.Generic.List[string]]::new()
function Expect([bool]$ok, [string]$what) { if ($ok) { Write-Host "  PASS $what" } else { Write-Host "  FAIL $what"; $failures.Add($what) } }
function Core([string]$arg) {
  $out = & $Exe $arg 2>&1
  if ($LASTEXITCODE -ne 0) { throw "sovereign-core $arg failed ($LASTEXITCODE): $out" }
  $out | ForEach-Object { Write-Host "   $_" }
}
function Send-PipeCommand([string]$json) {
  $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'sovereign-control', [System.IO.Pipes.PipeDirection]::InOut)
  try {
    $pipe.Connect(5000)
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    $pipe.Write($bytes, 0, $bytes.Length); $pipe.Flush()
    $buf = New-Object byte[] 65536
    $n = $pipe.Read($buf, 0, $buf.Length)
    return [Text.Encoding]::UTF8.GetString($buf, 0, $n)
  } finally { $pipe.Dispose() }
}

if (Get-Service SovereignCore -ErrorAction SilentlyContinue) {
  Write-Host '== removing the existing SovereignCore service'
  Stop-Service SovereignCore -Force -ErrorAction SilentlyContinue
  Core '--uninstall'
}

Write-Host '== --install'
$started = Get-Date
Core '--install'
$auto = logman query 'autosession\Sovereign-Core' 2>&1 | Out-String
Expect ($LASTEXITCODE -eq 0 -and $auto -match '356e995a-3c2d-5ae3-fad1-410ee24d609d') 'Windows parses the AutoLogger key (provider enabled in it)'
$live = logman query 'Sovereign-Core' -ets 2>&1 | Out-String
Expect ($LASTEXITCODE -eq 0 -and $live -match 'sovereign\.etl') 'the session runs right after install, writing sovereign.etl'

Write-Host '== the installed service'
Start-Service SovereignCore
for ($i = 0; $i -lt 100 -and -not ([IO.Directory]::GetFiles('\\.\pipe\') -contains '\\.\pipe\sovereign-control'); $i++) { Start-Sleep -Milliseconds 100 }
$r = Send-PipeCommand '{"cmd":"ping"}'; Expect ($r -match '"pong"') 'ping'
$r = Send-PipeCommand '{"cmd":"box_ping"}'; Expect ($r -match '"box_pong"') 'box_ping'
$r = Send-PipeCommand '{"cmd":"box_start","config":{"log":{"level":"info"},"outbounds":[{"type":"direct","tag":"direct"}]}}'
Expect ($r -match '"box_started"') 'box_start'
$r = Send-PipeCommand '{"cmd":"box_logs","since":0}'
Expect ($r -match 'sing-box started') 'sing-box info lines reach the tray ring (box_logs)'
$r = Send-PipeCommand '{"cmd":"box_stop"}'; Expect ($r -match '"box_stopped"') 'box_stop'
Stop-Service SovereignCore

Write-Host '== the recorder file'
& $sovtrace flush
$events = @(& $sovtrace dump -AsObject | Where-Object { $_.Time -ge $started.AddSeconds(-2) })
Write-Host "   $($events.Count) events since install"
$states = ($events | Where-Object { $_.Event -eq 'ProcessState' -and $_.Fields.Mode -eq 'service' } | ForEach-Object { $_.Fields.State }) -join ','
Expect ($states -eq 'starting,running,stopping,stopped') "service lifecycle recorded ($states)"
$commands = @($events | Where-Object Event -eq 'Command' | ForEach-Object { $_.Fields.Cmd })
foreach ($cmd in 'echo', 'box_ping', 'box_start', 'box_logs', 'box_stop') { Expect ($commands -contains $cmd) "Command $cmd recorded" }
$verbose = @($events | Where-Object { $_.Event -eq 'CoreLog' -and $_.Fields.Level -in 'info', 'debug', 'trace' })
Expect ($verbose.Count -eq 0) "no sing-box info/debug lines in the recorder ($($verbose.Count))"

if (-not $KeepInstalled) {
  Write-Host '== --uninstall'
  Core '--uninstall'
  logman query 'Sovereign-Core' -ets *> $null
  Expect ($LASTEXITCODE -ne 0) 'session stopped'
  logman query 'autosession\Sovereign-Core' *> $null
  Expect ($LASTEXITCODE -ne 0) 'AutoLogger key removed'
  Expect (-not (Get-Service SovereignCore -ErrorAction SilentlyContinue)) 'service removed'
  Expect ([bool](Get-ChildItem (Join-Path $env:ProgramData 'Sovereign\Logs') -Filter 'sovereign.etl*')) 'the recorded .etl is kept'
}

if ($failures.Count) {
  Write-Host '--- decoded recorder file ---'
  & $sovtrace dump
  throw "autologger smoke failed: $($failures -join '; ')"
}
