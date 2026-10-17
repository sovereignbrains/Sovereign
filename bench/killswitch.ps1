# What a client does when its tunnel breaks, while a probe asks every 250 ms
# where traffic exits. Two breaks, each with the client connected first:
#   core   - the client's core process killed (a crash);
#   server - the server unreachable for 20 s (dropped on the server), then back.
# A leak: any answer from an address other than the server's (-Server).
# Needs admin. Appends to results\<client>-killswitch.jsonl.
#
#   powershell -File killswitch.ps1 -Client hiddify -Core HiddifyCli -Server 5.83.147.210
param(
  [Parameter(Mandatory)] [string]$Client,
  [string[]]$Core = @(),         # process names to kill for "core"; empty: that test skipped
  [string]$Server = '',          # the server's address for "server"; empty: skipped
  [int]$Before = 5, [int]$After = 40, [int]$Blocked = 20
)
$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $here 'results'

# The probe, in a job: time since start (ms) and what came back.
function StartProbe([int]$seconds) {
  Start-Job -ArgumentList $seconds -ScriptBlock {
    param($seconds)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $seconds) {
      $t = $sw.ElapsedMilliseconds
      $ip = (& curl.exe -4 -s -m 2 https://api.ipify.org) 2>$null
      [pscustomobject]@{ ms = $t; ip = "$ip" }
      Start-Sleep -Milliseconds 250
    }
  }
}

function Judge($samples, [int]$breakMs) {
  $after = @($samples | Where-Object { $_.ms -ge $breakMs })
  $leaks = @($after | Where-Object { $_.ip -and $_.ip -ne $Server })
  $vpn = @($after | Where-Object { $_.ip -eq $Server })
  $dead = @($after | Where-Object { -not $_.ip })
  # Back: the first VPN answer after the last failure.
  $lastFail = ($after | Where-Object { $_.ip -ne $Server } | Select-Object -Last 1).ms
  $back = if ($lastFail) { ($after | Where-Object { $_.ms -gt $lastFail -and $_.ip -eq $Server } | Select-Object -First 1).ms } else { $null }
  [pscustomobject]@{
    samples     = $after.Count
    leaked      = $leaks.Count
    firstLeakMs = if ($leaks) { $leaks[0].ms - $breakMs } else { $null }
    viaVpn      = $vpn.Count
    noInternet  = $dead.Count
    backAfterMs = if ($back) { $back - $breakMs } else { $null }
  }
}

$result = [ordered]@{ client = $Client; at = (Get-Date).ToString('s') }

if ($Core) {
  Write-Host "[$Client] core kill: $($Core -join ', ')"
  $job = StartProbe ($Before + $After)
  Start-Sleep -Seconds $Before
  $breakAt = $Before * 1000
  Get-Process -Name $Core -ErrorAction SilentlyContinue | Stop-Process -Force
  Wait-Job $job | Out-Null
  $samples = @(Receive-Job $job); Remove-Job $job
  $result.core = Judge $samples $breakAt
  $result.coreAlive = @(Get-Process -Name $Core -ErrorAction SilentlyContinue).Count  # restarted by itself?
  Write-Host "[$Client] core: $($result.core | ConvertTo-Json -Compress)"
}

if ($Server) {
  if ($Core) { Write-Host "[$Client] reconnect it before the server test; waiting 20 s"; Start-Sleep -Seconds 20 }
  Write-Host "[$Client] server blocked for $Blocked s"
  $job = StartProbe ($Before + $Blocked + $After)
  Start-Sleep -Seconds $Before
  # Cut on the server itself (its proxy ports dropped, ssh kept), opened
  # again by the server's own timer: a firewall here wouldn't do (a client's
  # WFP filters may outrank Windows Firewall), and a command to open it would
  # have to come through the very tunnel that's cut. ssh isn't waited for.
  $cut = "setsid sh -c 'iptables -I INPUT -p tcp --dport 443 -j DROP; iptables -I INPUT -p udp --dport 8443 -j DROP; sleep $Blocked; iptables -D INPUT -p tcp --dport 443 -j DROP; iptables -D INPUT -p udp --dport 8443 -j DROP' </dev/null >/dev/null 2>&1 &"
  $ssh = Start-Process ssh.exe -ArgumentList '-n', '-o', 'BatchMode=yes', "root@$Server", $cut -WindowStyle Hidden -PassThru
  Start-Sleep -Seconds $Blocked
  if (-not $ssh.HasExited) { Stop-Process -Id $ssh.Id -Force -ErrorAction SilentlyContinue }
  Wait-Job $job | Out-Null
  $samples = @(Receive-Job $job); Remove-Job $job
  $result.server = Judge $samples ($Before * 1000)
  $result.serverBack = Judge $samples (($Before + $Blocked) * 1000)
  Write-Host "[$Client] server: $($result.server | ConvertTo-Json -Compress)"
}

Add-Content -Path (Join-Path $out "$Client-killswitch.jsonl") -Value ($result | ConvertTo-Json -Depth 5 -Compress) -Encoding UTF8
$result | ConvertTo-Json -Depth 5
