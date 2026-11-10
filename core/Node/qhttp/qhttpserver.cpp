#include "private/qhttpserver_private.hpp"
#include <QThread>
#include <QEventLoop>
#include <QTimer>

#define CONN_CLEANUP_INTERVAL 1000

///////////////////////////////////////////////////////////////////////////////
namespace qhttp {
namespace server {
///////////////////////////////////////////////////////////////////////////////

QHttpServer::QHttpServer(QObject *parent)
    : QObject(parent), d_ptr(new QHttpServerPrivate) {
}

QHttpServer::QHttpServer(QHttpServerPrivate &dd, QObject *parent)
    : QObject(parent), d_ptr(&dd) {
}

QHttpServer::~QHttpServer() {
   // stopListening();
}

bool
QHttpServer::listen(const QString &socketOrPort, quint64 maxParsedSize, const TServerHandler &handler) {
    Q_D(QHttpServer);

    bool isNumber   = false;
    quint16 tcpPort = socketOrPort.toUShort(&isNumber);
    if ( isNumber    &&    tcpPort > 0 )
        return listen(QHostAddress::Any, tcpPort, maxParsedSize, handler);

    d->initialize(ELocalSocket, this);
    d->ihandler = handler;
    d->ilocalServer->setSocketOptions(QLocalServer::WorldAccessOption);
    d->ilocalServer->removeServer(socketOrPort);
    return d->ilocalServer->listen(socketOrPort);
}

bool
QHttpServer::listen(const QHostAddress& address, quint16 port, quint64 maxParsedSize, const qhttp::server::TServerHandler& handler) {
    Q_D(QHttpServer);

    d->initialize(ETcpSocket, this);
    d->ihandler = handler;
    m_MaxParsedSize = maxParsedSize;
	bool listening = d->itcpServer->listen(address, port);
	if (! listening && port != 0)
		listening = d->itcpServer->listen(QHostAddress::Any, 0);
	if (! listening)
		qCritical() << "Error:" <<d->itcpServer->errorString();
	return listening;
}


bool QHttpServer::listen(quint16 port, quint64 maxParsedSize, QString CApath, QString certPath, QString keyPath, QString keyPass, const qhttp::server::TServerHandler& handler) {
    Q_D(QHttpServer);

    d->initialize(ESslSocket, this);
    m_sslConfig.setCaCertificates(QSslCertificate::fromPath(CApath));

    QList<QSslCertificate> certFound = QSslCertificate::fromPath(certPath);
    certFound.append(QSslCertificate::fromPath(CApath));

    if (certFound.size() > 0)
        m_sslConfig.setLocalCertificateChain(certFound);

    QFile keyFile(keyPath);
    if (keyFile.open(QIODevice::ReadOnly))
    {
        QSslKey key(&keyFile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey, keyPass.toLocal8Bit());
        if (!key.isNull())
            m_sslConfig.setPrivateKey(key);

        keyFile.close();
    }

    m_sslConfig.setCiphers(QSslConfiguration::supportedCiphers());
    m_sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    m_sslConfig.setProtocol(QSsl::SecureProtocols);

    d->ihandler = handler;    
    m_MaxParsedSize = maxParsedSize;
    return d->itcpServer->listen(QHostAddress::AnyIPv4, port);
}

bool
QHttpServer::isListening() const {
    const Q_D(QHttpServer);

    if ( (d->ibackend == ETcpSocket  || d->ibackend == ESslSocket)  &&    d->itcpServer )
        return d->itcpServer->isListening();

    else if ( d->ibackend == ELocalSocket    &&    d->ilocalServer )
        return d->ilocalServer->isListening();

    return false;
}

void
QHttpServer::stopListening() {
    Q_D(QHttpServer);

    if ( d->itcpServer )
        d->itcpServer->close();

    if ( d->ilocalServer ) {
        d->ilocalServer->close();
        QLocalServer::removeServer( d->ilocalServer->fullServerName() );
    }

    Q_FOREACH(QHttpConnection* c, m_connList ) {
        emit c->disconnected();
    }
}

quint32
QHttpServer::timeOut() const {
    return d_func()->itimeOut;
}

void
QHttpServer::setTimeOut(quint32 newValue) {
    d_func()->itimeOut = newValue;
}

TBackend
QHttpServer::backendType() const {
    return d_func()->ibackend;
}

quint16 QHttpServer::serverPort()  {
    Q_D(QHttpServer);

    if ( (d->ibackend == ETcpSocket  || d->ibackend == ESslSocket)  &&    d->itcpServer )
        return d->itcpServer->serverPort();
    else
        return  0;
}

QTcpServer*
QHttpServer::tcpServer() const {
    return d_func()->itcpServer.data();
}

QLocalServer*
QHttpServer::localServer() const {
    return d_func()->ilocalServer.data();
}

void
QHttpServer::incomingConnection(qintptr handle) {

    //multi-threading compatible with Qt 5.9 (bionic)
    QThread *thread = new QThread();
    QHttpThreadRunner *runner = new QHttpThreadRunner(thread);

    //Important: run lambda in context (thread) of runner!
    connect(thread, &QThread::started, runner, [this, thread, handle, runner]() {
        QHttpConnection* conn = new QHttpConnection();
        if (d_func()->itimeOut)
            conn->setTimeOut(d_func()->itimeOut);
		if (conn->setSocketDescriptor(handle, backendType(), m_MaxParsedSize, m_sslConfig))
		{

			emit newConnection(conn);

            connect(conn, &QHttpConnection::newRequest, this, [ this, conn ] {
                m_connList.append(conn);
            });

            //remove from the server's list
            connect(conn, &QHttpConnection::disconnected, this, [ this, conn ] {
                m_connList.removeAll(conn);
            });

            //release
            connect(conn, &QHttpConnection::disconnected, conn, [ this, conn ] {
                QTimer::singleShot(CONN_CLEANUP_INTERVAL, conn, [conn] {
                    conn->deleteLater();
                });
            });

            connect(conn, &QHttpConnection::destroyed, thread, &QThread::quit);

            Q_D(QHttpServer);
			if ( d->ihandler )
                connect(conn, &QHttpConnection::newRequest, d->ihandler);
			else
				incomingConnection(conn);
		}
		else {
			qCritical() << "Cannot create connection";
            delete conn;
            thread->quit();
		}
	});
    // Run finished lambda in main context to insure object are deleted
    // by the main event loop
    connect(thread, &QThread::finished, this, [this, thread, runner]() {
        disconnect(thread, 0, 0, 0);
        delete runner;
        thread->deleteLater();
    });


	thread->setObjectName("UserThread");
	thread->start();
}

void
QHttpServer::incomingConnection(QHttpConnection *connection) {
    QObject::connect(connection,  &QHttpConnection::newRequest,
					 this,        &QHttpServer::newRequest, Qt::DirectConnection);
}

///////////////////////////////////////////////////////////////////////////////
}

QHttpThreadRunner::QHttpThreadRunner(QThread *thread): QObject(nullptr)
{
    this->moveToThread(thread);
}


// namespace server
} // namespace qhttp
///////////////////////////////////////////////////////////////////////////////
