// Subscription relay: fetches a subscription for the tray from Cloudflare's
// network, so the subscription's server sees Cloudflare's address, not the
// user's. Whoever runs it sees the subscription links (tokens included): one
// in the user's own account, or the one Sovereign offers as an opt-in. It keeps
// nothing but a big answer's pieces for five minutes, and logs nothing. See README.md.
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
// The key ships inside the open-source client, so it only keeps scanners out;
// what stops abuse is the per-address limit (LIMIT binding) and the rules in
// allowedTarget(): this is a subscription fetcher, not an open proxy.

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

// Only a public host name: no addresses, no local names, no credentials in the link.
function allowedTarget(target) {
  if (target.protocol !== "https:" && target.protocol !== "http:") {
    return false;
  }
  if (target.username !== "" || target.password !== "") {
    return false;
  }
  const host = target.hostname.toLowerCase();
  if (host.startsWith("[") || /^[0-9.x]+$/.test(host) || !host.includes(".")) {
    return false;  // an IPv6 or IPv4 literal (any spelling), or a name without a dot
  }
  return !/\.(local|localhost|internal|lan|home|corp|workers\.dev|pages\.dev)$/.test(host);
}

function plain(body, status, headers) {
  // no-transform: Cloudflare mustn't recompress a piece (its size is the point).
  const h = new Headers(headers);
  h.set("Cache-Control", "no-store, no-transform");
  return new Response(body, { status, headers: h });
}

// The body, or null if it is over MAX_BODY - read piece by piece, so a huge answer never sits in memory.
async function readCapped(response) {
  const declared = Number(response.headers.get("content-length"));
  if (declared > MAX_BODY || !response.body) {
    return declared > MAX_BODY ? null : new Uint8Array(0);
  }
  const reader = response.body.getReader();
  const parts = [];
  let size = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) {
      break;
    }
    size += value.byteLength;
    if (size > MAX_BODY) {
      await reader.cancel();
      return null;
    }
    parts.push(value);
  }
  const body = new Uint8Array(size);
  let at = 0;
  for (const part of parts) {
    body.set(part, at);
    at += part.byteLength;
  }
  return body;
}

async function fetchSubscription(job) {
  let target;
  try {
    target = new URL(job.url);
  } catch {
    return { error: plain("bad url", 400) };
  }
  if (!allowedTarget(target)) {
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
  const body = await readCapped(upstream);
  if (body === null) {
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
    // Per address, per location (the LIMIT binding: 12 a minute); a missing binding means no limit.
    if (env.LIMIT && !(await env.LIMIT.limit({ key: request.headers.get("CF-Connecting-IP") ?? "-" })).success) {
      return plain("slow down", 429);
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
