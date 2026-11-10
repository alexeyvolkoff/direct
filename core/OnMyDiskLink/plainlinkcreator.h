#ifndef PLAINLINKCREATOR_H
#define PLAINLINKCREATOR_H

#include <QObject>

class PlainLinkCreator:public QObject
{
	Q_OBJECT
private:
	QString m_storageId;
public:
	PlainLinkCreator(const QString &storageId, QObject* parent = nullptr);
public slots:
    void addLink(const QString& itemPath, const QString& key);
    void removeLink(const QString& link);
    void updateStorageId(QString linkId, QString validTo);
};

#endif // PLAINLINKCREATOR_H
