#include "udp_discovery.h"

#include <QNetworkDatagram>
#include <QNetworkInterface>

namespace {

constexpr quint8 kIdNet   = 0x77;   // настройки сети, вне штатного протокола
constexpr int    kNetLen  = 20;     // нагрузка ID_NET, см. tap_mbox.h прошивки
constexpr int    kAskMs   = 400;

/** Кадр KLF: BB 55 route mode id len нагрузка c1 c2, Флетчер-8 с байта route. */
QByteArray klfFrame(quint8 mode, quint8 id, const QByteArray& payload)
{
    QByteArray f;
    f.append(char(0xBB));
    f.append(char(0x55));
    f.append(char(0x00));                       // route
    f.append(char(mode));
    f.append(char(id));
    f.append(char(payload.size()));
    f.append(payload);
    quint8 c1 = 0, c2 = 0;
    for (int i = 2; i < f.size(); ++i) {
        c1 = quint8(c1 + quint8(f[i]));
        c2 = quint8(c2 + c1);
    }
    f.append(char(c1));
    f.append(char(c2));
    return f;
}

} // namespace

UdpDiscovery::UdpDiscovery(QObject* parent) : QObject(parent)
{
    askTimer_.setInterval(kAskMs);
    stopTimer_.setSingleShot(true);
    connect(&askTimer_, &QTimer::timeout, this, &UdpDiscovery::ask);
    connect(&stopTimer_, &QTimer::timeout, this, &UdpDiscovery::stop);
}

UdpDiscovery::~UdpDiscovery()
{
    stop();
}

QByteArray UdpDiscovery::request()
{
    // Запрос адресован широковещательному MAC: отвечают все приборы на линии.
    // Поля адресов в запросе прибор не читает, но длина должна быть полной.
    QByteArray pl(kNetLen, char(0));
    pl[0] = char(1);                            // версия раскладки
    for (int i = 2; i < 8; ++i)
        pl[i] = char(0xFF);
    return klfFrame(0x03, kIdNet, pl);          // 0x03 — GETTING
}

void UdpDiscovery::start(int msec)
{
    if (socket_)
        return;

    seen_.clear();
    socket_ = new QUdpSocket(this);
    // Слушаем на всех адресах: ответ приходит широковещательно, а сокет,
    // привязанный к одному адресу, такой датаграммы под Linux не увидит.
    if (!socket_->bind(QHostAddress::AnyIPv4, 0)) {
        delete socket_;
        socket_ = nullptr;
        emit finished(0);
        return;
    }
    connect(socket_, &QUdpSocket::readyRead, this, &UdpDiscovery::readReplies);

    emit busyChanged();
    ask();
    askTimer_.start();
    stopTimer_.start(msec);
}

void UdpDiscovery::ask()
{
    if (!socket_)
        return;

    const QByteArray req = request();
    // По широковещательному адресу каждой сети отдельно: так запрос уходит
    // именно в тот интерфейс, а не только в маршрут по умолчанию.
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (!iface.flags().testFlag(QNetworkInterface::IsUp)
            || !iface.flags().testFlag(QNetworkInterface::IsRunning)
            || iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            if (entry.broadcast().isNull())
                continue;
            socket_->writeDatagram(req, entry.broadcast(), kDiscoveryPort);
        }
    }
    socket_->writeDatagram(req, QHostAddress::Broadcast, kDiscoveryPort);
}

void UdpDiscovery::readReplies()
{
    while (socket_ && socket_->hasPendingDatagrams()) {
        const QNetworkDatagram dg = socket_->receiveDatagram();
        const QByteArray d = dg.data();
        const uchar* b = reinterpret_cast<const uchar*>(d.constData());

        for (int i = 0; i + 8 <= d.size(); ++i) {
            if (b[i] != 0xBB || b[i + 1] != 0x55)
                continue;
            const int len = b[i + 5];
            if (i + 8 + len > d.size())
                break;
            quint8 c1 = 0, c2 = 0;
            for (int k = i + 2; k < i + 6 + len; ++k) {
                c1 = quint8(c1 + b[k]);
                c2 = quint8(c2 + c1);
            }
            if (b[i + 6 + len] != c1 || b[i + 7 + len] != c2)
                continue;
            if (b[i + 4] != kIdNet || len < kNetLen) {
                i += 7 + len;
                continue;
            }

            const uchar* pl = b + i + 6;
            QString mac;
            for (int k = 0; k < 6; ++k)
                mac += QStringLiteral("%1%2").arg(k ? ":" : "")
                           .arg(pl[2 + k], 2, 16, QLatin1Char('0')).toUpper();
            const QString address = QStringLiteral("%1.%2.%3.%4")
                                        .arg(pl[8]).arg(pl[9]).arg(pl[10]).arg(pl[11]);

            if (!seen_.contains(mac)) {
                seen_.insert(mac);
                emit found(address, mac);
            }
            i += 7 + len;
        }
    }
}

void UdpDiscovery::stop()
{
    askTimer_.stop();
    stopTimer_.stop();
    if (!socket_)
        return;
    socket_->deleteLater();
    socket_ = nullptr;
    emit busyChanged();
    emit finished(seen_.size());
}
