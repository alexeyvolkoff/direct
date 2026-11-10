#ifndef SIMPLEHANDLER_H
#define SIMPLEHANDLER_H

#include <QObject>
#include <QPointer>
#include <QDir>
#include <atomic>
#include <QFile>
#include <QFileInfo>
#include <QSslSocket>
#include <QHash>
#include <QMimeData>
#include <QMimeDatabase>
#include <QUuid>
#include <QNetworkAccessManager>

#ifdef SERVER
#include <QMessageAuthenticationCode>
#endif
#include <QMetaEnum>
#include <QMimeDatabase>
#include <QStorageInfo>
#include <QThread>
#include <QSettings>
#include <QUuid>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include "qhttpserver.hpp"
#include "qhttpserverconnection.hpp"
#include "qhttpserverrequest.hpp"
#include "qhttpserverresponse.hpp"
#include "cloudresource.h"
#include "localresource.h"
#include "rootresource.h"
#include "hostresource.h"
#include "sslhelper.h"

#include "sessionauth.h"
#include "basicauth.h"
#include "digestauth.h"
#include "sessionauth.h"
#include "httprange.h"
#include "filereader.h"
#include "clientsession.h"
#include "omdnode.h"
#include "timelogger.h"
#include "crcollector.h"
#include <QThread>
#include "onmydisklink_global.h"
#include "onmydisklink_global.h"
#include "pkihelper.h"

#ifdef SERVER
#include "onmydiskmailer.h"
#endif

using namespace qhttp::server;


class HttpHandler : public QObject
{
    Q_OBJECT
public:
     HttpHandler(QSettings *settings,
                           QHttpRequest *req,
                           QHttpResponse *resp,
                           QList<QString> indexes,
                           QString serverString,
                           quint64 maxFileSize,
                           bool genIndex,
                           QString userName,
                           QString rootOwner,
                           SessionAuth* sessionAuthProvider,
                           RootResource* rootResource,
                           bool searchEnabled, bool keyLogin,
                           QObject *parent = nullptr);

     HttpHandler(QSettings *settings,
                           QHttpRequest *req,
                           QHttpResponse *resp,
                           QList<QString> indexes,
                           QString serverString,
                           quint64 maxFileSize,
                           bool genIndex,
                           QString userName,
                           QString rootOwner,

                           PKIHelper *pkiHelper,
                           SessionAuth* sessionAuthProvider,

                           RootResource* rootResource,
                           bool searchEnabled,
                           bool keyLogin,
                           QObject *parent = nullptr);

    ~HttpHandler();

    enum Depth {zero, one, infinity};
    static int m_reqNumber;

signals:
    void completed();

public slots:
    void doWork();
    void abort();
    bool isAborted() const { return m_aborted.load(std::memory_order_acquire); }
    void stop();
	void requestReady(bool reinit);
	void requestCompleted();
	void readNextDataPart();
    bool isStreamingPath(const QString& path);
    void releaseHandler();

private slots:
    void receivePostData(const QByteArray &data);
    void receivePutData(const QByteArray &data);
    void writePutData();
    void endHandlePost();
    bool parsePostData(QHash<QByteArray, QByteArray> *parsedPostData);
    void endHandlePut();
#ifdef SERVER
#endif
    void requestData();
    void requestedDataReady(const QByteArray& data, int size);
    void converterDataReady();
    void converterCompleted(int exitCode, QProcess::ExitStatus exitStatus);
private:

    const QString m_davMethods = "COPY,LOCK,MKCOL,MOVE,PROPFIND,PROPPATCH,SEARCH,UNLOCK,BIND,REBIND,UNBIND,ACL";
    QString m_ServerString;
    QStringList m_indexFiles;
    QString m_iconDir;
    //QJsonObject m_sharedUrl;
    QPointer <QHttpRequest> m_req;
    QPointer <QHttpResponse> m_resp;
	RootResource *m_rootResource = nullptr;
	FileReader* m_reader = nullptr;
    FileReader* m_rpcReader = nullptr;
    qint64 m_currentIOPosition;
    quint64 m_maxBufferSize;
    qint64 m_bytesToSend;
    quint64 m_bytesReceived;
    quint64 m_bytesWritten;
    quint64 m_totalBytes;
    quint64 m_requestPackets;
    bool m_genIndex;
    QBuffer m_buffer;
    QByteArray m_bufferBytes;
    QByteArray m_postData;
    QByteArray m_putData;
    Depth m_Depth;
	CloudResource* m_Reciever = nullptr;
    QSettings *m_Settings;
    QString m_userName;
    QString m_rootOwner;
    QString m_reqId;
    bool m_searchEnabled = false;
    bool m_keyLogin = false;
    bool callUpstream(const QJsonObject &request, QJsonObject &response); //proxy user APIs upstream (legacy gateway / node server)
    PKIHelper *m_pkiHelper;
    SessionAuth* m_sessionAuthProvider;
    CRCollector m_resourceCollector;
    QByteArray m_nextDataPart;
    QList<quint64> m_bufferSizes;
    bool m_putJsonResponse;
    bool m_isServerNode;
    bool m_requestReady;
	bool m_reinitConnection = false;
	bool m_working = false;
    bool m_reading = false;
    bool m_streaming = false;
    bool m_writing = false;
    bool m_converting = false;
    std::atomic<bool> m_aborted{false};
    std::atomic<bool> m_stopped{false};
    bool m_released = false;
    bool m_delayedRelease = false;
    bool m_exiting = false;
    QTimer *m_delayedOpTimer = nullptr;
    QNetworkAccessManager *m_externalAccessManager = nullptr;
    QProcess *m_converter = nullptr;
    QByteArray m_externalDoc;
    QString m_savePath;
    QString m_transcodeError;
    bool m_rpcCall = false;
    QString m_rpcCallPath;
    QStringList m_requestedItemPathList;
    bool m_localAiProxy = false;
    QString m_localAiProxyUrl;
    bool m_headerSent = false;

    void init();

    bool checkSitePath(const QString& urlPath, QString& redirectUrl);
    void handleLocalAiProxy(const QByteArray& payload = QByteArray());
    void handleGet(bool asHead);
    void handleOptions();
    void beginHandlePost();
    void beginHandlePut();

    void startOpTimer();
#ifdef SERVER
    bool signCSR(QByteArray csrBody, QByteArray &signedCertBody);
    void handleMount();
#endif
    CloudResource *checkCopyMovePrereq(bool &destExist, bool &isOk);
    void handleDelete();
    void replyNotImplemented();

    void replyNotFound();
    void replyNotFoundPage();
    void replyNoContent();
    void replyMethodNotAllowed();
    void replyConflict(QString s_details);
    void replyForbidden();
    void replyCreated();
    void replyFakeIndex(CloudResource *requestedResource);
    void replyRobotIndex(CloudResource *requestedResource);
    void replyNotModified(QString ETag = "", QString LastModified = "");
    void replyInternalServerError(QString s_details);
    void replyBadRequest(QString s_details);
    void replyRequestEntityTooLarge(QString s_details);
    void replyUnsupportedMediaType (QString s_details);
    void replyMoved(QUrl s_newUrl);
    void replyFound(QUrl s_newUrl);
    void replySeeOther(QUrl s_newUrl);
    void replyBadGateway(QString s_details);
    void replyServiceUnavailable(QString s_details);
    void replyPreconditionFailed(QString s_details);
    void setServerString();
    void detectContentTypeValue();
    void sendFile(CloudResource *cloudResource, bool asHead);
    void CheckPartialGet(quint64 total);
    void SortAndMergeRanges();
    bool isNotExpired(CloudResource *cloudResource);
    void handleGetFile(CloudResource* cloudResource, bool asHead);
    quint64 CalculateBodyLength();
    void endSession(const QString& session_id = "");
    void writeReply(QByteArray body, qhttp::TStatusCode code);
    void endReply(QByteArray body, const QString& contentType, qhttp::TStatusCode code);
    void endReply(QByteArray body, qhttp::TStatusCode code);
    void endHtmlReply(QByteArray body, qhttp::TStatusCode code);
    void endXmlReply(QByteArray body, qhttp::TStatusCode code);
    bool serveHtmlTemplatePage(const QString& fileName, const QString& templateName, bool asHead = false);

    // LinkThreadTask interface
    void bridgeCopy(QJsonObject request);
    void bridgeMove(QJsonObject request);
    void bridgeEdit(QJsonObject request);
    void bridgeMkCol(QJsonObject request);
    void bridgeRemove(QJsonObject request);
    void bridgeRename(QJsonObject request);
    void bridgeGetContent(QJsonObject request);
    void bridgeGetAttributes(QJsonObject request);
    void bridgeinvokeClient(QJsonObject request);
    void bridgeGetVideoAttributes(QJsonObject request);
    void bridgeTranscodeVideo(QJsonObject request);
    void bridgeDownscaleVideo(QJsonObject request);
    void bridgeTranscodeAbort(QJsonObject request);
    void bridgeGetTranscodeProgress(QJsonObject request);
    void bridgeList(QJsonObject request);
    void bridgeList(QString path);
    void bridgeShareByLink(QJsonObject request);
    void bridgeUnshareByLink(QJsonObject request);
    void bridgeSharePathToSession(QJsonObject request);
    void bridgeUnsharePathFromSession(QJsonObject request);
    void bridgeInviteUsersToCall(QJsonObject request);
    void bridgeGetProfileData(QJsonObject request);
    void bridgeGetPublicProfile(QJsonObject request);
    void bridgeGetUserInfo(QJsonObject request);
    void bridgeSignIn(QJsonObject request);
    void bridgeInvite(QJsonObject request);
    void bridgeUpdateProfileData(QJsonObject request);
	void bridgeCreateProfile(QJsonObject request);
	void bridgeConnectDevice(QJsonObject request);
    void bridgeRemoveSharedLink(QJsonObject request);
    void bridgeExcludeFromIndexing(QJsonObject request);
    void bridgeCreateNew(QJsonObject request);
    void bridgeSubscribe(QJsonObject request);
    void bridgeSubscriptionSuccess(QJsonObject request);
    void bridgeUnsubscribe(QJsonObject request);
    void bridgeCreateSite(QJsonObject request);
    void bridgeSignInWithGoogle(QJsonObject request);
    void bridgeProvisionGroup(QJsonObject request);
    void bridgeRelogin(QJsonObject request);
    void bridgeSessionLogin(QJsonObject request);
    void endJsonReply(QByteArray body, qhttp::TStatusCode code);
    void endChunkedReply(QByteArray chunk, qhttp::TStatusCode code);
    void writeChunk(QByteArray chunk, qhttp::TStatusCode code = qhttp::ESTATUS_PARTIAL_CONTENT);
    void replyJsonNotFound();
    void replyJsonNotImplemented();
    void replyJsonSuccess();
    void replyJsonBasic(int Error);
    void replyJsonError(QString errorString);
    void replyJsonMethodNotAllowed();
    void replyJsonRequestEntityTooLarge(QString s_details);
    void replyJsonBadRequest(QString s_details);
    void replyJsonInternalServerError(QString s_details);
    void replyJsonForbidden();
    void replyJsonNotModified(QString ETag, QString LastModified);
    void replyJsonMoved(QUrl s_newUrl);
	void replySendSignedCert(QByteArray signedCert);
    void replySendCa(QString caCert);

    void saveSessionCookie(const QString& sessionID);
    bool saveFile(const QStringList& path, const QByteArray& data);

    void reportTokenUsage(QString action, double units = 1.0);

    QString handleAddToQuery(const QString& query, const QString& addToQuery);
    void updateProfileWithRights(QJsonObject &profile, const QString &username, const QString &pkey, ClientSession* session = nullptr);
    float readTranscodeProgress(QString transcodeLogFile);
    float getSecondsFromLog(const QString& valueInLog);
    bool removeTranscodeLog(QString transcodeLogFile, bool forced);
    void doReinit();
public:
    int returnCode() const;
    void replyJsonSeeOther(QUrl s_newUrl);    
    QString reqId () const;
};

#endif // SIMPLEHANDLER_H
