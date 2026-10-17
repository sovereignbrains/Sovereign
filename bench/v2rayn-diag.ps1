# v2rayN with TUN on, watched without anyone at the keyboard (its TUN took the
# internet down, the chat with it): TUN switched on in its settings, v2rayN
# started, for $Seconds every 2 s where traffic exits and which cores run;
# then v2rayN and its cores closed and TUN switched off again, so the
# internet comes back by itself. Needs admin. Log: results\v2rayn-diag.log.
param([int]$Seconds = 40)
$dir = 'C:\Users\user\bench-dl\v2rayN-windows-64\v2rayN-windows-64'
$cfg = "$dir\guiConfigs\guiNConfig.json"
$log = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'results\v2rayn-diag.log'
function Say($s) { "$(Get-Date -Format 'HH:mm:ss.fff') $s" | Tee-Object -FilePath $log -Append }
function SetTun([bool]$on) {
  $j = Get-Content $cfg -Raw | ConvertFrom-Json
  $j.TunModeItem.EnableTun = $on
  $j | ConvertTo-Json -Depth 20 | Set-Content -Encoding UTF8 $cfg
}
Get-Process v2rayN, xray, sing-box -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1
SetTun $true
Say "start: TUN on in settings, launching v2rayN"
$p = Start-Process "$dir\v2rayN.exe" -PassThru
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
  $ip = (& curl.exe -4 -s -m 2 https://api.ipify.org) 2>$null
  $ip = if ($ip -eq '89.169.33.181') { 'VPN' } elseif ($ip) { 'DIRECT' } else { 'none' }
  $cores = (Get-Process xray, sing-box -ErrorAction SilentlyContinue | ForEach-Object { "$($_.ProcessName):$($_.Id)" }) -join ','
  $tun = (Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.InterfaceDescription -match 'tun|wintun' -and $_.Status -eq 'Up' } | ForEach-Object Name) -join ','
  $dns = try { (Resolve-DnsName -QuickTimeout -DnsOnly www.google.com -Type A -ErrorAction Stop | Where-Object IPAddress | Select-Object -First 1).IPAddress } catch { 'fail' }
  $socks = (& curl.exe -s -m 2 -x socks5h://127.0.0.1:10808 https://api.ipify.org) 2>$null
  Say ("exit={0} cores=[{1}] tun=[{2}] dns={3} via-socks10808={4}" -f $ip, $cores, $tun, $dns, $(if ($socks -eq '89.169.33.181') { 'VPN' } elseif ($socks) { 'other' } else { 'none' }))
  Start-Sleep 2
}
Get-Process v2rayN, xray, sing-box -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 2
SetTun $false
Say "done: v2rayN closed, TUN off in settings; exit now $((& curl.exe -4 -s -m 5 https://api.ipify.org) 2>$null | ForEach-Object { if ($_ -eq '89.169.33.181') { 'VPN' } elseif ($_) { 'DIRECT' } else { 'none' } })"
