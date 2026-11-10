/******************************************************************************
**
**      omdclient.h - On My Disk Console Client
**
**      Copyright 2017 On My Disk Team
**
**     Licensed under the Apache License, Version 2.0 (the "License");
**   you may not use this file except in compliance with the License.
**   You may obtain a copy of the License at
**
**       http://www.apache.org/licenses/LICENSE-2.0
**
**   Unless required by applicable law or agreed to in writing, software
**   distributed under the License is distributed on an "AS IS" BASIS,
**   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
**   See the License for the specific language governing permissions and
**   limitations under the License.
**
*******************************************************************************/

#ifndef OMDCLIENT_H
#define OMDCLIENT_H

#include <QObject>
#include <QSettings>
#include <QProcess>
#include <QAuthenticator>
#include <QTimer>
#include <QNetworkRequest>
#include <QAbstractSocket>
#include <QSslError>
#include <QFileSystemWatcher>

#include "omd_common.h"
#include "omdsettings.h"
#include "hostresource.h"
#include "plainlinkcreator.h"

#ifdef DESKTOP
#include <QFileDialog>
#include <QSystemTrayIcon>
#include <QMenu>
#endif

#ifdef EMBEDDED
#ifndef NO_QDRIVE
#include <QDrive.h>
#include <Monitor.h>
#include <Drive.h>
#include <Partition.h>
#include <BlockDevice.h>
#include <FileSystem.h>
#endif
#ifdef A20_GPIO
#include "A20-gpio.hpp"
#define A20_GPIO_LED_OFFSET     256
#endif
#include "consolelogger.h"
#endif


#ifdef Q_OS_WIN
#include <io.h>
#endif



class OnMyDiskClient: public QObject
{
    Q_OBJECT

public:


    enum Led {
        None = -1,
        Red = 0,
        Green,
        Blue
    };
    Q_ENUM(OnMyDiskClient::Led)

private:

    OnMyDiskSettings m_omdSettings; //editable settings
    QSettings *m_settings;
    QMap<QString, QString> m_shares;
    QMap<QString, bool> m_persistentShare;
    QMap<QString, QStringList> m_mountedShares;
    OnMyDisk::ConnectionState m_state;
    QString m_statusMessage;
    QString m_commonName;
    QString m_validTo;
    QString m_deviceId;
    QString m_linkId;
    bool m_needReconnect;
    bool m_quitOnDisconnect;
    bool m_connectorLeds;
    bool m_loading;
	bool m_connectOnStart;
    bool m_keyLogin;
    bool m_service;
    QTimer m_reconnectTimer;

    HostResource* m_hostResource;

    //mount points
    QStringList m_ignoredDevices;
    QTimer m_nodeTimer;

	PlainLinkCreator *m_plainLinkCreator = nullptr;
    QMap<QString, QProcess*> m_rcloneProcesses;
    QString obscurePassword(const QString &password);
    void updateRcloneConfig(QVariantList &shares);
    void updateRcloneProcesses(QVariantList &shares);
    bool applyRcloneUserPrivileges(QProcess *proc, long &uid, long &gid);
    QString rcloneConfigPath() const;

#ifdef EMBEDDED
    ConsoleLogger m_consoleLogger;
#ifdef A20_GPIO
    void showLed(Led led, bool on);
    A20::GPIO_output *m_redLed;
    A20::GPIO_output *m_greenLed;
    A20::GPIO_output *m_blueLed;
#endif
#ifndef NO_QDRIVE
    void reloadConfig();
    QTimer m_reloadConfigTimer;
    QDrive::Monitor m_driveMonitor;
    void updateSmbConf();
    void updateSitesConf(const QString &action, const QString &site = "");
#endif
#endif
    void doResetLink();
    void updateStorageId(QString storageId, QString linkId, QString validTo);

public slots:
    //internal
    void setState(OnMyDisk::ConnectionState state,  QString statusMessage);
    void applySettings(const OnMyDiskSettings& settings);
    //status change
    void linkConnected();
    void linkDisconnected();
    void linkConnecting();
	void certLoaded(QString commonName, QString deviceId, QString linkId, QString validFrom, QString validTo);
    //errors
    void connectionError(QAbstractSocket::SocketError error);
    void sslError(QList<QSslError> errorList, bool &ignore);
    void pkiStatusChange(QString statusMessage);
    void pkiError(QString errorMessage);

    //called from ui
    void signIn();
    void signOut();
	void reconnect();
    //share management
    void shareAddRequested(QString user, QString path);
    void shareRemoveRequested(QString user, QString sharePath);


#ifdef EMBEDDED
#ifndef NO_QDRIVE
    void deviceAdded(const QDBusObjectPath &path);
    void deviceRemoved(const QDBusObjectPath &path);
    void detectPartitions();
#endif
    void addFolder(const QString &path);
    void removeFolder(const QString &path);
    void restartSites(const QString& action, const QString& site = "");
#endif

    //can be called from js
    void addShare(QString share);
    void addNamedShare(QString shareName, QString sharePath, bool persistent = true);
    void removeShare(QString shareName);
    void accountCreated(QString server, QString user);
    void deviceIdUpdated(QString deviceId);

    void quit();

    void linkError(const QString &item);
public:
    explicit OnMyDiskClient(QSettings *settings, HostResource* hostResource,  QObject* parent = nullptr);
    void readSettings();
    void writeSettings();
    void saveShares();
    void saveSettings();

    QString systemFileNodeValue(const QString &key) const;

    ~OnMyDiskClient();

signals:
    void connectCloud();
    void disconnectCloud();
    void resetLink();
    void shareAdded(QString name, QString path, bool persistent);
    void shareRemoved(QString name);
    void stateChanged(OnMyDisk::ConnectionState state,  QString statusMessage); //state as int for js
    void storageIdUpdated(QString deviceId, QString linkId, QString validTo);
    void settingsLoaded(const OnMyDiskSettings& settings);
    void settingsSaved(const OnMyDiskSettings& settings);
    void authRequired(QString userName, QString password);
    void showCloud();
    void showStorage();
    void collectionChanged(const QString& path, const QString& localPath);
    void logOutput(const QtMsgType type, const QString &message);
private slots:
#ifdef EMBEDDED
#ifndef NO_QDRIVE
    void doReloadConfig();
#endif
#endif
};

#endif // OMDCLIENT_H
