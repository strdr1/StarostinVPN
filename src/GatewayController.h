#pragma once
//
//  GatewayController — режим «шлюз для локальной сети».
//
//  Когда включён: этот ПК становится посредником для всех устройств в
//  локалке. Их трафик: устройство → ПК → TUN (sing-box) → VPN.
//
//  Что делает на ПК (всё откатывается при выключении):
//    1) включает IP-forwarding в Windows
//    2) создаёт NetNat правило для локальной подсети
//    3) детектит локальный IP/подсеть и показывает их пользователю,
//       чтобы он мог сказать на роутере «шлюз по умолчанию = этот IP»
//
//  Без kill-switch: если VPN упал — трафик идёт мимо (как просил юзер).
//
//  Для устройств локалки нужно (один раз): в админке роутера →
//  настройки DHCP → «шлюз по умолчанию» = LocalIp этого ПК.
//
#include <QObject>
#include <QString>

class GatewayController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled NOTIFY enabledChanged)
    Q_PROPERTY(QString localIp READ localIp NOTIFY localIpChanged)
    Q_PROPERTY(QString subnet READ subnet NOTIFY subnetChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)

public:
    explicit GatewayController(QObject *parent = nullptr);
    ~GatewayController() override;

    bool enabled() const { return m_enabled; }
    QString localIp() const { return m_localIp; }
    QString subnet() const { return m_subnet; }   // напр. "192.168.1.0/24"
    QString statusText() const { return m_status; }

public slots:
    void setEnabled(bool on);
    // Перечитать локальный IP/подсеть — на случай если сменился Wi-Fi.
    void refreshNetwork();

signals:
    void enabledChanged();
    void localIpChanged();
    void subnetChanged();
    void statusTextChanged();

private:
    void setStatus(const QString &t) { m_status = t; emit statusTextChanged(); }
    bool runPwsh(const QString &script, QString *out = nullptr);
    void detectLocalNetwork();   // заполняет m_localIp, m_subnet

    bool m_enabled = false;
    QString m_localIp;
    QString m_subnet;
    QString m_natName = QStringLiteral("StarostinVpnNat");
    QString m_status = QStringLiteral("Выключено");
};
