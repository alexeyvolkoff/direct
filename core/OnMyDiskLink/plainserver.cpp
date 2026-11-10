#include "plainserver.h"
#include "cloudresource.h"
#include "onmydisklink.h"

PlainSocketServer::PlainSocketServer(): QTcpServer(nullptr)
{

}

void PlainSocketServer::init(QSettings *settings)
{
    m_port = settings->value("Network/plainPort", 9001).toUInt();
    if (listen(QHostAddress::AnyIPv4, m_port)) {
        qInfo() << "Accepting plain links on port" <<  serverPort();
    } else
        qCritical() << "Can't start plain link server on port" << m_port;
}

void PlainSocketServer::readReady(LinkSocket * socket, const LinkMessage& message)
{
    if (message.getMessageType() == LinkMessage::InitLink)
    {
       // qDebug() << socket << "Init OK";
        socket->setPeerID(message.getData()["linkid"].toString());
        socket->setUser(message.getData()["user"].toString());
        socket->setEmitMessages(false);
        emit plainSocketReady(socket);
    }
}


void PlainSocketServer::incomingConnection(qintptr handle)
{
    LinkSocket* socket = new LinkSocket(true);
    connect(socket, &LinkSocket::newMessage, this, &PlainSocketServer::readReady);

    connect(socket, &LinkSocket::connected, this, [this,  socket]() {
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        socket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, CloudResource::m_maxFileSize * 2);
        socket->setReadBufferSize(CloudResource::m_maxFileSize * 2);
        socket->setKeepAlive();
    });

    connect(socket, &QSslSocket::errorOccurred, socket, [socket] (QAbstractSocket::SocketError err)  {
        //qCritical() << "Dropped plain connection from" << socket->peerAddress();
        //qCritical() << err;
        Q_UNUSED(err)
        if (socket->getPeerID() == "") { // dropped socket
            QTimer::singleShot(LINK_CLEANUP_INTERVAL, socket, [socket]() {
                socket->deleteLater();
            });
        }
        socket->abort();
    });

    socket->setSocketDescriptor(handle);
}
