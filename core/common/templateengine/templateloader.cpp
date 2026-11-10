/**
  @file
  @author Stefan Frings
*/
#include "version.h"
#include "templateloader.h"
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QStringList>
#include <QDir>
#include <QSet>

using namespace stefanfrings;

TemplateLoader::TemplateLoader(const QString &configFile, QObject* parent)
    : QObject(parent), m_settings(configFile, QSettings::IniFormat)
{
    m_settings.setIniCodec("UTF-8");
    m_settings.beginGroup("templates");
    templatePath=m_settings.value("path",":/").toString();
    // Convert relative path to absolute, based on the directory of the config file.
    if(!(templatePath.startsWith(":/") || templatePath.startsWith("qrc://")))
    {

#ifdef Q_OS_WIN32
    if (QDir::isRelativePath(templatePath) && m_settings.format()!=QSettings::NativeFormat)
#else
    if (QDir::isRelativePath(templatePath))
#endif
    {
        //Alexey: root is OS specific
        #ifndef Q_OS_LINUX
        // on Windows and Mac webroot is relative to exe
        QString webroot = QCoreApplication::applicationDirPath();
        #else
        #ifdef QT_DEBUG
            //On linux use preject subdir for develomepent in debug build
            QString webroot = QCoreApplication::applicationDirPath();
        #else
            //Or /var/<APP> in production
            QString webroot = QString("/var/%1").arg(QString(APP_GENERIC_NAME));
        #endif
        #endif
        templatePath=QFileInfo(webroot, templatePath).absoluteFilePath();
      }
    }
    fileNameSuffix=m_settings.value("suffix",".html").toString();
    QString encoding=m_settings.value("encoding").toString();
    if (encoding.isEmpty())
    {
        textCodec=QTextCodec::codecForLocale();
    }
    else
    {
       textCodec=QTextCodec::codecForName(encoding.toLocal8Bit());
   }
   //qDebug("TemplateLoader: path=%s, codec=%s",qPrintable(templatePath),textCodec->name().data());
}

TemplateLoader::~TemplateLoader()
{}

QString TemplateLoader::tryFile(QString localizedName)
{
    QString fileName=templatePath+"/"+localizedName+fileNameSuffix;
    //qDebug("TemplateCache: trying file %s",qPrintable(fileName));
    QFile file(fileName);
    if (file.exists()) {
        file.open(QIODevice::ReadOnly);
        QString document=textCodec->toUnicode(file.readAll());
        file.close();
        if (file.error())
        {
            qCritical("TemplateLoader: cannot load file %s, %s",qPrintable(fileName),qPrintable(file.errorString()));
            return "";
        }
        else
        {
            return document;
        }
    }
    return "";
}

Template TemplateLoader::getTemplate(QString templateName, QString locales)
{
    QSet<QString> tried; // used to suppress duplicate attempts
    QStringList locs=locales.split(',', Qt::SplitBehaviorFlags::SkipEmptyParts);

    // Search for exact match
    foreach (QString loc,locs)
    {
        loc.replace(QRegExp(";.*"),"");
        loc.replace('-','_');
        QString localizedName=templateName+"-"+loc.trimmed();
        if (!tried.contains(localizedName))
        {
            QString document=tryFile(localizedName);
            if (!document.isEmpty()) {
                return Template(document,localizedName);
            }
            tried.insert(localizedName);
        }
    }

    // Search for correct language but any country
    foreach (QString loc,locs)
    {
        loc.replace(QRegExp("[;_-].*"),"");
        QString localizedName=templateName+"-"+loc.trimmed();
        if (!tried.contains(localizedName))
        {
            QString document=tryFile(localizedName);
            if (!document.isEmpty())
            {
                return Template(document,localizedName);
            }
            tried.insert(localizedName);
        }
    }

    // Search for default file
    QString document=tryFile(templateName);
    if (!document.isEmpty())
    {
        return Template(document,templateName);
    }

    qCritical("TemplateCache: cannot find template %s",qPrintable(templateName));
    return Template("",templateName);
}
