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
    return data.iceServers && data.iceServers.length ? data.iceServers : STUN_ONLY;
  } catch (e) {
    console.warn(`TURN credentials: ${e}`);
    return STUN_ONLY;
  }
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

    if (url.pathname === "/host" || url.pathname === "/offer" || url.pathname === "/answer" || url.pathname === "/ice") {
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
    if (url.pathname === "/offer" && request.method === "POST") return this.relay({ type: "offer" });
    if (url.pathname === "/answer" && request.method === "POST") {
      const id = url.searchParams.get("id") || "";
      const sdp = await request.text();
      if (!id || sdp.length > 64 * 1024) return json({ error: "bad request" }, 400);
      return this.relay({ type: "answer", id, sdp });
    }
    return json({ error: "not found" }, 404);
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
        // The player answers on its own (direct/STUN) path; the viewer relays through TURN when it has to,
        // and a TURN relay address is reachable from anywhere, so one relayed side is enough.

        try { server.send(JSON.stringify({ type: "ice", iceServers: STUN_ONLY })); } catch (e) {}
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
