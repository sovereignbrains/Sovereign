# Both of the bench server's links through the reference sing-box, each on a
# mixed inbound of its own: where traffic exits through each. Prints no secret.
param(
  [string]$Links = 'C:\Users\user\bench-dl\aeza-links.txt',
  [string]$SingBox = 'C:\Users\user\projects\sb-ref\sing-box-1.14.2-windows-amd64\sing-box.exe',
  [string]$Url = 'https://hel1-speed.hetzner.com/100MB.bin'
)
$ErrorActionPreference = 'Stop'
$outbounds = @(); $inbounds = @(); $rules = @(); $port = 21080
foreach ($line in Get-Content $Links) {
  if (-not $line.Trim()) { continue }
  $u = [Uri]$line
  $q = @{}
  foreach ($kv in $u.Query.TrimStart('?').Split('&')) { $p = $kv.Split('=', 2); $q[$p[0]] = [Uri]::UnescapeDataString($p[1]) }
  $tag = $u.Scheme
  if ($u.Scheme -eq 'vless') {
    $outbounds += @{ type = 'vless'; tag = $tag; server = $u.Host; server_port = $u.Port; uuid = $u.UserInfo; flow = $q.flow
      tls = @{ enabled = $true; server_name = $q.sni; utls = @{ enabled = $true; fingerprint = $q.fp }
               reality = @{ enabled = $true; public_key = $q.pbk; short_id = $q.sid } } }
  } else {
    $outbounds += @{ type = 'hysteria2'; tag = $tag; server = $u.Host; server_port = $u.Port; password = $u.UserInfo
      tls = @{ enabled = $true; server_name = $q.sni; insecure = $true } }
  }
  $inbounds += @{ type = 'mixed'; tag = "in-$tag"; listen = '127.0.0.1'; listen_port = $port }
  $rules += @{ inbound = @("in-$tag"); outbound = $tag }
  $port++
}
$cfg = @{ log = @{ level = 'warn' }; inbounds = $inbounds; outbounds = $outbounds; route = @{ rules = $rules } }
$path = Join-Path $env:TEMP 'bench-check.json'
$cfg | ConvertTo-Json -Depth 8 | Set-Content -Encoding ASCII $path
$p = Start-Process $SingBox -ArgumentList 'run', '-c', $path -PassThru -WindowStyle Hidden
Start-Sleep 2
try {
  for ($i = 0; $i -lt $inbounds.Count; $i++) {
    $port = $inbounds[$i].listen_port
    $ip = (& curl.exe -s -m 15 -x "socks5h://127.0.0.1:$port" https://api.ipify.org) 2>$null
    $speed = (& curl.exe -s -o NUL -m 40 -w '%{speed_download}' -x "socks5h://127.0.0.1:$port" $Url) 2>$null
    "$($outbounds[$i].type): exit=$ip  100MB at $([math]::Round([double]$speed * 8 / 1e6, 1)) Mbit/s"
  }
} finally { Stop-Process -Id $p.Id -Force; Remove-Item $path }
