#ifndef OMDUPDATER_H
#define OMDUPDATER_H

#include <QObject>
#include <QString>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>

class OMDUpdater : public QObject
{
    Q_OBJECT
public:
    enum Status {
        Idle = 0,
        Checking,
        UpdateAvailable,
        UpToDate,
        Downloading,
        Installing,
        Error
    };
    Q_ENUM(Status)

    static OMDUpdater &instance();

    void init();
    void setCheckInterval(int hours);
    void setAutoUpdate(bool enabled);
    bool isAutoUpdate() const { return m_autoUpdate; }

    QString currentVersion() const;
    QString newVersion() const { return m_newVersion; }
    bool isUpdateAvailable() const { return m_updateAvailable; }
    Status status() const { return m_status; }
    QString statusMessage() const { return m_statusMessage; }

public slots:
    void checkForUpdates();
    void downloadAndApplyUpdate();

signals:
    void statusChanged(OMDUpdater::Status status, const QString &message);
    void updateCheckFinished(bool updateAvailable, const QString &newVersion);
    void downloadProgress(qint64 bytesReceived, qint64 bytesTotal);

private slots:
    void onVersionReplyFinished();
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void onDownloadReplyFinished();

private:
    explicit OMDUpdater(QObject *parent = nullptr);
    ~OMDUpdater() override = default;

    QString detectArch() const;
    void setStatus(Status s, const QString &msg = QString());
    bool isVersionNewer(const QString &remote, const QString &local) const;

    QNetworkAccessManager *m_nam{nullptr};
    QTimer *m_timer{nullptr};
    Status m_status{Idle};
    QString m_statusMessage;
    QString m_newVersion;
    QString m_packageUrl;
    QString m_expectedSha256;
    bool m_updateAvailable{false};
    bool m_autoUpdate{false};

    QNetworkReply *m_currentDownloadReply{nullptr};
    QFile *m_tempFile{nullptr};
};

#endif // OMDUPDATER_H
