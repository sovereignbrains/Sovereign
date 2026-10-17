# How many fresh connections in a row get through the bench server's REALITY,
# through a local proxy already running (a client's socks/mixed port), or
# through the reference sing-box started here (-Ref). Prints + / - each.
param([int]$Port = 21080, [int]$N = 15, [switch]$Ref,
      [string]$Links = 'C:\Users\user\bench-dl\aeza-links.txt',
      [string]$SingBox = 'C:\Users\user\projects\sb-ref\sing-box-1.14.2-windows-amd64\sing-box.exe',
      [string]$Expect = '89.169.33.181')
$p = $null
if ($Ref) {
  $u = [Uri](Get-Content $Links | Where-Object { $_ -like 'vless://*' } | Select-Object -First 1)
  $q = @{}
  foreach ($kv in $u.Query.TrimStart('?').Split('&')) { $kv2 = $kv.Split('=', 2); $q[$kv2[0]] = [Uri]::UnescapeDataString($kv2[1]) }
  $cfg = @{ log = @{ level = 'warn' }
    inbounds = @(@{ type = 'mixed'; listen = '127.0.0.1'; listen_port = $Port })
    outbounds = @(@{ type = 'vless'; tag = 'proxy'; server = $u.Host; server_port = $u.Port; uuid = $u.UserInfo; flow = $q.flow
      tls = @{ enabled = $true; server_name = $q.sni; utls = @{ enabled = $true; fingerprint = $q.fp }
               reality = @{ enabled = $true; public_key = $q.pbk; short_id = $q.sid } } }) }
  $path = Join-Path $env:TEMP 'reality-rate.json'
  $cfg | ConvertTo-Json -Depth 8 | Set-Content -Encoding ASCII $path
  $p = Start-Process $SingBox -ArgumentList 'run', '-c', $path -PassThru -WindowStyle Hidden
  Start-Sleep 2
}
$ok = 0
$marks = foreach ($i in 1..$N) {
  $r = (& curl.exe -s -m 6 -x "socks5h://127.0.0.1:$Port" https://api.ipify.org) 2>$null
  if ($r -eq $Expect) { $ok++; '+' } else { '-' }
}
"$ok/$N  $($marks -join '')"
if ($p) { Stop-Process -Id $p.Id -Force; Remove-Item $path }
