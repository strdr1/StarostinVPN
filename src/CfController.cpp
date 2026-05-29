#include "CfController.h"
#include "Store.h"
#include "VlessController.h"
#include "LogController.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>

CfController::CfController(Store *store, VlessController *vless, QObject *parent)
    : QObject(parent), m_store(store), m_vless(vless)
{
    m_net = new QNetworkAccessManager(this);

    // Авто-проверка раз в 30 минут — quick-tunnel живёт долго, но иногда
    // обрывается. Раз в полчаса спросим у сервера актуальный host.
    m_timer = new QTimer(this);
    m_timer->setInterval(30 * 60 * 1000);
    connect(m_timer, &QTimer::timeout, this, &CfController::refresh);
    m_timer->start();

    // Стартовое обновление — через 7 сек после запуска (после UpdateChecker).
    QTimer::singleShot(7000, this, &CfController::refresh);
}

QString CfController::buildVlessUri(const QString &host, const QString &path) const
{
    // Cloudflare всегда отдаёт HTTPS/WSS на 443 с TLS-сертификатом для своего домена.
    // sni и host header == имени trycloudflare-поддомена.
    return QStringLiteral(
        "vless://%1@%2:443?type=ws&security=tls&sni=%2&host=%2&path=%3"
        "&fp=chrome&encryption=none#STAROSTIN-CF")
        .arg(cfUuid(), host, QString(path).replace('/', QStringLiteral("%2F")));
}

void CfController::refresh()
{
    if (m_refreshing) return;
    setRefreshing(true);
    setStatus(QStringLiteral("Запрашиваю свежий Cloudflare-хост…"));

    QNetworkRequest req{QUrl(endpointUrl())};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("StarostinVPN"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *r = m_net->get(req);
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        setRefreshing(false);
        if (r->error() != QNetworkReply::NoError) {
            setStatus(QStringLiteral("Ошибка: ") + r->errorString());
            r->deleteLater();
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(r->readAll()).object();
        r->deleteLater();
        const QString host = obj.value(QStringLiteral("host")).toString();
        QString path = obj.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) path = QStringLiteral("/amsales");
        if (host.isEmpty()) {
            setStatus(QStringLiteral("Сервер вернул пустой host"));
            return;
        }

        m_host = host; emit currentHostChanged();
        const QString uri = buildVlessUri(host, path);

        // Прописываем/обновляем CF-ключ в списке. Логика: ищем любой
        // ключ с #STAROSTIN-CF, заменяем его. Если такого нет — добавляем.
        if (m_store) {
            QStringList keys = m_store->keys();
            bool replaced = false;
            for (int i = 0; i < keys.size(); ++i) {
                if (keys[i].contains(QStringLiteral("#STAROSTIN-CF"))) {
                    keys[i] = uri;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) keys.append(uri);
            m_store->setKeys(keys);
        }
        // Просим VlessController перечитать список из Store, чтобы новый
        // ключ сразу появился в UI.
        if (m_vless)
            QMetaObject::invokeMethod(m_vless, "reloadFromStore", Qt::QueuedConnection);

        emit keyUpdated(uri);
        setStatus(QStringLiteral("Cloudflare-ключ обновлён · ") + host);
    });
}
