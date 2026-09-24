<#
.SYNOPSIS
  Manual/scripted proof that traffic flows through an AnyTLS + REALITY
  outbound started by sovereign-core (P2 acceptance: "traffic through
  AnyTLS+Reality"), and that REALITY authentication is really enforced.

.DESCRIPTION
  Same shape as anytls_traffic_smoke.ps1 (P2 atom 1b), with REALITY instead of
  a self-signed certificate. The client always runs the real way:
  sovereign-core.exe --run, box_start over the control pipe, the actual
  sovereign-gocore.dll. Two modes:

  Local (default) - self-contained: an AnyTLS+REALITY server via tools/boxrun
  (keys from `tools/gencert -reality`), a local HTTP target. The server's
  REALITY handshake target (-HandshakeServer) must be reachable over the
  internet: REALITY borrows its TLS handshake from that real site.

  Live (-LiveOutbound <file>) - against a real server: the file holds one
  sing-box outbound object (e.g. the AnyTLS-REALITY node from a packetlab
  subscription). Traffic goes to the internet and the exit IP is printed.
  The file is read at run time and never committed - it holds credentials.

  Both modes end with a negative check: the same outbound with a different
  REALITY public key must NOT carry traffic. Without it, a passing run would
  not show that REALITY is enforced rather than bypassed.

  Also reports box_stats and whether box_logs carried the anytls outbound's
  connection lines (P2 atom 2's stats and log-callback paths under real use).

.EXAMPLE
  pwsh tests/manual/anytls_reality_smoke.ps1 -BuildDir build/ci/src/service

.EXAMPLE
  pwsh tests/manual/anytls_reality_smoke.ps1 -BuildDir build/ci/src/service `
    -LiveOutbound $env:TEMP\outbound.json -BindInterface Wi-Fi
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$BuildDir,
  [string]$LiveOutbound,
  # Pin the client's outbound to one interface (sing-box bind_interface) - useful
  # when another VPN client on the machine owns the default route.
  [string]$BindInterface,
  [string]$HandshakeServer = "www.microsoft.com"
)

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$buildDir = Resolve-Path $BuildDir
$work = Join-Path $env:TEMP ("sovereign-reality-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null

$mixedPort = 19051
$serverPort = 18443
$targetPort = 9001
$httpMarker = "sovereign-reality-smoke-" + [guid]::NewGuid().ToString("N")
$serverProc = $null
$coreProc = $null
$targetProc = $null

function Wait-TcpPort([string]$hostName, [int]$port, [int]$timeoutMs = 5000) {
  $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
  while ([DateTime]::UtcNow -lt $deadline) {
    try {
      $client = New-Object System.Net.Sockets.TcpClient
      $client.Connect($hostName, $port)
      $client.Close()
      return $true
    } catch {
      Start-Sleep -Milliseconds 100
    }
  }
  return $false
}

# One request = one pipe message; the reply is read until the message is
# complete (box_logs replies can take more than one read).
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

function Get-GoTool([string]$name, [switch]$SingBoxTags) {
  $buildArgs = @()
  $suffix = ""
  if ($SingBoxTags) {
    # Same tags/ldflags as upstream's Windows release and as gocore itself
    # (src/service/CMakeLists.txt) - REALITY needs with_utls. The binary name
    # carries a hash of them so a tool built without tags is never reused.
    $release = Join-Path $repoRoot "vendor\sing-box\release"
    $tags = (Get-Content (Join-Path $release "DEFAULT_BUILD_TAGS_WINDOWS") -Raw).Trim()
    $ldflags = (Get-Content (Join-Path $release "LDFLAGS") -Raw).Trim()
    $buildArgs = @("-tags", $tags, "-ldflags=$ldflags")
    $hash = [System.Security.Cryptography.SHA256]::HashData([System.Text.Encoding]::UTF8.GetBytes("$tags|$ldflags"))
    $suffix = "-" + [System.Convert]::ToHexString($hash).Substring(0, 8).ToLower()
  }
  $exe = Join-Path $repoRoot "tools\$name\$name$suffix.exe"
  if (-not (Test-Path $exe)) {
    # Each tool is its own Go module: build from inside it (a package path from
    # outside the module fails with "directory outside main module").
    Push-Location (Join-Path $repoRoot "tools\$name")
    try {
      & go build @buildArgs -o $exe . 2>&1 | Write-Verbose
      if ($LASTEXITCODE -ne 0) { throw "go build tools/$name failed" }
    } finally {
      Pop-Location
    }
  }
  return $exe
}

function New-RealityKeyPair {
  $lines = & (Get-GoTool "gencert") -reality
  if ($LASTEXITCODE -ne 0) { throw "gencert -reality failed" }
  return @{
    Private = ($lines | Select-String 'PrivateKey: (\S+)').Matches[0].Groups[1].Value
    Public  = ($lines | Select-String 'PublicKey: (\S+)').Matches[0].Groups[1].Value
  }
}

# Starts the box with $outbound behind a mixed inbound, fetches $url through it
# and stops the box again. Returns @{ Ok; Body; Stats; AnytlsLogLines; Error }.
function Invoke-ThroughBox($outbound, [string]$url) {
  $config = @{
    log       = @{ level = "info"; output = ((Join-Path $work "client-box.log") -replace '\\', '/') }
    inbounds  = @(@{ type = "mixed"; tag = "mixed-in"; listen = "127.0.0.1"; listen_port = $mixedPort })
    outbounds = @($outbound)
    route     = @{ final = $outbound.tag }
  }
  $start = Send-PipeCommand (@{ cmd = "box_start"; config = $config } | ConvertTo-Json -Depth 20 -Compress)
  if ($start -notmatch '"box_started"') { return @{ Ok = $false; Error = "box_start: $start" } }
  try {
    if (-not (Wait-TcpPort "127.0.0.1" $mixedPort)) { return @{ Ok = $false; Error = "mixed inbound never opened" } }
    # --noproxy "" overrides a NO_PROXY from the environment: with 127.0.0.1 in it
    # curl would fetch the local target directly and the run would pass without
    # a single byte going through the box (seen on a dev machine, caught only by
    # the box_stats check below).
    $body = & curl.exe -s -m 20 --noproxy "" -x "socks5h://127.0.0.1:$mixedPort" $url
    $curlExit = $LASTEXITCODE
    $stats = Send-PipeCommand '{"cmd":"box_stats"}' | ConvertFrom-Json
    $logs = Send-PipeCommand '{"cmd":"box_logs","since":0}' | ConvertFrom-Json
    $anytlsLines = @($logs.entries | Where-Object { $_.message -match 'outbound/anytls' }).Count
    return @{ Ok = ($curlExit -eq 0); Body = "$body"; Stats = $stats; AnytlsLogLines = $anytlsLines; Error = "curl exit $curlExit" }
  } finally {
    Send-PipeCommand '{"cmd":"box_stop"}' | Out-Null
  }
}

try {
  $coreProc = Start-Process -FilePath (Join-Path $buildDir "sovereign-core.exe") -ArgumentList "--run" `
    -PassThru -WindowStyle Hidden -WorkingDirectory $work `
    -RedirectStandardOutput (Join-Path $work "core.out.log") `
    -RedirectStandardError (Join-Path $work "core.err.log")
  Start-Sleep -Milliseconds 800
  if ($coreProc.HasExited) { throw "sovereign-core exited early (exit code $($coreProc.ExitCode))" }

  if ($LiveOutbound) {
    $outbound = Get-Content $LiveOutbound -Raw | ConvertFrom-Json -AsHashtable
    if ($outbound.tls.reality.enabled -ne $true) { throw "$LiveOutbound is not a REALITY outbound (tls.reality.enabled)" }
    $outbound.tag = "proxy"
    $url = "https://api.ipify.org"
    $expect = { param($r) $r.Body -match '^\d{1,3}(\.\d{1,3}){3}$' }
  } else {
    $keys = New-RealityKeyPair
    $shortId = -join ((1..8) | ForEach-Object { '{0:x2}' -f (Get-Random -Maximum 256) })

    $targetProc = Start-Process -FilePath (Get-GoTool "httptarget") -ArgumentList "-listen", "127.0.0.1:$targetPort", "-body", $httpMarker `
      -PassThru -WindowStyle Hidden `
      -RedirectStandardOutput (Join-Path $work "target.out.log") -RedirectStandardError (Join-Path $work "target.err.log")
    if (-not (Wait-TcpPort "127.0.0.1" $targetPort)) { throw "httptarget never opened 127.0.0.1:$targetPort" }

    # The server side is plain sing-box (boxrun), same JSON schema box_start takes.
    $serverConfig = @{
      log       = @{ level = "info" }
      inbounds  = @(@{
        type = "anytls"; tag = "anytls-reality-in"; listen = "127.0.0.1"; listen_port = $serverPort
        users = @(@{ name = "test"; password = "hunter2" })
        tls = @{
          enabled = $true; server_name = $HandshakeServer
          reality = @{
            enabled = $true; handshake = @{ server = $HandshakeServer; server_port = 443 }
            private_key = $keys.Private; short_id = @($shortId)
          }
        }
      })
      outbounds = @(@{ type = "direct"; tag = "direct" })
    }
    $serverConfigPath = Join-Path $work "server.json"
    $serverConfig | ConvertTo-Json -Depth 20 | Set-Content -Path $serverConfigPath -Encoding utf8
    $serverProc = Start-Process -FilePath (Get-GoTool "boxrun" -SingBoxTags) -ArgumentList "-config", $serverConfigPath `
      -PassThru -WindowStyle Hidden `
      -RedirectStandardOutput (Join-Path $work "server.out.log") -RedirectStandardError (Join-Path $work "server.err.log")
    if (-not (Wait-TcpPort "127.0.0.1" $serverPort)) { throw "REALITY server never opened 127.0.0.1:$serverPort" }

    $outbound = @{
      type = "anytls"; tag = "proxy"; server = "127.0.0.1"; server_port = $serverPort; password = "hunter2"
      tls = @{
        enabled = $true; server_name = $HandshakeServer
        utls = @{ enabled = $true; fingerprint = "chrome" }
        reality = @{ enabled = $true; public_key = $keys.Public; short_id = $shortId }
      }
    }
    $url = "http://127.0.0.1:$targetPort/"
    $expect = { param($r) $r.Body -match [regex]::Escape($httpMarker) }
  }
  if ($BindInterface) { $outbound.bind_interface = $BindInterface }

  # 1. Positive: the right key carries traffic.
  $good = Invoke-ThroughBox $outbound $url
  if (-not $good.Ok -or -not (& $expect $good)) {
    throw "traffic did not go through the AnyTLS+REALITY outbound: $($good.Error) body=[$($good.Body)]"
  }
  Write-Host "PASS positive: $url via AnyTLS+REALITY -> [$($good.Body.Trim().Substring(0, [Math]::Min(60, $good.Body.Trim().Length)))]"
  Write-Host "     box_stats: up=$($good.Stats.uplink) down=$($good.Stats.downlink) generation=$($good.Stats.generation); anytls log lines: $($good.AnytlsLogLines)"
  if ($good.Stats.uplink -le 0 -or $good.Stats.downlink -le 0) { throw "box_stats did not count the traffic" }

  # 2. Negative: a different public key must be rejected.
  $wrong = $outbound.Clone()
  $wrong.tls = $outbound.tls.Clone()
  $wrong.tls.reality = $outbound.tls.reality.Clone()
  $wrong.tls.reality.public_key = (New-RealityKeyPair).Public
  $bad = Invoke-ThroughBox $wrong $url
  if ($bad.Ok -and (& $expect $bad)) {
    throw "a WRONG REALITY public key still carried traffic - REALITY is not being enforced"
  }
  Write-Host "PASS negative: wrong REALITY public key carried no traffic ($($bad.Error))"
} finally {
  if ($coreProc -and -not $coreProc.HasExited) { Stop-Process -Id $coreProc.Id -Force -ErrorAction SilentlyContinue }
  foreach ($p in @($serverProc, $targetProc)) {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  }
  $stderr = Get-Content (Join-Path $work "core.err.log") -Raw -ErrorAction SilentlyContinue
  if ($stderr) { Write-Host "sovereign-core stderr:`n$stderr" }
  Remove-Item -Path $work -Recurse -Force -ErrorAction SilentlyContinue
}
