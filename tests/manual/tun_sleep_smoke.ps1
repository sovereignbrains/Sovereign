<#
.SYNOPSIS
  Manual/scripted proof of P2 AC4/AC5: a TUN box started by the SovereignCore
  service survives a real sleep/wake - adapter and default route are back and
  real traffic flows again through AnyTLS+REALITY.

.DESCRIPTION
  Power events reach only a real service (SERVICE_CONTROL_POWEREVENT from the
  SCM); `sovereign-core.exe --run` never sees them. So this drives the
  installed SovereignCore service over its control pipe, and must run elevated
  (it stops/starts services and puts the machine to sleep).

    1. stop the production sing-box-daemon (its TUN would fight ours for the
       default route - see 1c notes); the finally block always restarts it
    2. start SovereignCore, box_start: tun (10.250.0.1/30, auto_route,
       stack system) -> AnyTLS+REALITY outbound from -LiveOutbound
    3. before sleep: default route through our TUN, exit IP = the proxy's
    4. arm a wake timer (SetWaitableTimer, fResume) and SetSuspendState
    5. after wake: poll until traffic flows again through the TUN (AC5),
       check adapter + default route (AC4) and the service's ETW power events,
       recorded by this script's own session (suspend -> resume -> restart_stop
       -> restart_start, none failed)
    6. box_stop, stop SovereignCore, restart sing-box-daemon, check the
       machine's own internet is back

  Everything goes to a transcript under C:\ProgramData\Sovereign\ - an elevated
  window is not visible to whoever launched it, the log is read afterwards.
  The outbound file holds credentials: read at run time, never committed.

.EXAMPLE
  Start-Process pwsh -Verb RunAs -ArgumentList '-NoProfile','-File',
    'tests\manual\tun_sleep_smoke.ps1','-LiveOutbound',"$env:TEMP\outbound.json"
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$LiveOutbound,
  [int]$WakeAfterSeconds = 120,
  [int]$ResumeTimeoutSeconds = 180,
  [string]$ProdService = "sing-box-daemon",
  # Start the production client at the end even if it was not running at the start
  # (recovering from an aborted run that left it stopped).
  [switch]$StartProdAfter
)

$ErrorActionPreference = "Stop"
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logDir = "C:\ProgramData\Sovereign"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$transcript = Join-Path $logDir "sleep-test-$stamp.log"
$resultFile = Join-Path $logDir "sleep-test-$stamp.result.json"
$sovtrace = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "tools\trace\sovtrace.ps1"
$traceSession = "Sovereign-SleepTest"
$traceFile = Join-Path $logDir "sleep-test-$stamp.etl"
$traceStarted = $false
Start-Transcript -Path $transcript | Out-Null
$result = [ordered]@{ started = (Get-Date).ToString("s"); checks = [ordered]@{} }
function Step([string]$m) { Write-Host ("[{0:HH:mm:ss}] {1}" -f (Get-Date), $m) }
function Check([string]$name, [bool]$ok, [string]$detail) {
  $result.checks[$name] = [ordered]@{ ok = $ok; detail = $detail }
  Write-Host ("  {0} {1}: {2}" -f $(if ($ok) { "PASS" } else { "FAIL" }), $name, $detail)
}

function Send-PipeCommand([string]$json) {
  $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "sovereign-control", [System.IO.Pipes.PipeDirection]::InOut)
  $pipe.Connect(5000)
  $pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
  $pipe.Write($bytes, 0, $bytes.Length)
  $pipe.Flush()
  $buffer = New-Object System.IO.MemoryStream
  $chunk = New-Object byte[] 4096
  do {
    $n = $pipe.Read($chunk, 0, $chunk.Length)
    $buffer.Write($chunk, 0, $n)
  } while ($n -gt 0 -and -not $pipe.IsMessageComplete)
  $pipe.Dispose()
  return [System.Text.Encoding]::UTF8.GetString($buffer.ToArray())
}

$tunAddress = "10.250.0.1/30"   # 172.19.0.1/30 is the prod sing-box's tun0
function Get-TunState {
  $ip = Get-NetIPAddress -IPAddress "10.250.0.1" -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $ip) { return @{ Up = $false; Alias = ""; DefaultVia = $false } }
  $adapter = Get-NetAdapter -InterfaceIndex $ip.InterfaceIndex -ErrorAction SilentlyContinue
  $best = Find-NetRoute -RemoteIPAddress 1.1.1.1 -ErrorAction SilentlyContinue | Select-Object -First 1
  return @{
    Up = ($adapter -and $adapter.Status -eq "Up"); Alias = $ip.InterfaceAlias
    DefaultVia = ($best -and $best.InterfaceIndex -eq $ip.InterfaceIndex)
  }
}
function Get-ExitIp([int]$timeout = 8) {
  $ip = & curl.exe -s -m $timeout --noproxy "*" https://api.ipify.org 2>$null
  if ($LASTEXITCODE -eq 0 -and $ip -match '^\d{1,3}(\.\d{1,3}){3}$') { return $ip } else { return $null }
}

Add-Type -Namespace Sov -Name Power -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true)]
public static extern System.IntPtr CreateWaitableTimer(System.IntPtr attrs, bool manualReset, string name);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool SetWaitableTimer(System.IntPtr timer, ref long dueTime, int period, System.IntPtr completion, System.IntPtr arg, bool resume);
[DllImport("kernel32.dll")]
public static extern uint SetThreadExecutionState(uint flags);
[DllImport("powrprof.dll", SetLastError = true)]
public static extern bool SetSuspendState(bool hibernate, bool forceCritical, bool disableWakeEvent);
'@

$prodWasRunning = [bool]$StartProdAfter
try {
  $outbound = Get-Content $LiveOutbound -Raw | ConvertFrom-Json -AsHashtable
  $outbound.tag = "proxy"
  # The exit IP is the proxy server's own address. A subscription node names it by
  # domain, and plain DNS on this line is spoofed (25.09.2026: 1.1.1.1 and 8.8.8.8
  # over UDP answered packetlab.tech with a foreign 198.20.0.35) - so resolve over
  # DoH and put the IP into the outbound; REALITY's SNI is separate and stays.
  $expectedExit = $outbound.server
  if ($expectedExit -notmatch '^\d{1,3}(\.\d{1,3}){3}$') {
    $doh = & curl.exe -s -m 10 -H "accept: application/dns-json" "https://cloudflare-dns.com/dns-query?name=$($outbound.server)&type=A" | ConvertFrom-Json
    $expectedExit = ($doh.Answer | Where-Object { $_.type -eq 1 } | Select-Object -First 1).data
    if (-not $expectedExit) { throw "could not resolve $($outbound.server) over DoH" }
    $outbound.server = $expectedExit
  }
  Step "proxy -> $expectedExit (expected exit IP)"
  $svc = Get-CimInstance Win32_Service -Filter "Name='SovereignCore'"
  if (-not $svc) { throw "SovereignCore service is not installed (sovereign-core.exe --install)" }
  Step "SovereignCore -> $($svc.PathName)"

  # 1. Production client out of the way.
  $prod = Get-Service $ProdService -ErrorAction SilentlyContinue
  if ($prod -and $prod.Status -eq "Running") {
    $prodWasRunning = $true
    Step "stopping $ProdService (internet via it is down from here until the finally block)"
    Stop-Service $ProdService -Force
    Start-Sleep 2
  }

  # 2. Our box. The service's power events are ETW (src/service/trace.h): record
  # them into this run's own file, decoded after the wake.
  & $sovtrace start -Name $traceSession -Path $traceFile -Keywords default
  $traceStarted = $true
  if ((Get-Service SovereignCore).Status -ne "Running") { Start-Service SovereignCore; Start-Sleep 1 }
  else { Send-PipeCommand '{"cmd":"box_stop"}' | Out-Null }   # a box left over from an aborted run
  $config = @{
    log       = @{ level = "debug"; timestamp = $true; output = (Join-Path $logDir "sleep-test-$stamp.box.log") -replace '\\', '/' }
    dns       = @{
      servers = @(
        @{ type = "tls"; tag = "remote"; server = "1.1.1.1"; detour = "proxy" },
        @{ type = "local"; tag = "local" }
      )
      final = "remote"; strategy = "ipv4_only"
    }
    inbounds  = @(@{ type = "tun"; tag = "tun-in"; interface_name = "sovereign-tun"; address = @($tunAddress); auto_route = $true; stack = "system" })
    outbounds = @($outbound, @{ type = "direct"; tag = "direct" })
    route     = @{
      rules = @(@{ port = 53; action = "hijack-dns" }, @{ ip_is_private = $true; outbound = "direct" })
      final = "proxy"; auto_detect_interface = $true; default_domain_resolver = @{ server = "local" }
    }
  }
  $start = Send-PipeCommand (@{ cmd = "box_start"; config = $config } | ConvertTo-Json -Depth 20 -Compress)
  if ($start -notmatch '"box_started"') { throw "box_start: $start" }
  Step "box started"

  # 3. Before sleep.
  $deadline = (Get-Date).AddSeconds(20)
  do { Start-Sleep 1; $tun = Get-TunState; $exit = if ($tun.DefaultVia) { Get-ExitIp } } until (($exit) -or (Get-Date) -gt $deadline)
  Check "before: TUN up, default route via it" ($tun.Up -and $tun.DefaultVia) "adapter=$($tun.Alias)"
  Check "before: exit IP is the proxy" ($exit -eq $expectedExit) "exit=$exit expected=$expectedExit"
  if (-not ($tun.DefaultVia -and $exit -eq $expectedExit)) {
    Write-Host "  diagnostics:"
    Get-NetRoute -AddressFamily IPv4 -ErrorAction SilentlyContinue |
      Where-Object { $_.DestinationPrefix -in @('0.0.0.0/0', '0.0.0.0/1', '128.0.0.0/1') -or $_.InterfaceAlias -eq $tun.Alias } |
      ForEach-Object { Write-Host ("    route {0,-16} if={1,-3} {2,-14} nexthop={3,-14} metric={4}" -f $_.DestinationPrefix, $_.ifIndex, $_.InterfaceAlias, $_.NextHop, $_.RouteMetric) }
    Get-NetIPInterface -AddressFamily IPv4 -ConnectionState Connected -ErrorAction SilentlyContinue |
      ForEach-Object { Write-Host ("    iface {0,-3} {1,-28} metric={2}" -f $_.ifIndex, $_.InterfaceAlias, $_.InterfaceMetric) }
    $fr = Find-NetRoute -RemoteIPAddress 1.1.1.1 -ErrorAction SilentlyContinue
    $fr | ForEach-Object { Write-Host ("    find-route 1.1.1.1 -> if={0} {1}" -f $_.InterfaceIndex, $_.InterfaceAlias) }
    $probe = & curl.exe -s -m 8 --noproxy "*" -w " http=%{http_code}" https://api.ipify.org 2>&1
    Write-Host "    curl ipify anyway: $probe"
    throw "box not working before sleep - no point sleeping"
  }
  $genBefore = (Send-PipeCommand '{"cmd":"box_stats"}' | ConvertFrom-Json).generation

  # 4. Sleep with a wake timer.
  $timer = [Sov.Power]::CreateWaitableTimer([IntPtr]::Zero, $true, $null)
  $due = - [long]$WakeAfterSeconds * 10000000   # negative = relative, 100 ns units
  if (-not [Sov.Power]::SetWaitableTimer($timer, [ref]$due, 0, [IntPtr]::Zero, [IntPtr]::Zero, $true)) {
    throw "SetWaitableTimer failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
  }
  $suspendAt = Get-Date
  Step "suspending, wake timer in $WakeAfterSeconds s"
  [Sov.Power]::SetSuspendState($false, $false, $false) | Out-Null
  $resumeAt = Get-Date   # this thread was frozen with the machine
  # A wake-timer wake is "unattended" for Windows: display off, Wi-Fi not brought
  # back, and ~2 min later it sleeps again (25.09.2026 first run: Wi-Fi stayed down
  # until a person woke the machine 37 min later). Declaring the display required
  # makes it a normal wake; ES_CONTINUOUS holds it until the flags are cleared.
  # Unsigned on purpose: PowerShell reads 0x80000000 as a negative Int32, which the UInt32
  # parameter rejects (25.09.2026: that threw right after the wake and in the cleanup).
  [Sov.Power]::SetThreadExecutionState([uint32]0x80000003L) | Out-Null   # ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED
  Step ("back, slept {0:N0} s" -f ($resumeAt - $suspendAt).TotalSeconds)
  $result.sleptSeconds = [int]($resumeAt - $suspendAt).TotalSeconds

  # 5. After wake: when the machine's own network is back (Wi-Fi reconnect is
  # Windows' and the driver's business, not ours), then first traffic through
  # the TUN (AC5), then adapter/route (AC4). The two times are reported apart:
  # the second one is what Sovereign is answerable for.
  $firstOk = $null; $netAt = $null
  $deadline = $resumeAt.AddSeconds($ResumeTimeoutSeconds)
  while ((Get-Date) -lt $deadline) {
    if (-not $netAt) {
      $phys = Get-NetAdapter -Physical -ErrorAction SilentlyContinue | Where-Object Status -eq "Up"
      $gw = Get-NetRoute -DestinationPrefix "0.0.0.0/0" -ErrorAction SilentlyContinue |
        Where-Object { $_.ifIndex -in $phys.ifIndex -and $_.NextHop -ne "0.0.0.0" }
      if ($phys -and $gw) { $netAt = Get-Date; Step ("network back: {0} after {1:N1} s" -f ($phys.Name -join ","), ($netAt - $resumeAt).TotalSeconds) }
    }
    $tun = Get-TunState
    if ($netAt -and $tun.DefaultVia) { $exit = Get-ExitIp 5; if ($exit -eq $expectedExit) { $firstOk = Get-Date; break } }
    Start-Sleep 1
  }
  if ($netAt) { $result.secondsToNetwork = [math]::Round(($netAt - $resumeAt).TotalSeconds, 1) }
  Check "machine network back after wake (Windows/driver, not Sovereign)" ([bool]$netAt) $(if ($netAt) { "{0:N1} s" -f $result.secondsToNetwork } else { "not within $ResumeTimeoutSeconds s" })
  $tun = Get-TunState
  Check "AC4: TUN adapter back and up" $tun.Up "adapter=$($tun.Alias)"
  Check "AC4: default route via our TUN" $tun.DefaultVia ""
  if ($firstOk) {
    $result.secondsToTraffic = [math]::Round(($firstOk - $resumeAt).TotalSeconds, 1)
    $result.secondsNetworkToTraffic = [math]::Round(($firstOk - $netAt).TotalSeconds, 1)
    Check "AC5: real HTTPS through AnyTLS+REALITY after wake" $true ("exit=$exit {0:N1} s after wake, {1:N1} s after the network was back" -f $result.secondsToTraffic, $result.secondsNetworkToTraffic)
  } else {
    Check "AC5: real HTTPS through AnyTLS+REALITY after wake" $false "no traffic within $ResumeTimeoutSeconds s (last exit=$exit)"
  }
  $dns = Resolve-DnsName example.com -Type A -DnsOnly -ErrorAction SilentlyContinue | Select-Object -First 1
  Check "after: DNS through the TUN" ([bool]$dns) "$($dns.IPAddress)"
  $genAfter = (Send-PipeCommand '{"cmd":"box_stats"}' | ConvertFrom-Json).generation
  Check "box was restarted on resume (generation changed)" ($genAfter -ne $genBefore) "generation $genBefore -> $genAfter"
  & $sovtrace stop -Name $traceSession
  $traceStarted = $false
  $power = @(& $sovtrace dump -Path $traceFile -AsObject | Where-Object { $_.Event -in 'Power', 'PowerFailed' })
  $power | ForEach-Object { Write-Host ("    {0:HH:mm:ss} {1} {2} {3}" -f $_.Time, $_.Event, $_.Fields.Action, $_.Fields.Error) }
  $seq = ($power | ForEach-Object { "$($_.Event):$($_.Fields.Action)" }) -join ' '
  Check "power events: suspend -> resume -> restart_stop -> restart_start, none failed" `
    ($seq -match 'Power:suspend Power:resume Power:restart_stop Power:restart_start' -and $seq -notmatch 'PowerFailed') $seq
} catch {
  Write-Host "ERROR: $_"
  $result.error = "$_"
} finally {
  Step "cleanup"
  try { [Sov.Power]::SetThreadExecutionState([uint32]0x80000000L) | Out-Null } catch { Write-Host "execution state: $_" }   # back to normal sleep rules
  # Every step on its own: whatever fails, the production client must come back
  # (25.09.2026: one throwing line here left sing-box-daemon stopped).
  if ($traceStarted) { try { & $sovtrace stop -Name $traceSession } catch { Write-Host "trace stop: $_" } }
  try { Send-PipeCommand '{"cmd":"box_stop"}' | Out-Null } catch { Write-Host "box_stop: $_" }
  try { Stop-Service SovereignCore -Force -ErrorAction Stop } catch { Write-Host "stop SovereignCore: $_" }
  if ($prodWasRunning) {
    try { Start-Service $ProdService -ErrorAction Stop } catch { Write-Host "start ${ProdService}: $_" }
    $deadline = (Get-Date).AddSeconds(40)
    do { Start-Sleep 2; $code = & curl.exe -s -o NUL -m 8 -w "%{http_code}" https://www.gstatic.com/generate_204 } until ($code -eq "204" -or (Get-Date) -gt $deadline)
    Check "cleanup: $ProdService back, machine online" ($code -eq "204") "generate_204 -> $code"
  }
  $result.finished = (Get-Date).ToString("s")
  $result | ConvertTo-Json -Depth 5 | Set-Content -Path $resultFile -Encoding utf8
  Stop-Transcript | Out-Null
}
