/** @file qsocket.hpp
 *
 * @copyright (C) 2016
 * @date 2016.05.26
 * @version 1.0.0
 * @author amir zamani <azadkuh@live.com>
 *
 */

#ifndef __QHTTP_SOCKET_HPP__
#define __QHTTP_SOCKET_HPP__

#include "qhttpfwd.hpp"

#include <QTcpSocket>
#include <QLocalSocket>
#include <QUrl>
#include <QSsl>
#include <QSslSocket>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QPointer>
#include <QThread>
#include <QMetaObject>
///////////////////////////////////////////////////////////////////////////////
namespace qhttp {
namespace details {
///////////////////////////////////////////////////////////////////////////////

/** an adapter for different socket types.
 * the main purpose of QHttp was to create a small HTTP server with ability to
 * support UNIX sockets (QLocalSocket)
 */
class QSocket
{
public:
    void close() {
        if ( !itcpSocket.isNull() ) {
            if (itcpSocket->thread() != QThread::currentThread()) {
                QPointer<QTcpSocket> sock = itcpSocket;
                QMetaObject::invokeMethod(sock, [sock]() {
                    if (sock) sock->close();
                }, Qt::QueuedConnection);
            } else {
                itcpSocket->close();
            }
        }

        if ( !isslSocket.isNull() ) {
            if (isslSocket->thread() != QThread::currentThread()) {
                QPointer<QSslSocket> sock = isslSocket;
                QMetaObject::invokeMethod(sock, [sock]() {
                    if (sock) sock->close();
                }, Qt::QueuedConnection);
            } else {
                isslSocket->close();
            }
        }

        if ( !ilocalSocket.isNull() ) {
            if (ilocalSocket->thread() != QThread::currentThread()) {
                QPointer<QLocalSocket> sock = ilocalSocket;
                QMetaObject::invokeMethod(sock, [sock]() {
                    if (sock) sock->close();
                }, Qt::QueuedConnection);
            } else {
                ilocalSocket->close();
            }
        }
    }

    void release() {
        if ( !itcpSocket.isNull() ) {
            QPointer<QTcpSocket> sock = itcpSocket;
            itcpSocket = nullptr;
            if (sock->thread() != QThread::currentThread()) {
                QMetaObject::invokeMethod(sock, [sock]() {
                    if (sock) {
                        QObject::disconnect(sock, 0, 0, 0);
                        sock->abort();
                        sock->close();
                        sock->deleteLater();
                    }
                }, Qt::QueuedConnection);
            } else {
                QObject::disconnect(sock, 0, 0, 0);
                sock->abort();                // Cancel pending operations
                sock->close();
                sock->deleteLater();
            }
        }

        if ( ! isslSocket.isNull() ) {
            QPointer<QSslSocket> sock = isslSocket;
            isslSocket = nullptr;
            if (sock->thread() != QThread::currentThread()) {
                QMetaObject::invokeMethod(sock, [sock]() {
                    if (sock) {
                        QObject::disconnect(sock, 0, 0, 0);
                        sock->abort();
                        sock->close();
                        sock->deleteLater();
                    }
                }, Qt::QueuedConnection);
            } else {
                QObject::disconnect(sock, 0, 0, 0);
                sock->abort();
                sock->close();
                sock->deleteLater();
            }
        }

        if ( !ilocalSocket.isNull() ) {
            QPointer<QLocalSocket> sock = ilocalSocket;
            ilocalSocket = nullptr;
            if (sock->thread() != QThread::currentThread()) {
                QMetaObject::invokeMethod(sock, [sock]() {
                    if (sock) {
                        QObject::disconnect(sock, 0, 0, 0);
                        sock->abort();
                        sock->close();
                        sock->deleteLater();
                    }
                }, Qt::QueuedConnection);
            } else {
                QObject::disconnect(sock, 0, 0, 0);
                sock->abort();
                sock->close();
                sock->deleteLater();
            }
        }

        itcpSocket   = nullptr;
        isslSocket   = nullptr;
        ilocalSocket = nullptr;
    }

    qint64 waitForBytesWritten(int msecs = 30000) {
        if (!isOpen())
            return 0;

        if ( !itcpSocket.isNull() )
            return itcpSocket->waitForBytesWritten(msecs);
        else if ( !isslSocket.isNull() )
            return isslSocket->waitForBytesWritten(msecs);
        else if ( !ilocalSocket.isNull() )
            return ilocalSocket->waitForBytesWritten(msecs);

        return 0;
    }


    void flush() {
        if ( !itcpSocket.isNull() && itcpSocket->isValid() &&  itcpSocket->isOpen())
            itcpSocket->flush();
        else if ( !isslSocket.isNull()  && isslSocket->isValid() &&  isslSocket->isOpen())
            isslSocket->flush();
        else if ( !ilocalSocket.isNull() && ilocalSocket->isValid() &&  ilocalSocket->isOpen())
            ilocalSocket->flush();
    }

    bool isOpen() const {
        if ( ibackendType == ETcpSocket   &&   !itcpSocket.isNull())
            return itcpSocket->isOpen()
                && itcpSocket->state() == QTcpSocket::ConnectedState;

        else if ( ibackendType == ESslSocket    &&    !isslSocket.isNull() )
            return isslSocket->isOpen()    &&    isslSocket->state() == QSslSocket::ConnectedState;

        else if ( ibackendType == ELocalSocket    &&    !ilocalSocket.isNull() )
            return ilocalSocket->isOpen()
                && ilocalSocket->state() == QLocalSocket::ConnectedState;

        return false;
    }

    void connectTo(const QUrl& url) {
        if ( ilocalSocket )
            ilocalSocket->connectToServer(url.path());
    }

    void connectTo(const QString& host, quint16 port) {
        if ( itcpSocket )
            itcpSocket->connectToHost(host, port);
	else if ( isslSocket )
            isslSocket->connectToHostEncrypted(host, port);
    }

    qint64 readRaw(char* buffer, int maxlen) {
        if ( itcpSocket )
            return itcpSocket->read(buffer, maxlen);

        else if ( isslSocket )
            return isslSocket->read(buffer, maxlen);

        else if ( ilocalSocket )
            return ilocalSocket->read(buffer, maxlen);

        return -1;
    }

    qint64 writeRaw(const QByteArray& data) {

        if (!isOpen())
            return 0;

        if ( !itcpSocket.isNull() )
            return itcpSocket->write(data);

        else if ( !isslSocket.isNull() )
            return isslSocket->write(data);

        else if ( !ilocalSocket.isNull() )
            return ilocalSocket->write(data);

        return -1;
    }

    qint64 bytesAvailable() {
        if ( !itcpSocket.isNull() )
            return itcpSocket->bytesAvailable();

        else if ( !isslSocket.isNull() )
            return isslSocket->bytesAvailable();

        else if ( !ilocalSocket.isNull() )
            return ilocalSocket->bytesAvailable();

        return 0;
    }

	qint64 bytesToWrite() {
        if ( !itcpSocket.isNull() )
			return itcpSocket->bytesToWrite();

        else if ( !isslSocket.isNull() )
			return isslSocket->encryptedBytesToWrite();

        else if ( !ilocalSocket.isNull() )
			return ilocalSocket->bytesToWrite();

		return 0;
	}


    void disconnectAllQtConnections() {
        if ( !itcpSocket.isNull() ) {
            QObject::disconnect(itcpSocket, 0, 0, 0);
        }

        if ( !isslSocket.isNull() )
            QObject::disconnect(isslSocket, 0, 0, 0);

        if ( !ilocalSocket.isNull() )
            QObject::disconnect(ilocalSocket, 0, 0, 0);
    }

public:
    TBackend      ibackendType = ETcpSocket;
    QPointer<QTcpSocket>   itcpSocket   = nullptr;
    QPointer<QSslSocket>   isslSocket   = nullptr;
    QPointer<QLocalSocket> ilocalSocket = nullptr;
}; // class QSocket

///////////////////////////////////////////////////////////////////////////////
} // namespace details
} // namespace qhttp
///////////////////////////////////////////////////////////////////////////////
#endif // __QHTTP_SOCKET_HPP__
