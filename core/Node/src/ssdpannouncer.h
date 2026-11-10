#ifndef SSDPANNOUNCER_H
#define SSDPANNOUNCER_H

#include <QObject>
#include <QUdpSocket>
#include <QHostAddress>
#include <QTimer>
#include <QString>
#include "qhttpserverresponse.hpp"


using namespace qhttp;
using namespace server;

class SsdpAnnouncer : public QObject
{
    Q_OBJECT
public:
    explicit SsdpAnnouncer(QObject *parent = nullptr);
    ~SsdpAnnouncer();

    void setHostName(const QString &name);
    void setModelName(const QString &model);
    void setServerName(const QString &server);
    void setHttpServerPort(quint16 port);

    void start();
    void stop();
    //Send description
    void sendDeviceDescription(QHttpResponse* resp);
private:
    QString m_hostName;
    QString m_modelName;
    QString m_serverName;
    QString m_uuid;
    QString m_deviceType;
    QUdpSocket *m_udpSocket;
    QTimer *m_broadcastTimer;
    quint16 m_httpPort;
    QHostAddress m_bindAddr; // bind address


    void sendNotify(const QString &nt, const QString &usn);
    void sendAllNotifies();
    QByteArray buildSearchResponse();
    QString deviceDescriptionXml();

};


#endif // SSDPANNOUNCER_H
