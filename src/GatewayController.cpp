#include "GatewayController.h"

#include <QProcess>
#include <QNetworkInterface>
#include <QHostAddress>
#include <QRegularExpression>

GatewayController::GatewayController(QObject *parent)
    : QObject(parent)
{
    detectLocalNetwork();
}

GatewayController::~GatewayController()
{
    if (m_enabled) {
        // Не оставляем NetNat торчать после выхода — иначе после следующей
        // загрузки Windows может вести себя странно с маршрутизацией.
        setEnabled(false);
    }
}

void GatewayController::detectLocalNetwork()
{
    // Берём первый non-loopback IPv4 в подсети 192.168.x / 10.x / 172.16-31.x
    // (типовые домашние/офисные сети).
    QString ip;
    QString subnet;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        if (!(iface.flags() & QNetworkInterface::IsUp)) continue;
        if (iface.flags() & QNetworkInterface::IsLoopBack) continue;
        // Игнорируем TUN/виртуальные интерфейсы (sing-box, Hyper-V и т.п.)
        const QString human = iface.humanReadableName().toLower();
        if (human.contains(QStringLiteral("amsales"))
         || human.contains(QStringLiteral("starostin"))
         || human.contains(QStringLiteral("singbox"))
         || human.contains(QStringLiteral("tun"))) continue;

        for (const QNetworkAddressEntry &ent : iface.addressEntries()) {
            const QHostAddress a = ent.ip();
            if (a.protocol() != QAbstractSocket::IPv4Protocol) continue;
            const quint32 raw = a.toIPv4Address();
            const bool isPrivate =
                ((raw & 0xFF000000u) == 0x0A000000u) ||      // 10.0.0.0/8
                ((raw & 0xFFF00000u) == 0xAC100000u) ||      // 172.16.0.0/12
                ((raw & 0xFFFF0000u) == 0xC0A80000u);        // 192.168.0.0/16
            if (!isPrivate) continue;
            ip = a.toString();
            // CIDR: преобразуем netmask в число бит префикса.
            int prefix = ent.prefixLength();
            if (prefix <= 0) prefix = 24;   // дефолт для большинства домашних
            const QHostAddress network(raw & (~0u << (32 - prefix)));
            subnet = network.toString() + QStringLiteral("/%1").arg(prefix);
            break;
        }
        if (!ip.isEmpty()) break;
    }
    if (ip != m_localIp) { m_localIp = ip; emit localIpChanged(); }
    if (subnet != m_subnet) { m_subnet = subnet; emit subnetChanged(); }
}

void GatewayController::refreshNetwork()
{
    detectLocalNetwork();
}

// Запустить PowerShell-скрипт от админа (мы уже elevated).
bool GatewayController::runPwsh(const QString &script, QString *out)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("powershell"),
            { QStringLiteral("-NoProfile"),
              QStringLiteral("-NonInteractive"),
              QStringLiteral("-Command"), script });
    if (!p.waitForFinished(15000)) {
        if (out) *out = QStringLiteral("timeout");
        return false;
    }
    const QByteArray buf = p.readAll();
    if (out) *out = QString::fromLocal8Bit(buf);
    return p.exitCode() == 0;
}

void GatewayController::setEnabled(bool on)
{
    if (on == m_enabled) return;

    if (on) {
        detectLocalNetwork();
        if (m_subnet.isEmpty()) {
            setStatus(QStringLiteral("Нет локальной сети — нечего раздавать"));
            return;
        }
        // 1) включаем IP-forwarding глобально.
        // 2) создаём NetNat для нашей подсети (если уже есть — переcоздаём).
        // 3) маршрут по умолчанию через TUN sing-box задаст сам через
        //    auto_route=true (он уже это делает в нашем конфиге).
        const QString script = QStringLiteral(
            "Set-NetIPInterface -Forwarding Enabled -ErrorAction SilentlyContinue;"
            "Get-NetNat -Name '%1' -ErrorAction SilentlyContinue | Remove-NetNat -Confirm:$false;"
            "New-NetNat -Name '%1' -InternalIPInterfaceAddressPrefix '%2' -ErrorAction Stop;"
            "Write-Output 'OK'"
        ).arg(m_natName, m_subnet);
        QString out;
        const bool ok = runPwsh(script, &out);
        if (!ok || !out.contains(QStringLiteral("OK"))) {
            setStatus(QStringLiteral("Не удалось включить шлюз: ") + out.trimmed());
            return;
        }
        m_enabled = true; emit enabledChanged();
        setStatus(QStringLiteral("Шлюз включён. Скажите на роутере: «шлюз = %1»").arg(m_localIp));
    } else {
        // Откатываем.
        const QString script = QStringLiteral(
            "Get-NetNat -Name '%1' -ErrorAction SilentlyContinue | Remove-NetNat -Confirm:$false;"
            "Write-Output 'OK'"
        ).arg(m_natName);
        QString out;
        runPwsh(script, &out);
        m_enabled = false; emit enabledChanged();
        setStatus(QStringLiteral("Выключено"));
    }
}
