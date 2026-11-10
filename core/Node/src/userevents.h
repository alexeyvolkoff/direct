#ifndef USEREVENTS_H
#define USEREVENTS_H

#include <QObject>
#include <QJsonObject>
#include "onmydisklink_global.h"

class UserEvents : public QObject
{
    Q_OBJECT
private:
    explicit UserEvents();
public:
    static UserEvents& instance();
signals:
    void bytesUploaded(QString user, int bytes);
    void sharedLinkAdded(QString user,  QString linkID, QString shareName, QString owner, bool writable, bool isDir = true, QString itemPath = QString());
    void sharedLinkRemoved(QString user, QString shareName);
    void sessionEnded(QString user, QString sessionId);
    void signedIn(QString user);
    void userEvent(QString user, QString event, QJsonObject data);
    void publicLinkRemoved(QString linkID, QString path, QString owner);
    void publicLinkRequested(QString linkID, QString path, QString owner, QVariantHash linkData);
#ifndef ROUTER
    void addShareRequested(QString user, QString path);
    void removeShareRequested(QString user, QString path);
    void referenceRequested(QString linkID,  QString refName, QString path,  QVariantHash data);
    void accountCreated(QString user);
#endif
    void subscribe(QString owner, QString user, QString site);
    void unsubscribe(QString owner, QString user, QString site);
    void siteUpdated(QString owner, QString site, bool rebuild);
    void fileRemoved(QString owner, QString user, QString path);
    void fileUpdated(QString owner, QString user, QString path);
    void siteCreated(QString owner, SiteInfo site);
};

#endif // USEREVENTS_H
