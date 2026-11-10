#include "private/qhttpserverresponse_private.hpp"
#include <QThread>

///////////////////////////////////////////////////////////////////////////////
namespace qhttp {
namespace server {
///////////////////////////////////////////////////////////////////////////////
QHttpResponse::QHttpResponse(QHttpConnection* conn)
    : QHttpAbstractOutput(nullptr) , d_ptr(new QHttpResponsePrivate(conn, this)) {
    d_ptr->initialize();
    QHTTP_LINE_LOG
}

QHttpResponse::QHttpResponse(QHttpResponsePrivate& dd, QHttpConnection* conn)
    : QHttpAbstractOutput(nullptr) , d_ptr(&dd) {
    Q_UNUSED(conn)
    d_ptr->initialize();
    QHTTP_LINE_LOG
}

QHttpResponse::~QHttpResponse() {
    QHTTP_LINE_LOG
}

void
QHttpResponse::setStatusCode(TStatusCode code) {
    if (QThread::currentThread() != this->thread()) {
        QPointer<QHttpResponse> that(this);
        QMetaObject::invokeMethod(this, [that, code] {
            if (that) that->setStatusCode(code);
        }, Qt::QueuedConnection);
    } else {
        d_func()->istatus   = code;
        emit statuscodeIsSet(code);
    }
}

TStatusCode QHttpResponse::getStatusCode()
{
    return d_func()->istatus;
}

void
QHttpResponse::setVersion(const QString &versionString) {
    d_func()->iversion  = versionString;
}

void
QHttpResponse::addHeader(const QByteArray &field, const QByteArray &value) {
    if (QThread::currentThread() != this->thread()) {
        QPointer<QHttpResponse> that(this);
        QMetaObject::invokeMethod(this, [that, field, value] {
            if (that) that->addHeader(field, value);
        }, Qt::QueuedConnection);
    } else {
        d_func()->addHeader(field, value);
        emit headerAdded(field, value);
    }
}

void
QHttpResponse::addHeaders(const QMultiHash<QByteArray, QByteArray> &headers)
{
    if (QThread::currentThread() != this->thread()) {
        QPointer<QHttpResponse> that(this);
        QMetaObject::invokeMethod(this, [that, headers] {
            if (that) that->addHeaders(headers);
        }, Qt::QueuedConnection);
    } else {
        Q_FOREACH(QByteArray key, headers.keys())
        {
            d_func()->addHeader(key, headers.value(key));
            emit headerAdded(key, headers.value(key));
        }
    }
}

THeaderHash&
QHttpResponse::headers() {
    return d_func()->iheaders;
}

bool QHttpResponse::send100Continue()
{
    return d_func()->send100Continue();
}

bool QHttpResponse::send102Progress(const QString& progress)
{
    return d_func()->send102Progress(progress);
}

bool QHttpResponse::write(const QByteArray &data) {
    if (QThread::currentThread() != this->thread()) {
        // Called from another thread
        QPointer<QHttpResponse> that(this);
        QMetaObject::invokeMethod(this, [that, data] {
            if (!that) return;  // Object deleted
            that->write(data);
        }, Qt::QueuedConnection);
        return true;
    } else {
        bool written = d_func()->writeData(data);
        if (written) {
            emit bytesAdded(data);
        }
        return written;
    }
}

bool QHttpResponse::waitForBytesWritten(int msecs)
{
    return d_func()->waitForBytesWritten(msecs);
}

void QHttpResponse::flush()
{
    if (QThread::currentThread() != this->thread()) {
        QPointer<QHttpResponse> that(this);
        QMetaObject::invokeMethod(this, [that] {
            if (that) that->flush();
        }, Qt::QueuedConnection);
    } else {
        d_func()->flush();
    }
}

void
QHttpResponse::end(const QByteArray &data) {

    if (QThread::currentThread() != this->thread()) {
        // Called from another thread
        QPointer<QHttpResponse> that(this);
        QMetaObject::invokeMethod(this, [that, data] {
            if (!that) return;  // Object deleted
            that->end(data);
        }, Qt::QueuedConnection);
    } else {
        Q_D(QHttpResponse);
        emit bytesAdded(data);
        bool written = d->endPacket(data);
        emit done(written);
    }
}

qint64 QHttpResponse::bytesSent()
{
    return d_func()->bytesSent();
}

qint64 QHttpResponse::bytesToSend()
{
	return d_func()->bytesToSend();
}


QHttpConnection*
QHttpResponse::connection() const {
    return d_func()->iconnection;
}

///////////////////////////////////////////////////////////////////////////////
QByteArray
QHttpResponsePrivate::makeTitle() {

    QString title = QString("HTTP/%1 %2 %3\r\n")
                    .arg(iversion)
                    .arg(istatus)
                    .arg(Stringify::toString(istatus));

    return title.toLatin1();
}

void
QHttpResponsePrivate::prepareHeadersToWrite() {

    if ( !iheaders.contains("Date") ) {
        // Sun, 06 Nov 1994 08:49:37 GMT - RFC 822. Use QLocale::c() so english is used for month and
        // day.
        QString dateString = QLocale::c().
            toString(QDateTime::currentDateTimeUtc(),
                    "ddd, dd MMM yyyy hh:mm:ss")
            .append(" GMT");
        addHeader("Date", dateString.toLatin1());
    }
}

///////////////////////////////////////////////////////////////////////////////
} // namespace server
} // namespace qhttp
///////////////////////////////////////////////////////////////////////////////
