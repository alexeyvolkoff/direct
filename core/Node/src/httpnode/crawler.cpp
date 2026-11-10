#include "crawler.h"
#include "userevents.h"
#include <QFile>


Crawler::Crawler(): QNetworkAccessManager(nullptr)
{
    connect(this, &Crawler::finished, this, &Crawler::requestFinished);
    connect(this, &Crawler::sslErrors, this, &Crawler::onSslErrors);
}

Crawler &Crawler::instance()
{
   static Crawler crawler;
   return crawler;
}

void Crawler::init(const QString &searchServar, const QString &server)
{
    m_searchServer = searchServar;
    m_server = server;

    QFile excludeFile("/srv/etc/config/crawler.exclude");
    if (excludeFile.open(QIODevice::ReadOnly))
    {
        QTextStream in(&excludeFile);
        while (!in.atEnd())
        {
            QString line = in.readLine();
            qInfo() << "Crawler: excluding" << line;
            m_excluded[line] = "recursive"; //may be recursive or single
        }
    }
    excludeFile.close();
}

void Crawler::doCrawlRequst()
{
    if (! m_requests.isEmpty()) {
        QString path = m_requests.firstKey();
        qInfo() << "Crawling request for" << path;
        this->get(m_requests.take(path));
    }
}


void  Crawler::crawlPath(const QString &path, bool notify, uint protectedInterval, const QString& collection)
{
    if (QThread::currentThread() != this->thread()) {
        // Called from another thread
        QMetaObject::invokeMethod(this, "crawlPath", Qt::QueuedConnection, Q_ARG(const QString &, path), Q_ARG(bool, notify), Q_ARG(uint, protectedInterval), Q_ARG(const QString &, collection));
    } else {

        //skip system-wide locations excluded from indexing manually
        Q_FOREACH(QString exclPath, m_excluded.keys()) {
        if (path.startsWith(exclPath) && m_excluded[exclPath] != "")
            return;
        }

        // check if path is not already an a queue
       if (!m_requests.keys().contains(path) && (!m_crawledTime.keys().contains(path) || m_crawledTime[path].secsTo(QDateTime::currentDateTime()) > protectedInterval )) {
            QString url = collection.isEmpty()
                ? QString("%1/indexer/from_crawl?path=%2&async=true").arg(m_searchServer, path)
                : QString("%1/indexer/from_crawl?path=%2&async=true&collection=%3").arg(m_searchServer, path, collection);
            //CrawlerRequest crawlerReq;
            QUrl requestUrl(url);
            QNetworkRequest request(requestUrl);
                request.setRawHeader("Connection", "Keep-Alive");

            m_requests.insert(m_requests.cend(), path, request);
            QDateTime crawlDate = QDateTime::currentDateTime(); // save the date
            //TODO: add to database
            m_crawledTime[path] = crawlDate;
            m_needNotify[path] = notify;

            doCrawlRequst();
            qInfo() << "Crawling:" <<  path;
            if (notify) {
                QStringList pathInfo = path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
                if (pathInfo.count() > 0) {
                    QString user = pathInfo.takeFirst();
                    QString sharePath = "/" + pathInfo.join("/");
                    emit UserEvents::instance().userEvent(user, "indexing_started", QJsonObject{{"path", sharePath}});
                }
            }

        } else
         qWarning() << "Already queued for crawling: " << path;
    }
}

void Crawler::removePath(const QString &path)
{
     if (QThread::currentThread() != this->thread()) {
        // Called from another thread
        QMetaObject::invokeMethod(this, "removePath", Qt::QueuedConnection, Q_ARG(const QString &, path));
    } else {

        //skip system-wide locations excluded from indexing manually
        Q_FOREACH(QString exclPath, m_excluded.keys()) {
        if (path.startsWith(exclPath) && m_excluded[exclPath] != "")
            return;
        }

        QString url = QString("%1/api/urls/delete?path=%2").arg(m_searchServer, path);
        QUrl requestUrl(url);
        QNetworkRequest request(requestUrl);
        m_requests.insert(m_requests.cend(), path, request);
        doCrawlRequst();
    }
}

void Crawler::movePath(const QString &src, const QString &dst)
{
     if (QThread::currentThread() != this->thread()) {
        // Called from another thread
        QMetaObject::invokeMethod(this, "movePath", Qt::QueuedConnection, Q_ARG(const QString &, src), Q_ARG(const QString &, dst));
    } else {

        //skip system-wide locations excluded from indexing manually
        Q_FOREACH(QString exclPath, m_excluded.keys()) {
        if ((dst.startsWith(exclPath) ||  src.startsWith(exclPath)) && m_excluded[exclPath] != "")
            return;
        }
        QString url = QString("%1/api/urls/move?src=%2&target=%3").arg(m_searchServer, src, dst);
        QUrl requestUrl(url);
        QNetworkRequest request(requestUrl);
        m_requests.insert(m_requests.cend(), src, request);
        doCrawlRequst();
    }
}


void Crawler::requestFinished(QNetworkReply * reply)
{

    QNetworkReply::NetworkError error =  reply->error();
    QString url = reply->url().toString();
    int p = url.lastIndexOf("http") + m_server.length() + 1;
    QString path = url.mid(p);

    //if (path.endsWith("/"))
    //    path.chop(1);

    if (error != QNetworkReply::NoError) {
       qCritical() <<  "Crawling error:" << reply->errorString() << "for" << path ;
    } else {
       qInfo() << "Crawling completed for" << path;
    }

    if (m_needNotify[path]) {
      QStringList pathInfo = path.split("/", Qt::SplitBehaviorFlags::SkipEmptyParts);
      if (pathInfo.count() > 0) {
          QString user = pathInfo.takeFirst();
          QString sharePath = "/" + pathInfo.join("/");
          emit UserEvents::instance().userEvent(user, "indexing_completed", QJsonObject{{"path", sharePath}});
      }
    }

    m_needNotify.remove(path);

    //do next request
    if (! m_requests.isEmpty())
        doCrawlRequst();

    reply->deleteLater();
}


void Crawler::onSslErrors(QNetworkReply *reply, const QList<QSslError> &errors)
{
    Q_FOREACH(QSslError error, errors)
    {
        qWarning() << "Crawler SSL notice:" << error.errorString();
    }
    reply->ignoreSslErrors();
}

