#include "httpdevicebackend.h"
#include "localdb.h"
#include "onmydisklinkclient.h"
#include "templateparser.h"
#include "userevents.h"


HttpDeviceBackend::HttpDeviceBackend(QWebSocketServer *server) : QObject(server),
m_clientWrapper(server)
{
    //register self in javascript as "backend"
    m_webChannel.registerObject(QStringLiteral("backend"), this);
    m_webChannel.registerObject(QStringLiteral("linksBackend"), &LocalDB::instance());
    // setup web channel
    connect(&m_clientWrapper, &WebSocketClientWrapper::clientConnected,  &m_webChannel, &QWebChannel::connectTo);

    //user events from HttpHandler
    connect(&UserEvents::instance(), &UserEvents::bytesUploaded, this, &HttpDeviceBackend::bytesUploadedEvent);
    connect(&UserEvents::instance(), &UserEvents::fileUpdated, this, &HttpDeviceBackend::fileUpdatedEvent);
    connect(&UserEvents::instance(), &UserEvents::fileRemoved, this, &HttpDeviceBackend::fileRemovedEvent);
    connect(&UserEvents::instance(), &UserEvents::userEvent, this, &HttpDeviceBackend::userEventEvent);
}


HttpDeviceBackend::~HttpDeviceBackend()
{

}

void HttpDeviceBackend::progressEvent(QString user, float p)
{
    Q_UNUSED(user)
    emit progress(p);
}

void HttpDeviceBackend::bytesUploadedEvent(const QString &user, int bytes)
{
    Q_UNUSED(user)
    emit bytesUploaded(bytes);
}

void HttpDeviceBackend::fileUpdatedEvent(const QString &owner, const QString &user, const QString &path)
{
    Q_UNUSED(owner)
    Q_UNUSED(user)
    emit fileUpdated(path);
}

void HttpDeviceBackend::fileRemovedEvent(const QString &owner, const QString &user, const QString &path)
{
    Q_UNUSED(owner)
    Q_UNUSED(user)
    emit fileRemoved(path);
}

void HttpDeviceBackend::userEventEvent(const QString &user, const QString &event, QJsonObject data)
{
    Q_UNUSED(user)
    emit userEvent(event, data);
}

void HttpDeviceBackend::subscribe(QJsonObject subscription)
{
    Q_UNUSED(subscription)
}


//Serialize to JSON for WebUI
QJsonObject HttpDeviceBackend::getJsonSettings()
{
     QJsonObject settings = m_omdSettings;
     settings["deviceName"] = m_omdSettings.hostName();
     return settings;
}

//Changed in WebUI - deserialize from JSON and apply
void HttpDeviceBackend::setJsonSettings(QJsonObject settings)
{
    OnMyDiskSettings newSettings(settings);
    m_omdSettings = newSettings;
    emit settingsChanged(newSettings);
    m_omdSettings.setResetId(false);
}


//update storageId and validity in WebUI
void HttpDeviceBackend::updateStorageId(QString storageId, QString linkId, QString validTo)
{
	m_omdSettings.setStorageId(storageId);
	m_omdSettings.setLinkId(linkId);
    m_omdSettings.setValidTo(validTo);
    emit storageIdChanged(storageId,  linkId, validTo);
}

//loaded by app
void HttpDeviceBackend::settingsLoaded(const OnMyDiskSettings &settings)
{
    m_omdSettings = settings;
}

//needed for JS
void HttpDeviceBackend::updateState(OnMyDisk::ConnectionState state, QString statusMessage)
{
    m_state = state;
    m_statusMessage = statusMessage;
    emit stateChanged(m_state, m_statusMessage);
}


void HttpDeviceBackend::logMessage(QtMsgType type, const QString message)
{
        QString t = (QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"));

        QString outMsg;

        switch (type)
        {
            case QtDebugMsg: outMsg = QString("%1 DEBUG: ").arg(t).toUtf8(); break;
            case QtWarningMsg: outMsg = QString("%1 WARNING: ").arg(t).toUtf8(); break;
            case QtCriticalMsg: outMsg = QString("%1 CRITICAL: ").arg(t).toUtf8(); break;
            case QtFatalMsg: outMsg = QString("%1 FATAL: ").arg(t).toUtf8(); break;
            case QtInfoMsg:  outMsg = QString("%1 INFO: ").arg(t).toUtf8(); break;
        }
        outMsg += message;

        emit log(outMsg); //show in js
}
void HttpDeviceBackend::addShare(const QString &sharePath)
{
    emit UserEvents::instance().addShareRequested("", sharePath);
}

void HttpDeviceBackend::removeShare(const QString &sharePath)
{
    emit UserEvents::instance().removeShareRequested("", sharePath);
}
