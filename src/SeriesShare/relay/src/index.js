// MPC-HC series-player — signaling relay for "Спільний перегляд" (Cloudflare Worker + Durable Object).
//
// Only the WebRTC handshake passes through here (a few KB per viewer); audio/video flow directly
// between the player and the viewer. Nothing is stored except the room's host key.
//
//   GET  /r/<room>                     viewer page
//   GET  /host?room=<room>&key=<key>   WebSocket from mpc-share (the player)
//   POST /offer?room=<room>            viewer asks for a WebRTC offer      -> {id, sdp}
//   POST /answer?room=<room>&id=<id>   viewer sends its answer (SDP body)  -> 204

import { VIEWER_HTML } from "./viewer.js";
import { ICON_192, ICON_512 } from "./icons.js";

// Installable web app: "Наше кіно" on the viewer's home screen opens straight into the room.
function manifest(room) {
  return JSON.stringify({
    name: "Наше кіно",
    short_name: "Наше кіно",
    description: "Спільний перегляд з MPC-HC",
    id: `/r/${room}`,
    start_url: `/r/${room}`,
    scope: `/r/${room}`,
    display: "standalone",
    orientation: "any",
    background_color: "#000000",
    theme_color: "#000000",
    lang: "uk",
    icons: [
      { src: "/icon-192-v4.png", sizes: "192x192", type: "image/png", purpose: "any maskable" },
      { src: "/icon-512-v4.png", sizes: "512x512", type: "image/png", purpose: "any maskable" },
    ],
  });
}
// The service worker only makes the page installable; it never caches (the stream is live).
const SW_JS = "self.addEventListener(\"install\", () => self.skipWaiting());\nself.addEventListener(\"activate\", (e) => e.waitUntil(self.clients.claim()));\nself.addEventListener(\"fetch\", () => {});\n";

function png(b64) {
  const bin = atob(b64), bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new Response(bytes, { headers: { "Content-Type": "image/png", "Cache-Control": "public, max-age=3600" } });
}

const ROOM_RE = /^[a-z0-9]{8,32}$/;
const STUN_ONLY = [{ urls: ["stun:stun.cloudflare.com:3478", "stun:stun.l.google.com:19302"] }];

// Short-lived TURN credentials from Cloudflare Realtime (secrets TURN_KEY_ID / TURN_KEY_API_TOKEN).
// Used when a direct peer-to-peer path is impossible (CGNAT, strict firewalls).
async function iceServers(env, ttl = 6 * 3600) {
  const keyId = (env.TURN_KEY_ID || "").trim();
  const token = (env.TURN_KEY_API_TOKEN || "").trim();
  if (!keyId || !token) { return STUN_ONLY; }
  try {
    const r = await fetch(`https://rtc.live.cloudflare.com/v1/turn/keys/${keyId}/credentials/generate-ice-servers`, {
      method: "POST",
      headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
      body: JSON.stringify({ ttl }),
    });
    if (!r.ok) { console.warn(`TURN credentials: HTTP ${r.status}`); return STUN_ONLY; }
    const data = await r.json();
    if (!data.iceServers || !data.iceServers.length) return STUN_ONLY;
    return withIpv4Turn(data.iceServers);
  } catch (e) {
    console.warn(`TURN credentials: ${e}`);
    return STUN_ONLY;
  }
}

// A viewer on an IPv6 network (many mobile operators) reaches turn.cloudflare.com over IPv6 and gets an
// IPv6 relay address, which cannot pair with the player's IPv4-only candidates: ICE then has no pairs at
// all. Adding the server's IPv4 address as well yields IPv4 relay candidates (via the phone's 464XLAT).
// turns: URLs keep the hostname, since TLS needs it for the certificate check.
const TURN_IPV4_FALLBACK = "141.101.90.1"; // turn.cloudflare.com A record (anycast)
async function withIpv4Turn(servers) {
  let ip = "";
  try {
    const r = await fetch("https://cloudflare-dns.com/dns-query?name=turn.cloudflare.com&type=A", { headers: { Accept: "application/dns-json" } });
    const a = ((await r.json()).Answer || []).find((x) => x.type === 1);
    ip = a ? a.data : "";
  } catch (e) {}
  if (!/^\d+\.\d+\.\d+\.\d+$/.test(ip)) ip = TURN_IPV4_FALLBACK;
  return servers.flatMap((s) => {
    const v4 = [].concat(s.urls).filter((u) => u.startsWith("turn:turn.cloudflare.com")).map((u) => u.replace("turn.cloudflare.com", ip));
    return v4.length ? [s, { ...s, urls: v4 }] : [s];
  });
}

function cors(headers = {}) {
  return {
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type",
    "Cache-Control": "no-store",
    ...headers,
  };
}

function json(obj, status = 200) {
  return new Response(JSON.stringify(obj), { status, headers: cors({ "Content-Type": "application/json" }) });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: cors() });

    const mf = url.pathname.match(/^\/r\/([a-z0-9]{8,32})\/manifest\.webmanifest$/);
    if (mf) return new Response(manifest(mf[1]), { headers: { "Content-Type": "application/manifest+json", "Cache-Control": "no-store" } });
    // Versioned names: phones cache icons hard, a new file name makes them fetch the new picture.
    if (/^\/icon-192(-v\d+)?\.png$/.test(url.pathname)) return png(ICON_192);
    if (/^\/icon-512(-v\d+)?\.png$/.test(url.pathname)) return png(ICON_512);
    if (url.pathname === "/sw.js") return new Response(SW_JS, { headers: { "Content-Type": "text/javascript", "Cache-Control": "no-store" } });

    const page = url.pathname.match(/^\/r\/([a-z0-9]{8,32})\/?$/);
    if (page && request.method === "GET") {
      return new Response(VIEWER_HTML, {
        headers: {
          "Content-Type": "text/html; charset=utf-8",
          "Cache-Control": "no-store",
          "Referrer-Policy": "no-referrer",
          "X-Robots-Tag": "noindex, nofollow",
        },
      });
    }

    if (url.pathname === "/host" || url.pathname === "/offer" || url.pathname === "/answer" || url.pathname === "/ice" || url.pathname === "/diag") {
      const room = url.searchParams.get("room") || "";
      if (!ROOM_RE.test(room)) return json({ error: "bad room" }, 400);
      const stub = env.ROOMS.get(env.ROOMS.idFromName(room));
      return stub.fetch(request);
    }

    if (url.pathname === "/" || url.pathname === "/health") {
      return new Response("MPC-HC series-player relay", { headers: cors({ "Content-Type": "text/plain; charset=utf-8" }) });
    }
    return new Response("Not found", { status: 404, headers: cors() });
  },
};

export class Room {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.host = null;        // WebSocket to the player
    this.pending = new Map(); // reqId -> {resolve, timer}
  }

  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/host") return this.acceptHost(request, url);
    if (url.pathname === "/ice") {
      // Viewers get TURN credentials only while the player is online (no free TURN for strangers).
      if (!this.host) return json({ error: "offline" }, 404);
      const list = await iceServers(this.env);
      return json({ iceServers: list });
    }
    if (url.pathname === "/diag") return this.diag(request, url);
    if (url.pathname === "/offer" && request.method === "POST") return this.relay({ type: "offer" });
    if (url.pathname === "/answer" && request.method === "POST") {
      const id = url.searchParams.get("id") || "";
      const sdp = await request.text();
      if (!id || sdp.length > 64 * 1024) return json({ error: "bad request" }, 400);
      return this.relay({ type: "answer", id, sdp });
    }
    return json({ error: "not found" }, 404);
  }

  // Viewer connection reports (states, candidate types, ICE errors; no addresses). Only the room's host key can read them.
  async diag(request, url) {
    if (request.method === "POST") {
      const body = await request.text();
      if (body.length > 16 * 1024) return json({ error: "too large" }, 413);
      let report;
      try { report = JSON.parse(body); } catch (e) { return json({ error: "bad json" }, 400); }
      const cf = request.cf || {};
      report.at = new Date().toISOString();
      report.net = { country: cf.country, as: cf.asOrganization };
      const list = (await this.state.storage.get("diag")) || [];
      list.push(report);
      await this.state.storage.put("diag", list.slice(-20));
      return new Response(null, { status: 204, headers: cors() });
    }
    const stored = await this.state.storage.get("key");
    if (!stored || url.searchParams.get("key") !== stored) return json({ error: "forbidden" }, 403);
    return json((await this.state.storage.get("diag")) || []);
  }

  async acceptHost(request, url) {
    if (request.headers.get("Upgrade") !== "websocket") return json({ error: "websocket expected" }, 426);
    const key = url.searchParams.get("key") || "";
    if (key.length < 16) return json({ error: "bad key" }, 403);
    // The first player that claims a room owns it; afterwards only the same key may host it.
    const stored = await this.state.storage.get("key");
    if (stored && stored !== key) return json({ error: "room belongs to another player" }, 403);
    if (!stored) await this.state.storage.put("key", key);

    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    server.accept();
    if (this.host) {
      try { this.host.close(4000, "replaced"); } catch (e) {}
    }
    this.host = server;
    server.addEventListener("message", (event) => {
      let msg;
      try { msg = JSON.parse(event.data); } catch (e) { return; }
      if (msg.type === "ping") { try { server.send('{"type":"pong"}'); } catch (e) {} return; }
      if (msg.type === "get-ice") {
        // The player gets TURN too: with relay candidates on both sides the media can always meet inside
        // Cloudflare, whatever the viewer's network (CGNAT, IPv6-only, filtered UDP).
        iceServers(this.env).then((list) => {
          try { server.send(JSON.stringify({ type: "ice", iceServers: list })); } catch (e) {}
        });
        return;
      }
      const p = msg.reqId && this.pending.get(msg.reqId);
      if (p) {
        clearTimeout(p.timer);
        this.pending.delete(msg.reqId);
        p.resolve(msg);
      }
    });
    const gone = () => {
      if (this.host === server) this.host = null;
      for (const [id, p] of this.pending) { clearTimeout(p.timer); p.resolve({ error: "offline" }); }
      this.pending.clear();
    };
    server.addEventListener("close", gone);
    server.addEventListener("error", gone);
    return new Response(null, { status: 101, webSocket: client });
  }

  relay(message) {
    if (!this.host) return json({ error: "offline" }, 404);
    const reqId = crypto.randomUUID();
    const result = new Promise((resolve) => {
      const timer = setTimeout(() => { this.pending.delete(reqId); resolve({ error: "timeout" }); }, 15000);
      this.pending.set(reqId, { resolve, timer });
    });
    try {
      this.host.send(JSON.stringify({ ...message, reqId }));
    } catch (e) {
      this.pending.delete(reqId);
      return json({ error: "offline" }, 404);
    }
    return result.then((msg) => {
      if (msg.error === "offline") return json({ error: "offline" }, 404);
      if (msg.error) return json({ error: msg.error }, message.type === "answer" ? 410 : 502);
      if (message.type === "offer") return json({ id: msg.id, sdp: msg.sdp });
      return new Response(null, { status: 204, headers: cors() });
    });
  }
}
