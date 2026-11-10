#ifndef ONMYDISKLINK_H
#define ONMYDISKLINK_H

#define LINK_CONFIG_RELOAD_TIMEOUT 1000
#define LINK_INACTIVITY_TIMEOUT 600000 /*10 minutes. 1 hour is too long 3600000*/
//#define LINK_INACTIVITY_TIMEOUT 6000 /*10 minutes. 1 hour is too long 3600000*/
#define LINK_CLEANUP_INTERVAL 3000

#include <QObject>
#include <QTimer>
#include "linksocket.h"

#include <QRecursiveMutex>

class LINK_API OnMyDiskLink:public QObject
{
    Q_OBJECT
public:

    enum ServerLinkState {Connected, Connecting, Inactive, Disconnected};
    Q_ENUM (OnMyDiskLink::ServerLinkState)
private:
    QString m_linkID;
    QString m_user;
    QString m_shareName = "";
    QString m_version = "";
    QString m_key = "";
    QStringList m_sharedWith;
    LinkSocket* m_socket;
    bool m_searchSopported = false;
    bool m_tunnelOnly = false;
    QTimer m_timer;
    QTimer m_inactivityTimer;
    QTimer m_pingTimer;    //client-side keep-alive: sends Ping over the command socket
    QRecursiveMutex m_mutex;
    ServerLinkState m_state = Connecting;
    friend class OnMyDiskLinkServer;
private slots:
    void inactivityTimeout();
    void pingTimeout();
public:

    explicit OnMyDiskLink(const QString& linkID, const QString &user, LinkSocket* socket );
    ~OnMyDiskLink();
    void disconnectLink();
    QString linkID() { return m_linkID; }
    void setLinkID(const QString& linkID) { m_linkID = linkID; }
    QString linkVersion() { return m_version; }
    void setLinkVersion(const QString& version) { m_version = version; }
    QString shareName() { return m_shareName; }
    void setShareName(const QString& shareName) { m_shareName = shareName; }
    QStringList sharedWith() { return m_sharedWith; }
    void setSharedWith(const QStringList& sharedWith) { m_sharedWith = sharedWith; }
    bool searchSupported() {return m_searchSopported ; }
    void setSearchSupported(bool searchSupported);
    LinkSocket* linkSocket() { return m_socket; }
    QTimer &timer() { return m_timer; }
    void release();
    void invoke(const QString& cmd, const QVariantHash& params);
    void initLink(LinkSocket *socket);
    ServerLinkState state();
    QString user() {return m_user; }
    QString key() {return m_key; }
    bool tunnelOnly() { return m_tunnelOnly; }
    void setTunnelOnly(bool tunnelOnly) { m_tunnelOnly = tunnelOnly; }

public slots:
    void startTimers();
    void stopTimers();
    void addShare(QString sharePath);
    void removeShare(QString sharePath);
signals:
    void linkInactive();
    void linkReleased();
};


#endif // ONMYDISKLINK_H
