#include "plainlinkcreator.h"
#include "localdb.h"
#include "simplecrypt.h"
#include <QFileInfo>
#include <QDebug>

PlainLinkCreator::PlainLinkCreator(const QString &storageId, QObject *parent): QObject(parent),
		m_storageId(storageId)
{
    connect(&LocalDB::instance(), &LocalDB::addLink, this,  &PlainLinkCreator::addLink, Qt::DirectConnection);
    connect(&LocalDB::instance(), &LocalDB::removeLink, this,  &PlainLinkCreator::removeLink, Qt::DirectConnection);
    LocalDB::instance().clientConnected();
}

void PlainLinkCreator::addLink(const QString &itemPath, const QString &key)
{
    quint64 keyHash = key.isEmpty() ? SimpleCrypt::defaultKey : SimpleCrypt::stringToHash(key);
    SimpleCrypt crypt(keyHash);

    QString path = itemPath;
	if (path.endsWith("/"))
		path.chop(1);

	QString link = QString("/shared/") + crypt.encryptToString(QString("%1:%2").arg(m_storageId, QFileInfo(path).fileName())) ;
    qDebug() << "Public link for" << qPrintable(QString("%1: %2").arg(QFileInfo(path).fileName(), link)) ;
    LocalDB::instance().linkAdded(link);
}

void PlainLinkCreator::removeLink(const QString &link)
{
    LocalDB::instance().linkRemoved(link);
}

void PlainLinkCreator::updateStorageId(QString linkId, QString validTo)
{
	Q_UNUSED(validTo);
    m_storageId = linkId;
}



