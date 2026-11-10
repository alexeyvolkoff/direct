#ifndef PLAINSERVER_H
#define PLAINSERVER_H

#include <QTcpServer>
#include <QObject>
#include <QSettings>
#include "linksocket.h"

class PlainSocketServer : public QTcpServer
{
    Q_OBJECT
private:
    int m_port;
public:
    PlainSocketServer();
    void init(QSettings *settings);
private slots:
    void readReady(LinkSocket * socket, const LinkMessage& message);
protected:
    void incomingConnection(qintptr handle);
signals:
    void plainSocketReady( LinkSocket *socket);

};

#endif // PLAINSERVER_H
