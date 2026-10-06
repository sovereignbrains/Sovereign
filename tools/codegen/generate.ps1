# Every generated header, from the pinned sing-box in vendor/sing-box. CI runs
# this and fails on any difference from what's committed (drift): a field
# renamed or added upstream shows up as a diff, not as a config sing-box
# refuses at the user's.
#
#   pwsh tools/codegen/generate.ps1
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '../..')
$src = Join-Path $root 'vendor/sing-box'
$out = Join-Path $root 'src/generated'

# Header -> the option types it holds. Types sharing structs (the TLS
# options, the dialer's) must share a header: each struct is defined once.
$headers = [ordered]@{
  'outbounds.gen.h'      = @(
    # Everything the client writes from share links and subscriptions.
    'AnyTLSOutboundOptions', 'VLESSOutboundOptions', 'VMessOutboundOptions', 'TrojanOutboundOptions',
    'ShadowsocksOutboundOptions', 'Hysteria2OutboundOptions', 'HysteriaOutboundOptions', 'TUICOutboundOptions',
    'SOCKSOutboundOptions', 'HTTPOutboundOptions', 'NaiveOutboundOptions', 'SSHOutboundOptions',
    'SnellOutboundOptions', 'WireGuardEndpointOptions')
  'anytls_inbound.gen.h' = @('AnyTLSInboundOptions')
}

Push-Location (Join-Path $root 'tools/codegen')
try {
  foreach ($header in $headers.Keys) {
    $path = Join-Path $out $header
    # main.go, not '.': smoke_test.cpp next to it makes Go refuse the package
    # (C++ files without cgo) - on Linux at least.
    go run main.go -src $src -type ($headers[$header] -join ',') -out $path
    if ($LASTEXITCODE -ne 0) { throw "codegen failed for $header" }
  }
} finally {
  Pop-Location
}
