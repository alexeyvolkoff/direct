#include "omdnode.h"
#include "omdsettings.h"
#include <QHostInfo>
#include <QCoreApplication>


OnMyDiskNode::OnMyDiskNode(QString configFile, QObject *parent) : QObject(parent),
m_systemSettings(configFile, QSettings::IniFormat),
#ifdef DESKTOP
    m_userSettings()
#else
#ifdef INNBOX
    m_userSettings(configFile, QSettings::IniFormat)
#else
    m_userSettings(configFile + ".user", QSettings::IniFormat)
#endif
#endif
{
	qRegisterMetaType<OnMyDiskSettings>("OnMyDiskSettings");
	m_systemSettings.setIniCodec("UTF-8");
    m_userSettings.setIniCodec("UTF-8");
    #ifndef QT_DEBUG
    QString docRoot = QStringLiteral(":/filemanager");
    #else
    QString docRoot = QFileInfo(QCoreApplication::applicationDirPath() + "/../../../../filemanager").absoluteFilePath();
    #endif

    m_docRoot = new LocalResource(docRoot,  QStringLiteral("/"), QStringLiteral("/"), QStringLiteral(""),  true, nullptr);
    m_themeRoot = new LocalResource(QString("%1/customtheme").arg(docRoot),  QStringLiteral("/"), QStringLiteral("/"), QStringLiteral(""),  true, nullptr);
    m_rootResource = new HostResource(QHostInfo::localHostName(), m_docRoot, m_themeRoot);

	#ifdef INNBOX
    m_rootResource->addCustomThemeItem("colors", "/etc/www/layout/innda_tl_color");
    m_rootResource->addCustomThemeItem("mainlogo", ":/filemanager/customtheme/InnboxShare1.svg");
    m_rootResource->addCustomThemeItem("logo", ":/filemanager/customtheme/Innshare_white.svg");
    m_rootResource->addCustomThemeItem("logo.css", ":/filemanager/customtheme/innbox-logo.css");
    m_rootResource->addCustomThemeItem("loginbackground", ":/filemanager/customtheme/whitebackground.png");
    m_rootResource->addCustomThemeItem("favicon.ico", "/etc/www/graphic/favicon.ico");
	#else
    m_rootResource->addCustomThemeItem("colors", QString("%1/customtheme/colors").arg(docRoot));
    m_rootResource->addCustomThemeItem("mainlogo", QString("%1/customtheme/logo-dark.svg").arg(docRoot));
    m_rootResource->addCustomThemeItem("loginbackground", QString("%1/customtheme/canvas1.png").arg(docRoot));
    m_rootResource->addCustomThemeItem("logo", QString("%1/customtheme/onmydisk.svg").arg(docRoot));
    m_rootResource->addCustomThemeItem("logo.css", QString("%1/customtheme/logo.css").arg(docRoot));
    m_rootResource->addCustomThemeItem("favicon.ico", QString("%1/customtheme/favicon.ico").arg(docRoot));
    #endif
    m_rootResource->addCustomThemeItem("mainlogo-chat", QString("%1/customtheme/logo-chat.svg").arg(docRoot));
    m_rootResource->addCustomThemeItem("mainlogo-chat-dark", QString("%1/customtheme/logo-chat-dark.svg").arg(docRoot));
    m_rootResource->addCustomThemeItem("logo-white", QString("%1/customtheme/logo.svg").arg(docRoot));
    m_rootResource->addCustomThemeItem("meetlogo",  QString("%1/customtheme/meetlogo.svg").arg(docRoot));
    m_rootResource->addCustomThemeItem("meetlogo.css", QString("%1/customtheme/meetlogo.css").arg(docRoot));
}

OnMyDiskNode::~OnMyDiskNode()
{
    delete m_rootResource;
    delete m_themeRoot;
    delete m_docRoot;
}
