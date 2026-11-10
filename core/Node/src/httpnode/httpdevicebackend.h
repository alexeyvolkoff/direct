#ifndef HTTPBACKEND_H
#define HTTPBACKEND_H

//Backend for Web UI clients.

#include <QObject>
#include <QWebChannel>
#include <QWebSocketServer>
#include <QJsonObject>

#include "omd_common.h"
#include "websocketclientwrapper.h"
#include "websockettransport.h"
#include "linkmessage.h"
#include "version.h"
#include "omdsettings.h"
#include "crawler.h"
#include "ipcclient.h"



class HttpDeviceBackend: public QObject
{
    Q_OBJECT

private:
    QWebChannel m_webChannel;
    OnMyDiskSettings m_testSettings;
    WebSocketClientWrapper m_clientWrapper;
    OnMyDiskSettings m_omdSettings;
    OnMyDisk::ConnectionState m_state = OnMyDisk::Disconnected;
    QString m_statusMessage = tr("Disconnected");
    QString m_token;
    QString m_version = APP_VERSION;
#ifdef ROUTER
	QString m_backendType = "router";
#else
    QString m_backendType = "client";
#endif
private slots:
    void progressEvent(QString user, float p);
    void bytesUploadedEvent(const QString &user, int bytes);
    void fileUpdatedEvent(const QString& owner, const QString& user, const QString& path);
    void fileRemovedEvent(const QString& owner, const QString& user, const QString& path);
    void userEventEvent(const QString &user, const QString& event, QJsonObject data);
    void subscribe(QJsonObject subscription);
public:
    HttpDeviceBackend(QWebSocketServer *server = nullptr);
    ~HttpDeviceBackend();

    QJsonObject getJsonSettings();
    void setJsonSettings(QJsonObject settings); //called from  JS frontend

    OnMyDiskSettings omdSettings() const { return m_omdSettings; }

    OnMyDisk::ConnectionState getState() {return  m_state ; }
    QString getStatusMessage() { return m_statusMessage; }
    QString getBackendType() { return m_backendType; }
    QString getVersion() { return m_version; }
    bool getIsService() { return isService(); }


    Q_PROPERTY(QJsonObject settings READ getJsonSettings WRITE setJsonSettings NOTIFY settingsChanged)
    Q_PROPERTY(QString backendType READ  getBackendType CONSTANT)
    Q_PROPERTY(QString token MEMBER m_token NOTIFY userConnected)
    Q_PROPERTY(QString version READ  getVersion CONSTANT)
    Q_PROPERTY(bool isService READ  getIsService CONSTANT)

#if EMBEDDED
    Q_PROPERTY(int state READ  getState CONSTANT)
#else
	Q_PROPERTY(OnMyDisk::ConnectionState state READ  getState CONSTANT)
#endif
    Q_PROPERTY(QString statusMessage READ  getStatusMessage CONSTANT)

public slots:
    void logMessage(QtMsgType type, const QString message);
    //changed by new cert
    void updateStorageId(QString storageId, QString linkId, QString validTo);
    void settingsLoaded(const OnMyDiskSettings &settings);
    void updateState(OnMyDisk::ConnectionState state,  QString statusMessage);
    void addShare(const QString& sharePath);
    void removeShare(const QString& sharePath);
signals:
    void shareAdded(const QString& shareName, const QString& sharePath, bool persistent);
    void shareRemoved(const QString& sharePath);
    void stateChanged(OnMyDisk::ConnectionState state,  const QString& statusMessage);
    void settingsChanged(const OnMyDiskSettings& settings);
    void storageIdChanged(const QString& storageId, const QString& linkId, const QString& validTo);
    void resetLink();
    void log(const QString& message);
    void progress(float progress);
    void bytesUploaded(int bytes);
    void fileUpdated(const QString& path);
    void fileRemoved(const QString& path);
    void userEvent(const QString& event, QJsonObject data);
    void userConnected(const QString& user);
    void userDisconnected(const QString& user);
    void linkCommand(const QJsonObject& cmd);
};

#endif // CLIENTNOTIFIER_H
