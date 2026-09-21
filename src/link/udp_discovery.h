#pragma once

#include <QByteArray>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUdpSocket>

/**
 * Finds Kogger devices on the local network.
 *
 * The device answers a discovery request on a UDP port fixed in its firmware,
 * and it answers by broadcast: that way it is found even when its address and
 * the host's are in different subnets, which is the whole point of a discovery
 * that also works after someone mistyped an address. The request is the
 * network-settings frame of the ordinary protocol (ID_NET, 0x77) addressed to
 * the broadcast MAC, so any device on the link answers; the reply carries the
 * device's MAC and its current addresses.
 *
 * A device is identified by its MAC, because that is what the reply is keyed
 * on and what stays put when the address changes. The address from the reply
 * is what a connection is then opened to.
 *
 * The search is a short burst: the request goes out a few times over about two
 * seconds, to every interface that has a broadcast address, and answers are
 * collected the whole time. Repeats are there because a lost datagram is
 * cheaper to re-send than to detect.
 */
class UdpDiscovery : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    explicit UdpDiscovery(QObject* parent = nullptr);
    ~UdpDiscovery() override;

    bool busy() const { return socket_ != nullptr; }

    /** Starts a search of the given length; ignored while one is running. */
    Q_INVOKABLE void start(int msec = 2000);

    /** UDP port the device answers discovery on. Fixed in the firmware. */
    static constexpr quint16 kDiscoveryPort = 18020;
    /** UDP port that carries the protocol itself. Fixed in the firmware. */
    static constexpr quint16 kControlPort = 18021;

    Q_INVOKABLE int controlPort() const { return kControlPort; }

signals:
    void busyChanged();
    /** A device answered: its address to connect to, and its adapter MAC. */
    void found(const QString& address, const QString& mac);
    /** The search is over; count is how many distinct devices answered. */
    void finished(int count);

private:
    void ask();
    void readReplies();
    void stop();
    static QByteArray request();

    QUdpSocket*   socket_ = nullptr;
    QTimer        askTimer_;
    QTimer        stopTimer_;
    QSet<QString> seen_;      // по MAC: один прибор — один ответ
};
