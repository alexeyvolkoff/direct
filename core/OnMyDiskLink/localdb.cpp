#include "localdb.h"
#include <QSqlQuery>
#include <QSqlRecord>
#include <QFileInfo>
#include <QDebug>
#include <QCryptographicHash>
#include <QDataStream>
#include <QSqlError>
#include <QThread>
#include <QMetaObject>
#include <QDir>
#include <QTimer>
#include <QDateTime>
#include "simplecrypt.h"

#define ADD_LINK_TIMEOUT 15000
#define REMOVE_LINK_TIMEOUT 10000


LocalDB::LocalDB(QObject *parent): QObject(parent), m_db(QSqlDatabase::addDatabase("QSQLITE", "links"))
{

}

QString LocalDB::trimLink(QString strLink)
{
    while (strLink.endsWith("="))
        strLink.chop(1);
    return strLink;
}

void LocalDB::doRemoveLink(qint64 linkId)
{
    QSqlQuery a_query(m_db);
    a_query.prepare("DELETE from links where linkid = :linkid");
    a_query.bindValue(":linkid", linkId);
    a_query.exec();
}

void  LocalDB::doFixLink(qint64 storedId, qint64 fixedId)
{
    QSqlQuery a_query(m_db);
    a_query.prepare("UPDATE links SET linkid = :fixedid WHERE linkid = :linkid");
    a_query.bindValue(":linkid", storedId);
    a_query.bindValue(":fixedid", fixedId);
    a_query.exec();
}

LocalDB &LocalDB::instance()
{
    static LocalDB db;
    return db;
}


void LocalDB::createDb(const QString &path)
{
    QString dbFile = path + "/share.db";

    QDir d; d.mkpath(path);

    m_db.setDatabaseName(dbFile);
    bool needCreate =  ! QFileInfo(dbFile).exists();

    if (! m_db.open())
    {
       qCritical() << "Can not open db:" << dbFile;
       return;
    }

    if (needCreate)
    {
        //update database structure
        QSqlQuery create_links(m_db);
        create_links.prepare("CREATE TABLE links (linkid INTEGER PRIMARY KEY, link TEXT UNIQUE, itempath TEXT UNIQUE,"
                             "filepath TEXT UNIQUE, expires DATETIME, writable BOOLEAN, discuss BOOLEAN, meet DATETIME)");
        if (! create_links.exec()) {
              qCritical() << "Can not create db:" << dbFile;
              return;
        }
    } else
        updateDb();

    m_dbconnected = true;
}

void LocalDB::updateDb()
{
    QSqlQuery a_query(m_db);
    a_query.prepare("SELECT * from links");
    if (!a_query.exec())
    {
        qDebug() << "Can't get links from db";
    }
    QSqlRecord rec = a_query.record();
    if (! rec.contains("expires")) {
         QSqlQuery update_links(m_db);
         update_links.prepare("ALTER TABLE links ADD COLUMN expires DATETIME");
         if (!update_links.exec())
         {
             qWarning() << "Error while updating links db:" << update_links.lastError();
         }
    }

    if (! rec.contains("writable")) {
         QSqlQuery update_links(m_db);
         update_links.prepare("ALTER TABLE links ADD COLUMN writable BOOLEAN");
         if (!update_links.exec())
         {
             qWarning() << "Error while updating links db:" << update_links.lastError();
         }
    }

    if (! rec.contains("discuss")) {
         QSqlQuery update_links(m_db);
         update_links.prepare("ALTER TABLE links ADD COLUMN discuss BOOLEAN");
         if (!update_links.exec())
         {
             qWarning() << "Error while updating links db:" << update_links.lastError();
         }
    }

    if (! rec.contains("meet")) {
         QSqlQuery update_links(m_db);
         update_links.prepare("ALTER TABLE links ADD COLUMN meet DATETIME");
         if (!update_links.exec())
         {
             qWarning() << "Error while updating links db:" << update_links.lastError();
         }
    }

    //Recalc hashes
    qDebug() << "Checking link hashes...";
    while (a_query.next()) {
             QString itemPath = a_query.value("itempath").toString();
             qint64 storedId = a_query.value("linkid").toLongLong();
             qint64 linkId = SimpleCrypt::stringToHash(itemPath);
             if (storedId != linkId) {
                qDebug() << "Fixing link for" << itemPath << "stored hash:" <<  storedId << "new:" << linkId;
                doFixLink(storedId, linkId);
             }
    }
}


QString LocalDB::getPublicLinkForFile(const QString &itemPath, QHash<QString, QVariant> &linkData )
{
    if (! m_db.isOpen())
    {
        qCritical() << "Database error" ;
        return "";
    }

    QMutexLocker lock(&m_mutex);
    QString path = itemPath;
    qint64 linkId = SimpleCrypt::stringToHash(itemPath);
    QSqlQuery a_query(m_db);
    a_query.prepare("SELECT link, expires, discuss, writable, meet from  links where linkid = :linkid");
    a_query.bindValue(":linkid", linkId);
    if (!a_query.exec())
    {
        qDebug() << "Can't get link from db" << a_query.lastError().text();
        return "";
    }

    if (a_query.next())
    {
       linkData["expires"] = a_query.value("expires").toDateTime().toString("yyyy-MM-dd hh:mm:ss");
       linkData["discuss"] = a_query.value("discuss").toBool();
       linkData["writable"] = a_query.value("writable").toBool();
       linkData["meet"] = a_query.value("meet").toDateTime().toString("yyyy-MM-dd hh:mm:ss");;
       QString link = trimLink(a_query.value("link").toString());
       return link;
    } else {
       return "";
    }
}

QString  LocalDB::addPublicLinkForFile(const QString &itemPath, const QString &filePath, QHash<QString, QVariant> linkData)
{
    if (! m_clientConnected)
    {
        qCritical() << "Not connected";
        emit error(itemPath);
        return "";
    }

    m_mutex.lock(); //protect

    m_link = "";
    m_filePath = filePath;
    m_itemPath = itemPath;
    m_linkData = linkData;
    //qDebug() << "Adding link" << itemPath;
    QString key = m_linkData.contains("setKey") && m_linkData["setKey"].toBool() ? m_linkData["key"].toString() : "";

    invokeAddLink(itemPath, key); //call link generator

    m_wait.wait(&m_mutex, ADD_LINK_TIMEOUT); //wait for response or error

	QString result = m_link;


    m_mutex.unlock();

    return result;
}

void LocalDB::removePublicLinkForFile(const QString &itemPath)
{
	if (! m_clientConnected)
	{
		emit error(itemPath);
		return;
	}

	//qDebug() << "Removing link: " << itemPath;
        QString link = getPublicLinkForFile(itemPath, m_linkData);
	m_mutex.lock();
	m_link = link;
	m_itemPath = itemPath;
	m_filePath = "";
	invokeRemoveLink(link); //remove from NodeJS app
	m_wait.wait(&m_mutex, REMOVE_LINK_TIMEOUT); //wait for response or error

	//check if deleted
	link = m_link;
	m_mutex.unlock();
	//remove if not deleted
	if (link != "") {
		linkRemoved(link);
    }
}

bool LocalDB::linkExists(qint64 linkId )
{
    QSqlQuery a_query(m_db);
    a_query.prepare("SELECT * from links where linkid = :linkid");
    a_query.bindValue(":linkid", linkId);
    return (a_query.exec() && a_query.next()) ;
}

QMap<QString, QString> LocalDB::publicLinkItems()
{
    QMutexLocker lock(&m_mutex);

    removeExpiredLinks();

    QMap<QString, QString>  result;
    QSqlQuery a_query(m_db);
    a_query.prepare("SELECT link, itempath, filepath from links");
    if (!a_query.exec())
    {
        qDebug() << "Can't get links from db";
        return result;
    }

    while (a_query.next()) {

       QString filePath = a_query.value("filepath").toString();

       if (QFileInfo(filePath).exists()) {
            result[a_query.value("link").toString()] = a_query.value("itempath").toString();
       }
    }

    return result;
}

QMap<QString, QString> LocalDB::publicLinkFiles()
{
    QMutexLocker lock(&m_mutex);
    QMap<QString, QString>  result;
    QSqlQuery a_query(m_db);
    a_query.prepare("SELECT link, itempath, filepath from links");
    if (!a_query.exec())
    {
        qDebug() << "Can't get links from db";
        return result;
    }

    while (a_query.next()) {
        QString filePath = a_query.value("filepath").toString();

        if (QFileInfo(filePath).exists()) {
             result[a_query.value("link").toString()] = a_query.value("filepath").toString();
        }
    }

    return result;
}

//Remove all expired links
void LocalDB::removeExpiredLinks()
{
    QSqlQuery delete_query(m_db);
    delete_query.prepare("DELETE from links where (expires IS NOT NULL) AND expires <= datetime('now')");
    if (! delete_query.exec())
        qWarning() <<  delete_query.lastError();
}

//Connected
void LocalDB::clientConnected()
{
	m_clientConnected = true;
    removeExpiredLinks();
    qDebug() << "Shared links:" << publicLinkFiles().count();
}


void LocalDB::linkAdded(const QString &strLink)
{
    QMutexLocker lock(&m_mutex);
    QString  link = trimLink(strLink);
    //qDebug() << "Added public link" << link;

    qint64 linkId = SimpleCrypt::stringToHash(m_itemPath);

    bool exists = linkExists(linkId);
    bool updateLink = m_linkData.contains("setKey") && m_linkData["setKey"].toBool();
    bool setExpires = m_linkData.contains("setExpires") && m_linkData["setExpires"].toBool();
    bool setMeet = m_linkData.contains("setMeet") && m_linkData["setMeet"].toBool();

    if (!exists)
	{
            QSqlQuery a_query(m_db);
            a_query.prepare(QString("INSERT INTO links (linkid, link, itempath, filepath, %1 %2 writable, discuss) "
                        "VALUES (:linkid, :link, :itempath, :filepath, %3 %4 :writable, :discuss)").arg(
                            setExpires ? "expires," : "", setMeet ? "meet," : "",
                            setExpires ? ":expires," : "", setMeet ? ":meet," : ""));
		a_query.bindValue(":linkid", linkId);
                a_query.bindValue(":link", link);
		a_query.bindValue(":itempath", m_itemPath);
		a_query.bindValue(":filepath", m_filePath);
                if (setExpires)
                    a_query.bindValue(":expires",  m_linkData["expires"].toDateTime());
                if (setMeet)
                    a_query.bindValue(":meet", m_linkData["meet"].toDateTime());
                a_query.bindValue(":writable", m_linkData.contains("writable") ? m_linkData["writable"].toBool() : false);
                a_query.bindValue(":discuss", m_linkData.contains("discuss") ? m_linkData["discuss"].toBool() : false);

		if (!a_query.exec())
		{
                    qWarning() << "Error while adding link:" << a_query.lastError().text();
		}
    } else {
        QSqlQuery a_query(m_db);
        a_query.prepare(QString("UPDATE links "
                        "SET %1 writable = :writable, discuss = :discuss, expires = :expires, meet = :meet "
                        "WHERE linkid = :linkid").arg( updateLink ? "link = :link," : ""));
        if (updateLink)
            a_query.bindValue(":link", link);
        if (setExpires)
            a_query.bindValue(":expires",  m_linkData["expires"].toDateTime());
        else
            a_query.bindValue(":expires", QVariant(QVariant::DateTime));

        if (setMeet)
            a_query.bindValue(":meet", m_linkData["meet"].toDateTime());
        else
            a_query.bindValue(":meet", QVariant(QVariant::DateTime));
        a_query.bindValue(":writable", m_linkData.contains("writable") ? m_linkData["writable"].toBool() : false);
        a_query.bindValue(":discuss", m_linkData.contains("discuss") ? m_linkData["discuss"].toBool() : false);
        a_query.bindValue(":linkid", linkId);

        // a_query.lastQuery()
        if (!a_query.exec())
        {
            qWarning() << "Error while updating link:" <<a_query.lastQuery() << a_query.lastError().text();
        }
    }

    m_link = link;
    QString itemPath = m_itemPath;

    m_filePath = "";
    m_itemPath = "";

    m_wait.wakeAll();

    emit publicLinkAdded(itemPath, m_link, m_linkData);

}

void LocalDB::linkRemoved(const QString &link)
{
    QMutexLocker lock(&m_mutex);
    m_link = "";
    Q_UNUSED(link)

    qint64 linkId = SimpleCrypt::stringToHash(m_itemPath);
    doRemoveLink(linkId);

    m_filePath = "";
    m_itemPath = "";

    m_wait.wakeAll();
}

void LocalDB::linkError(const QString &error)
{
    QMutexLocker lock(&m_mutex);

    // remove problematic link from db
    qDebug() << "Error while adding" << m_filePath << ", removing from db.";

    doRemoveLink(SimpleCrypt::stringToHash(m_itemPath));

    qDebug() << "Error:" << error;

    m_filePath = "";
    m_itemPath = "";
    m_link = "";

    m_wait.wakeAll();
}

void LocalDB::invokeAddLink(const QString &fileName, const QString &key)
{
    if (QThread::currentThread() != this->thread()) {
       // Called from different thread
        QMetaObject::invokeMethod(this, "invokeAddLink", Qt::QueuedConnection,  QGenericReturnArgument(nullptr), Q_ARG(QString, fileName), Q_ARG(QString, key));
     } else {
       // m_mutex.unlock();
        emit addLink(fileName, key);
    }
}

void LocalDB::invokeRemoveLink(const QString &linkId)
{
    if (QThread::currentThread() != this->thread()) {
        // Called from different thread
        QMetaObject::invokeMethod(this, "invokeRemoveLink", Qt::QueuedConnection,  QGenericReturnArgument(nullptr), Q_ARG(QString, linkId));
    } else {
       // m_mutex.unlock();
        emit removeLink(linkId);
    }
}
