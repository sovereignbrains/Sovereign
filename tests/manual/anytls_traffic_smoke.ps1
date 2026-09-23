<#
.SYNOPSIS
  Manual/scripted proof that P2 atom 1b's box_start actually proxies traffic
  through a real anytls outbound, not just that it parses a config.

.DESCRIPTION
  Not a CTest — it needs two live sing-box instances and a real TCP proxy
  round-trip, which is closer in shape to P3's future conformance harness
  than to P1/P2's unit-style golden tests. Until that harness exists, this
  script is the reproducible record of the proof: it stands up a real
  anytls server (tools/boxrun + the pinned vendored sing-box), starts a
  client instance through the actual sovereign-gocore.dll via the service's
  box_start pipe command (the same path the real service uses), and drives
  real bytes through it — a local HTTP responder on one end, a hand-rolled
  SOCKS5 client on the other, talking through the client's "mixed" inbound.

  Requires the same dev shell setup CMake's ci preset does: go and a
  MinGW-w64 gcc (for cgo) on PATH. Does not build anything itself — pass
  -BuildDir pointing at an already-built sovereign-core.exe +
  sovereign-gocore.dll pair (e.g. build/ci/src/service).

.EXAMPLE
  pwsh tests/manual/anytls_traffic_smoke.ps1 -BuildDir build/ci/src/service
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$BuildDir
)

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$buildDir = Resolve-Path $BuildDir
$work = Join-Path $env:TEMP ("sovereign-smoke-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null

$httpMarker = "sovereign-anytls-smoke-" + [guid]::NewGuid().ToString("N")
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

function Send-PipeCommand([string]$json) {
  $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "sovereign-control", [System.IO.Pipes.PipeDirection]::InOut)
  $pipe.Connect(3000)
  $writer = New-Object System.IO.StreamWriter($pipe)
  $reader = New-Object System.IO.StreamReader($pipe)
  $writer.AutoFlush = $true
  $writer.WriteLine($json)
  $response = $reader.ReadLine()
  $pipe.Dispose()
  return $response
}

# SOCKS5, no-auth, CONNECT — just enough to drive one TCP stream through the
# client's "mixed" inbound. Returns the connected NetworkStream.
function Connect-Socks5([string]$proxyHost, [int]$proxyPort, [string]$targetHost, [int]$targetPort) {
  $client = New-Object System.Net.Sockets.TcpClient
  $client.Connect($proxyHost, $proxyPort)
  $stream = $client.GetStream()

  $stream.Write([byte[]](0x05, 0x01, 0x00), 0, 3)
  $greeting = New-Object byte[] 2
  $stream.Read($greeting, 0, 2) | Out-Null
  if ($greeting[0] -ne 5 -or $greeting[1] -ne 0) {
    throw "SOCKS5 greeting failed: $($greeting -join ',')"
  }

  $targetIp = [System.Net.IPAddress]::Parse($targetHost).GetAddressBytes()
  $portHigh = [byte](($targetPort -shr 8) -band 0xFF)
  $portLow = [byte]($targetPort -band 0xFF)
  $request = [byte[]]@(0x05, 0x01, 0x00, 0x01) + $targetIp + [byte[]]@($portHigh, $portLow)
  $stream.Write($request, 0, $request.Length)
  $reply = New-Object byte[] 10
  $stream.Read($reply, 0, 10) | Out-Null
  if ($reply[1] -ne 0) {
    throw "SOCKS5 CONNECT failed, reply code $($reply[1])"
  }
  return @{ Client = $client; Stream = $stream }
}

try {
  # 1. Local HTTP target — stands in for "the internet" the anytls server's
  #    direct outbound would otherwise reach, without depending on it.
  $httptargetExe = Join-Path $repoRoot "tools\httptarget\httptarget.exe"
  if (-not (Test-Path $httptargetExe)) {
    & go build -o $httptargetExe (Join-Path $repoRoot "tools\httptarget") 2>&1 | Write-Verbose
  }
  $targetProc = Start-Process -FilePath $httptargetExe -ArgumentList "-listen", "127.0.0.1:9000", "-body", $httpMarker `
    -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $work "target.out.log") `
    -RedirectStandardError (Join-Path $work "target.err.log")
  if (-not (Wait-TcpPort "127.0.0.1" 9000)) { throw "httptarget never opened 127.0.0.1:9000" }

  # 2. Ephemeral self-signed cert for the anytls server (outbound uses
  #    tls.insecure — this is a localhost smoke test, not a trust proof).
  $gencertExe = Join-Path $repoRoot "tools\gencert\gencert.exe"
  if (-not (Test-Path $gencertExe)) {
    & go build -o $gencertExe (Join-Path $repoRoot "tools\gencert") 2>&1 | Write-Verbose
  }
  & $gencertExe -host 127.0.0.1 -out-cert (Join-Path $work "cert.pem") -out-key (Join-Path $work "key.pem")
  if ($LASTEXITCODE -ne 0) { throw "gencert failed" }

  # 3. anytls server via boxrun, using the exact JSON schema box_start
  #    itself accepts — same parser, same registries.
  $serverConfig = @{
    log       = @{ level = "info" }
    inbounds  = @(@{
      type        = "anytls"
      tag         = "in"
      listen      = "127.0.0.1"
      listen_port = 8443
      users       = @(@{ name = "test"; password = "hunter2" })
      tls         = @{
        enabled          = $true
        certificate_path = (Join-Path $work "cert.pem") -replace '\\', '/'
        key_path         = (Join-Path $work "key.pem") -replace '\\', '/'
      }
    })
    outbounds = @(@{ type = "direct"; tag = "direct" })
  }
  $serverConfigPath = Join-Path $work "server.json"
  $serverConfig | ConvertTo-Json -Depth 10 | Set-Content -Path $serverConfigPath -Encoding utf8

  $boxrunExe = Join-Path $repoRoot "tools\boxrun\boxrun.exe"
  if (-not (Test-Path $boxrunExe)) {
    & go build -o $boxrunExe (Join-Path $repoRoot "tools\boxrun") 2>&1 | Write-Verbose
  }
  $serverProc = Start-Process -FilePath $boxrunExe -ArgumentList "-config", $serverConfigPath `
    -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $work "server.out.log") `
    -RedirectStandardError (Join-Path $work "server.err.log")
  if (-not (Wait-TcpPort "127.0.0.1" 8443)) { throw "anytls server never opened 127.0.0.1:8443" }

  # 4. The client instance, started the real way: sovereign-core.exe --run,
  #    box_start over the same named pipe the tray/service use.
  $coreProc = Start-Process -FilePath (Join-Path $buildDir "sovereign-core.exe") -ArgumentList "--run" `
    -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $work "core.out.log") `
    -RedirectStandardError (Join-Path $work "core.err.log")
  Start-Sleep -Milliseconds 500

  $clientConfig = @{
    log       = @{ level = "info" }
    inbounds  = @(@{ type = "mixed"; tag = "in"; listen = "127.0.0.1"; listen_port = 19050 })
    outbounds = @(@{
      type        = "anytls"
      tag         = "proxy"
      server      = "127.0.0.1"
      server_port = 8443
      password    = "hunter2"
      tls         = @{ enabled = $true; insecure = $true }
    })
  }
  $startRequest = @{ cmd = "box_start"; config = $clientConfig } | ConvertTo-Json -Depth 10 -Compress
  $startResponse = Send-PipeCommand $startRequest
  Write-Host "box_start response: $startResponse"
  if ($startResponse -notmatch '"box_started"') { throw "box_start failed: $startResponse" }

  if (-not (Wait-TcpPort "127.0.0.1" 19050)) { throw "client mixed inbound never opened 127.0.0.1:19050" }

  # 5. The actual proof: fetch through client mixed inbound -> anytls
  #    outbound -> anytls server -> direct outbound -> local HTTP target.
  $conn = Connect-Socks5 "127.0.0.1" 19050 "127.0.0.1" 9000
  try {
    $requestBytes = [System.Text.Encoding]::ASCII.GetBytes("GET / HTTP/1.1`r`nHost: 127.0.0.1`r`nConnection: close`r`n`r`n")
    $conn.Stream.Write($requestBytes, 0, $requestBytes.Length)
    $reader = New-Object System.IO.StreamReader($conn.Stream)
    $response = $reader.ReadToEnd()
  } finally {
    $conn.Client.Close()
  }

  if ($response -notmatch [regex]::Escape($httpMarker)) {
    throw "response did not contain the expected marker.`nGot:`n$response"
  }

  Write-Host "PASS: traffic round-tripped through box_start's anytls outbound (mixed:19050 -> anytls:8443 -> direct -> http:9000)."
} finally {
  if ($coreProc -and -not $coreProc.HasExited) {
    try { Send-PipeCommand '{"cmd":"box_stop"}' | Out-Null } catch {}
    Stop-Process -Id $coreProc.Id -Force -ErrorAction SilentlyContinue
  }
  if ($serverProc -and -not $serverProc.HasExited) {
    Stop-Process -Id $serverProc.Id -Force -ErrorAction SilentlyContinue
  }
  if ($targetProc -and -not $targetProc.HasExited) {
    Stop-Process -Id $targetProc.Id -Force -ErrorAction SilentlyContinue
  }
  Remove-Item -Path $work -Recurse -Force -ErrorAction SilentlyContinue
}
