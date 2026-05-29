// ─────────────────────────────────────────────────────────────────────────
//  Cloudflare Worker: WebSocket → TCP мост для Telegram (и не только).
//
//  Принимает WebSocket-соединение на /apiws?dst=HOST:PORT, открывает TCP-сокет
//  к указанному хосту через Cloudflare connect() и двунаправленно гоняет байты.
//  Клиент (наш локальный прокси) заворачивает MTProto в этот WebSocket — так
//  трафик к Telegram идёт через IP Cloudflare (которые провайдер не режет).
//
//  Деплой: Cloudflare → Workers & Pages → Create → Hello World → вставить это →
//  Deploy. URL будет вида https://<имя>.<аккаунт>.workers.dev
// ─────────────────────────────────────────────────────────────────────────

import { connect } from "cloudflare:sockets";

// Разрешённые цели — только дата-центры Telegram (безопасность: чтобы воркер
// не превратился в открытый прокси куда угодно).
const TELEGRAM_DC = new Set([
  "149.154.175.50",   // DC1
  "149.154.167.51",   // DC2
  "149.154.175.100",  // DC3
  "149.154.167.91",   // DC4
  "149.154.171.5",    // DC5
  "149.154.167.92",   // DC2 alt
  "149.154.175.53",   // DC1 alt
]);

export default {
  async fetch(request) {
    const url = new URL(request.url);

    // Только WebSocket-апгрейд.
    if ((request.headers.get("Upgrade") || "").toLowerCase() !== "websocket") {
      return new Response("AM.SALES TG proxy: expected websocket", { status: 426 });
    }

    // Целевой хост:порт из ?dst=
    const dst = url.searchParams.get("dst") || "";
    const [host, portStr] = dst.split(":");
    const port = parseInt(portStr || "443", 10);

    // Пускаем только к дата-центрам Telegram.
    if (!host || !TELEGRAM_DC.has(host)) {
      return new Response("forbidden destination", { status: 403 });
    }

    // Поднимаем WebSocket-пару (клиент ↔ воркер).
    const pair = new WebSocketPair();
    const client = pair[0];
    const server = pair[1];
    server.accept();

    // Открываем TCP к дата-центру Telegram.
    let socket;
    try {
      socket = connect({ hostname: host, port: port });
    } catch (e) {
      server.close(1011, "connect failed");
      return new Response(null, { status: 101, webSocket: client });
    }

    const writer = socket.writable.getWriter();
    const reader = socket.readable.getReader();

    // WS → TCP: что пришло от клиента, пишем в сокет Telegram.
    server.addEventListener("message", async (evt) => {
      try {
        const data = evt.data instanceof ArrayBuffer
          ? new Uint8Array(evt.data)
          : new TextEncoder().encode(evt.data);
        await writer.write(data);
      } catch (e) { try { server.close(); } catch {} }
    });
    server.addEventListener("close", () => { try { writer.close(); } catch {} });

    // TCP → WS: что пришло от Telegram, шлём клиенту.
    (async () => {
      try {
        while (true) {
          const { value, done } = await reader.read();
          if (done) break;
          server.send(value);
        }
      } catch (e) {} finally { try { server.close(); } catch {} }
    })();

    return new Response(null, { status: 101, webSocket: client });
  }
};
