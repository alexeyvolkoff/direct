/** HTTP response from a server.
 * https://github.com/azadkuh/qhttp
 *
 * @author amir zamani
 * @version 2.0.0
 * @date 2014-07-11
  */

#ifndef QHTTPSERVER_RESPONSE_HPP
#define QHTTPSERVER_RESPONSE_HPP

///////////////////////////////////////////////////////////////////////////////

#include "qhttpabstracts.hpp"

///////////////////////////////////////////////////////////////////////////////
namespace qhttp {
namespace server {
///////////////////////////////////////////////////////////////////////////////
/** The QHttpResponse class handles sending data back to the client as a response to a request.
 * @sa QHttpConnection
 */
class QHTTP_API QHttpResponse : public QHttpAbstractOutput
{
    Q_OBJECT

private:
    bool m_disowned = false;

public:
    virtual        ~QHttpResponse();

    void disown() { setParent(nullptr); m_disowned = true; }
    bool isDisowned() const { return m_disowned; }

public:
    /** set the response HTTP status code. @sa TStatusCode.
     * default value is ESTATUS_BAD_REQUEST.
     * @sa write()
     */
    void            setStatusCode(TStatusCode code);
    /** get the response HTTP status code.
     */
    TStatusCode            getStatusCode();

public: // QHttpAbstractOutput methods:
    /** @see QHttpAbstractOutput::setVersion(). */
    void            setVersion(const QString& versionString) override;

    /** @see QHttpAbstractOutput::addHeader(). */
    void            addHeader(const QByteArray& field, const QByteArray& value) override;

    /** @see QHttpAbstractOutput::addHeaders(). */
    void            addHeaders(const QMultiHash<QByteArray, QByteArray> &headers) override;

    /** @see QHttpAbstractOutput::headers(). */
    THeaderHash&    headers() override;

    bool send100Continue();
    bool send102Progress(const QString &progress);


    /** @see QHttpAbstractOutput::write(). */
    bool waitForBytesWritten(int msecs = 30000) override;


    qint64 bytesSent();
	qint64 bytesToSend();
    void flush();

signals:
    void headerAdded(const QByteArray& field, const QByteArray& value);
    void bytesAdded(const QByteArray &data);
    void statuscodeIsSet(TStatusCode code);

public:
    /** returns the parent QHttpConnection object. */
    QHttpConnection* connection() const;

public slots:
    /** @see QHttpAbstractOutput::write(). */
    bool write(const QByteArray &data) override;
    /** @see QHttpAbstractOutput::end(). */
    void end(const QByteArray &data = QByteArray()) override;

protected:
    explicit        QHttpResponse(QHttpConnection*);
    explicit        QHttpResponse(QHttpResponsePrivate&, QHttpConnection*);
    friend class    QHttpConnectionPrivate;

    Q_DECLARE_PRIVATE(QHttpResponse)
    QScopedPointer<QHttpResponsePrivate> d_ptr;
};

///////////////////////////////////////////////////////////////////////////////
} // namespace server
} // namespace qhttp
///////////////////////////////////////////////////////////////////////////////
#endif // define QHTTPSERVER_RESPONSE_HPP
