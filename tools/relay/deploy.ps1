# Deploys the subscription relay (worker.js) to your own Cloudflare account.
#
#   $env:CLOUDFLARE_API_TOKEN = '<token>'   # Account: Workers Scripts Edit, Account Settings Read
#   .\deploy.ps1 [-Script fetch] [-Out relay.json]
#
# Writes the relay's address and a new random key to -Out (keep it private;
# re-running keeps the key that's there). Paste both into Sovereign:
# Настройки -> Подписки без подключения. The address is
# https://<script>.<your workers.dev subdomain>.workers.dev/ - that subdomain is
# what a subscription's server sees in the CF-Worker header, so pick a neutral
# one (dash.cloudflare.com -> Workers -> your subdomain).
param([string]$Script = 'fetch', [string]$Out = (Join-Path $PSScriptRoot 'relay.json'))
$ErrorActionPreference = 'Stop'
if (-not $env:CLOUDFLARE_API_TOKEN) { throw 'set CLOUDFLARE_API_TOKEN first' }
$api = 'https://api.cloudflare.com/client/v4'
$auth = "Authorization: Bearer $env:CLOUDFLARE_API_TOKEN"
$h = @{ Authorization = "Bearer $env:CLOUDFLARE_API_TOKEN" }
$acc = (Invoke-RestMethod -Headers $h "$api/accounts").result[0].id
$sub = (Invoke-RestMethod -Headers $h "$api/accounts/$acc/workers/subdomain").result.subdomain
if (-not $sub) { throw 'no workers.dev subdomain yet: open Workers in the dashboard once and pick one' }

function Upload([bool]$withMigration) {
    # JOBS: a big answer's pieces (Durable Object, SQLite-backed: on the free plan too).
    $meta = @{
        main_module        = 'worker.js'
        compatibility_date = '2026-09-01'
        bindings           = @(@{ type = 'durable_object_namespace'; name = 'JOBS'; class_name = 'Job' })
    }
    if ($withMigration) { $meta.migrations = @{ new_tag = 'v1'; new_sqlite_classes = @('Job') } }
    $metaFile = Join-Path $env:TEMP 'relay-meta.json'
    [IO.File]::WriteAllText($metaFile, ($meta | ConvertTo-Json -Depth 5 -Compress))
    $answer = & curl.exe -s -X PUT "$api/accounts/$acc/workers/scripts/$Script" -H $auth `
        -F "metadata=@$metaFile;type=application/json" `
        -F "worker.js=@$(Join-Path $PSScriptRoot 'worker.js');filename=worker.js;type=application/javascript+module"
    return $answer | ConvertFrom-Json
}
# The Durable Object class is created once (migration v1); later deploys just update the code.
$result = Upload $true
if (-not $result.success) { $result = Upload $false }
if (-not $result.success) { throw "upload failed: $($result.errors | ConvertTo-Json -Compress)" }

$alphabet = 'abcdefghijkmnpqrstuvwxyz23456789'
$key = if (Test-Path $Out) { (Get-Content $Out -Raw | ConvertFrom-Json).key } else { -join (1..40 | ForEach-Object { $alphabet[(Get-Random -Maximum 32)] }) }
Invoke-RestMethod -Method Put -Headers $h -ContentType 'application/json' `
    -Body (@{ name = 'KEY'; text = $key; type = 'secret_text' } | ConvertTo-Json) "$api/accounts/$acc/workers/scripts/$Script/secrets" | Out-Null
Invoke-RestMethod -Method Post -Headers $h -ContentType 'application/json' -Body '{"enabled":true}' `
    "$api/accounts/$acc/workers/scripts/$Script/subdomain" | Out-Null

$url = "https://$Script.$sub.workers.dev/"
[IO.File]::WriteAllText($Out, (@{ url = $url; key = $key } | ConvertTo-Json))
"deployed: $url (key in $Out). A new workers.dev name may take a minute to get its certificate."
