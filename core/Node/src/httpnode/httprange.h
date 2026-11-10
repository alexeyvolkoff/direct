#ifndef HTTPRANGE_H
#define HTTPRANGE_H

#include "qhttpfwd.hpp"
#include <QObject>
#include <QDebug>
#include <QStringList>
#include "localresource.h"

enum IOType
    {
        Write,
        SimpleGet,
        SingleRangeGet,
        MultiRangeGet,
        NotSatisfiableRangeGet
    };


class HttpGetRange : public QObject
{
    Q_OBJECT
public:
    explicit HttpGetRange(QString rangeString, CloudResource *IO, bool isMulti = false, bool isLast = false, QObject *parent = 0);
    explicit HttpGetRange(CloudResource *IO, bool isMulti = false, bool isLast = false, QObject *parent = 0);
    ~HttpGetRange();
    void setMulti(bool isMulti);
    void setLast(bool isLast);
    bool isValid();
    const QString toString();
    quint64 bodySize();
    void calculateBodySize();
    quint64 size();
    qint64 start();
    qint64 end();
    bool atEnd(quint64 pos);
    QByteArray multiRangeTitle() const;
    QByteArray lastRangeEnd() const;
    QByteArray read(quint64 from, quint64 maxLength);

signals:
    void payloadReady(const QByteArray& payload, int size);


private:
    bool parseString(QString RangeString);
    bool checkRange();
    quint64 m_from;
    quint64 m_to;
    quint64 m_bodySize;
    quint64 m_total;
    quint64 m_blockSize;
    quint64 m_requestedFrom;
    bool m_isValidRange;
    bool m_fullSize = false;
    QString m_contentType;
    QByteArray m_titleBytes;
    QByteArray m_endLineBytes;
    QByteArray m_header;
    CloudResource *m_IO;
    QByteArray readBytesFromTitle(quint64 from, quint64 maxLength);
    QByteArray readBytesFromBody(quint64 from, quint64 maxLength);
    QByteArray readBytesFromFooter(quint64 from, quint64 maxLength);
};

#endif // HTTPRANGE_H
