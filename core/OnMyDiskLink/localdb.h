#ifndef LOCALDB_H
#define LOCALDB_H

#include <QObject>
#include <QSettings>
#include <QSqlDatabase>
#include <QMutex>
#include <QWaitCondition>

class LocalDB : public QObject
{
    Q_OBJECT
private:
    QWaitCondition m_wait;
    QMutex m_mutex;
    QSqlDatabase m_db;
    LocalDB(QObject* parent = nullptr);
    QString m_link = "";
	QString m_filePath = "";
	QString m_itemPath = "";
    QHash<QString, QVariant> m_linkData;
	bool m_clientConnected = false;
    bool m_dbconnected = false;
    QString trimLink(QString strLink);
    void doRemoveLink(qint64 linkId);
    void doFixLink(qint64 storedId, qint64 fixedId);

public:
    static LocalDB &instance();

    void createDb(const QString& path);
    void updateDb();
    QString getPublicLinkForFile(const QString& itemPath, QHash<QString, QVariant> &linkData);
    QString addPublicLinkForFile(const QString& itemPath, const QString &filePath,  QHash<QString, QVariant> linkData);
    void removePublicLinkForFile(const QString& itemPath);
    bool linkExists(qint64 linkId);
    QMap<QString, QString> publicLinkItems();
    QMap<QString, QString> publicLinkFiles();
    bool dbConnected() { return m_dbconnected; }
    void removeExpiredLinks();

    Q_INVOKABLE void clientConnected();
    Q_INVOKABLE void linkAdded(const QString& strLink);
    Q_INVOKABLE void linkRemoved(const QString& link);
    Q_INVOKABLE void linkError(const QString& error);

private slots:
    void invokeAddLink(const QString& fileName, const QString& key);
    void invokeRemoveLink(const QString& linkId);

signals:

    void addLink(const QString& fileName, const QString& key);
    void removeLink(const QString& linkId);
    void error(const QString& linkId);
    void publicLinkAdded(const QString& itemPath, const QString& link, const QHash<QString, QVariant> &linkData);

public slots:
};

#endif // LOCALDB_H
