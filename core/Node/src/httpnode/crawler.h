#ifndef CRAWLER_H
#define CRAWLER_H

#include <QObject>
#include <QThread>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>


class Crawler : public QNetworkAccessManager
{
    Q_OBJECT
private:
    QString m_searchServer;
    QString m_server;
    QString m_lastPath;
    QMap<QString, QNetworkRequest> m_requests;
    QMap<QString, QDateTime> m_crawledTime;
    QMap<QString, bool> m_needNotify;
    QMap<QString, QString> m_excluded; //system-level excluded list for legacy clients
    void doCrawlRequst();
    explicit Crawler();
public:

    static Crawler &instance();
    void init(const QString& searchServar, const QString& server);

public slots:
    void crawlPath(const QString& path, bool notify = false, uint protectedInterval = 0, const QString& collection = QString());
    void removePath(const QString& path);
    void movePath(const QString& src, const QString& dst);
signals:

private slots:
    void requestFinished(QNetworkReply *reply);
    void onSslErrors(QNetworkReply *reply, const QList<QSslError> &errors);
};

#endif // CRAWLER_H
