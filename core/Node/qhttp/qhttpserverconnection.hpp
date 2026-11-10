/** HTTP connection class.
 * https://github.com/azadkuh/qhttp
 *
 * @author amir zamani
 * @version 2.0.0
 * @date 2014-07-11
  */

#ifndef QHTTPSERVER_CONNECTION_HPP
#define QHTTPSERVER_CONNECTION_HPP
///////////////////////////////////////////////////////////////////////////////
#include "qhttpfwd.hpp"

#include <QObject>
#include <QSsl>
#include <QSslSocket>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslConfiguration>
#include <QMutex>

///////////////////////////////////////////////////////////////////////////////
namespace qhttp {
namespace server {
///////////////////////////////////////////////////////////////////////////////
/** a HTTP connection in the server.
 * this class controls the HTTP connetion and handles life cycle and the memory management
 *  of QHttpRequest and QHttpResponse instances autoamtically.
 */
class QHTTP_API QHttpConnection : public QObject
{
    Q_OBJECT
private:
    QMutex m_mutex;
public:
    virtual        ~QHttpConnection();

    /** set an optional timer event to close the connection. */
    void            setTimeOut(quint32 miliSeconds);

    /** forcefully kills (closes) a connection. */
    void            killConnection();

    /** optionally set a handler for connection class.
     * @note if you set this handler, the newRequest() signal won't be emitted.
     */
    void            onHandler(const TServerHandler& handler);

    /** returns the backend type of the connection. */
    TBackend        backendType() const;

    /** returns connected socket if the backend() == ETcpSocket. */
    QTcpSocket*     tcpSocket() const;

    /** returns connected ssl socket if the backend() == ESslSocket. */
    QSslSocket*     sslSocket() const;

    /** returns connected socket if the backend() == ELocalSocket. */
    QLocalSocket*   localSocket() const;

	/** moves socket to thread. */
	void moveSocketToThread(QThread * thread);

	/* reinitialize connection in other thread **/
	void reinit();
    /* check if connection is alive */
    bool isAlive();

    /* check if connection is moved to LinkThread */
    bool moved();

    /* last request */
    QString requestedUrl();

    /* started receiving */
    bool receiveStarted();

    /** creates a new QHttpConnection based on arguments. */
    /*
    static
    QHttpConnection* create(qintptr sokDescriptor, TBackend backendType, quint64 maxParseSize, QObject* parent, QString *CA = 0, QString *Cert = 0, QString *Key = 0) {
        QHttpConnection* conn = new QHttpConnection(parent);
        conn->setSocketDescriptor(sokDescriptor, backendType, maxParseSize, CA, Cert, Key);
        return conn;
    }
    */

signals:
    /** emitted when a pair of HTTP request and response are ready to interact.
     * @param req incoming request by the client.
     * @param res outgoing response to the client.
     */
    void            newRequest(QHttpRequest* req, QHttpResponse* res);

    /** emitted when the tcp/local socket, disconnects. */
    void            disconnected();

protected:
    explicit        QHttpConnection(QObject *parent = 0);
    //explicit        QHttpConnection(QHttpConnectionPrivate&, QObject *);

    bool setSocketDescriptor(qintptr sockDescriptor, TBackend backendType, quint64 maxParseSize, const QSslConfiguration &sslConfig);
    void            timerEvent(QTimerEvent*) override;

    Q_DISABLE_COPY(QHttpConnection)
    Q_DECLARE_PRIVATE(QHttpConnection)
    QScopedPointer<QHttpConnectionPrivate>    d_ptr;

    friend class    QHttpServer;
};

///////////////////////////////////////////////////////////////////////////////
} // namespace server
} // namespace qhttp
///////////////////////////////////////////////////////////////////////////////
#endif // #define QHTTPSERVER_CONNECTION_HPP
