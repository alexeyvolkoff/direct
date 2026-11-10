/** private imeplementation.
 * https://github.com/azadkuh/qhttp
 *
 * @author amir zamani
 * @version 2.0.0
 * @date 2014-07-11
  */

#ifndef QHTTPSERVER_CONNECTION_PRIVATE_HPP
#define QHTTPSERVER_CONNECTION_PRIVATE_HPP
///////////////////////////////////////////////////////////////////////////////

#include "qhttpserverconnection.hpp"
#include "httpparser.hxx"
#include "qhttpserverrequest.hpp"
#include "qhttpserverresponse.hpp"

#include "private/qhttpserverrequest_private.hpp"
#include "private/qhttpserverresponse_private.hpp"

#include <QBasicTimer>
#include <QFile>
#include <QThread>
#include <QPointer>


///////////////////////////////////////////////////////////////////////////////
namespace qhttp {
namespace server {
///////////////////////////////////////////////////////////////////////////////
class QHttpConnectionPrivate  :
    public details::HttpRequestParser<QHttpConnectionPrivate>
{
    Q_DECLARE_PUBLIC(QHttpConnection)
public:
        explicit QHttpConnectionPrivate(QHttpConnection* q) : q_ptr(q) {

        QHTTP_LINE_DEEPLOG
    }

    virtual ~QHttpConnectionPrivate() {        
                QHTTP_LINE_DEEPLOG
         release();
   }


    bool createSocket(qintptr sokDesc, TBackend bend, quint64 maxParseSize, const QSslConfiguration &sslConfig ) {
        isocket.ibackendType = bend;
        if ( bend == ETcpSocket ) {
            return initTcpSocket(sokDesc, maxParseSize);
        } else if ( bend == ESslSocket) {
            return initSslSocket(sokDesc, maxParseSize, sslConfig);
        } else if ( bend == ELocalSocket ) {
            return initLocalSocket(sokDesc);
        }
        return false;
    }

    void release() {
        m_reading = false;
        isocket.disconnectAllQtConnections();
        //allow disconnected link to last for some extra time to protect from connection overflow attack

        isocket.release();

        if (!ilastRequest.isNull()) {
            if (!ilastRequest->isDisowned())
                ilastRequest->deleteLater();
        }

        if (!ilastResponse.isNull()) {
            if (!ilastResponse->isDisowned())
                ilastResponse->deleteLater();
        }

		ilastRequest  = nullptr;
		ilastResponse = nullptr;
    }


	//break handling in the current http server thread
	//and move socket to client thread
	void moveSocketToThread(QThread *thread) {
        m_reading = false;
        ilastRequest->collectData(); //make sure to collect all data while we are switching
        isocket.disconnectAllQtConnections();
		if ( isocket.ibackendType == ETcpSocket ) {
			isocket.itcpSocket->moveToThread(thread);
		} else if ( isocket.ibackendType == ESslSocket) {
			isocket.isslSocket->moveToThread(thread);
		} else if ( isocket.ibackendType == ELocalSocket ) {
			isocket.ilocalSocket->moveToThread(thread);;
		}
        m_moved = true;
	}

	//continue handling in the client (link) thread
	void reinit() {

        if (!isocket.isOpen()) {
                emit q_ptr->disconnected();
                return;
        }

        if ( isocket.ibackendType == ETcpSocket ) {
			handleTcpSocket(isocket.itcpSocket);
		} else if ( isocket.ibackendType == ESslSocket) {
			handleSslSocket( isocket.isslSocket);
		} else if ( isocket.ibackendType == ELocalSocket ) {
			handleLocalSocket(isocket.ilocalSocket);
		}
		if ( isocket.bytesAvailable() > 0 ) {
            //qInfo() << "DATA available:" <<  isocket.bytesAvailable();
			onReadyRead();
		}

	}


public:
    void onReadyRead() {
          m_reading = true;
          itimer.stop();

           while (!m_released && m_reading && isocket.isOpen() && isocket.bytesAvailable() > 0 ) {
                char m_parserBuffer[4097] = {0};

                qint64 readLength = (size_t) isocket.readRaw(m_parserBuffer, 4096);

                if (readLength == -1 || readLength == 0) break;

                if (!isocket.isOpen()) break;

                try {
                    if (m_moved)
                        ilastRequest->append(m_parserBuffer, readLength);
                    else
                        parse(m_parserBuffer, readLength);
                } catch (...) {
                    qCritical() << "Request parse error";
                    m_reading = false;
                    isocket.close();
                    //finalizeConnection();
                }
                if (!m_reading) break;
                //if (!wasMoved && m_moved) break;
            }
            m_reading = false;
    }

    void finalizeConnection() {
        m_reading = false;
       // m_released = true;
        if ( ilastRequest == nullptr )
            return;

        ilastRequest->d_func()->finalizeSending([this]{
			if (ilastRequest)
				emit ilastRequest->end();
        });
    }

public:
    int  messageBegin(http_parser* parser);
    int  url(http_parser* parser, const char* at, size_t length);
    int  status(http_parser*, const char*, size_t) {
        return 0;   // not used in parsing incoming request.
    }
    int  headerField(http_parser* parser, const char* at, size_t length);
    int  headerValue(http_parser* parser, const char* at, size_t length);
    int  headersComplete(http_parser* parser);
    int  body(http_parser* parser, const char* at, size_t length);
    int  messageComplete(http_parser* parser);

  friend class QAbstractSocket;
private:
	bool m_reading = true;
    bool m_moved = false;
    bool m_released = false;

	void handleTcpSocket(QTcpSocket* sok) {
        QObject::connect(sok,  &QTcpSocket::readyRead, q_ptr, [this](){
            onReadyRead();
        });

        QObject::connect(sok,  &QTcpSocket::bytesWritten, q_ptr, [this](){
                    if (isocket.itcpSocket) {
                        auto btw = isocket.itcpSocket->bytesToWrite();
                        if (ilastResponse) {
                            if ( btw == 0 )
                                emit ilastResponse->allBytesWritten();
                            else
                                emit ilastResponse->bytesWritten();
                        }
                    }
        });

        QObject::connect(sok,  &QTcpSocket::disconnected,  q_ptr, [this, sok](){
                        m_released = true;
                        emit q_func()->disconnected();
                        //QObject::disconnect(sok, 0, 0, 0);
        });
    }
    bool initTcpSocket(qintptr sokDesc, quint64 maxParseSize) {
		QTcpSocket* sok  = new QTcpSocket(nullptr);
                isocket.itcpSocket = sok;
                bool result = sok->setSocketDescriptor(sokDesc);
                if (! result) {
                        qCritical() << "Cannot connect to socket";
                }
                sok->setSocketOption(QAbstractSocket::LowDelayOption, 1);
                sok->setSocketOption(QAbstractSocket::KeepAliveOption, 1);

                sok->setReadBufferSize(maxParseSize);

		handleTcpSocket(sok);

        return result;
    }

	void handleSslSocket(QSslSocket* sok ) {
		QObject::connect(
                           sok,   &QSslSocket::readyRead, q_ptr,
			  [this](){ onReadyRead(); }
		);
		QObject::connect(
                           sok,   &QSslSocket::encryptedBytesWritten, q_ptr,
			   [this](){
                    if (isocket.isslSocket) {
                        auto btw = isocket.isslSocket->encryptedBytesToWrite();
                        if (ilastResponse) {
                            if ( btw == 0 )
                                emit ilastResponse->allBytesWritten();
                            else
                                emit ilastResponse->bytesWritten();
                        }
                    }
		});
                QObject::connect(
                                  sok,    &QSslSocket::disconnected,  q_ptr,
                                  [this](){
                                              emit q_func()->disconnected();
                                              q_ptr->deleteLater();
                                 });
	}

    bool initSslSocket(qintptr sokDesc, quint64 maxParseSize, const QSslConfiguration &sslConfig) {
        QSslSocket* sok    = new QSslSocket( q_func() );
        isocket.isslSocket = sok;
        sok->setProtocol(QSsl::SecureProtocols);
        if (!QSslSocket::supportsSsl())
            return false;

        if (sok->setSocketDescriptor(sokDesc))
        {
            sok->setSslConfiguration(sslConfig);
            sok->setReadBufferSize(maxParseSize);
            sok->startServerEncryption();


            if (sok->waitForEncrypted())
            {
				sok->setSocketOption(QAbstractSocket::LowDelayOption, 1);
				sok->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
				handleSslSocket(sok);
                return true;
            }
            else
            {
                qCritical() << sok->errorString();
                sok->disconnect();
                return false;
            }
        }
        return false;
    }

	void handleLocalSocket(QLocalSocket* sok) {
		QObject::connect(
                                sok, &QLocalSocket::readyRead, q_ptr,
				[this](){ onReadyRead(); }
				);
		QObject::connect(
                                sok, &QLocalSocket::bytesWritten, q_ptr,
				[this](){
                    if (isocket.ilocalSocket) {
                        auto btw = isocket.ilocalSocket->bytesToWrite();
                        if (ilastResponse) {
                            if ( btw == 0 )
                                emit ilastResponse->allBytesWritten();
                            else
                                emit ilastResponse->bytesWritten();
                        }
                    }
				});
                QObject::connect(
                                  sok,    &QLocalSocket::disconnected,  q_ptr,
                                  [this](){
                                              emit q_func()->disconnected();
                                              q_ptr->deleteLater();
                                 });
        }

    bool initLocalSocket(qintptr sokDesc) {
        QLocalSocket* sok    = new QLocalSocket( q_func() );
        isocket.ilocalSocket = sok;
        bool result = sok->setSocketDescriptor(sokDesc);
		handleLocalSocket(sok);
        return result;
    }

protected:
    QHttpConnection* const q_ptr;
    QByteArray             itempUrl;

    // Since there can only be one request/response pair per connection at any
    // time even with pipelining.
    QPointer<QHttpRequest>          ilastRequest = nullptr;
    QPointer<QHttpResponse>         ilastResponse = nullptr;

    TServerHandler         ihandler      = nullptr;
};

///////////////////////////////////////////////////////////////////////////////
} // namespace server
} // namespace qhttp
///////////////////////////////////////////////////////////////////////////////
#endif // QHTTPSERVER_CONNECTION_PRIVATE_HPP
