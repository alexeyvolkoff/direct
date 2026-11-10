#ifndef LINKSOCKET_H
#define LINKSOCKET_H

#include <QObject>
#include <QSslSocket>
#include <QBuffer>
#include <QString>
#include <QVariant>
#include <QDataStream>
#include <QSsl>
#include <QSslCertificate>
#include <QSslKey>
#include <QThread>
#include <QTimer>
#include <QMutex>
#include <QEventLoop>
#include "onmydisklink_global.h"
#include "linkmessage.h"

class  LinkSocket : public QSslSocket
{
    Q_OBJECT

public:
    enum LinkSocketState {
		lsInactive = 0, // socket is not performing file operation
        lsActive,
        lsOpen,
        lsReserved,
        lsError //error state
    };
    Q_ENUM(LinkSocketState)
private:
    QString m_ID;
    QString m_user;
    LinkSocketState m_state;
    bool m_working;
    bool m_emitMessages;
    qint64 m_requestCount;
    QByteArray m_buffer;
    QByteArray m_outBuffer;
    quint64 m_blockSize;
    QTimer m_pingTimer;
    QList<LinkMessage> m_responseList;
    LinkMessage m_msg;
    QRecursiveMutex m_mutex;
    QMutex m_readMutex; // serializes dataReady() (may run in main thread via readyRead or in caller threads via sendMessage)
    //LinkMessage m_response;
    LinkMessage::LinkMessageType m_type;
    LinkMessage::LinkMessageType m_responseType;

private:
    bool doWrite(const LinkMessage &message);

private slots:
    void linkEncrypted();
    void timeToPingSlot();

public slots:
    void DisconnectFromCloud();
    void dataReady();
    void writeMessageQueued(const LinkMessage &message) { writeMessage(message); }
    void sendAsyncMessage(const LinkMessage &message);
    bool writeMessage(const LinkMessage &message);
    LinkMessage sendMessage(const LinkMessage &message, int timeout = 30000);

public:
    LinkSocket(bool emitMessages);
    /*LinkSocket(const QString& linkid,  const QString& user);*/
    ~LinkSocket();
    QString getPeerID() const;
    QString getUser() const { return m_user; };
    void setPeerID(const QString& id) { m_ID = id; }
    void setUser(const QString& user)  { m_user = user; }
    LinkMessage readMessage();
    void setEmitMessages(bool emitMessages);
    void startPingTimer(int timeout = PING_INTERVAL);
    void stopPingTimer();
    LinkSocketState linkState();
    bool reserveSocket();
    void releaseSocket();
    bool socketAvailable();
    void cleanup();
    void stop();
    void setKeepAlive();
signals:
    void newMessage(LinkSocket* serverSocket, const LinkMessage& message);
    void timeToPing();
    void socketReady();
    void stopped();
};

#endif // LINKSOCKET_H
