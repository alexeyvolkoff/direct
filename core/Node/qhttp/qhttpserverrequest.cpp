#include "private/qhttpserverrequest_private.hpp"

///////////////////////////////////////////////////////////////////////////////
namespace qhttp {
namespace server {
///////////////////////////////////////////////////////////////////////////////
QHttpRequest::QHttpRequest(QHttpConnection *conn, int collectAtMost)
    : QHttpAbstractInput(nullptr),  d_ptr(new QHttpRequestPrivate(conn, this)) {
	QHTTP_LINE_LOG
	d_ptr->initialize();
    d_func()->collectData(collectAtMost);
}

QHttpRequest::QHttpRequest(QHttpRequestPrivate &dd, QHttpConnection *conn)
    : QHttpAbstractInput(nullptr), d_ptr(&dd) {
    Q_UNUSED(conn)
    d_ptr->initialize();
    QHTTP_LINE_LOG
}

QHttpRequest::~QHttpRequest() {
    QHTTP_LINE_LOG
}

THttpMethod QHttpRequest::method() const {
    return d_func()->imethod;
}

const QString QHttpRequest::methodString() const {
    return http_method_str(static_cast<http_method>(d_func()->imethod));
}

const QUrl& QHttpRequest::url() const {
    return d_func()->iurl;
}

const QStringList &QHttpRequest::urlPath() const {
    return d_func()->iurlPath;
}

const QString& QHttpRequest::httpVersion() const {
    return d_func()->iversion;
}

const THeaderHash& QHttpRequest::headers() const {
    return d_func()->iheaders;
}

const QString& QHttpRequest::remoteAddress() const {
    return d_func()->iremoteAddress;
}

quint16 QHttpRequest::remotePort() const {
    return d_func()->iremotePort;
}

bool QHttpRequest::isSuccessful() const {
    return d_func()->isuccessful;
}

void QHttpRequest::collectData(int atMost) {
     d_func()->collectData(atMost);
}

void QHttpRequest::stopCollectData() {
    d_func()->stopCollectData();
}

const QByteArray &QHttpRequest::collectedData() const
{
    return d_func()->icollectedData;
}

void QHttpRequest::append(const char *at, size_t length)
{
    QMutexLocker lock(&m_mutex);
    if (d_func()->icollectRequired)
        d_func()->append(at, length);
    else {
        lock.unlock();
        emit data(QByteArray(at, length));
    }
}

//thread-safe getter of data. stops receiving to buffer
QByteArray QHttpRequest::getCollectedData()
{
    QMutexLocker lock(&m_mutex);
    QByteArray data = d_func()->getCollectedData();
    d_func()->stopCollectData();
    return data;
}


QHttpConnection* QHttpRequest::connection() const
{
    return d_ptr->iconnection;
}

bool QHttpRequest::isDropped() const
{
    return d_ptr->isDropped;
}

void QHttpRequest::setDropped()
{
	if (d_ptr && ! d_ptr.isNull())
        d_ptr->isDropped = true;
}

void QHttpRequest::setSessionId(const QString& sessionId)
{
    if (sessionId.isEmpty() || sessionId.isNull()) {
        qCritical() << "Invalid session for" << this;
    }
    m_sessionId = sessionId;
}

QString QHttpRequest::sessionId() {
    return m_sessionId;
}

QHash<QString, QString> QHttpRequest::parseCookies()
{
#if QT_VERSION < 0x050F00
    QString::SplitBehavior sb = QString::SplitBehavior::SkipEmptyParts;
    #else
    Qt::SplitBehaviorFlags sb = Qt::SplitBehaviorFlags::SkipEmptyParts;
    #endif
    QString allCookies = headers().value("cookie");

    QStringList cookies = allCookies.split("; ", sb);
    QHash<QString, QString> parsedCookies;
    Q_FOREACH(QString cookie, cookies)
    {
        QStringList keyValue = cookie.split('=', sb);
        if (! parsedCookies.keys().contains(keyValue[0])) { //avoid overwritting top-domain cookies by sub-domain cookies
            if (keyValue.size() == 2)
                parsedCookies.insert(keyValue[0], keyValue[1]);
            else
                parsedCookies.insert(keyValue[0], "true");
        }
    }
    return parsedCookies;
}

///////////////////////////////////////////////////////////////////////////////
} // namespace server
} // namespace qhttp
///////////////////////////////////////////////////////////////////////////////
