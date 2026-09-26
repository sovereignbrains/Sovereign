<#
.SYNOPSIS
  End-to-end check of the service's ETW events (src/service/trace.h) - needs admin.

.DESCRIPTION
  Records a real ETW session (all keywords, verbose) around tests/smoke/core_smoke.ps1
  (gocore mode, the real DLL) plus one box_start whose config carries a password
  marker, decodes it with tools/trace/sovtrace.ps1 and checks:
    - lifecycle, core load and a Command event for every command the smoke sends;
    - the rejected second box_start as CommandFailed with the core's error;
    - sing-box's own lines as CoreLog ("sing-box started");
    - no WIL Failure events on this happy path;
    - the password marker in no field of any event (configs never reach ETW).

.EXAMPLE
  pwsh tests/smoke/trace_smoke.ps1 -Exe build/ci/src/service/sovereign-core.exe
#>
param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sovtrace = Join-Path $repo 'tools/trace/sovtrace.ps1'
$work = Join-Path ([IO.Path]::GetTempPath()) "sovereign-trace-smoke-$PID"
New-Item -ItemType Directory -Force $work | Out-Null
$etl = Join-Path $work 'trace.etl'
$session = "Sovereign-TraceSmoke-$PID"
$marker = "pw-marker-$([guid]::NewGuid().ToString('N'))"

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

& $sovtrace start -Name $session -Path $etl -Keywords all -Level 5
try {
  & (Join-Path $PSScriptRoot 'core_smoke.ps1') -Exe $Exe -Mode gocore

  # A config with a secret in it. A shadowsocks outbound is only dialed on
  # traffic, so the box starts without any server behind it.
  Write-Host '== box_start with a password in the config'
  $core = Start-Process -FilePath $Exe -ArgumentList '--run' -PassThru -WindowStyle Hidden -WorkingDirectory $work `
    -RedirectStandardOutput (Join-Path $work 'out.log') -RedirectStandardError (Join-Path $work 'err.log')
  try {
    for ($i = 0; $i -lt 100 -and -not ([IO.Directory]::GetFiles('\\.\pipe\') -contains '\\.\pipe\sovereign-control'); $i++) {
      if ($core.HasExited) { throw 'core exited before opening its pipe' }
      Start-Sleep -Milliseconds 100
    }
    $config = '{"log":{"level":"debug","output":"box.log"},"outbounds":[{"type":"direct","tag":"direct"},' +
              '{"type":"shadowsocks","tag":"ss","server":"127.0.0.1","server_port":9,"method":"aes-128-gcm","password":"' + $marker + '"}]}'
    $r = Send-PipeCommand ('{"cmd":"box_start","config":' + $config + '}')
    if ($r -notmatch '"box_started"') { throw "box_start with a password failed: $r" }
    $r = Send-PipeCommand '{"cmd":"box_stop"}'
    if ($r -notmatch '"box_stopped"') { throw "box_stop failed: $r" }
    Write-Host '   OK'
  } finally {
    Stop-Process -Id $core.Id -Force -ErrorAction SilentlyContinue
  }
} finally {
  & $sovtrace stop -Name $session
}

$events = @(& $sovtrace dump -Path $etl -AsObject)
$failures = [System.Collections.Generic.List[string]]::new()
function Expect([bool]$ok, [string]$what) { if ($ok) { Write-Host "  PASS $what" } else { Write-Host "  FAIL $what"; $failures.Add($what) } }

Write-Host "== $($events.Count) events"
$commands = $events | Where-Object Event -eq 'Command' | ForEach-Object { $_.Fields.Cmd }
Expect ([bool]($events | Where-Object { $_.Event -eq 'ProcessState' -and $_.Fields.State -eq 'running' })) 'ProcessState running'
Expect ([bool]($events | Where-Object Event -eq 'CoreLoaded')) 'CoreLoaded'
foreach ($cmd in 'echo', 'box_ping', 'box_start', 'box_stop', 'box_stats', 'box_logs') {
  Expect ($commands -contains $cmd) "Command $cmd"
}
Expect ([bool]($events | Where-Object { $_.Event -eq 'CommandFailed' -and $_.Fields.Cmd -eq 'box_start' -and $_.Fields.Error -match 'already started' })) 'CommandFailed for the second box_start'
Expect ([bool]($events | Where-Object { $_.Event -eq 'CoreLog' -and $_.Fields.Message -match 'sing-box started' })) 'CoreLog carries sing-box lines'
$wilFailures = @($events | Where-Object Event -eq 'Failure')
Expect ($wilFailures.Count -eq 0) "no WIL failures on the happy path ($($wilFailures.Count))"
$leaks = @($events | Where-Object { $_.Fields.Values | Where-Object { "$_" -match [regex]::Escape($marker) } })
Expect ($leaks.Count -eq 0) "the config's password is in no event ($($leaks.Count) events carry it)"

if ($failures.Count) {
  Write-Host '--- decoded trace ---'
  & $sovtrace dump -Path $etl
  throw "trace smoke failed: $($failures -join '; ')"
}
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
