/** @file httpwriter.hxx
 *
 * @copyright (C) 2016
 * @date 2016.05.26
 * @version 1.0.0
 * @author amir zamani <azadkuh@live.com>
 *
 */

#ifndef __QHTTP_HTTPWRITER_HXX__
#define __QHTTP_HTTPWRITER_HXX__

#include "qhttpbase.hpp"

///////////////////////////////////////////////////////////////////////////////
namespace qhttp {
namespace details {
///////////////////////////////////////////////////////////////////////////////

// usage in client::QHttpRequest, server::QHttpResponse
template<class TBase, class TImpl>
class HttpWriter : public TBase
{
public:
    bool addHeader(const QByteArray &field, const QByteArray &value) {
        if ( ifinished )
            return false;

        TBase::iheaders.insert(field, value);
        return true;
    }

    bool writeHeader(const QByteArray& field, const QByteArray& value) {
        if ( ifinished )
            return false;

        QByteArray buffer = QByteArray(field)
                            .append(": ")
                            .append(value)
                            .append("\r\n");

        m_bytesSent += isocket.writeRaw(buffer);
        return true;
    }

    bool send100Continue() {
        if ( ifinished )
            return false;

        m_bytesSent += isocket.writeRaw("HTTP/1.1 100 Continue\r\n\r\n");
        return true;
    }

    bool send102Progress(const QString& progress) {
        if ( ifinished )
            return false;

        m_bytesSent += isocket.writeRaw(progress.toUtf8());
        return true;
    }


    bool writeData(const QByteArray& data) {
        if ( ifinished )
            return false;

        ensureWritingHeaders();
        qint64 written = 0;
        if (! data.isEmpty())
             written = isocket.writeRaw(data);
        m_bytesSent += written;
        return (written > 0);
    }

    void flush() {
        if ( ifinished )
            return;
        isocket.flush();
        return ;
    }

    bool waitForBytesWritten(int msecs) {
        if ( ifinished )
            return false;

        return isocket.waitForBytesWritten(msecs);
    }

    bool endPacket(const QByteArray& data) {
        bool written = writeData(data);
        ifinished = true;
        return written;
    }


    void ensureWritingHeaders() {
        if ( ifinished    ||    iheaderWritten )
            return;

        TImpl* me = static_cast<TImpl*>(this);
        isocket.writeRaw(me->makeTitle());
        writeHeaders(true);

        iheaderWritten = true;
    }

    void writeHeaders(bool doFlush = false) {
        if ( ifinished    ||    iheaderWritten )
            return;

        if ( TBase::iheaders.keyHasValue("Connection", "keep-alive") )
            ikeepAlive = true;
        else
            TBase::iheaders.insert("Connection", "close");

        TImpl* me = static_cast<TImpl*>(this);
        me->prepareHeadersToWrite();

        for (THeaderHash::iterator i = TBase::iheaders.begin(); i != TBase::iheaders.end(); ++i)
            this->writeHeader(
                        i.key(),
                        i.value()
                        );

        isocket.writeRaw("\r\n");
        if ( doFlush )
            isocket.flush();
    }

    qint64 bytesSent() {
        return m_bytesSent;
    }

	qint64 bytesToSend() {
		return isocket.bytesToWrite();
	}


public:
    QSocket isocket;

    bool    ifinished      = false;
    bool    iheaderWritten = false;
    bool    ikeepAlive     = false;

private:
    qint64 m_bytesSent = 0;
};


///////////////////////////////////////////////////////////////////////////////
} // namespace details
} // namespace qhttp
///////////////////////////////////////////////////////////////////////////////
#endif // __QHTTP_HTTPWRITER_HXX__
