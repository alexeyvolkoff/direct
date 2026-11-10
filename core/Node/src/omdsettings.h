#ifndef OMDSETTINGS_H
#define OMDSETTINGS_H

#include <QObject>
#include <QSettings>
#include <QDateTime>
#include <QHostInfo>
#include <QJsonObject>
#include <QHash>
#include <QVariant>
#include <QVariantMap>
#include "version.h"


//settings
class OnMyDiskSettings: public QObject {
     Q_OBJECT

public:

    //constructors & operators
    explicit OnMyDiskSettings(QObject *parent = nullptr);
    OnMyDiskSettings(const QJsonObject& json, QObject *parent = nullptr);
    OnMyDiskSettings(const OnMyDiskSettings& other);
    OnMyDiskSettings &operator =(const OnMyDiskSettings& other);
    OnMyDiskSettings &operator =(const QJsonObject& json);
    virtual operator const QJsonObject() const;

    //runtime props
    //connection
    QString server() { return m_server; }
    void setServer(QString server) { m_server = server ; }
    int port() { return m_port; }
    void setPort(int  port) { m_port = port ; }
    QString storageId() { return m_storageId; }
    void setStorageId(QString storageId) { m_storageId = storageId ; }
    QString linkId() { return m_linkId; }
    void setLinkId(QString linkId) { m_linkId = linkId ; }
    QString validTo() { return m_validTo; }
    void setValidTo(QString validTo) { m_validTo = validTo ; }
    bool resetId()  { return m_resetId; }
    void setResetId(bool resetId)  { m_resetId = resetId; }
    //host
    QString hostName() { return m_hostName; }
    void setHostname(QString hostname)  { m_hostName = hostname; }

    //proxy
    bool useProxy() { return m_useProxy; }
    void setUseProxy(bool useProxy)  { m_useProxy = useProxy; }
    QString proxyHost() {return  m_proxyHost; }
    void setProxyHost(QString proxyHost) {m_proxyHost = proxyHost; }
    int proxyPort() { return m_proxyPort; }
    void setProxyPort(int proxyPort) {m_proxyPort = proxyPort; }
    QString proxyUser() { return m_proxyUser; }
    void setProxyUser(QString proxyUser) {m_proxyUser = proxyUser; }
    QString proxyPassword() {return m_proxyPassword; }
    void setProxyPassword(QString proxyPassword) {m_proxyPassword = proxyPassword; }

    //general & view
    QString language() {return m_language; }
    void setLanguage(QString language) {m_language = language; }
    bool generateThumbs() { return m_generateThumbs; }
    void setGenerateThumbs(bool generate) {m_generateThumbs = generate; }
    bool downscaleImages() { return m_downscaleImages; }
    void setDownscaleImages(bool downscale) {m_downscaleImages = downscale; }
    int scaleTo() { return m_scaleTo; }
    void setScaleTo(int scaleTo) {m_scaleTo = scaleTo; }
    QString user() {return  m_user;}
    void setUser(QString user) { m_user = user;}
    QString password() {return  m_password;}
    void setPassword(QString password) { m_password = password;}
    bool generateIconsFromVideos() { return m_generateIconsFromVideos; }
    void setGenerateIconsFromVideos(bool generateiconsfromvideos) {m_generateIconsFromVideos = generateiconsfromvideos; }
    bool secure(){return m_secure;}

#ifdef DESKTOP
    bool autostart() { return m_autostart; }
    void setAutostart(bool autostart) {m_autostart = autostart; }
#endif

#if defined(EMBEDDED) || defined(DESKTOP) || defined(SERVICE)
    //NAS management
    bool automount() { return m_automount; }
    void setAutomount(bool automount) {m_automount = automount; }
    bool updateSamba() const { return m_updateSamba; }
    void setUpdateSamba(bool updateSamba) {m_updateSamba = updateSamba; }
    bool autoReindex() { return  m_autoReindex; }
    void setAtoreindex(bool reindex) {m_autoReindex = reindex; }
#endif

#if ((EMBEDDED || SERVICE || defined(DESKTOP) || defined(SERVER)) && !defined(ROUTER))
    bool localSearch() { return m_localSearch; }
    void setLocalSearch (bool localSearch) {m_localSearch = localSearch; }
    QString localSearchBase() { return m_localSearchBase; }
    void setLocalAI (bool localAI) {m_localAI = localAI; }
    QString sharedWith() {return m_sharedWith; }
    bool localAI() {return m_localAI; }
    QString sitesRoot() {return m_sitesRoot; }
    QString syncPaths() {return m_syncPaths; }
    QString aiBase() {return m_aiBase; }
    QString omdToken() { return m_omdToken; }
    void setOmdToken(QString omdToken) { m_omdToken = omdToken; }
    QString omdUser() { return m_omdUser; }
    void setOmdUser(QString omdUser) { m_omdUser = omdUser; }
    bool sitesEnabled() {return m_sitesEnabled; }
    QString aiProvider() const { return m_aiProvider; }
    QString hordeApiKey() const { return m_hordeApiKey; }
    QString hordeModel() const { return m_hordeModel; }
    //External rclone WebDAV bridge
    int rcloneWebdavPort() { return m_rcloneWebdavPort; }
    void setRcloneWebdavPort(int v) { m_rcloneWebdavPort = v; }
    QString rcloneBaseUrl() {
        return QString("http://127.0.0.1:%1/").arg(m_rcloneWebdavPort);
    }
    QVariantList rcloneShares() const { return m_rcloneShares; }
    void setRcloneShares(const QVariantList &v) { m_rcloneShares = v; }

#endif

    //persistance - read/write to user's profile
    void readSettings(QSettings* settings);
    void writeSettings(QSettings* settings);
private:
    //Connection
    QString m_server = DEFAULT_CLOUD;
    int m_port = DEFAULT_PORT;
    bool m_resetId = false; //new cert is needed on reconnect
    QString m_user = "";
    QString m_password = "";
    QString m_storageId = "";
    QString m_linkId = "";
    QString m_validTo = "";
    //Proxy
    bool m_useProxy = false;
    QString m_proxyHost = "";
    int m_proxyPort = 1080;
    QString m_proxyUser = "";
    QString m_proxyPassword = "";

    //Preview
    QString m_language = "en";
    bool m_generateThumbs = true;
    bool m_downscaleImages = true;
    int m_scaleTo = 960;
    bool m_generateIconsFromVideos = true;

    //Plain/secure link
    bool m_secure = DEFAULT_SECURE;
#ifdef DESKTOP
    bool m_autostart = true;
#endif

    QString m_hostName = QHostInfo::localHostName();

#if defined(EMBEDDED) || defined(DESKTOP) || defined(SERVICE)
    // For embedded clients
    //Automount drives and update samba & dlna servises
    bool m_automount = false;
    bool m_updateSamba = true;
    bool m_autoReindex = true;
#endif

#if ((EMBEDDED || SERVICE || defined(DESKTOP) || defined(SERVER)) && !defined(ROUTER))
    bool m_localSearch = false;
    bool m_sitesEnabled = false;
    QString m_localSearchBase =  DEFAULT_SEARCH_URL;
    QString m_omdToken = "";
    QString m_omdUser = "";
    QString m_aiBase = DEFAULT_AI_URL;
#if defined(Q_OS_WIN)
    QString m_sitesRoot = "C:/ProgramData/OnMyDisk/sites";
#else
    QString m_sitesRoot = "/var/onmydisk/sites";
#endif
    QString m_syncPaths = "";
    QString m_sharedWith = "";
    bool m_localAI = true;
    QString m_aiProvider = "hybrid";
    QString m_hordeApiKey = "0000000000";
    QString m_hordeModel = "aphrodite/TheDrummer/Behemoth-X-123B-v2.1";
    //External rclone WebDAV bridge
    int m_rcloneWebdavPort = 0;
    QVariantList m_rcloneShares;
#endif

public:
    //connection
#ifndef SERVICE
    Q_PROPERTY(QString server MEMBER m_server)

    Q_PROPERTY(int port MEMBER m_port)
    Q_PROPERTY(bool resetId MEMBER m_resetId STORED false)
    Q_PROPERTY(QString storageId MEMBER m_storageId STORED false)
    Q_PROPERTY(QString validTo MEMBER m_validTo STORED false)
    Q_PROPERTY(QString user MEMBER m_user STORED false)
    Q_PROPERTY(QString password MEMBER m_password STORED false)
#endif
    //Node ID
    Q_PROPERTY(QString linkId MEMBER m_linkId )

    //proxy
    Q_PROPERTY(bool useProxy MEMBER m_useProxy)
    Q_PROPERTY(QString proxyHost MEMBER m_proxyHost)
    Q_PROPERTY(int proxyPort MEMBER  m_proxyPort)
    Q_PROPERTY(QString proxyUser MEMBER m_proxyUser)
    Q_PROPERTY(QString proxyPassword MEMBER m_proxyPassword)

#ifndef SERVICE
    Q_PROPERTY(QString language MEMBER m_language)
    Q_PROPERTY(bool generateThumbs MEMBER m_generateThumbs)
    Q_PROPERTY(bool downscaleImages MEMBER m_downscaleImages)
    Q_PROPERTY(int scaleTo MEMBER m_scaleTo)
    Q_PROPERTY(bool generateIconsFromVideos MEMBER m_generateIconsFromVideos)
#endif

    Q_PROPERTY(bool secure MEMBER m_secure)

#ifdef DESKTOP
    Q_PROPERTY(int autostart MEMBER m_autostart)
#endif

    Q_PROPERTY(QString hostname MEMBER m_hostName)

#if defined(EMBEDDED) || defined(DESKTOP)
    // For embedded clients
    //Automount drives and update samba & dlna servises
    Q_PROPERTY(bool automount MEMBER m_automount)
    Q_PROPERTY(bool updateSamba MEMBER m_updateSamba)
    Q_PROPERTY(bool autoreindex MEMBER m_autoReindex)
#endif
#if ((EMBEDDED || SERVICE || defined(DESKTOP) || defined(SERVER)) && !defined(ROUTER))
    Q_PROPERTY(bool localSearch MEMBER m_localSearch)
    Q_PROPERTY(QString localSearchBase MEMBER m_localSearchBase)
    Q_PROPERTY(QString omdToken MEMBER m_omdToken)
    Q_PROPERTY(QString omdUser MEMBER m_omdUser)
    Q_PROPERTY(QString aiBase MEMBER  m_aiBase)
    Q_PROPERTY(bool sitesEnabled MEMBER m_sitesEnabled)
    Q_PROPERTY(QString sitesRoot MEMBER m_sitesRoot)
    Q_PROPERTY(QString syncPaths MEMBER m_syncPaths)
    Q_PROPERTY(QString sharedWith MEMBER m_sharedWith)
    Q_PROPERTY(bool localAI MEMBER m_localAI)
    Q_PROPERTY(QString aiProvider MEMBER m_aiProvider)
    Q_PROPERTY(QString hordeApiKey MEMBER m_hordeApiKey)
    Q_PROPERTY(QString hordeModel MEMBER m_hordeModel)
    //External rclone WebDAV bridge
    Q_PROPERTY(int rcloneWebdavPort MEMBER m_rcloneWebdavPort)
    Q_PROPERTY(QVariantList rcloneShares READ rcloneShares WRITE setRcloneShares STORED false)

#endif
};

Q_DECLARE_METATYPE(OnMyDiskSettings)

#endif // OMDSETTINGS_H
