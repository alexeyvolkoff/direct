#include "httprange.h"

HttpGetRange::HttpGetRange(QString rangeString, CloudResource *IO, bool isMulti, bool isLast, QObject *parent) : QObject(parent), m_IO(IO)
{
    m_total = m_IO->size();
    m_fullSize = false;
    m_contentType = m_IO->getContentType();
    m_isValidRange = parseString(rangeString);
    if (m_isValidRange)
    {
        m_bodySize = m_to - m_from + 1;        
        setMulti(isMulti);
        setLast(isLast);
    }
    else
    {
        qCritical() << "Bad range: " << rangeString << m_from << m_to << m_total;
    }
}

HttpGetRange::HttpGetRange(CloudResource *IO, bool isMulti, bool isLast, QObject *parent): QObject(parent), m_IO(IO)
{
    m_total = m_IO->size();
    m_fullSize = true;
    m_from = 0;
    m_to = m_total - 1;
    m_contentType = m_IO->getContentType();
    m_isValidRange = true;
    m_bodySize = m_total;
    setMulti(isMulti);
    setLast(isLast);
}

HttpGetRange::~HttpGetRange()
{
    m_header.clear();
    //m_header.squeeze();
}

void HttpGetRange::setMulti(bool isMulti)
{
    if (isMulti)
    {
        m_titleBytes.append("\r\n");
        m_titleBytes.append("--");
        m_titleBytes.append(m_IO->getBoundary().toUtf8());
        m_titleBytes.append("\r\n");
        m_titleBytes.append(QString("Content-Type: %1").arg(m_contentType).toUtf8());
        m_titleBytes.append("\r\n");
        m_titleBytes.append(QString("Content-Range: %1").arg(this->toString()).toUtf8());
        m_titleBytes.append("\r\n");
        m_titleBytes.append("\r\n");        
    }
    else
        m_titleBytes.clear();
}

void HttpGetRange::setLast(bool isLast)
{
    if (isLast)
    {
        m_endLineBytes.append("\r\n");
        m_endLineBytes.append("--");
        m_endLineBytes.append(m_IO->getBoundary().toUtf8());
        m_endLineBytes.append("--");
        m_endLineBytes.append("\r\n");
    }
    else
        m_endLineBytes.clear();
}

bool HttpGetRange::isValid()
{
    return m_isValidRange;
}

const QString HttpGetRange::toString()
{
    return QString("bytes %1-%2/%3").arg(m_from).arg(m_to).arg(m_total);
}

quint64 HttpGetRange::bodySize()
{
    return m_bodySize;
}

void HttpGetRange::calculateBodySize()
{
     m_total = m_IO->size();
     if (m_fullSize) {
        m_bodySize = m_total;
        m_from = 0;
        m_to = m_total > 0 ? m_total - 1 : 0;
     } else {
        if (m_to >= m_total)
            m_to = m_total > 0 ? m_total - 1 : 0;
        if (m_from <= m_to)
            m_bodySize = m_to - m_from + 1;
        else
            m_bodySize = 0;
     }
}

quint64 HttpGetRange::size()
{
    return m_titleBytes.count() + m_bodySize + m_endLineBytes.count();
}

qint64 HttpGetRange::start()
{
    return m_from;
}

qint64 HttpGetRange::end()
{
    return m_to;
}

bool HttpGetRange::atEnd(quint64 pos)
{
    return pos >= size();
}

QByteArray HttpGetRange::multiRangeTitle() const
{
    return m_titleBytes;
}

QByteArray HttpGetRange::lastRangeEnd() const
{
    return m_endLineBytes;
}

QByteArray HttpGetRange::read(quint64 from, quint64 maxLength)
{
    Q_ASSERT(m_IO);
    quint64 titleSize = (quint64)m_titleBytes.count();
    quint64 footerSize = (quint64)m_endLineBytes.count();

    if (from < titleSize)
    {
        return readBytesFromTitle(from, maxLength);
    }
    else if (titleSize <= from && from < titleSize + m_bodySize)
    {
        from -= titleSize;
        return readBytesFromBody(from, maxLength);
    }
    else if (titleSize + m_bodySize <= from && from < titleSize + m_bodySize + footerSize)
    {
        from -= titleSize;
        from -= m_bodySize;
        return readBytesFromFooter(from, maxLength);
    }
    else
        return QByteArray("");
}


bool HttpGetRange::parseString(QString RangeString)
{
    bool successConvert = false;
    QStringList parts = RangeString.split('-', Qt::SplitBehaviorFlags::KeepEmptyParts);
    if (parts.count() != 2)
        return false;

    //qDebug() << this << RangeString;

    if (parts.at(0) == parts.at(1) && parts.at(0) == "")
        return false;

    if (parts.at(0) == "")
    {
        m_from = m_total - QString(parts.at(1)).toLongLong(&successConvert);
        m_to = m_total - 1;
        return successConvert && checkRange();
    }

    if (parts.at(1) == "")
    {
        m_from = QString(parts.at(0)).toLongLong(&successConvert);
        m_to = m_total - 1;
        return successConvert && checkRange();
    }

    m_from = QString(parts.at(0)).toLongLong(&successConvert);

    if (!successConvert)
        return false;

    m_to = QString(parts.at(1)).toLongLong(&successConvert);

    if (!successConvert)
        return false;

    if (m_to >= m_total)
        m_to = m_total - 1;

    return successConvert && checkRange();
}

bool HttpGetRange::checkRange()
{
    return m_total > 0 && m_from <= m_to;
}

QByteArray HttpGetRange::readBytesFromTitle(quint64 from, quint64 maxLength)
{
    QByteArray bytes = m_titleBytes.mid(from, maxLength);

    if ((quint64)bytes.count() < maxLength)
        bytes.append(readBytesFromBody(0, maxLength - (m_titleBytes.count() - from)));

    return bytes;
}

QByteArray HttpGetRange::readBytesFromBody(quint64 from, quint64 maxLength)
{
    //qDebug() << "From: " << from << ", body size: " << m_bodySize;
    //qDebug() << "From + m_from: " << m_from + from;
	//m_IO->seek(m_from + from);
	QByteArray bytes;
	if (m_IO && m_IO->isOpen())
		bytes = m_IO->read(m_from + from, maxLength);
	else
		return  bytes;

    if ((quint64)bytes.count() < maxLength)
        bytes.append(readBytesFromFooter(0, maxLength - (m_bodySize - from)));

    return bytes;
}

QByteArray HttpGetRange::readBytesFromFooter(quint64 from, quint64 maxLength)
{
    return m_endLineBytes.mid(from, maxLength);
}
