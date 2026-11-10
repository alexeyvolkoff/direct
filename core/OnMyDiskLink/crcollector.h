#ifndef CRCOLLECTOR_H
#define CRCOLLECTOR_H

#include <QObject>
#include <cloudresource.h>

class  CRCollector : public QObject
{
    Q_OBJECT
public:
    explicit CRCollector(QObject *parent = nullptr);
    void cleanup();
    ~CRCollector();
    void watchFor(CloudResource* r);

signals:

public slots:

private:
    QList<CloudResource*> m_collectedResource;
};

#endif // CRCOLLECTOR_H
