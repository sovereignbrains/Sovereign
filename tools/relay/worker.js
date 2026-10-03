// Subscription relay: fetches a subscription for the tray from Cloudflare's
// network, so the subscription's server sees Cloudflare's address, not the
// user's. Deployed in the user's own Cloudflare account (never a shared one:
// whoever runs it sees the subscription links). See README.md.
//
// POST / with header X-Relay-Key: <the KEY secret> and a JSON body
//   {"url": "https://...", "headers": {"User-Agent": "...", "x-hwid": "..."}}
// fetches it once. A small answer comes back right away: the subscription's
// status and body, and the headers that describe a subscription. A bigger one
// is kept for a few minutes and the answer says so (X-Relay-Job, -Size,
// -Chunk, empty body): GET /chunk?job=<id>&n=<k> (same key) gives its pieces.
// Why pieces: Russian networks cut connections to Cloudflare after about
// 24 KB (measured 03.10.2026) - every piece comes over a connection of its own.
// Anything else - a wrong key, another method or path - gets a plain 404.

import { DurableObject } from "cloudflare:workers";

const PASSED_REQUEST_HEADERS = new Set(["user-agent", "x-hwid", "accept", "accept-language"]);
const PASSED_RESPONSE_HEADERS = [
  "content-type",
  "content-disposition",
  "profile-title",
  "profile-update-interval",
  "profile-web-page-url",
  "subscription-userinfo",
  "support-url",
  "announce",
  "routing",
];
const MAX_BODY = 4 * 1024 * 1024;
const CHUNK = 16000;           // well under the ~24 KB a throttled connection lets through
const KEEP_MS = 5 * 60 * 1000;  // a job's pieces are gone after this

function notFound() {
  return new Response("Not Found", { status: 404 });
}

// Constant-time comparison: the key isn't guessable byte by byte from timing.
function sameKey(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length === 0 || a.length !== b.length) {
    return false;
  }
  let diff = 0;
  for (let i = 0; i < a.length; i++) {
    diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  }
  return diff === 0;
}

function plain(body, status, headers) {
  // no-transform: Cloudflare mustn't recompress a piece (its size is the point).
  const h = new Headers(headers);
  h.set("Cache-Control", "no-store, no-transform");
  return new Response(body, { status, headers: h });
}

async function fetchSubscription(job) {
  let target;
  try {
    target = new URL(job.url);
  } catch {
    return { error: plain("bad url", 400) };
  }
  if (target.protocol !== "https:" && target.protocol !== "http:") {
    return { error: plain("bad url", 400) };
  }
  const headers = new Headers();
  for (const [name, value] of Object.entries(job.headers ?? {})) {
    if (PASSED_REQUEST_HEADERS.has(name.toLowerCase()) && typeof value === "string") {
      headers.set(name, value);
    }
  }
  let upstream;
  try {
    upstream = await fetch(target, { headers, redirect: "follow" });
  } catch (e) {
    return { error: plain("fetch failed: " + e.message, 502) };
  }
  const body = new Uint8Array(await upstream.arrayBuffer());
  if (body.byteLength > MAX_BODY) {
    return { error: plain("too large", 502) };
  }
  const out = { "X-Relay-Status": String(upstream.status) };
  for (const name of PASSED_RESPONSE_HEADERS) {
    const value = upstream.headers.get(name);
    if (value !== null) {
      out[name] = value;
    }
  }
  return { status: upstream.status, headers: out, body };
}

// One big answer's pieces, for a few minutes.
export class Job extends DurableObject {
  async keep(body) {
    let pieces = {};
    let count = 0;
    for (let n = 0; n * CHUNK < body.byteLength; n++) {
      pieces["c" + n] = body.slice(n * CHUNK, (n + 1) * CHUNK);
      if (++count === 128) {  // storage.put takes at most 128 keys at once
        await this.ctx.storage.put(pieces);
        pieces = {};
        count = 0;
      }
    }
    if (count > 0) {
      await this.ctx.storage.put(pieces);
    }
    await this.ctx.storage.setAlarm(Date.now() + KEEP_MS);
  }

  async piece(n) {
    return (await this.ctx.storage.get("c" + n)) ?? null;
  }

  async alarm() {
    await this.ctx.storage.deleteAll();
  }
}

export default {
  async fetch(request, env) {
    if (!sameKey(request.headers.get("X-Relay-Key"), env.KEY)) {
      return notFound();
    }
    const url = new URL(request.url);

    if (request.method === "GET" && url.pathname === "/chunk") {
      const job = url.searchParams.get("job") ?? "";
      const n = Number(url.searchParams.get("n"));
      if (!/^[0-9a-f-]{36}$/.test(job) || !Number.isInteger(n) || n < 0) {
        return plain("bad request", 400);
      }
      const piece = await env.JOBS.get(env.JOBS.idFromName(job)).piece(n);
      return piece === null ? plain("gone", 410) : plain(piece, 200, { "Content-Type": "application/octet-stream" });
    }

    if (request.method !== "POST" || url.pathname !== "/") {
      return notFound();
    }
    let job;
    try {
      job = await request.json();
    } catch {
      return plain("bad request", 400);
    }
    const result = await fetchSubscription(job);
    if (result.error) {
      return result.error;
    }
    if (result.body.byteLength <= CHUNK) {
      return plain(result.body, result.status, result.headers);
    }
    const id = crypto.randomUUID();
    await env.JOBS.get(env.JOBS.idFromName(id)).keep(result.body);
    return plain(null, result.status, {
      ...result.headers,
      "X-Relay-Job": id,
      "X-Relay-Size": String(result.body.byteLength),
      "X-Relay-Chunk": String(CHUNK),
    });
  },
};
