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
    // Ищем настоящий LAN. ПК часто имеет ещё WSL2 (172.30/16, 172.17/16),
    // Hyper-V (172.16/12), VMware (192.168.179/24) — это «фантомные» сети
    // внутри ПК, через них раздавать VPN бессмысленно.
    //
    // Приоритет: физический Ethernet/Wi-Fi с DEFAULT GATEWAY (т.е. через
    // который идёт реальный интернет). Если такого нет — берём любой
    // частный non-virtual.
    QString ip;
    QString subnet;
    QString bestIp, bestSubnet;
    int bestPriority = -1;

    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        if (!(iface.flags() & QNetworkInterface::IsUp)) continue;
        if (iface.flags() & QNetworkInterface::IsLoopBack) continue;
        if (!(iface.flags() & QNetworkInterface::IsRunning)) continue;

        const QString human = iface.humanReadableName().toLower();
        // Виртуальные/TUN интерфейсы — пропускаем целиком.
        // vEthernet — это и WSL2, и Hyper-V switch, и Docker; нам не нужны.
        if (human.contains(QStringLiteral("amsales"))
         || human.contains(QStringLiteral("starostin"))
         || human.contains(QStringLiteral("singbox"))
         || human.contains(QStringLiteral("tun"))
         || human.contains(QStringLiteral("wsl"))
         || human.contains(QStringLiteral("hyper-v"))
         || human.contains(QStringLiteral("vethernet"))
         || human.contains(QStringLiteral("vmware"))
         || human.contains(QStringLiteral("virtualbox"))
         || human.contains(QStringLiteral("vbox"))
         || human.contains(QStringLiteral("docker"))
         || human.contains(QStringLiteral("loopback"))) continue;

        // Приоритет: Ethernet > Wi-Fi > всё остальное.
        int priority = 0;
        if (human.startsWith(QStringLiteral("ethernet"))
         || human.contains(QStringLiteral("eth"))) priority = 3;
        else if (human.contains(QStringLiteral("wi-fi"))
              || human.contains(QStringLiteral("wifi"))
              || human.contains(QStringLiteral("wlan"))
              || human.contains(QStringLiteral("wireless"))) priority = 2;
        else priority = 1;

        for (const QNetworkAddressEntry &ent : iface.addressEntries()) {
            const QHostAddress a = ent.ip();
            if (a.protocol() != QAbstractSocket::IPv4Protocol) continue;
            const quint32 raw = a.toIPv4Address();
            const bool isPrivate =
                ((raw & 0xFF000000u) == 0x0A000000u) ||      // 10.0.0.0/8
                ((raw & 0xFFF00000u) == 0xAC100000u) ||      // 172.16.0.0/12
                ((raw & 0xFFFF0000u) == 0xC0A80000u);        // 192.168.0.0/16
            if (!isPrivate) continue;

            // Внутри 172.16/12 отсеиваем WSL/Hyper-V диапазоны: 172.16/16,
            // 172.17/16, 172.18/16... часто заняты WSL2 и Docker.
            // Реальный домашний LAN там почти никогда не бывает.
            if ((raw & 0xFFF00000u) == 0xAC100000u) {
                continue;   // на 172.x вообще не претендуем
            }

            int prefix = ent.prefixLength();
            if (prefix <= 0) prefix = 24;
            const QHostAddress network(raw & (~0u << (32 - prefix)));
            const QString thisSubnet =
                network.toString() + QStringLiteral("/%1").arg(prefix);

            if (priority > bestPriority) {
                bestPriority = priority;
                bestIp = a.toString();
                bestSubnet = thisSubnet;
            }
        }
    }
    ip = bestIp; subnet = bestSubnet;
    if (ip != m_localIp) { m_localIp = ip; emit localIpChanged(); }
    if (subnet != m_subnet) { m_subnet = subnet; emit subnetChanged(); }
}

void GatewayController::refreshNetwork()
{
    detectLocalNetwork();
}

// Запустить PowerShell-скрипт от админа (мы уже elevated).
// Прокидываем PYTHONIOENCODING + chcp 65001, чтобы вывод был в UTF-8,
// иначе на русской локали Windows выдаёт CP866 → кракозябры в Qt.
bool GatewayController::runPwsh(const QString &script, QString *out)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    // Команда выше уже содержит [Console]::OutputEncoding=UTF8,
    // здесь читаем результат как UTF-8.
    p.start(QStringLiteral("powershell"),
            { QStringLiteral("-NoProfile"),
              QStringLiteral("-NonInteractive"),
              QStringLiteral("-Command"), script });
    if (!p.waitForFinished(15000)) {
        if (out) *out = QStringLiteral("timeout");
        return false;
    }
    const QByteArray buf = p.readAll();
    if (out) *out = QString::fromUtf8(buf);
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
        // 2) сносим ВСЕ существующие NAT, у которых подсеть совпадает с
        //    нашей (Error 52 = ERROR_DUP_NAME означает что такой NAT уже
        //    висит — обычно от прошлой кривой попытки или от WSL/Hyper-V).
        // 3) создаём свежий NetNat.
        // Вывод PowerShell в UTF-8 принудительно — без этого Qt получает
        // кракозябры и пользователь не видит реальной ошибки.
        const QString script = QStringLiteral(
            "[Console]::OutputEncoding=[Text.Encoding]::UTF8;"
            "$ErrorActionPreference='Stop';"
            "try {"
            "  Set-NetIPInterface -Forwarding Enabled -ErrorAction SilentlyContinue;"
            "  Get-NetNat -ErrorAction SilentlyContinue |"
            "    Where-Object { $_.Name -eq '%1' -or $_.InternalIPInterfaceAddressPrefix -eq '%2' } |"
            "    Remove-NetNat -Confirm:$false -ErrorAction SilentlyContinue;"
            "  Start-Sleep -Milliseconds 200;"
            "  New-NetNat -Name '%1' -InternalIPInterfaceAddressPrefix '%2' | Out-Null;"
            "  Write-Output 'OK'"
            "} catch {"
            "  Write-Output ('ERR: ' + $_.Exception.Message)"
            "}"
        ).arg(m_natName, m_subnet);
        QString out;
        const bool ok = runPwsh(script, &out);
        if (!ok || !out.contains(QStringLiteral("OK"))) {
            // Урезаем длинные сообщения, оставляем суть.
            QString brief = out.trimmed();
            const int idx = brief.indexOf(QStringLiteral("ERR:"));
            if (idx >= 0) brief = brief.mid(idx);
            if (brief.length() > 200) brief = brief.left(200) + QStringLiteral("…");
            setStatus(QStringLiteral("Не удалось включить шлюз: ") + brief);
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
