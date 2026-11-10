#include "crcollector.h"

CRCollector::CRCollector(QObject *parent) : QObject(parent)
{

}

void CRCollector::cleanup()
{
    while(m_collectedResource.count() > 0)
    {
        CloudResource* r = m_collectedResource.takeFirst();
        if (r == NULL) {
            continue;
        }
        //qDebug() << "Collector deleting" << r->fileName();
        delete r;
    }
}

CRCollector::~CRCollector()
{
}

void CRCollector::watchFor(CloudResource *r)
{
    if (r && r->isCopy() && !m_collectedResource.contains(r)) {
        //qDebug() << "Added to Collector" << r->fileName();
        m_collectedResource.append(r);
    }
}
