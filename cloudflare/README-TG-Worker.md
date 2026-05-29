# Telegram через Cloudflare Worker — настройка

Воркер проксирует Telegram через IP Cloudflare (которые провайдер не режет).
Схема: Telegram → локальный прокси (tg-ws-proxy) → CF Worker → дата-центры Telegram.

## Шаг 1. Задеплоить Worker (одноразово)

1. Зайди на https://dash.cloudflare.com → **Compute (Workers)** → **Workers & Pages**.
2. **Create application** → **Create Worker** (шаблон Hello World).
3. Дай имя, например `tg-amsales`. **Deploy**.
4. Открой его → **Edit code**.
5. Удали весь дефолтный код, вставь содержимое файла **`tg-worker.js`** (лежит рядом).
6. **Deploy**.
7. Скопируй домен воркера — вид `tg-amsales.<твой-аккаунт>.workers.dev`.

## Шаг 2. Вписать в tg-ws-proxy (та программа в трее)

В настройках tg-ws-proxy:
1. Раздел **Cloudflare Proxy** → галка **«Включить CF-прокси»**.
2. Раздел **Cloudflare Worker** → поле **«Cloudflare Worker домен»** → вставь
   `tg-amsales.<аккаунт>.workers.dev` (без https://).
3. **Сохранить**.
4. Telegram → Настройки → Данные и память → Прокси → подключи локальный
   `127.0.0.1:1443` (tg-ws-proxy выдаёт ссылку сам).

## Проверка

- В tg-ws-proxy кнопка **«Тест»** возле CF-прокси должна стать зелёной.
- Telegram заработает даже если прямой доступ к его IP заблокирован.

## Важно

- Воркер пускает трафик ТОЛЬКО к дата-центрам Telegram (список в `tg-worker.js`).
  Это защита, чтобы он не стал открытым прокси.
- Бесплатный план Cloudflare: 100 000 запросов/день — для личного TG хватает.
- `workers.dev` иногда сам под DPI — если домен не открывается, добавь
  `workers.dev` в обход (zapret) или привяжи свой домен к воркеру.
