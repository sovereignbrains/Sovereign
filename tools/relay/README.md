# Subscription relay

With no connection up, a subscription can't be fetched through the proxy, and
fetched directly its server learns the user's real IP address. Sovereign then
fetches it through this relay: a Cloudflare Worker in **your own** Cloudflare
account. The subscription's server sees a Cloudflare address
(`2a06:98c0:3600::103` and the like) and your workers.dev name in the
`CF-Worker` header - not your IP. Cloudflare sees your IP.

Never use someone else's relay, and don't run one for others: whoever runs it
sees every subscription link that passes through, tokens included.

## What it does

`POST /` with `X-Relay-Key` and `{"url": ..., "headers": {"User-Agent": ..., "x-hwid": ...}}`
fetches the subscription once, passing on only those headers. A small answer
(up to 16000 bytes) comes back whole, with the subscription's own headers
(`Profile-Title`, `Profile-Update-Interval`, `Subscription-Userinfo`...) and
`X-Relay-Status`. A bigger one is kept for five minutes in a Durable Object and
fetched piece by piece (`GET /chunk?job=...&n=...`), **each piece over a
connection of its own**: Russian networks cut connections to Cloudflare after
about 24 KB (measured 03.10.2026 - 12 KB came whole, 60 KB and 200 KB stopped
at 24576 bytes; a 164 KB file came whole in 11 pieces in 3.3 s). Anything else -
no key, a wrong key, another method or path - gets a plain 404.

Free plan limits are far beyond what this needs (100 000 requests a day).

## Deploying

1. In the Cloudflare dashboard, open Workers once and pick a workers.dev
   subdomain - a neutral name, nothing that points to you or to Sovereign
   (subscription servers see it).
2. Create an API token with **Account -> Workers Scripts: Edit** and
   **Account -> Account Settings: Read**.
3. `$env:CLOUDFLARE_API_TOKEN = '...'; .\deploy.ps1`
4. In Sovereign: Настройки -> Подписки без подключения -> the address and the
   key from `relay.json`. The tray keeps the key encrypted for your Windows
   user (DPAPI).

## How the tray uses it

- The connection is up and runs a config whose rules send the subscription's
  host through the proxy: fetched as before, through the proxy.
- Otherwise, with a relay set: through the relay.
- Otherwise not at all. A scheduled refresh waits quietly for a connection or a
  relay; a new subscription is kept, marked "не скачана", and its menu offers
  "Скачать напрямую" (after a warning that its server will see your IP).
