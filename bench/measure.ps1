# Measures a VPN client that is connected right now, the same way for every
# client: its processes' memory and CPU, where traffic exits, leaks (DNS,
# WebRTC/STUN, IPv6), latency and download speed. One JSON line per run is
# appended to results\<client>.jsonl.
#
#   powershell -File measure.ps1 -Client sovereign -Dirs 'C:\Program Files\Sovereign' [-Expect 5.83.147.210]
#
# -Dirs: install folders; every process whose image lives under one of them
# is the client's (a GUI, its core, its service, Electron helpers).
# -Names: extra process names (a core outside the folder).
param(
  [Parameter(Mandatory)] [string]$Client,
  [string[]]$Dirs = @(),
  [string[]]$Names = @(),
  [string]$Expect = '',          # the server's exit address; empty: not checked
  [int]$IdleSeconds = 60,        # CPU sampled over this, window hidden, nothing running
  [int]$Pings = 20,
  [int]$Downloads = 3,
  [switch]$Quick                 # resources and exit only
)
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $here 'results'
New-Item -ItemType Directory -Force $out | Out-Null

function ClientProcesses {
  Get-Process | Where-Object {
    $p = $_
    $path = $null
    try { $path = $p.Path } catch {}
    ($Names -contains $p.ProcessName) -or
    ($path -and ($Dirs | Where-Object { $path.StartsWith($_, [StringComparison]::OrdinalIgnoreCase) }))
  }
}

function Resources {
  $ps = @(ClientProcesses)
  [pscustomobject]@{
    processes  = $ps.Count
    names      = ($ps | Group-Object ProcessName | ForEach-Object { "$($_.Name)x$($_.Count)" }) -join ' '
    privateMB  = [math]::Round(($ps | Measure-Object PrivateMemorySize64 -Sum).Sum / 1MB, 1)
    workingMB  = [math]::Round(($ps | Measure-Object WorkingSet64 -Sum).Sum / 1MB, 1)
    handles    = ($ps | Measure-Object HandleCount -Sum).Sum
    threads    = ($ps | ForEach-Object { $_.Threads.Count } | Measure-Object -Sum).Sum
  }
}

# CPU over a window: the processes' CPU time used, as a share of one core and of the machine.
function CpuOver([int]$seconds) {
  $before = @{}
  foreach ($p in ClientProcesses) { try { $before[$p.Id] = $p.TotalProcessorTime.TotalMilliseconds } catch {} }
  Start-Sleep -Seconds $seconds
  $used = 0.0
  foreach ($p in ClientProcesses) {
    try {
      $now = $p.TotalProcessorTime.TotalMilliseconds
      $used += if ($before.ContainsKey($p.Id)) { $now - $before[$p.Id] } else { $now }
    } catch {}
  }
  [pscustomobject]@{
    seconds        = $seconds
    cpuMs          = [math]::Round($used)
    oneCorePercent = [math]::Round(100 * $used / ($seconds * 1000), 3)
  }
}

function ExitIp {
  $v4 = (& curl.exe -4 -s -m 10 https://api.ipify.org) 2>$null
  $v6 = (& curl.exe -6 -s -m 10 https://api6.ipify.org) 2>$null
  [pscustomobject]@{ v4 = "$v4"; v6 = "$v6" }
}

# DNS leak (bash.ws): names under a fresh id resolved by the system, then
# asked which resolvers came to its servers for them.
function DnsLeak {
  try {
    $id = (Invoke-RestMethod -TimeoutSec 15 https://bash.ws/id).ToString().Trim()
    1..10 | ForEach-Object { try { Resolve-DnsName -DnsOnly -QuickTimeout "$_.$id.bash.ws" -ErrorAction SilentlyContinue | Out-Null } catch {} }
    Start-Sleep -Seconds 2
    $r = Invoke-RestMethod -TimeoutSec 15 "https://bash.ws/dnsleak/test/$($id)?json"
    @($r | Where-Object { $_.type -eq 'dns' } | ForEach-Object { "$($_.ip) $($_.country_name) / $($_.asn)" })
  } catch { @("error: $($_.Exception.Message)") }
}

# WebRTC's view: a STUN binding request over UDP, the address the STUN server saw.
function StunAddress([string]$server = 'stun.l.google.com', [int]$port = 19302) {
  $udp = New-Object System.Net.Sockets.UdpClient
  try {
    $udp.Client.ReceiveTimeout = 4000
    $tx = New-Object byte[] 12
    (New-Object Random).NextBytes($tx)
    $req = [byte[]](0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42) + $tx
    [void]$udp.Send($req, $req.Length, $server, $port)
    $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
    $res = $udp.Receive([ref]$ep)
    $i = 20
    while ($i + 4 -le $res.Length) {
      $type = ($res[$i] -shl 8) -bor $res[$i + 1]
      $len = ($res[$i + 2] -shl 8) -bor $res[$i + 3]
      if (($type -eq 0x0020 -or $type -eq 0x0001) -and $res[$i + 5] -eq 1) {
        $b = $res[($i + 8)..($i + 11)]
        if ($type -eq 0x0020) { $b = @(($b[0] -bxor 0x21), ($b[1] -bxor 0x12), ($b[2] -bxor 0xA4), ($b[3] -bxor 0x42)) }
        return ($b -join '.')
      }
      $i += 4 + $len + ((4 - $len % 4) % 4)
    }
    'no address'
  } catch { "error: $($_.Exception.Message)" } finally { $udp.Close() }
}

function Median([double[]]$xs) {
  if (-not $xs -or $xs.Count -eq 0) { return $null }
  $s = $xs | Sort-Object
  if ($s.Count % 2) { $s[[int][math]::Floor($s.Count / 2)] } else { ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2 }
}

# Fresh connections each time: TCP connect, TLS, first byte (ms).
function Latency([string]$url, [int]$n) {
  $connect = @(); $tls = @(); $first = @(); $fail = 0
  for ($k = 0; $k -lt $n; $k++) {
    $w = (& curl.exe -s -o NUL -m 10 -w '%{time_connect} %{time_appconnect} %{time_starttransfer} %{http_code}' $url) 2>$null
    $f = "$w".Split(' ')
    if ($f.Count -eq 4 -and $f[3] -ne '000') {
      $connect += [double]$f[0] * 1000; $tls += [double]$f[1] * 1000; $first += [double]$f[2] * 1000
    } else { $fail++ }
  }
  [pscustomobject]@{
    url = $url; n = $n; failed = $fail
    connectMs = [math]::Round((Median $connect), 1)
    tlsMs     = [math]::Round((Median $tls), 1)
    firstMs   = [math]::Round((Median $first), 1)
  }
}

function Download([string]$url, [int]$n) {
  $mbps = @()
  for ($k = 0; $k -lt $n; $k++) {
    $s = (& curl.exe -s -o NUL -m 60 -w '%{speed_download}' $url) 2>$null
    if ($s) { $mbps += [math]::Round([double]$s * 8 / 1e6, 1) }
  }
  [pscustomobject]@{ url = $url; runs = $mbps; medianMbps = (Median $mbps) }
}

$run = [ordered]@{ client = $Client; at = (Get-Date).ToString('s') }
Write-Host "[$Client] resources (connected, idle)"
$run.resources = Resources
$run.exit = ExitIp
$run.exitOk = if ($Expect) { $run.exit.v4 -eq $Expect } else { $null }
Write-Host "[$Client] exit v4=$($run.exit.v4) ok=$($run.exitOk)"
Write-Host "[$Client] CPU over $IdleSeconds s"
$run.idleCpu = CpuOver $IdleSeconds
if (-not $Quick) {
  Write-Host "[$Client] leaks"
  $run.stun = StunAddress
  $run.dns = DnsLeak
  Write-Host "[$Client] latency"
  $run.latency = @(
    (Latency 'https://www.google.com/generate_204' $Pings),
    (Latency 'https://www.cloudflare.com/cdn-cgi/trace' $Pings)
  )
  Write-Host "[$Client] download"
  $before = Resources
  $run.download = Download 'https://fsn1-speed.hetzner.com/100MB.bin' $Downloads
  $run.resourcesAfterLoad = Resources
}
$line = $run | ConvertTo-Json -Depth 6 -Compress
Add-Content -Path (Join-Path $out "$Client.jsonl") -Value $line -Encoding UTF8
$run | ConvertTo-Json -Depth 6
