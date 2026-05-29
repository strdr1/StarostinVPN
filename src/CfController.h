#pragma once
//
//  CfController — следит за Cloudflare-обёрткой над нашим VPN.
//
//  Что делает:
//    • При старте и потом каждые 30 мин стучит в http://78.17.103.241:8080/cf.json
//    • Получает { "host": "X.trycloudflare.com", "path": "/amsales" }
//    • Пересобирает «волшебный» VLESS-ключ AM.SALES-CF с актуальным host
//    • Складывает его в Store как отдельный ключ — пользователь его не теряет
//      и всегда видит свежий
//
//  Вызов вручную: Q_INVOKABLE refresh() — кнопка «Обновить CF-ключ» в UI.
//
#include <QObject>
#include <QString>
#include <QTimer>

class QNetworkAccessManager;
class Store;
class VlessController;

class CfController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentHost READ currentHost NOTIFY currentHostChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(bool refreshing READ refreshing NOTIFY refreshingChanged)

public:
    // endpointUrl — куда стучимся (http://сервер:8080/cf.json).
    explicit CfController(Store *store, VlessController *vless,
                          QObject *parent = nullptr);

    QString currentHost() const { return m_host; }
    QString statusText() const { return m_status; }
    bool refreshing() const { return m_refreshing; }

    static QString endpointUrl() {
        return QStringLiteral("http://78.17.103.241:8080/cf.json");
    }
    // UUID для CF-ключа — берём первый из двух (или из Store).
    static QString cfUuid() {
        return QStringLiteral("74553d23-3ff1-4e25-b06f-ff24e22f97ba");
    }

public slots:
    // Запрашивает свежий host у сервера и пересобирает ключ AM.SALES-CF.
    void refresh();

signals:
    void currentHostChanged();
    void statusTextChanged();
    void refreshingChanged();
    void keyUpdated(const QString &vlessUri);

private:
    QString buildVlessUri(const QString &host, const QString &path) const;
    void setStatus(const QString &t) { m_status = t; emit statusTextChanged(); }
    void setRefreshing(bool r)       { m_refreshing = r; emit refreshingChanged(); }

    Store *m_store = nullptr;
    VlessController *m_vless = nullptr;
    QNetworkAccessManager *m_net = nullptr;
    QTimer *m_timer = nullptr;
    QString m_host;
    QString m_status = QStringLiteral("Не проверялось");
    bool m_refreshing = false;
};
