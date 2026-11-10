#include "filereader.h"

FileReader::FileReader(CloudResource *IO, QString rangeString, bool saveAs, bool useChunked, QObject *parent) : QObject(parent), m_IO(IO)
{
    m_bodyLength = 0;
    m_currentRangeIndex = 0;
    m_currentRangePosition = 0;
    m_rangeString = rangeString;
    m_forceSaveAs = saveAs;
    m_chunked = useChunked;
    parsePartialHeaderString();
    //calculateBodyLength();
}

FileReader::FileReader(CloudResource *IO, QObject *parent): QObject(parent), m_IO(IO)
{
    m_bodyLength = 0;
    m_currentRangeIndex = 0;
    m_currentRangePosition = 0;
    m_rangeString = "";
    m_forceSaveAs = "";
    m_chunked = true;
    m_getType = SimpleGet;
    m_ranges.clear();
    m_currentRangeIndex = 0;
}

bool FileReader::invoke(const QString &rpc, const QHash<QString, QVariant> &params, const QByteArray &payload)
{
    m_backend = m_IO ? m_IO->backend() : nullptr;
    if (m_backend) {
        connect(m_backend, &IResourceBackend::responseReady, this, &FileReader::responseReady);
        return m_backend->invoke(rpc, params, payload);
    } else
        return false;
}

FileReader::~FileReader()
{
    if (m_backend) {
        disconnect(m_backend, 0, 0, 0);
    }
	m_ranges.clear();
}

void FileReader::parsePartialHeaderString()
{
    m_getType = SimpleGet;
    m_ranges.clear();
    m_currentRangeIndex = 0;
    if (m_rangeString != "")
    {
        if (m_rangeString.startsWith("bytes="))
        {
            m_rangeString = m_rangeString.toLower().replace("bytes=","");
            QStringList stringRanges = m_rangeString.split(',', Qt::SplitBehaviorFlags::SkipEmptyParts);
            Q_FOREACH (QString r, stringRanges) {
                HttpGetRange *httpRange = new HttpGetRange(r, m_IO, false, false, this);
                //connect(httpRange, &HttpGetRange::payloadReady, this, &FileReader::rangePayloadReady);
                if (httpRange->isValid())
                    m_ranges.append(httpRange);
            }

            if (m_ranges.count() == 0)
                m_getType = NotSatisfiableRangeGet;
            else if (m_ranges.count() == 1)
                m_getType = SingleRangeGet;
            else
            {
                m_getType = MultiRangeGet;
                Q_FOREACH (HttpGetRange *r, m_ranges) {
                    r->setMulti(true);
                }
                m_ranges.last()->setLast(true);
            }
        }
        else
            m_getType = NotSatisfiableRangeGet;
    }
    else
    {
        m_getType = SimpleGet;
        HttpGetRange *httpRange = new HttpGetRange(m_IO, false, false, this);
        //connect(httpRange, &HttpGetRange::payloadReady, this, &FileReader::rangePayloadReady);
        m_ranges.append(httpRange);
    }
}

void FileReader::calculateBodyLength()
{
    switch (m_getType) {
    case SimpleGet:
    case SingleRangeGet:
        m_ranges.first()->calculateBodySize();
        m_bodyLength =  m_ranges.first()->size();
        break;
    case MultiRangeGet:
    {
        m_bodyLength = 0;
        Q_FOREACH (HttpGetRange *r, m_ranges) {
            r->calculateBodySize();
            m_bodyLength += r->size();
        }
        break;
    }
    case NotSatisfiableRangeGet:
    default:
        m_bodyLength = 0;
        break;
    }
}

IOType FileReader::getRequestGetType()
{
    return m_getType;
}

QByteArray FileReader::read(qint64 maxlen)
{
   // qDebug() << "Reading " << maxlen << " bytes, staring from " << m_currentRangePosition << " in " << m_CurrentRangeIndex << " range of" << m_ranges.count();
    QByteArray bytesToSend;

    if (m_getType == NotSatisfiableRangeGet)
        return bytesToSend;

    qint64 bytesReadLastTime = 0;
    do
    {
        bytesReadLastTime = bytesToSend.count();
        bytesToSend.append(m_ranges[m_currentRangeIndex]->read(m_currentRangePosition, maxlen));
        bytesReadLastTime = bytesToSend.count() - bytesReadLastTime;
        m_currentRangePosition += bytesReadLastTime;

        if (m_ranges[m_currentRangeIndex]->atEnd(m_currentRangePosition)
                && m_currentRangeIndex + 1 < (quint64)m_ranges.count())
        {
            m_currentRangeIndex++;
            m_currentRangePosition = 0;
        }

    } while(bytesReadLastTime && m_IO->isOpen() &&  maxlen > (qint64)bytesToSend.size());



    return bytesToSend;
}

qint64 FileReader::getBlockSize(qint64 maxlen)
{
    switch (m_getType)
    {
        case SingleRangeGet:
        case MultiRangeGet:
        {
            return  m_bodyLength < (quint64) maxlen ? m_bodyLength : maxlen;
        }
        default: return maxlen;
    }
}



QByteArray FileReader::getLastBytes()
{
    if (m_chunked)
        return m_lastChunkBytes;
    else
        return QByteArray();
}

bool FileReader::atEnd()
{
    return m_ranges[m_currentRangeIndex]->atEnd(m_currentRangePosition);
}

quint64 FileReader::size()
{
    return m_bodyLength;
}

QMultiHash<QByteArray, QByteArray> FileReader::getHeaders()
{
    QMultiHash<QByteArray, QByteArray> headers;
    switch (m_getType)
    {
        case SimpleGet:
        {
            if (m_chunked)
                headers.insert("Transfer-Encoding", "chunked");
            else
                headers.insert("Content-Length", QString::number(m_bodyLength).toUtf8());
            headers.insert("Content-Type", m_IO->getContentType().toUtf8());
            if (m_forceSaveAs)
            {
                headers.insert("Content-Disposition", QString("attachment; filename=\"%1\"").arg(m_IO->fileName()).toUtf8());
                headers.insert("Cache-Control", QString("must-revalidate, post-check=0, pre-check=0").toUtf8());
            }
            break;
        }
        case SingleRangeGet:
        {
            if (m_chunked)
                headers.insert("Transfer-Encoding", "chunked");
            else
                headers.insert("Content-Length", QString::number(m_bodyLength).toUtf8());
            headers.insert("Content-Range", m_ranges.first()->toString().toUtf8());
            headers.insert("Content-Type", m_IO->getContentType().toUtf8());
            break;
        }
        case MultiRangeGet:
        {
            if (m_chunked)
                headers.insert("Transfer-Encoding", "chunked");
            else
                headers.insert("Content-Length", QString::number(m_bodyLength).toUtf8());
            headers.insert("Content-Type", QString("multipart/byteranges; boundary=%1").arg(m_IO->getBoundary()).toUtf8());
            break;
        }
        case NotSatisfiableRangeGet:
        {
            headers.insert("Content-Range", QString("bytes */%1").arg(m_bodyLength).toUtf8());
            break;
        }
        case Write:
            break;
    }

    return headers;
}

qhttp::TStatusCode FileReader::getStatusCode()
{
    switch (m_getType)
    {
        case SimpleGet:
        {
            return qhttp::ESTATUS_OK;
        }
        case SingleRangeGet:
        {
            return qhttp::ESTATUS_PARTIAL_CONTENT;
        }
        case MultiRangeGet:
        {
            return qhttp::ESTATUS_PARTIAL_CONTENT;
        }
        case NotSatisfiableRangeGet:
        {
            return qhttp::ESTATUS_REQUESTED_RANGE_NOT_SATISFIABLE;
        }

        case Write:
        {
            return qhttp::ESTATUS_CREATED;
        }

        default: return qhttp::ESTATUS_BAD_REQUEST;
    }
}

bool FileReader::isOpen()
{
    return m_IO->isOpen();
}

bool FileReader::isReadable()
{
    return m_IO->isReadable();
}

bool FileReader::open()
{
    bool openOk = m_IO->openResource(QIODevice::ReadOnly) != FILEHANDLE_ERROR;
    if (openOk)
        calculateBodyLength();
    return openOk;
}

void FileReader::close()
{
    m_IO->close();
}

void FileReader::rangePayloadReady(const QByteArray &payload, int size)
{
    QByteArray data;

    data.append(payload);

    if (m_ranges[m_currentRangeIndex]->atEnd(m_currentRangePosition)
            && m_currentRangeIndex + 1 < (quint64)m_ranges.count())
    {
        m_currentRangeIndex++;
        m_currentRangePosition = 0;
    }

    emit dataReady(data, size);
}

void FileReader::requestData(qint64 maxLen)
{
   QByteArray data = read(getBlockSize(maxLen));
   int size = data.size();
   emit dataReady(data, size);
}

QByteArray FileReader::readData(qint64 maxLen)
{
    QByteArray data = this->read(getBlockSize(maxLen));
    return data;
}
