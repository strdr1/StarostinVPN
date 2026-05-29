#include "VlessController.h"
#include "Store.h"
#include "AdminCheck.h"

#ifdef Q_OS_WIN
#  include <windows.h>  // CREATE_NO_WINDOW для скрытия консоли sing-box
#endif

#include <QProcess>
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QRegularExpression>

// ─────────────────────────────────────────────────────────────────────────
//  Реализация VlessController.
//
//  Конфиг sing-box строим по образцу рабочего Hiddify-конфига:
//   - inbound TUN (весь системный трафик заворачивается в туннель);
//   - outbound vless с reality;
//   - правила маршрута: домены .ru и geoip-ru → direct (мимо VPN).
// ─────────────────────────────────────────────────────────────────────────

VlessController::VlessController(Store *store, QObject *parent)
    : QObject(parent), m_store(store)
{
    // Keep-alive: каждые 15 сек проверяем что туннель жив и проходит трафик.
    m_keepAliveTimer = new QTimer(this);
    m_keepAliveTimer->setInterval(15000);
    connect(m_keepAliveTimer, &QTimer::timeout, this, &VlessController::keepAliveTick);

    if (m_store) {
        // Восстанавливаем настройку маршрутизации.
        m_routeRuDirect = m_store->setting(
            QStringLiteral("routeRuDirect"), true).toBool();
        // tg-прокси ссылка: из настроек либо дефолтная (наш mtg-сервер).
        m_tgProxyLink = m_store->setting(QStringLiteral("tgProxyLink"),
            QStringLiteral("tg://proxy?server=155.212.228.232&port=8443&secret=7pwrj8t67zLQ4Q3gAljWB4tnb29nbGUuY29t")).toString();
        // Сайты-исключения.
        m_bypassSites = m_store->setting(QStringLiteral("bypassSites")).toStringList();
        // Использовать ли zapret (по умолчанию да).
        m_useZapret = m_store->setting(QStringLiteral("useZapret"), true).toBool();
        // Загружаем сохранённые ключи.
        QStringList saved = m_store->keys();

        // Первый запуск (ключей нет) — добавим два встроенных ключа
        // нашего сервера, чтобы пользователь не вводил их руками.
        if (saved.isEmpty()) {
            const QStringList defaults = {
                QStringLiteral("vless://74553d23-3ff1-4e25-b06f-ff24e22f97ba@78.17.103.241:443"
                               "?type=tcp&security=reality&sni=ya.ru&fp=firefox"
                               "&pbk=TiUgA_8KxozBVo35PWZczwodV5aDoypTbQDRyEuwpSU"
                               "&sid=23f62cc284c8fbb4#STAROSTIN-1"),
                QStringLiteral("vless://048f5a49-476b-4b72-8f3c-034103caf7bc@78.17.103.241:443"
                               "?type=tcp&security=reality&sni=ya.ru&fp=firefox"
                               "&pbk=TiUgA_8KxozBVo35PWZczwodV5aDoypTbQDRyEuwpSU"
                               "&sid=23f62cc284c8fbb4#STAROSTIN-2"),
            };
            for (const QString &uri : defaults)
                m_store->addKey(uri);
            saved = defaults;
        }

        for (const QString &uri : saved) {
            const Server s = parseVless(uri);
            if (s.valid)
                m_serversData.append(s);
        }
        rebuildServerList();
    }
}

VlessController::~VlessController()
{
    if (m_proc) {
        m_proc->kill();
        m_proc->waitForFinished(2000);
    }
}

QString VlessController::engineDir() const
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/engine");
}
QString VlessController::singBoxPath() const
{
    return engineDir() + QStringLiteral("/vpn/sing-box.exe");
}
QString VlessController::configPath() const
{
    // Кладём конфиг рядом с движком (sing-box запускается оттуда).
    return engineDir() + QStringLiteral("/vpn/config.json");
}

void VlessController::setConnected(bool c)
{
    if (m_connected == c) return;
    m_connected = c;
    emit connectedChanged();
}
void VlessController::setConnecting(bool c)
{
    if (m_connecting == c) return;
    m_connecting = c;
    emit connectingChanged();
}
void VlessController::setStatus(const QString &t)
{
    if (m_statusText == t) return;
    m_statusText = t;
    emit statusTextChanged();
}

void VlessController::setCurrentServer(int idx)
{
    if (idx < 0 || idx >= m_serversData.size() || idx == m_currentServer)
        return;
    m_currentServer = idx;
    emit currentServerChanged();
}

void VlessController::setRouteRuDirect(bool v)
{
    if (m_routeRuDirect == v) return;
    m_routeRuDirect = v;
    if (m_store) m_store->setSetting(QStringLiteral("routeRuDirect"), v);
    emit routeRuDirectChanged();
}

void VlessController::setTgProxyLink(const QString &v)
{
    if (m_tgProxyLink == v) return;
    m_tgProxyLink = v;
    if (m_store) m_store->setSetting(QStringLiteral("tgProxyLink"), v);
    emit tgProxyLinkChanged();
}

void VlessController::setUseZapret(bool v)
{
    if (m_useZapret == v) return;
    m_useZapret = v;
    if (m_store) m_store->setSetting(QStringLiteral("useZapret"), v);
    emit useZapretChanged();
}

// Запуск локального Telegram-прокси (tg-ws-proxy). Ищем .exe в типичных
// местах. Он поднимает MTProto на 127.0.0.1 и гонит TG через Cloudflare.
void VlessController::launchTgProxy()
{
    const QStringList candidates = {
        QDir::homePath() + QStringLiteral("/Downloads/TgWsProxy_windows.exe"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/TgWsProxy_windows.exe"),
        QDir::homePath() + QStringLiteral("/Desktop/TgWsProxy_windows.exe"),
    };
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path)) {
            QProcess::startDetached(path, {}, QFileInfo(path).absolutePath());
            setStatus(QStringLiteral("Telegram-прокси запущен"));
            return;
        }
    }
    emit errorOccurred(QStringLiteral(
        "TgWsProxy не найден. Положите TgWsProxy_windows.exe в Downloads."));
}

// ── Удаление сервера из списка ──────────────────────────────────────────
void VlessController::removeServer(int index)
{
    if (index < 0 || index >= m_serversData.size())
        return;
    m_serversData.removeAt(index);
    // Пересохраняем список ключей в БД.
    if (m_store) {
        QStringList keys;
        for (const Server &s : m_serversData)
            keys << s.rawUri;
        m_store->setKeys(keys);
    }
    if (m_currentServer >= m_serversData.size())
        m_currentServer = qMax(0, m_serversData.size() - 1);
    rebuildServerList();
    setStatus(QStringLiteral("Сервер удалён"));
}

// Изменить ключ сервера: парсим новый vless:// и заменяем на месте.
void VlessController::editServer(int index, const QString &newUri)
{
    if (index < 0 || index >= m_serversData.size())
        return;
    const Server s = parseVless(newUri);
    if (!s.valid) {
        emit errorOccurred(QStringLiteral("Неверный ключ vless://"));
        return;
    }
    m_serversData[index] = s;
    if (m_store) {
        QStringList keys;
        for (const Server &x : m_serversData) keys << x.rawUri;
        m_store->setKeys(keys);
    }
    rebuildServerList();
    setStatus(QStringLiteral("Ключ изменён: %1").arg(s.name));
}

// Сгенерировать QR-код ключа через встроенный Python (qrcode).
// Возвращает file:// путь к PNG для отображения в QML Image.
QString VlessController::qrForServer(int index)
{
    if (index < 0 || index >= m_serversData.size())
        return QString();
    const QString uri = m_serversData.at(index).rawUri;
    const QString dir = QCoreApplication::applicationDirPath();
    const QString py = dir + QStringLiteral("/engine/python/python.exe");
    const QString out = dir + QStringLiteral("/engine/qr.png");

    if (!QFileInfo::exists(py))
        return QString();

    // Однострочный Python: qrcode → PNG. Если qrcode нет — вернём пусто.
    const QString code = QStringLiteral(
        "import sys\n"
        "try:\n"
        " import qrcode\n"
        " img=qrcode.make(sys.argv[1])\n"
        " img.save(sys.argv[2])\n"
        "except Exception as e:\n"
        " sys.exit(1)\n");
    const QString scriptPath = dir + QStringLiteral("/engine/_qr.py");
    QFile sf(scriptPath);
    if (sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        sf.write(code.toUtf8());
        sf.close();
    }
    QProcess p;
    p.start(py, {scriptPath, uri, out});
    p.waitForFinished(5000);
    if (QFileInfo::exists(out) && p.exitCode() == 0)
        return QStringLiteral("file:///") + out;
    return QString();
}

// ── Сайты-исключения (мимо VPN и zapret) ────────────────────────────────
void VlessController::addBypassSite(const QString &domain)
{
    QString d = domain.trimmed().toLower();
    // Чистим от схемы/путей: оставляем только домен.
    d.remove(QRegularExpression(QStringLiteral("^https?://")));
    d = d.section('/', 0, 0);
    if (d.isEmpty() || m_bypassSites.contains(d))
        return;
    m_bypassSites << d;
    if (m_store) m_store->setSetting(QStringLiteral("bypassSites"), m_bypassSites);
    syncZapretExclude();
    emit bypassSitesChanged();
}

void VlessController::removeBypassSite(int index)
{
    if (index < 0 || index >= m_bypassSites.size())
        return;
    m_bypassSites.removeAt(index);
    if (m_store) m_store->setSetting(QStringLiteral("bypassSites"), m_bypassSites);
    syncZapretExclude();
    emit bypassSitesChanged();
}

// Пишем сайты-исключения в zapret list-exclude-user.txt, чтобы zapret их
// не трогал (не фрагментировал). engine/lists рядом с .exe.
void VlessController::syncZapretExclude()
{
    const QString path = QCoreApplication::applicationDirPath()
        + QStringLiteral("/engine/lists/list-exclude-user.txt");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    for (const QString &d : m_bypassSites)
        f.write((d + "\n").toUtf8());
    f.close();
}

// ── Парсинг vless://uuid@host:port?params#name ──────────────────────────
VlessController::Server VlessController::parseVless(const QString &uri) const
{
    Server s;
    const QString trimmed = uri.trimmed();
    if (!trimmed.startsWith(QStringLiteral("vless://")))
        return s;  // не наш формат

    QUrl url(trimmed);
    if (!url.isValid())
        return s;

    s.uuid = url.userName();
    s.host = url.host();
    s.port = url.port(443);
    s.rawUri = trimmed;

    const QUrlQuery q(url.query());
    s.type        = q.queryItemValue(QStringLiteral("type"));
    s.security    = q.queryItemValue(QStringLiteral("security"));
    s.sni         = q.queryItemValue(QStringLiteral("sni"));
    s.publicKey   = q.queryItemValue(QStringLiteral("pbk"));
    s.shortId     = q.queryItemValue(QStringLiteral("sid"));
    s.fingerprint = q.queryItemValue(QStringLiteral("fp"));
    s.flow        = q.queryItemValue(QStringLiteral("flow"));
    // ws-параметры (path может быть %2F-кодирован → декодируем).
    s.path        = QUrl::fromPercentEncoding(
                        q.queryItemValue(QStringLiteral("path")).toUtf8());
    s.hostHeader  = q.queryItemValue(QStringLiteral("host"));
    // grpc serviceName.
    s.serviceName = q.queryItemValue(QStringLiteral("serviceName"));

    // Имя (после #), декодируем %xx.
    s.name = QUrl::fromPercentEncoding(url.fragment(QUrl::FullyEncoded).toUtf8());
    if (s.name.isEmpty())
        s.name = s.host;

    s.valid = !s.uuid.isEmpty() && !s.host.isEmpty();
    return s;
}

void VlessController::addKey(const QString &input)
{
    const QString trimmed = input.trimmed();

    // ── happ:// — формат K-VPN / Happ ───────────────────────────────────
    //   happ://add/<URL подписки или base64-конфиг>
    //   happ://add/aHR0cHM6Ly9leGFtcGxlLmNvbS9zdWI=    (base64 от URL)
    //   happ://add/https://example.com/sub             (plain URL)
    //   happ://crypt5/...                                (шифрованный — пока не поддерживаем)
    if (trimmed.startsWith(QStringLiteral("happ://"), Qt::CaseInsensitive)) {
        const QString rest = trimmed.mid(7);   // отрезаем "happ://"
        if (rest.startsWith(QStringLiteral("add/"), Qt::CaseInsensitive)) {
            QString payload = rest.mid(4);
            // Если payload — обычный URL, грузим как подписку.
            if (payload.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
             || payload.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
                loadSubscription(payload);
                return;
            }
            // Иначе — base64. Декодируем и пробуем снова.
            const QByteArray decoded = QByteArray::fromBase64(
                payload.toUtf8(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
            const QString text = QString::fromUtf8(decoded).trimmed();
            if (text.startsWith(QStringLiteral("http"), Qt::CaseInsensitive)) {
                loadSubscription(text);
                return;
            }
            if (text.startsWith(QStringLiteral("vless://"), Qt::CaseInsensitive)) {
                addKey(text);   // рекурсивно — это уже наш формат
                return;
            }
            emit errorOccurred(QStringLiteral("happ://-ссылка: не удалось расшифровать содержимое"));
            return;
        }
        if (rest.startsWith(QStringLiteral("crypt"), Qt::CaseInsensitive)) {
            emit errorOccurred(QStringLiteral(
                "happ://crypt-ссылки пока не поддерживаются. Попросите hosted-сервис "
                "выдать обычную подписку или vless://-ключ."));
            return;
        }
        emit errorOccurred(QStringLiteral("Незнакомый формат happ://-ссылки"));
        return;
    }

    // ── Готовая подписка — HTTP-URL без vless:// префикса ────────────────
    if (trimmed.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
     || trimmed.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        loadSubscription(trimmed);
        return;
    }

    // ── Стандартный vless://-ключ ────────────────────────────────────────
    const Server s = parseVless(trimmed);
    if (!s.valid) {
        emit errorOccurred(QStringLiteral("Не удалось распознать ключ. Поддерживаются: "
                                          "vless://, happ://add/, ссылка на подписку (https://...)"));
        return;
    }
    m_serversData.append(s);
    if (m_store) m_store->addKey(s.rawUri);
    rebuildServerList();
    setStatus(QStringLiteral("Ключ добавлен: %1").arg(s.name));
}

void VlessController::reloadFromStore()
{
    if (!m_store) return;
    // Полная перезагрузка: пересобираем m_serversData с нуля по тому что
    // лежит в Store. Текущий currentServer стараемся сохранить.
    const QString currentRaw = (m_currentServer >= 0 && m_currentServer < m_serversData.size())
                               ? m_serversData[m_currentServer].rawUri : QString();
    m_serversData.clear();
    const QStringList saved = m_store->keys();
    for (const QString &uri : saved) {
        const Server s = parseVless(uri);
        if (s.valid) m_serversData.append(s);
    }
    // Восстановим выделение по rawUri (если ключ всё ещё есть).
    if (!currentRaw.isEmpty()) {
        for (int i = 0; i < m_serversData.size(); ++i) {
            if (m_serversData[i].rawUri == currentRaw) {
                m_currentServer = i;
                emit currentServerChanged();
                break;
            }
        }
    }
    rebuildServerList();
}

void VlessController::loadSubscription(const QString &url)
{
    // happ://add/<real-url> → достаём настоящий URL.
    QString real = url.trimmed();
    const QString happPrefix = QStringLiteral("happ://add/");
    if (real.startsWith(happPrefix))
        real = real.mid(happPrefix.length());

    if (!real.startsWith(QStringLiteral("http"))) {
        emit errorOccurred(QStringLiteral("Неверная ссылка подписки"));
        return;
    }

    setStatus(QStringLiteral("Загрузка подписки…"));

    // Скачиваем синхронно (с таймаутом) — для прототипа достаточно.
    QNetworkAccessManager mgr;
    QNetworkRequest req((QUrl(real)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("AmSalesVPN"));
    QNetworkReply *reply = mgr.get(req);

    QEventLoop loop;
    QTimer t; t.setSingleShot(true);
    connect(&t, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    t.start(8000);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        emit errorOccurred(QStringLiteral("Ошибка загрузки подписки: %1")
                           .arg(reply->errorString()));
        reply->deleteLater();
        return;
    }

    QByteArray data = reply->readAll();
    reply->deleteLater();

    // Подписки часто закодированы base64. Пробуем декодировать.
    QByteArray decoded = QByteArray::fromBase64(data);
    QString text = QString::fromUtf8(
        decoded.contains("vless://") ? decoded : data);

    // Каждая строка — отдельный ключ.
    int added = 0;
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                         Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        if (line.trimmed().startsWith(QStringLiteral("vless://"))) {
            const Server s = parseVless(line);
            if (s.valid) {
                m_serversData.append(s);
                if (m_store) m_store->addKey(s.rawUri);   // сохраняем
                ++added;
            }
        }
    }

    rebuildServerList();
    if (added > 0)
        setStatus(QStringLiteral("Загружено серверов: %1").arg(added));
    else
        emit errorOccurred(QStringLiteral("В подписке не найдено ключей vless"));
}

void VlessController::rebuildServerList()
{
    m_servers.clear();
    for (const Server &s : m_serversData) {
        QVariantMap m;
        m[QStringLiteral("name")] = s.name;
        m[QStringLiteral("host")] = s.host;
        m[QStringLiteral("uri")] = s.rawUri;   // для "поделиться"/копирования
        m_servers.append(m);
    }
    emit serversChanged();
    if (m_currentServer >= m_serversData.size())
        setCurrentServer(0);
}

// ── Генерация конфига sing-box ──────────────────────────────────────────
QString VlessController::buildConfig(const Server &s) const
{
    // Outbound vless с reality (если security=reality).
    QJsonObject vless;
    vless["type"] = "vless";
    vless["tag"] = "proxy";
    vless["server"] = s.host;
    vless["server_port"] = s.port;
    vless["uuid"] = s.uuid;
    if (!s.flow.isEmpty())
        vless["flow"] = s.flow;
    vless["packet_encoding"] = "xudp";

    // TLS: включаем для reality и обычного tls. (security=none → без TLS)
    if (s.security != QStringLiteral("none") && !s.security.isEmpty()) {
        QJsonObject tls;
        tls["enabled"] = true;
        // server_name: sni, иначе host-заголовок (для ws через CF — это домен CF).
        const QString sn = !s.sni.isEmpty() ? s.sni
                          : (!s.hostHeader.isEmpty() ? s.hostHeader : s.host);
        tls["server_name"] = sn;
        QJsonObject utls;
        utls["enabled"] = true;
        utls["fingerprint"] = s.fingerprint.isEmpty() ? QStringLiteral("chrome")
                                                        : s.fingerprint;
        tls["utls"] = utls;
        if (s.security == QStringLiteral("reality")) {
            QJsonObject reality;
            reality["enabled"] = true;
            reality["public_key"] = s.publicKey;
            reality["short_id"] = s.shortId;
            tls["reality"] = reality;
        }
        vless["tls"] = tls;
    }

    // transport: grpc или ws.
    if (s.type == QStringLiteral("grpc")) {
        QJsonObject tr;
        tr["type"] = "grpc";
        if (!s.serviceName.isEmpty())
            tr["service_name"] = s.serviceName;
        vless["transport"] = tr;
    } else if (s.type == QStringLiteral("ws")) {
        QJsonObject tr;
        tr["type"] = "ws";
        if (!s.path.isEmpty())
            tr["path"] = s.path;
        // Host-заголовок (важно для Cloudflare-роутинга).
        const QString h = !s.hostHeader.isEmpty() ? s.hostHeader : s.host;
        tr["headers"] = QJsonObject{{"Host", h}};
        vless["transport"] = tr;
    }

    // Inbound TUN — заворачивает системный трафик.
    // strict_route=false: мягче к другим VPN на машине (Hiddify), меньше
    // риск namertво порвать инет. mtu стандартный.
    QJsonObject tun;
    tun["type"] = "tun";
    tun["tag"] = "tun-in";
    tun["interface_name"] = "amsales0";
    tun["address"] = QJsonArray{ "172.19.0.1/30" };
    tun["mtu"] = 9000;
    tun["auto_route"] = true;
    tun["strict_route"] = false;   // не воюем жёстко за маршрут
    tun["stack"] = "system";

    // Outbounds: только proxy + direct (формат sing-box 1.12 — без dns-out).
    QJsonArray outbounds;
    outbounds.append(vless);
    outbounds.append(QJsonObject{{"type","direct"},{"tag","direct"}});

    // DNS (формат 1.12). Без detour — иначе sing-box ругается
    // "detour to empty direct outbound". remote=DoH, local=обычный UDP.
    QJsonObject dns;
    QJsonArray dnsServers;
    dnsServers.append(QJsonObject{
        {"tag","remote"},{"type","https"},{"server","1.1.1.1"}});
    dnsServers.append(QJsonObject{
        {"tag","local"},{"type","udp"},{"server","77.88.8.8"}});
    dns["servers"] = dnsServers;
    dns["strategy"] = "prefer_ipv4";
    // .ru домены резолвим локально (если включён режим .ru мимо VPN).
    QJsonArray dnsRules;
    if (m_routeRuDirect) {
        dnsRules.append(QJsonObject{{"domain_suffix",".ru"},{"server","local"}});
        dnsRules.append(QJsonObject{{"rule_set","geosite-ru"},{"server","local"}});
    }
    dns["rules"] = dnsRules;
    dns["final"] = "remote";

    // Маршрутизация (новый формат: action вместо спец-outbound).
    QJsonObject route;
    QJsonArray rules;
    // Перехват DNS-запросов.
    rules.append(QJsonObject{{"action","sniff"}});
    rules.append(QJsonObject{{"protocol","dns"},{"action","hijack-dns"}});
    // КРИТИЧНО: трафик к самому VPN-серверу — direct (мимо туннеля),
    // иначе соединение sing-box зацикливается в своём же TUN и инет умирает.
    // Работает, когда server задан как IP (наш случай 155.212.228.232).
    {
        const QRegularExpression ipRe(
            QStringLiteral("^\\d{1,3}(?:\\.\\d{1,3}){3}$"));
        if (ipRe.match(s.host).hasMatch()) {
            rules.append(QJsonObject{
                {"ip_cidr", QJsonArray{ s.host + QStringLiteral("/32") }},
                {"action","route"},{"outbound","direct"}});
        }
    }
    // Сайты-исключения пользователя — direct (мимо VPN). Эти же домены
    // приложение исключает и из zapret (см. ZapretController).
    if (!m_bypassSites.isEmpty()) {
        QJsonArray bypassDomains;
        for (const QString &d : m_bypassSites)
            bypassDomains.append(d);
        rules.append(QJsonObject{
            {"domain_suffix", bypassDomains},
            {"action","route"},{"outbound","direct"}});
    }
    // .ru домены и российские IP — direct (мимо VPN). Только если включено.
    if (m_routeRuDirect) {
        rules.append(QJsonObject{{"domain_suffix",".ru"},{"action","route"},{"outbound","direct"}});
        rules.append(QJsonObject{
            {"rule_set", QJsonArray{"geoip-ru","geosite-ru"}},
            {"action","route"},{"outbound","direct"}});
    }
    route["rules"] = rules;

    // Гео-наборы РФ.
    QJsonArray ruleSet;
    ruleSet.append(QJsonObject{
        {"type","remote"},{"tag","geoip-ru"},{"format","binary"},
        {"url","https://raw.githubusercontent.com/hiddify/hiddify-geo/rule-set/country/geoip-ru.srs"},
        {"download_detour","direct"}});
    ruleSet.append(QJsonObject{
        {"type","remote"},{"tag","geosite-ru"},{"format","binary"},
        {"url","https://raw.githubusercontent.com/hiddify/hiddify-geo/rule-set/country/geosite-ru.srs"},
        {"download_detour","direct"}});
    route["rule_set"] = ruleSet;
    route["final"] = "proxy";
    route["auto_detect_interface"] = true;
    // Резолвер по умолчанию для исходящих соединений (убирает warn 1.12).
    route["default_domain_resolver"] = QJsonObject{{"server","local"}};

    // clash-api на 9090 — отсюда StatsTracker берёт трафик.
    QJsonObject experimental;
    experimental["clash_api"] = QJsonObject{
        {"external_controller","127.0.0.1:9090"}
    };

    QJsonObject root;
    root["log"] = QJsonObject{{"level","warn"}};
    root["dns"] = dns;
    root["inbounds"] = QJsonArray{ tun };
    root["outbounds"] = outbounds;
    root["route"] = route;
    root["experimental"] = experimental;

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void VlessController::connectVpn()
{
    if (m_connected || m_connecting)
        return;
    if (m_serversData.isEmpty()) {
        emit errorOccurred(QStringLiteral("Сначала добавьте ключ или подписку"));
        return;
    }
    // Пользователь нажал «Включить» — теперь keep-alive обязан держать
    // соединение, пока пользователь сам не нажмёт «Выключить».
    m_userWantsConnected = true;
    m_consecFailures = 0;
    // Проверка прав администратора. Без неё sing-box падает с
    // "configure tun interface: Access is denied" — а в UI это выглядит
    // как "не запустился (проверьте ключ)", что путает пользователя.
    if (!AdminCheck::isElevated()) {
        const QString msg = QStringLiteral(
            "Нет прав администратора — VPN не запустится. "
            "Закройте приложение и запустите от имени администратора "
            "(правой кнопкой по ярлыку → «Запуск от имени администратора»).");
        setStatus(msg);
        emit errorOccurred(msg);
        return;
    }
    if (m_currentServer < 0 || m_currentServer >= m_serversData.size())
        m_currentServer = 0;

    const Server &s = m_serversData.at(m_currentServer);
    setConnecting(true);
    setStatus(QStringLiteral("Подключение к %1…").arg(s.name));

    // Пишем конфиг.
    const QString cfg = buildConfig(s);
    QFile f(configPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setConnecting(false);
        emit errorOccurred(QStringLiteral("Не удалось записать конфиг"));
        return;
    }
    f.write(cfg.toUtf8());
    f.close();

    // sing-box в TUN-режиме требует администратора. Приложение уже от админа.
    // ВАЖНО: пишем stdout/stderr sing-box в sing-box.log / sing-box.err —
    // диагностика читает их.
    const QString sb      = singBoxPath();
    const QString cfgPath = configPath();
    const QString workDir = engineDir() + QStringLiteral("/vpn");
    const QString logPath = workDir + QStringLiteral("/sing-box.log");
    const QString errLog  = workDir + QStringLiteral("/sing-box.err");

    // Запускаем напрямую через QProcess — он сам корректно квотит аргументы
    // с пробелами (а в "Program Files\AM.SALES VPN\..." пробелы есть).
    // Предыдущая обёртка через PowerShell Start-Process ломалась на пути
    // с пробелом: sing-box получал "-c C:\Program" вместо полного пути.
    if (m_proc) {
        m_proc->kill();
        m_proc->waitForFinished(1500);
        m_proc->deleteLater();
    }
    m_proc = new QProcess(this);
    m_proc->setProgram(sb);
    m_proc->setArguments({QStringLiteral("run"),
                          QStringLiteral("-c"),
                          QDir::toNativeSeparators(cfgPath)});
    m_proc->setWorkingDirectory(QDir::toNativeSeparators(workDir));
    m_proc->setStandardOutputFile(logPath, QIODevice::Truncate);
    m_proc->setStandardErrorFile(errLog,  QIODevice::Truncate);
    // Скрыть консольное окно (CREATE_NO_WINDOW).
    m_proc->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *a) {
            a->flags |= CREATE_NO_WINDOW;
        });
    m_proc->start();

    // Через 5 сек проверяем: (1) поднялся ли sing-box, (2) ЕСТЬ ЛИ ИНЕТ.
    // Если sing-box работает, но интернета нет (туннель порвал маршрут) —
    // АВТО-ОТКАТ: отключаемся, чтобы вернуть пользователю связь.
    QTimer::singleShot(5000, this, [this, s]() {
        // 1) жив ли процесс
        QProcess chk;
        chk.start(QStringLiteral("tasklist"),
                  {QStringLiteral("/FI"),
                   QStringLiteral("IMAGENAME eq sing-box.exe"),
                   QStringLiteral("/NH")});
        chk.waitForFinished(3000);
        const bool up = QString::fromLocal8Bit(chk.readAllStandardOutput())
                        .contains(QStringLiteral("sing-box.exe"), Qt::CaseInsensitive);

        if (!up) {
            setConnecting(false);
            setConnected(false);
            setStatus(QStringLiteral("Не удалось подключиться"));
            emit errorOccurred(QStringLiteral("sing-box не запустился (проверьте ключ)"));
            return;
        }

        // 2) проверяем реальный выход в сеть (короткий TCP к 1.1.1.1:443).
        QTcpSocket probe;
        probe.connectToHost(QStringLiteral("1.1.1.1"), 443);
        const bool online = probe.waitForConnected(4000);
        probe.abort();

        setConnecting(false);
        if (online) {
            setConnected(true);
            setStatus(QStringLiteral("Подключено · %1").arg(s.name));
            m_consecFailures = 0;
            // С этого момента keep-alive дёргается каждые 15 сек —
            // при обрыве сам поднимет sing-box обратно.
            if (m_userWantsConnected && !m_keepAliveTimer->isActive())
                m_keepAliveTimer->start();
        } else {
            // Туннель поднялся, но связи нет — откатываемся, спасаем инет.
            disconnectVpn();
            setStatus(QStringLiteral("Нет связи через туннель — откат (инет возвращён)"));
            emit errorOccurred(QStringLiteral(
                "Туннель не пропускает трафик. Вероятно конфликт с другим VPN "
                "(Hiddify) или DPI. Интернет восстановлен."));
        }
    });
}

void VlessController::disconnectVpn()
{
    // Пользователь сам нажал «Выключить» — гасим keep-alive, чтобы он
    // не вернул нас обратно через 15 сек.
    m_userWantsConnected = false;
    if (m_keepAliveTimer && m_keepAliveTimer->isActive())
        m_keepAliveTimer->stop();

    // Приложение уже от админа — глушим напрямую.
    QProcess::startDetached(QStringLiteral("taskkill"),
        {QStringLiteral("/F"), QStringLiteral("/IM"),
         QStringLiteral("sing-box.exe")});
    setConnected(false);
    setConnecting(false);
    setStatus(QStringLiteral("Не подключено"));
}

// ─────────────────────────────────────────────────────────────────────────
//  Keep-alive: каждые 15 сек проверяем что туннель работает.
//
//  Что считаем «жив»:
//    1) процесс sing-box.exe запущен;
//    2) короткий TCP-коннект до 1.1.1.1:443 проходит за < 4 сек.
//
//  Что делаем при неудаче:
//    – 1 раз: ничего (флуктуация сети — Wi-Fi моргнул).
//    – 2 раза подряд: тихо рестартуем sing-box (reconnectInternal).
//
//  m_userWantsConnected = false → keep-alive не работает (юзер сам выключил).
// ─────────────────────────────────────────────────────────────────────────
void VlessController::keepAliveTick()
{
    if (!m_userWantsConnected)
        return;

    // 1) процесс
    QProcess chk;
    chk.start(QStringLiteral("tasklist"),
              {QStringLiteral("/FI"), QStringLiteral("IMAGENAME eq sing-box.exe"),
               QStringLiteral("/NH")});
    chk.waitForFinished(2000);
    const bool procUp = QString::fromLocal8Bit(chk.readAllStandardOutput())
                        .contains(QStringLiteral("sing-box.exe"), Qt::CaseInsensitive);

    // 2) реальный выход — короткий TCP-зонд к 1.1.1.1:443
    bool netOk = false;
    if (procUp) {
        QTcpSocket probe;
        probe.connectToHost(QStringLiteral("1.1.1.1"), 443);
        netOk = probe.waitForConnected(4000);
        probe.abort();
    }

    if (procUp && netOk) {
        m_consecFailures = 0;
        return;
    }

    ++m_consecFailures;
    if (m_consecFailures < 2) {
        // Дадим сети шанс — одна неудача может быть флуктуацией.
        return;
    }
    m_consecFailures = 0;
    reconnectInternal();
}

// Тихий рестарт: не показываем «Подключение…» в статусе, просто
// перезапускаем sing-box на том же сервере.
void VlessController::reconnectInternal()
{
    setStatus(QStringLiteral("Восстанавливаю соединение…"));
    emit errorOccurred(QStringLiteral("Связь пропала — переподключаюсь"));

    // Аккуратно прибиваем старый процесс (может быть зомби)
    QProcess kill;
    kill.start(QStringLiteral("taskkill"),
               {QStringLiteral("/F"), QStringLiteral("/IM"),
                QStringLiteral("sing-box.exe")});
    kill.waitForFinished(2000);

    setConnected(false);
    // Снимем флаг, чтобы connectVpn не выпал на guard'е m_connected.
    // m_userWantsConnected оставляем = true.
    connectVpn();
}

void VlessController::toggle()
{
    if (m_connected || m_connecting)
        disconnectVpn();
    else
        connectVpn();
}
