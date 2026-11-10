#include "omdupdater.h"
#include <QDebug>
#include <QUrl>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QCryptographicHash>
#include <QProcess>
#include <QDir>
#include <QSysInfo>
#include <QCoreApplication>
#include "version.h"

static const QString OTA_VERSION_URL = QStringLiteral("https://forge.onmydisk.net/repo/OnMyDisk/Connector/version.json");
static const QString UPDATE_HELPER_SCRIPT = QStringLiteral("/usr/share/onmydisk/omd-updater.sh");
static const QString TEMP_DEB_PATH = QStringLiteral("/tmp/onmydisk-update.deb");

OMDUpdater &OMDUpdater::instance()
{
    static OMDUpdater s_instance;
    return s_instance;
}

OMDUpdater::OMDUpdater(QObject *parent)
    : QObject(parent)
{
}

void OMDUpdater::init()
{
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    if (!m_timer) {
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &OMDUpdater::checkForUpdates);
    }

    // Default check interval: 6 hours (6 * 3600 * 1000 ms)
    m_timer->setInterval(6 * 3600 * 1000);
    m_timer->start();

    // Initial check 15 seconds after daemon startup
    QTimer::singleShot(15000, this, &OMDUpdater::checkForUpdates);
    qInfo() << "[OTA] OMDUpdater initialized. Current version:" << currentVersion();
}

void OMDUpdater::setCheckInterval(int hours)
{
    if (hours < 1) hours = 1;
    if (m_timer) {
        m_timer->setInterval(hours * 3600 * 1000);
    }
}

void OMDUpdater::setAutoUpdate(bool enabled)
{
    m_autoUpdate = enabled;
}

QString OMDUpdater::currentVersion() const
{
#ifdef APP_VERSION
    return QStringLiteral(APP_VERSION);
#else
    return QStringLiteral("2.1.5");
#endif
}

QString OMDUpdater::detectArch() const
{
    QString arch = QSysInfo::currentCpuArchitecture().toLower();
    if (arch == "x86_64" || arch == "amd64") {
        return QStringLiteral("x86_64");
    }
    if (arch.contains("aarch64") || arch == "arm64") {
        return QStringLiteral("aarch64");
    }
    if (arch.contains("arm")) {
        return QStringLiteral("armv7l");
    }

    // Fallback detection via uname -m
    QProcess proc;
    proc.start("uname", QStringList() << "-m");
    if (proc.waitForFinished(1000)) {
        QString unameArch = QString::fromUtf8(proc.readAllStandardOutput()).trimmed().toLower();
        if (unameArch == "x86_64") return QStringLiteral("x86_64");
        if (unameArch == "aarch64" || unameArch == "arm64") return QStringLiteral("aarch64");
        if (unameArch.startsWith("arm")) return QStringLiteral("armv7l");
    }

    return QStringLiteral("x86_64");
}

void OMDUpdater::setStatus(Status s, const QString &msg)
{
    m_status = s;
    m_statusMessage = msg;
    qInfo() << "[OTA]" << msg;
    emit statusChanged(m_status, m_statusMessage);
}

bool OMDUpdater::isVersionNewer(const QString &remote, const QString &local) const
{
    if (remote.isEmpty() || local.isEmpty()) return false;
    if (remote == local) return false;

    QStringList remoteParts = remote.split('.', Qt::SkipEmptyParts);
    QStringList localParts = local.split('.', Qt::SkipEmptyParts);

    int count = qMax(remoteParts.size(), localParts.size());
    for (int i = 0; i < count; ++i) {
        int rNum = i < remoteParts.size() ? remoteParts[i].toInt() : 0;
        int lNum = i < localParts.size() ? localParts[i].toInt() : 0;
        if (rNum > lNum) return true;
        if (rNum < lNum) return false;
    }
    return false;
}

void OMDUpdater::checkForUpdates()
{
    if (m_status == Checking || m_status == Downloading || m_status == Installing) {
        return;
    }

    setStatus(Checking, QStringLiteral("Checking for OTA updates..."));

    QUrl url(OTA_VERSION_URL + QStringLiteral("?t=") + QString::number(QDateTime::currentSecsSinceEpoch()));
    QNetworkRequest req(url);
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#else
    req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OnMyDisk-Agent/") + currentVersion());

    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, &OMDUpdater::onVersionReplyFinished);
}

void OMDUpdater::onVersionReplyFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) return;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        setStatus(Error, QStringLiteral("Update check failed: ") + reply->errorString());
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        setStatus(Error, QStringLiteral("Invalid version manifest format"));
        return;
    }

    QJsonObject root = doc.object();
    QString remoteVersion = root.value("version").toString();
    QJsonObject packages = root.value("packages").toObject();

    QString arch = detectArch();
    QJsonObject archPkg = packages.value(arch).toObject();

    m_packageUrl = archPkg.value("url").toString();
    m_expectedSha256 = archPkg.value("sha256").toString();
    m_newVersion = remoteVersion;

    if (m_packageUrl.isEmpty()) {
        setStatus(Error, QStringLiteral("No package URL found for architecture: ") + arch);
        return;
    }

    if (isVersionNewer(remoteVersion, currentVersion())) {
        m_updateAvailable = true;
        setStatus(UpdateAvailable, QStringLiteral("New version available: ") + remoteVersion + QStringLiteral(" (current: ") + currentVersion() + QStringLiteral(")"));
        emit updateCheckFinished(true, remoteVersion);

        if (m_autoUpdate) {
            qInfo() << "[OTA] AutoUpdate is enabled. Initiating update automatically...";
            downloadAndApplyUpdate();
        }
    } else {
        m_updateAvailable = false;
        setStatus(UpToDate, QStringLiteral("Daemon is up to date (version ") + currentVersion() + QStringLiteral(")"));
        emit updateCheckFinished(false, remoteVersion);
    }
}

void OMDUpdater::downloadAndApplyUpdate()
{
    if (m_packageUrl.isEmpty() || !m_updateAvailable) {
        qWarning() << "[OTA] No update available or package URL empty.";
        return;
    }

    if (m_status == Downloading || m_status == Installing) {
        return;
    }

    setStatus(Downloading, QStringLiteral("Downloading update package from ") + m_packageUrl);

    if (m_tempFile) {
        m_tempFile->close();
        delete m_tempFile;
        m_tempFile = nullptr;
    }

    m_tempFile = new QFile(TEMP_DEB_PATH, this);
    if (!m_tempFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus(Error, QStringLiteral("Failed to open temp file for download: ") + TEMP_DEB_PATH);
        delete m_tempFile;
        m_tempFile = nullptr;
        return;
    }

    QUrl downloadUrl(m_packageUrl);
    QNetworkRequest req(downloadUrl);
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#else
    req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OnMyDisk-Agent/") + currentVersion());

    m_currentDownloadReply = m_nam->get(req);
    connect(m_currentDownloadReply, &QNetworkReply::downloadProgress, this, &OMDUpdater::onDownloadProgress);
    connect(m_currentDownloadReply, &QNetworkReply::readyRead, this, [this]() {
        if (m_currentDownloadReply && m_tempFile) {
            m_tempFile->write(m_currentDownloadReply->readAll());
        }
    });
    connect(m_currentDownloadReply, &QNetworkReply::finished, this, &OMDUpdater::onDownloadReplyFinished);
}

void OMDUpdater::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    emit downloadProgress(bytesReceived, bytesTotal);
}

void OMDUpdater::onDownloadReplyFinished()
{
    if (!m_currentDownloadReply) return;
    m_currentDownloadReply->deleteLater();

    if (m_tempFile) {
        m_tempFile->flush();
        m_tempFile->close();
    }

    if (m_currentDownloadReply->error() != QNetworkReply::NoError) {
        setStatus(Error, QStringLiteral("Download failed: ") + m_currentDownloadReply->errorString());
        if (m_tempFile) {
            m_tempFile->remove();
            delete m_tempFile;
            m_tempFile = nullptr;
        }
        return;
    }

    m_currentDownloadReply = nullptr;

    // Verify SHA-256 hash if provided in manifest
    if (!m_expectedSha256.isEmpty()) {
        QFile checkFile(TEMP_DEB_PATH);
        if (checkFile.open(QIODevice::ReadOnly)) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (hash.addData(&checkFile)) {
                QString actualHash = QString::fromUtf8(hash.result().toHex()).toLower();
                if (actualHash != m_expectedSha256.toLower()) {
                    setStatus(Error, QStringLiteral("SHA256 checksum mismatch! Expected: ") + m_expectedSha256 + QStringLiteral(" Actual: ") + actualHash);
                    checkFile.close();
                    QFile::remove(TEMP_DEB_PATH);
                    return;
                }
            }
            checkFile.close();
        }
    }

    setStatus(Installing, QStringLiteral("Update package downloaded and verified. Applying update via helper script..."));

    QString helperScript = UPDATE_HELPER_SCRIPT;
    if (!QFile::exists(helperScript)) {
        // Fallback: check relative to application directory
        helperScript = QCoreApplication::applicationDirPath() + QStringLiteral("/../share/onmydisk/omd-updater.sh");
    }

    if (!QFile::exists(helperScript)) {
        setStatus(Error, QStringLiteral("Update helper script not found: ") + UPDATE_HELPER_SCRIPT);
        return;
    }

    bool started = QProcess::startDetached(helperScript, QStringList() << TEMP_DEB_PATH);
    if (!started) {
        setStatus(Error, QStringLiteral("Failed to launch update process: ") + helperScript);
        return;
    }

    qInfo() << "[OTA] Update script successfully launched in background. Daemon will restart soon.";
}
