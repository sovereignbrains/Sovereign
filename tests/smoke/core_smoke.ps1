<#
.SYNOPSIS
  Smoke tests of sovereign-core.exe over its control pipe (run by CI, runnable locally).

.DESCRIPTION
  -Mode gocore   The real sovereign-gocore.dll next to the exe: ping, box_ping,
                 box_start/box_stop, box_stats and box_logs (log callback).
                 CI runs this on the non-ASan build.
  -Mode nocore   No sovereign-gocore.dll next to the exe: the core must still
                 serve ping and answer core commands with "gocore not loaded".
                 CI runs this on the ASan build, which can't host the Go runtime:
                 ASan's CreateThread interceptor unwinds the caller's stack assuming
                 it runs on its TEB stack, Go runs on stacks it allocates itself,
                 and the process dies at random (issue #9). GoCore's C++ side is
                 covered under ASan by tests/unit/go_core_test.cpp against a stub DLL.

  Each scenario starts a fresh core. Non-empty stderr fails the run (under ASan
  it's where reports go); in nocore mode the one line saying GoCore couldn't be
  loaded is expected. On any failure the core's stdout/stderr and exit code are
  printed - an early exit used to hide them (CI only said "exit code 2").

.EXAMPLE
  pwsh tests/smoke/core_smoke.ps1 -Exe build/ci/src/service/sovereign-core.exe -Mode gocore
#>
param(
  [Parameter(Mandatory)][string]$Exe,
  [Parameter(Mandatory)][ValidateSet('gocore', 'nocore')][string]$Mode
)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
$gocoreDll = Join-Path (Split-Path $Exe) 'sovereign-gocore.dll'
if ($Mode -eq 'gocore' -and -not (Test-Path $gocoreDll)) { throw "gocore mode needs $gocoreDll" }
if ($Mode -eq 'nocore' -and (Test-Path $gocoreDll)) { throw "nocore mode needs $gocoreDll to be absent" }

$work = Join-Path ([IO.Path]::GetTempPath()) "sovereign-smoke-$PID"
New-Item -ItemType Directory -Force $work | Out-Null
$script:core = $null

function Show-CoreOutput {
  if (-not $script:core) { return }
  Write-Host '--- core stderr ---'; Get-Content (Join-Path $work 'core_err.log') -ErrorAction SilentlyContinue
  Write-Host '--- core stdout ---'; Get-Content (Join-Path $work 'core_out.log') -ErrorAction SilentlyContinue
  if ($script:core.HasExited) { Write-Host ("core exited with 0x{0:X8}" -f $script:core.ExitCode) } else { Write-Host 'core still running' }
}

function Start-Core {
  $script:core = Start-Process -FilePath $Exe -ArgumentList '--run' -PassThru -WindowStyle Hidden -WorkingDirectory $work `
    -RedirectStandardOutput (Join-Path $work 'core_out.log') -RedirectStandardError (Join-Path $work 'core_err.log')
  # Listing \\.\pipe\ doesn't touch the pipe; Test-Path on its name would open it,
  # and the core logs that as a failed request.
  for ($i = 0; $i -lt 100; $i++) {
    if ($script:core.HasExited) { throw 'sovereign-core exited before opening its control pipe' }
    if ([IO.Directory]::GetFiles('\\.\pipe\') -contains '\\.\pipe\sovereign-control') { return }
    Start-Sleep -Milliseconds 100
  }
  throw 'sovereign-core did not open its control pipe within 10 s'
}

function Send-PipeCommand([string]$json) {
  $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'sovereign-control', [System.IO.Pipes.PipeDirection]::InOut)
  try {
    $pipe.Connect(5000)
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    $pipe.Write($bytes, 0, $bytes.Length)
    $pipe.Flush()
    $buf = New-Object byte[] 65536
    $n = $pipe.Read($buf, 0, $buf.Length)
    return [Text.Encoding]::UTF8.GetString($buf, 0, $n)
  } finally { $pipe.Dispose() }
}

function Stop-Core {
  Start-Sleep -Milliseconds 300
  if ($script:core.HasExited) { throw 'sovereign-core died during the scenario' }
  Stop-Process -Id $script:core.Id -Force
  $script:core.WaitForExit(5000) | Out-Null
  $stderr = @(Get-Content (Join-Path $work 'core_err.log') -ErrorAction SilentlyContinue | Where-Object { $_ -match '\S' })
  if ($Mode -eq 'nocore') { $stderr = @($stderr | Where-Object { $_ -notmatch '^GoCore ' }) }
  if ($stderr.Count) { throw "sovereign-core stderr non-empty (possible ASan report):`n$($stderr -join "`n")" }
  $script:core = $null
}

function Scenario([string]$name, [scriptblock]$body) {
  Write-Host "== $name"
  Start-Core
  & $body
  Stop-Core
  Write-Host "   OK"
}

try {
  Scenario 'ping' {
    $r = Send-PipeCommand '{"cmd":"ping","from":"smoke"}'
    if ($r -notmatch '"cmd":"pong"') { throw "unexpected ping response: $r" }
  }

  if ($Mode -eq 'nocore') {
    Scenario 'core commands without gocore' {
      foreach ($cmd in '{"cmd":"box_ping"}', '{"cmd":"box_stats"}') {
        $r = Send-PipeCommand $cmd
        if ($r -notmatch 'gocore not loaded') { throw "expected 'gocore not loaded' for $cmd, got: $r" }
      }
    }
    return
  }

  Scenario 'box_ping' {
    $r = Send-PipeCommand '{"cmd":"box_ping"}'
    if ($r -notmatch '"cmd":"box_pong"') { throw "unexpected box_ping response: $r" }
  }

  # A minimal config (one direct outbound) exercises box_start's real path -
  # option parsing, box.New, box.Start - and box_stop's box.Close. Real traffic
  # through a proxy protocol is proven by tests/manual/*_smoke.ps1. log.disabled
  # keeps sing-box's own startup lines off stderr, which is checked above.
  Scenario 'box_start / box_stop' {
    $config = '{"log":{"disabled":true},"outbounds":[{"type":"direct","tag":"direct"}]}'
    $r = Send-PipeCommand ('{"cmd":"box_start","config":' + $config + '}')
    if ($r -notmatch '"box_started"') { throw "unexpected box_start response: $r" }
    $r = Send-PipeCommand ('{"cmd":"box_start","config":' + $config + '}')
    if ($r -notmatch 'box already started') { throw "expected an already-started error, got: $r" }
    $r = Send-PipeCommand '{"cmd":"box_stop"}'
    if ($r -notmatch '"box_stopped"') { throw "unexpected box_stop response: $r" }
  }

  # box_stats: counters from sing-box's trafficcontrol.Manager, generation +1
  # per successful start. box_logs: "sing-box started" is logged at info on every
  # start, so no traffic is needed to prove the callback path; log.output keeps
  # console logging off stderr.
  Scenario 'box_stats / box_logs' {
    $idle = Send-PipeCommand '{"cmd":"box_stats"}' | ConvertFrom-Json
    if ($idle.running -ne $false -or $idle.generation -ne 0) { throw "unexpected idle stats: $($idle | ConvertTo-Json -Compress)" }
    $config = '{"log":{"level":"info","output":"box.log"},"inbounds":[{"type":"mixed","tag":"mixed-in","listen":"127.0.0.1","listen_port":20990}],"outbounds":[{"type":"direct","tag":"direct"}]}'
    $r = Send-PipeCommand ('{"cmd":"box_start","config":' + $config + '}')
    if ($r -notmatch '"box_started"') { throw "unexpected box_start response: $r" }
    $stats = Send-PipeCommand '{"cmd":"box_stats"}' | ConvertFrom-Json
    if ($stats.running -ne $true -or $stats.generation -ne 1) { throw "unexpected running stats: $($stats | ConvertTo-Json -Compress)" }
    $logs = Send-PipeCommand '{"cmd":"box_logs","since":0}' | ConvertFrom-Json
    if (-not ($logs.entries | Where-Object { $_.message -match 'sing-box started' })) {
      throw "log callback did not deliver 'sing-box started': $($logs | ConvertTo-Json -Compress -Depth 4)"
    }
    $r = Send-PipeCommand '{"cmd":"box_stop"}'
    if ($r -notmatch '"box_stopped"') { throw "unexpected box_stop response: $r" }
    $after = Send-PipeCommand '{"cmd":"box_stats"}' | ConvertFrom-Json
    if ($after.running -ne $false -or $after.generation -ne 1) { throw "unexpected stats after stop: $($after | ConvertTo-Json -Compress)" }
  }
} catch {
  Show-CoreOutput
  if ($script:core -and -not $script:core.HasExited) { Stop-Process -Id $script:core.Id -Force -ErrorAction SilentlyContinue }
  throw
} finally {
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
