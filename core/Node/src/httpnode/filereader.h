#ifndef FILEREADER_H
#define FILEREADER_H

#include <qhttpfwd.hpp>
#include <QObject>
#include <QByteArray>
#include <QTimer>
#include "qhttpserver.hpp"
#include "qhttpserverconnection.hpp"
#include "qhttpserverrequest.hpp"
#include "qhttpserverresponse.hpp"
#include "localresource.h"
#include "httprange.h"

using namespace qhttp::server;

class FileReader : public QObject
{
    Q_OBJECT
public:
    explicit FileReader(CloudResource *IO, QString rangeString, bool saveAs, bool useChunked, QObject *parent = nullptr);
    explicit FileReader(CloudResource *IO, QObject *parent = nullptr);

    ~FileReader();
    void parsePartialHeaderString();
    void calculateBodyLength();
    IOType getRequestGetType();
    QList<HttpGetRange *> getRanges() {return m_ranges; }
    QByteArray getLastBytes();
    bool atEnd();
    quint64 size();
    QMultiHash<QByteArray, QByteArray> getHeaders();
    qhttp::TStatusCode getStatusCode();
    bool isOpen();
    bool isReadable();
    bool open();
    void close();
    bool isChanked() { return m_chunked; }
    void requestData(qint64 maxLen);
    QByteArray readData(qint64 maxLen);

    bool invoke(const QString &rpc, const QHash<QString, QVariant> &params, const QByteArray &payload);
    bool entryList();
signals:
    void dataReady(const QByteArray& data, int size);
    void responseReady(const QHash<QString, QVariant> &params, const QByteArray& payload);
    void listReady(const QHash<QString, QHash<QString, QVariant>> &entryList);

private:    
    QByteArray read(qint64 maxlen);
    CloudResource* m_IO;
    QString m_rangeString;
    QList<HttpGetRange*> m_ranges;
    IOType m_getType;
    quint64 m_currentRangeIndex;
    quint64 m_currentRangePosition;
    quint64 m_bodyLength;
    qint64 m_requestedSize;
    bool m_forceSaveAs;
    bool m_chunked;
    IResourceBackend* m_backend = nullptr;
    const QByteArray m_lastChunkBytes = "0\r\n\r\n";
    qint64 getBlockSize(qint64 maxlen);
private slots:
    void rangePayloadReady(const QByteArray& payload, int size);
};

#endif // FILEREADER_H
