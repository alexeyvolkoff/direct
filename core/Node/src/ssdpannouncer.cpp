#include "ssdpannouncer.h"
#include "version.h"
#include <QUuid>
#include <QDateTime>
#include <QDebug>
#include <QNetworkInterface>

SsdpAnnouncer::SsdpAnnouncer(QObject *parent)
    : QObject(parent),
      m_udpSocket(new QUdpSocket(this)),
      m_broadcastTimer(new QTimer(this)),
      m_httpPort(8080) // default
{
    m_uuid = "uuid:" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_deviceType = "urn:schemas-upnp-org:device:MediaServer:1";
    connect(m_broadcastTimer, &QTimer::timeout, this, &SsdpAnnouncer::sendAllNotifies);

    connect(m_udpSocket, &QUdpSocket::readyRead, this, [=]() {
        while (m_udpSocket->hasPendingDatagrams()) {
            QByteArray datagram;
            datagram.resize(m_udpSocket->pendingDatagramSize());
            QHostAddress sender;
            quint16 senderPort;
            m_udpSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

            if (datagram.contains("M-SEARCH") && datagram.contains("ssdp:discover")) {
                QString location = QString("http://%1:%2/description.xml").arg(m_bindAddr.toString()).arg(m_httpPort);
                auto sendResp = [&](const QString &stTarget, const QString &usnTarget) {
                    QByteArray response = QString(
                        "HTTP/1.1 200 OK\r\n"
                        "CACHE-CONTROL: max-age=1800\r\n"
                        "EXT:\r\n"
                        "LOCATION: %1\r\n"
                        "SERVER: %2\r\n"
                        "ST: %3\r\n"
                        "USN: %4\r\n"
                        "BOOTID.UPNP.ORG: 1\r\n"
                        "CONFIGID.UPNP.ORG: 1\r\n"
                        "\r\n"
                    ).arg(location, m_serverName, stTarget, usnTarget).toUtf8();
                    m_udpSocket->writeDatagram(response, sender, senderPort);
                };

                if (datagram.contains("ST: ssdp:all")) {
                    sendResp("upnp:rootdevice", QString("uuid:%1::upnp:rootdevice").arg(m_uuid));
                    sendResp("uuid:" + m_uuid, "uuid:" + m_uuid);
                    sendResp(m_deviceType, QString("uuid:%1::%2").arg(m_uuid, m_deviceType));
                } else if (datagram.contains("ST: upnp:rootdevice")) {
                    sendResp("upnp:rootdevice", QString("uuid:%1::upnp:rootdevice").arg(m_uuid));
                } else if (datagram.contains(("ST: uuid:" + m_uuid).toUtf8())) {
                    sendResp("uuid:" + m_uuid, "uuid:" + m_uuid);
                } else if (datagram.contains(("ST: " + m_deviceType).toUtf8()) || datagram.contains("ST: urn:schemas-upnp-org:device:MediaServer")) {
                    sendResp(m_deviceType, QString("uuid:%1::%2").arg(m_uuid, m_deviceType));
                }
            }
        }
    });
}

SsdpAnnouncer::~SsdpAnnouncer()
{
    delete m_udpSocket;
    delete m_broadcastTimer;
}

void SsdpAnnouncer::setHostName(const QString &name) {
    m_hostName = name;
}

void SsdpAnnouncer::setServerName(const QString &server) {
    m_serverName = server;
}

void SsdpAnnouncer::setModelName(const QString &model) {
    m_modelName = model;
}

void SsdpAnnouncer::setHttpServerPort(quint16 port) {
    m_httpPort = port;
}

void SsdpAnnouncer::start() {

    QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    QNetworkInterface selectedInterface;
    m_bindAddr = QHostAddress::LocalHost;

    for (const QNetworkInterface &iface : interfaces) {
        if (iface.flags().testFlag(QNetworkInterface::IsUp) &&
            iface.flags().testFlag(QNetworkInterface::IsRunning) &&
            iface.flags().testFlag(QNetworkInterface::CanMulticast) &&
            !iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {

            for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                    selectedInterface = iface;
                    m_bindAddr = entry.ip();
                    qDebug() << "Selected multicast interface:" << iface.humanReadableName();
                    break;
                }
            }

            if (selectedInterface.isValid())
                break;
        }
    }

    if (!selectedInterface.isValid()) {
        qWarning() << "No suitable network interface found for SSDP.";
        return;
    }


    if (!m_udpSocket->bind(QHostAddress::AnyIPv4, 1900,
        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        qWarning() << "Failed to bind SSDP socket";
        return;
    }

    m_udpSocket->setMulticastInterface(selectedInterface);
    m_udpSocket->setSocketOption(QAbstractSocket::MulticastTtlOption, 2);


    // Join multicast group explicitly
    if (!m_udpSocket->joinMulticastGroup(QHostAddress("239.255.255.250"))) {
        qWarning() << "Failed to join multicast group";
        return;
    }

    qInfo() << "SSDP announcer started on interface:" << selectedInterface.humanReadableName() << "IP:" << m_bindAddr.toString();

    sendAllNotifies();

    m_broadcastTimer->start(30000); // every 30 sec
}

void SsdpAnnouncer::stop()
{
    QString nt = "upnp:rootdevice";
    QString usn = QString("uuid:%1::upnp:rootdevice").arg(m_uuid);
    QString notify =
        "NOTIFY * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "NT: " + nt + "\r\n"
        "NTS: ssdp:alive\r\n"
        "USN: " + usn + "\r\n"
        "NTS: ssdp:byebye\r\n"
        "\r\n";

    m_udpSocket->writeDatagram(notify.toUtf8(), QHostAddress("239.255.255.250"), 1900);

    m_broadcastTimer->stop();
}

void SsdpAnnouncer::sendAllNotifies()
{
    sendNotify("upnp:rootdevice", QString("uuid:%1::upnp:rootdevice").arg(m_uuid));
    sendNotify("uuid:" + m_uuid, "uuid:" + m_uuid);
    sendNotify(m_deviceType, QString("uuid:%1::%2").arg(m_uuid, m_deviceType));
}

void SsdpAnnouncer::sendNotify(const QString &nt, const QString &usn) {
    QString location = QString("http://%1:%2/description.xml").arg(m_bindAddr.toString()).arg(m_httpPort);
    QString notify =
        "NOTIFY * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "LOCATION: " + location + "\r\n"
        "NT: " + nt + "\r\n"
        "NTS: ssdp:alive\r\n"
        "SERVER: " + m_serverName + "\r\n"
        "USN: " + usn + "\r\n"
        "DLNA.ORG_OP: 01\r\n"
        "DLNA.ORG_CI: 0\r\n"
        "\r\n";

    m_udpSocket->writeDatagram(notify.toUtf8(), QHostAddress("239.255.255.250"), 1900);
}


QByteArray SsdpAnnouncer::buildSearchResponse() {
    QString location = QString("http://%1:%2/description.xml").arg(m_bindAddr.toString()).arg(m_httpPort);
    return QString(
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "EXT:\r\n"
        "LOCATION: %1\r\n"
        "SERVER: %2\r\n"
        "ST: %3\r\n"
        "USN: %4::%3\r\n"
        "BOOTID.UPNP.ORG: 1\r\n"
        "CONFIGID.UPNP.ORG: 1\r\n"
        "\r\n"
    ).arg(location, m_serverName, m_deviceType, m_uuid).toUtf8();
}

QString SsdpAnnouncer::deviceDescriptionXml() {
    QString host = !m_bindAddr.isNull() && m_bindAddr != QHostAddress::LocalHost ? m_bindAddr.toString() : m_hostName;
    QString presentationUrl = (m_httpPort == 80) ? QString("http://%1/").arg(host) : QString("http://%1:%2/").arg(host).arg(m_httpPort);
    return QString(R"(
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <device>
    <deviceType>%1</deviceType>
    <friendlyName>%2 (%3)</friendlyName>
    <manufacturer>%4</manufacturer>
    <manufacturerURL>https://disk.direct</manufacturerURL>
    <modelName>%5</modelName>
    <modelNumber>%6</modelNumber>
    <modelDescription>%7</modelDescription>
    <modelURL>https://docs.disk.direct/advanced/</modelURL>
    <presentationURL>%8</presentationURL>
    <UDN>%9</UDN>
    <serialNumber>%10</serialNumber>
    <iconList>
      <icon>
        <mimetype>image/png</mimetype>
        <width>128</width>
        <height>128</height>
        <depth>24</depth>
        <url>/icons/onmydisk.png</url>
      </icon>
    </iconList>
 </device>
</root>)").arg(m_deviceType, APP_NAME, m_hostName, ORGANIZATION, m_modelName, APP_VERSION, DESCRIPTION, presentationUrl, m_uuid, "OMD111");
}


void SsdpAnnouncer::sendDeviceDescription(QHttpResponse *resp)
{
    QString descr = deviceDescriptionXml();
    QByteArray descrBytes = descr.toUtf8();
    resp->addHeader("Content-Type", "text/xml; charset=UTF-8");
    resp->addHeader("Content-Length", QString::number(descrBytes.size()).toUtf8());
    resp->addHeader("Server", m_serverName.toUtf8());
    resp->setStatusCode( qhttp::ESTATUS_OK);
    resp->end(descrBytes);
}
