#include "private/qhttpserverconnection_private.hpp"
#include "onmydisklink_global.h"

///////////////////////////////////////////////////////////////////////////////
namespace qhttp {
namespace server {
///////////////////////////////////////////////////////////////////////////////
QHttpConnection::QHttpConnection(QObject *parent)
    : QObject(parent), d_ptr(new QHttpConnectionPrivate(this)) {
    QHTTP_LINE_LOG
}

/*
QHttpConnection::QHttpConnection(QHttpConnectionPrivate& dd, QObject* parent)
    : QObject(parent), d_ptr(&dd) {
    QHTTP_LINE_LOG
}
*/

bool
QHttpConnection::setSocketDescriptor(qintptr sockDescriptor, TBackend backendType, quint64 maxParseSize, const QSslConfiguration &sslConfig ) {
    return d_ptr->createSocket(sockDescriptor, backendType, maxParseSize, sslConfig);
}

QHttpConnection::~QHttpConnection() {
	QHTTP_LINE_LOG
}

void
QHttpConnection::setTimeOut(quint32 miliSeconds) {
    if ( miliSeconds != 0 ) {
        d_func()->itimeOut = miliSeconds;
        d_func()->itimer.start(miliSeconds, Qt::CoarseTimer, this);
    }
}

void
QHttpConnection::killConnection() {
    //qCritical() << "Request connection timeout";
    d_func()->m_released = true;
    d_func()->m_reading = false;
    d_func()->isocket.disconnectAllQtConnections();
    d_func()->isocket.close();

    emit disconnected();
}

TBackend
QHttpConnection::backendType() const {
    return d_func()->isocket.ibackendType;
}

QTcpSocket*
QHttpConnection::tcpSocket() const {
    return d_func()->isocket.itcpSocket;
}

QSslSocket*
QHttpConnection::sslSocket() const {
    return d_func()->isocket.isslSocket;
}

QLocalSocket*
QHttpConnection::localSocket() const {
	return d_func()->isocket.ilocalSocket;
}

void QHttpConnection::moveSocketToThread(QThread *thread)
{
    QMutexLocker lock(&m_mutex);
    d_func()->itimer.stop();
	d_func()->moveSocketToThread(thread);
}

void QHttpConnection::reinit()
{
    QMutexLocker lock(&m_mutex);
    d_func()->reinit();
}

bool QHttpConnection::isAlive()
{
    return d_func()->isocket.isOpen();
}

bool QHttpConnection::moved()
{
    //QMutexLocker lock(&m_mutex);
    return d_func()->m_moved;
}

QString QHttpConnection::requestedUrl()
{
    return (receiveStarted() ? d_func()->ilastRequest->url().toString() : "(stuck)");
}

bool QHttpConnection::receiveStarted()
{
    return (d_func()->ilastRequest &&  !d_func()->ilastRequest.isNull());
}

void
QHttpConnection::onHandler(const TServerHandler &handler) {
    d_func()->ihandler = handler;
}

void
QHttpConnection::timerEvent(QTimerEvent *) {
    //qWarning() << "Connection timeout";
    if (! receiveStarted())
        killConnection();
}

///////////////////////////////////////////////////////////////////////////////

// if user closes the connection, ends the response or by any other reason the
// socket disconnects, then the irequest and iresponse instances may have
// been deleted. In these situations reading more http body or emitting end()
// for incoming request are not possible:
// if ( ilastRequest == nullptr )
//     return 0;


int
QHttpConnectionPrivate::messageBegin(http_parser*) {
    //itempUrl.clear();
    //itempUrl.reserve(128);
    itimer.stop();

	if (ilastRequest) {
        if (!ilastRequest->isDisowned())
		    ilastRequest->deleteLater();
	}

	ilastRequest = new QHttpRequest(q_func(), HTTP_MAX_HEADER_SIZE);

	return 0;
}

int
QHttpConnectionPrivate::url(http_parser*, const char* at, size_t length) {
    Q_ASSERT(ilastRequest);

    itempUrl.append(at, length);
    return 0;
}

int
QHttpConnectionPrivate::headerField(http_parser*, const char* at, size_t length) {
    if ( ilastRequest == nullptr || m_released )
        return -1;

    // insert the header we parsed previously
    // into the header map
    if ( !itempHeaderField.isEmpty() && !itempHeaderValue.isEmpty() ) {
        // header names are always lower-cased
        ilastRequest->d_func()->iheaders.insert(
                    itempHeaderField.toLower(),
                    itempHeaderValue//.toLower()
                    );
        // clear header value. this sets up a nice
        // feedback loop where the next time
        // HeaderValue is called, it can simply append
        itempHeaderField.clear();
        itempHeaderValue.clear();
    }

    itempHeaderField.append(at, length);
    return 0;
}

int
QHttpConnectionPrivate::headerValue(http_parser*, const char* at, size_t length) {
    if ( ilastRequest == nullptr || m_released)
        return -1;

    itempHeaderValue.append(at, length);
    return 0;
}

/* The one exception is on_headers_complete. In a HTTP_RESPONSE parser
* returning '1' from on_headers_complete will tell the parser that it
* should not expect a body. This is used when receiving a response to a
* HEAD request which may contain 'Content-Length' or 'Transfer-Encoding:
* chunked' headers that indicate the presence of a body.
*
* Returning `2` from on_headers_complete will tell parser that it should not
* expect neither a body nor any futher responses on this connection. This is
* useful for handling responses to a CONNECT request which may not contain
* `Upgrade` or `Connection: upgrade` headers.
*/

int
QHttpConnectionPrivate::headersComplete(http_parser* parser) {
    if ( ilastRequest.isNull() || m_released )
        return 2;


    // set method
    ilastRequest->d_func()->imethod =
            static_cast<THttpMethod>(parser->method);

    // set version
    ilastRequest->d_func()->iversion = QString("%1.%2")
                                       .arg(parser->http_major)
                                       .arg(parser->http_minor);

    // Insert last remaining header
    ilastRequest->d_func()->iheaders.insert(
                itempHeaderField.toLower(),
                itempHeaderValue//.toLower()
                );

    if (ilastRequest->d_func()->iheaders.contains("host"))
    {
        QString hostPort = ilastRequest->d_func()->iheaders["host"];
        itempUrl.prepend(hostPort.toUtf8());
        if (isocket.ibackendType == ETcpSocket)
            itempUrl.prepend("http://");
        else if (isocket.ibackendType == ESslSocket)
            itempUrl.prepend("https://");
        else if (isocket.ibackendType == ELocalSocket)
        {
            if (ilastRequest->d_func()->iheaders.contains("X-Forwarded-Proto"))
            {
                itempUrl.prepend("://");
                itempUrl.prepend(ilastRequest->d_func()->iheaders["X-Forwarded-Proto"]);
            }
            else
            {
                itempUrl.prepend("http://");
            }
        }
    }

    ilastRequest->d_func()->iurl = QUrl(itempUrl);
    ilastRequest->d_func()->iurlPath = ilastRequest->d_func()->iurl.path().split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);

    // set client information
    if ( isocket.ibackendType == ETcpSocket ) {
        ilastRequest->d_func()->iremoteAddress = isocket.itcpSocket->peerAddress().toString();
        ilastRequest->d_func()->iremotePort    = isocket.itcpSocket->peerPort();

    } else if ( isocket.ibackendType == ESslSocket ) {
        ilastRequest->d_func()->iremoteAddress = isocket.isslSocket->peerAddress().toString();
        ilastRequest->d_func()->iremotePort    = isocket.isslSocket->peerPort();

    }else if ( isocket.ibackendType == ELocalSocket ) {
        ilastRequest->d_func()->iremoteAddress = isocket.ilocalSocket->fullServerName();
        ilastRequest->d_func()->iremotePort    = 0; // not used in local sockets
    }

	if ( ilastResponse ) {
        if (!ilastResponse->isDisowned())
            ilastResponse->deleteLater();
	}

    ilastResponse  = new QHttpResponse(q_func());
    QObject::connect(ilastResponse, &QHttpResponse::done, this->q_ptr,  [this] (bool written)  {
        //if (! written) //data was not written - kill connection
            Q_UNUSED(written)
            this->q_ptr->killConnection();
    });

    if ( parser->http_major < 1 || parser->http_minor < 1  )
        ilastResponse->d_func()->ikeepAlive = false;
    else
        ilastResponse->d_func()->ikeepAlive = true;
    // we are good to go!
    if ( ihandler )
        ihandler(ilastRequest, ilastResponse);
    else
        emit q_ptr->newRequest(ilastRequest, ilastResponse);

    if (!isocket.isOpen()) {
        return 2;
    }

    return 0;
}

int
QHttpConnectionPrivate::body(http_parser*, const char* at, size_t length) {
    if ( ilastRequest.isNull() || m_released ) {
        return -1;
    }
     if (!isocket.isOpen()) {
        return -1;
    }

    ilastRequest->d_func()->ireadState = QHttpRequestPrivate::EPartial;

    ilastRequest->append(at, length);

    return 0;
}

int
QHttpConnectionPrivate::messageComplete(http_parser*) {
    if ( ilastRequest.isNull()|| m_released )
        return -1;

    itimer.stop();
    m_reading = false;
    // request is done
    finalizeConnection();
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
} // namespace server
} // namespace qhttp
///////////////////////////////////////////////////////////////////////////////
