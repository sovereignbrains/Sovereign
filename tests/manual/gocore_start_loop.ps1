<#
.SYNOPSIS
  Repro loop for the flaky crash seen only in the ASan build with GoCore in-process.

.DESCRIPTION
  Starts a fresh `sovereign-core.exe --run` N times; each time: wait for the control
  pipe, box_start a minimal config (one direct outbound, log disabled - exactly the
  CI asan smoke step), box_stop, kill. A run fails when a reply is wrong, the pipe
  never opens, or stderr is non-empty (the CI step treats stderr as an ASan report).

  Numbers on the dev machine, 25.09.2026 (build/ci-asan vs build/ci, same commit):
    ASan    2 / 160 failed - both died at startup, before the pipe opened, exit
            0x80000001 (STATUS_GUARD_PAGE_VIOLATION), stderr only
            "WARNING: ASan is ignoring requested __asan_handle_no_return: stack ...
            size: 0x0053dec2d000" (a "stack" of hundreds of GB)
    no ASan 0 / 120 failed
  In CI the same step died inside box_start with the Go runtime reporting
  "unexpected return pc for runtime.gopark" - a goroutine stack overwritten.

  Failed runs keep their out-/err- logs in %TEMP%\gocore-start-loop.

  Root cause (issue #9, 26.09.2026): ASan's CreateThread interceptor unwinds the
  caller's stack with RtlCaptureStackBackTrace, which assumes the TEB stack; Go
  calls CreateThread while running on a stack it allocated itself, so _chkstk
  probes the TEB stack's guard page and the exception can't be dispatched. No
  WER dump is written - run the core under cdb (sxe gp) to catch it. CI no longer
  hosts the real DLL in the ASan job; this script stays as the reproduction.

.EXAMPLE
  pwsh tests/manual/gocore_start_loop.ps1 -N 160 -Exe build/ci-asan/src/service/sovereign-core.exe
#>
param(
  [int]$N = 40,
  [string]$Exe = "build/ci-asan/src/service/sovereign-core.exe",
  [string]$Config = '{"log":{"disabled":true},"outbounds":[{"type":"direct","tag":"direct"}]}'
)
$Exe = (Resolve-Path $Exe).Path
$work = Join-Path $env:TEMP "gocore-start-loop"
New-Item -ItemType Directory -Force $work | Out-Null

function Send-PipeCommand([string]$json) {
  $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "sovereign-control", [System.IO.Pipes.PipeDirection]::InOut)
  $pipe.Connect(5000)
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
  $pipe.Write($bytes, 0, $bytes.Length)
  $pipe.Flush()
  $buf = New-Object byte[] 4096
  $n = $pipe.Read($buf, 0, $buf.Length)
  $pipe.Dispose()
  return [System.Text.Encoding]::UTF8.GetString($buf, 0, $n)
}

$fail = 0
$sw = [Diagnostics.Stopwatch]::StartNew()
for ($i = 1; $i -le $N; $i++) {
  $err = Join-Path $work "err-$i.log"
  $core = Start-Process -FilePath $Exe -ArgumentList "--run" -PassThru -WindowStyle Hidden -WorkingDirectory $work `
    -RedirectStandardOutput (Join-Path $work "out-$i.log") -RedirectStandardError $err
  $ok = $false; $why = ""
  try {
    for ($k = 0; $k -lt 50 -and -not (Test-Path '\\.\pipe\sovereign-control'); $k++) { Start-Sleep -Milliseconds 100 }
    $r = Send-PipeCommand ('{"cmd":"box_start","config":' + $Config + '}')
    if ($r -notmatch '"box_started"') { $why = "start: [$r]" }
    else {
      $s = Send-PipeCommand '{"cmd":"box_stop"}'
      if ($s -notmatch '"box_stopped"') { $why = "stop: [$s]" } else { $ok = $true }
    }
  } catch { $why = "exception: $_" }
  Start-Sleep -Milliseconds 200
  $exited = $core.HasExited
  $code = if ($exited) { '0x{0:X8}' -f $core.ExitCode } else { "" }
  Stop-Process -Id $core.Id -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 300
  $stderr = Get-Content $err -Raw -ErrorAction SilentlyContinue
  if (-not $ok -or $stderr) {
    $fail++
    $first = if ($stderr) { ($stderr -split "`n" | Where-Object { $_ -match '\S' } | Select-Object -First 2) -join ' / ' } else { "" }
    Write-Host ("#{0} FAIL {1} exited={2} code={3} stderr: {4}" -f $i, $why, $exited, $code, $first)
  } else {
    Remove-Item (Join-Path $work "*-$i.log") -ErrorAction SilentlyContinue
  }
}
Write-Host ("{0} runs, {1} failed, {2:N0} s" -f $N, $fail, $sw.Elapsed.TotalSeconds)
