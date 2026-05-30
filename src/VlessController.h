#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

class QProcess;
class Store;

// ─────────────────────────────────────────────────────────────────────────
//  VlessController — управляет VPN-туннелем через встроенный sing-box.
//
//  Что делает:
//    1. Принимает либо одиночный vless://-ключ, либо ссылку-подписку
//       (happ://add/<url> или прямой http(s) на список ключей).
//    2. Парсит ключ(и) → строит JSON-конфиг sing-box с маршрутизацией:
//       российский трафик (.ru / geoip-ru) идёт direct (мимо VPN),
//       остальное — через VLESS/REALITY-туннель.
//    3. Запускает sing-box.exe (от администратора — нужен TUN-режим) и
//       следит за состоянием.
//
//  Наружу (в QML) отдаёт: connected, statusText, список серверов, выбранный.
// ─────────────────────────────────────────────────────────────────────────
class VlessController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool connecting READ connecting NOTIFY connectingChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    // Список серверов: [{name, host, ...}] для выбора в UI.
    Q_PROPERTY(QVariantList servers READ servers NOTIFY serversChanged)
    // Индекс выбранного сервера.
    Q_PROPERTY(int currentServer READ currentServer WRITE setCurrentServer NOTIFY currentServerChanged)
    // Настройка: российский трафик мимо VPN (true) или весь через VPN (false).
    Q_PROPERTY(bool routeRuDirect READ routeRuDirect WRITE setRouteRuDirect NOTIFY routeRuDirectChanged)
    // tg://-ссылка на MTProto-прокси (для кнопки "Открыть Telegram").
    Q_PROPERTY(QString tgProxyLink READ tgProxyLink WRITE setTgProxyLink NOTIFY tgProxyLinkChanged)
    // Список доменов-исключений: мимо VPN И мимо zapret (direct).
    Q_PROPERTY(QStringList bypassSites READ bypassSites NOTIFY bypassSitesChanged)
    // Использовать ли обход DPI (zapret) вместе с VPN.
    Q_PROPERTY(bool useZapret READ useZapret WRITE setUseZapret NOTIFY useZapretChanged)

public:
    explicit VlessController(Store *store, QObject *parent = nullptr);
    ~VlessController() override;

    bool connected() const { return m_connected; }
    bool connecting() const { return m_connecting; }
    QString statusText() const { return m_statusText; }
    QVariantList servers() const { return m_servers; }
    int currentServer() const { return m_currentServer; }
    void setCurrentServer(int idx);
    bool routeRuDirect() const { return m_routeRuDirect; }
    void setRouteRuDirect(bool v);
    QString tgProxyLink() const { return m_tgProxyLink; }
    void setTgProxyLink(const QString &v);
    QStringList bypassSites() const { return m_bypassSites; }
    bool useZapret() const { return m_useZapret; }
    void setUseZapret(bool v);

public slots:
    // Добавить одиночный ключ vless://... — распарсить и положить в список.
    void addKey(const QString &vlessUri);
    // Перечитать список ключей из Store (нужно после внешнего обновления,
    // напр. CfController обновил CF-ключ).
    void reloadFromStore();
    // Загрузить подписку (happ://add/<url> или http(s)://...).
    void loadSubscription(const QString &url);
    // Удалить сервер из списка по индексу.
    void removeServer(int index);
    // Изменить ключ существующего сервера (перепарсить новый vless://).
    void editServer(int index, const QString &newUri);
    // Сгенерировать QR-код ключа сервера, вернуть file:// путь к PNG (для UI).
    Q_INVOKABLE QString qrForServer(int index);
    // Управление списком сайтов-исключений (мимо VPN и zapret).
    void addBypassSite(const QString &domain);
    void removeBypassSite(int index);
    // Запустить локальный Telegram-прокси (tg-ws-proxy через Cloudflare).
    void launchTgProxy();
    // Подключиться к выбранному серверу.
    void connectVpn();
    // Отключиться.
    void disconnectVpn();
    // Переключатель.
    void toggle();

signals:
    void connectedChanged();
    void connectingChanged();
    void statusTextChanged();
    void serversChanged();
    void currentServerChanged();
    void routeRuDirectChanged();
    void tgProxyLinkChanged();
    void bypassSitesChanged();
    void useZapretChanged();
    void errorOccurred(const QString &message);

private:
    struct Server {
        QString name;       // подпись (#... из ключа)
        QString host;
        int port = 443;
        QString uuid;
        QString flow;       // xtls-rprx-vision и т.п.
        QString sni;
        QString publicKey;  // reality pbk
        QString shortId;    // reality sid
        QString fingerprint;
        QString type;       // tcp / grpc / ws
        QString security;   // reality / tls / none
        QString path;       // ws path (напр. /amsales)
        QString hostHeader; // ws Host-заголовок
        QString serviceName;// grpc serviceName
        QString rawUri;     // исходный vless://
        bool valid = false;
    };

    void setConnected(bool c);
    void setConnecting(bool c);
    void setStatus(const QString &t);
    void rebuildServerList();
    void syncZapretExclude();   // пишет bypass-сайты в zapret exclude

    // Парсинг одного vless://-URI в структуру Server.
    Server parseVless(const QString &uri) const;
    // Сгенерировать JSON-конфиг sing-box для выбранного сервера.
    QString buildConfig(const Server &s) const;

    QString engineDir() const;
    QString singBoxPath() const;
    QString configPath() const;

    QList<Server> m_serversData;
    QVariantList m_servers;         // зеркало для QML
    int m_currentServer = 0;

    bool m_connected = false;
    bool m_connecting = false;
    bool m_routeRuDirect = true;   // по умолчанию .ru мимо VPN
    QString m_tgProxyLink;         // tg://-ссылка на MTProto-прокси
    QStringList m_bypassSites;     // домены-исключения (мимо VPN и zapret)
    bool m_useZapret = true;       // включён ли обход DPI вместе с VPN
    QString m_statusText = QStringLiteral("Не подключено");

    QProcess *m_proc = nullptr;     // процесс sing-box
    Store *m_store = nullptr;       // хранилище ключей/настроек

    // ── Keep-alive: соединение «никогда» не отключается ─────────────────
    // Каждые 15 сек: жив ли sing-box? есть ли реальная связь? Если нет —
    // переподключаемся (m_userWantsConnected). Считаем неудачи подряд,
    // чтоб не дёргаться от одной флуктуации сети.
    QTimer *m_keepAliveTimer = nullptr;
    bool m_userWantsConnected = false;   // пользователь нажал «Включить»
    int  m_consecFailures = 0;
    void keepAliveTick();
    void reconnectInternal();             // тихий пере-старт sing-box

    // ── Системный прокси Windows (для mixed-inbound режима) ────────────
    void setSystemProxy(const QString &proxy);   // "host:port"
    void clearSystemProxy();
    void notifyProxySettingsChanged();
};
