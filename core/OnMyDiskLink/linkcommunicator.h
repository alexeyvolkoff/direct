#ifndef LINKMESSAGEREADER_H
#define LINKMESSAGEREADER_H

#include <QDataStream>
#include <QBuffer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QDebug>
#include "onmydisklink_global.h"
#include "linkmessage.h"
#include "linksocket.h"


class LinkMessageCommunicator: public QObject
{
    Q_OBJECT

public:
    explicit LinkMessageCommunicator(LinkSocket *io, int timeout = 3000);
	LinkMessage sendMessage(const LinkMessage &outMessage);
	bool send(const LinkMessage &message);
    ~LinkMessageCommunicator();

private:
    // Blocking reads data from socket until buffer size becomes exactly n. No
    // additional data is read from the socket.
    QByteArray buffer;
	QBuffer outBuffer;
	QDataStream _outStream;
    LinkSocket* io;
    bool readOK;
    bool readError;
	int retryCount;
	quint64 blockSize;
    quint64 messageSize;
	quint64 bytesAvailable;
    int m_timeout;
    QEventLoop m_loop;
	LinkMessage::LinkMessageType m_type;
private slots:
    void dataReady();
};

#endif // LINKMESSAGEREADER_H
