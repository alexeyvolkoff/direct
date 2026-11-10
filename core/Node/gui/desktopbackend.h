/******************************************************************************
**
**
**      Copyright 2017 On My Disk Team
**
**     Licensed under the Apache License, Version 2.0 (the "License");
**   you may not use this file except in compliance with the License.
**   You may obtain a copy of the License at
**
**       http://www.apache.org/licenses/LICENSE-2.0
**
**   Unless required by applicable law or agreed to in writing, software
**   distributed under the License is distributed on an "AS IS" BASIS,
**   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
**   See the License for the specific language governing permissions and
**   limitations under the License.
**
*******************************************************************************/

#ifndef DESKTOPBACKEND_H
#define DESKTOPBACKEND_H


#include <QFrame>

#include <QStyle>
#include <QSortFilterProxyModel>
#include <QFileSystemModel>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QLocalSocket>
#include <QTimer>
#include <QMovie>
#include <QMenu>
#include <QSettings>
#include <QKeyEvent>
#include <QStatusTipEvent>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QFileIconProvider>
#include <QSystemTrayIcon>
#include <QHostInfo>

#include "omdsettings.h"
#include "omd_common.h"
#include "ui_manager.h"
#include "ui_settings.h"
#include "ui_advanced.h"




enum BookmarkRole {

    BR_Path = Qt::UserRole + 1,
    BR_OriginalIndex,
    BR_Type,
    BR_Value
};


class FilSystemLinksModel: public QStandardItemModel
{
        Q_OBJECT

private:
       QFileIconProvider m_provider;
       bool m_loaded = false;
public:
       FilSystemLinksModel ( QObject* parent = nullptr );
       //add and remove links
       void addFolderItem();
       void addLinkItem();
       void addPath (const QString& path);
       void removePath (const QString& path);
       QModelIndexList findPath(const QString& path);
       QVariant headerData(int section, Qt::Orientation orientation, int role) const;
       int columnCount(const QModelIndex &parent) const;
       void setLoaded(bool loaded) {m_loaded = loaded; }
       bool loaded(){return m_loaded; }
};




class DesktopBackend : public QFrame
{
    Q_OBJECT

public:
    explicit DesktopBackend(QWidget *parent = nullptr);
    void setRootFolder(const QString& rootFolder);
    bool isShared(const QString& folder);
    bool isInShared(const QString& folder);
    ~DesktopBackend();
    void openCloudUrl(QString path);
    void loaded();

    void showDevice();
private:

    Ui::ManagerDialog *ui;
    Ui::SettingsWidget *ui_settings;
    Ui::AdvancedSettings *ui_advanced;
    FilSystemLinksModel  m_sharesModel;

    //settings
    OnMyDiskSettings m_omdSettings;
    QSettings m_userSettings;
    QPropertyAnimation *heightAnimation;
    QPropertyAnimation *opacityAnimation;
    QGraphicsOpacityEffect* opacityEffect ;
    bool m_settingsVisible;
    bool m_advancedVisible;
    //share management
    QMenu m_popupMenu;

    QModelIndex m_selectedIndex;
    QMap<QString, QString> m_shares;

    //status
    OnMyDisk::ConnectionState m_status;
    QString m_statusMessage;
    bool m_loginRequired;
	bool m_resetNeeded;
    int m_step;
    QTimer m_statusTimer;
    //tray menu
    QSystemTrayIcon m_trayIcon;
    QMenu m_trayIconMenu;
    QAction *m_storageAction;
    void createTrayIcon();
    void setAutostart(bool autostart);

    void addSeparator();
    void updateButtons();
    void updateActions();
    void restoreSettings();
    void openFileManager(QString path);

private slots:
    void sysTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void showTrayIcon();
    void animateIcon();



protected:

    void keyPressEvent(QKeyEvent *event);
    bool eventFilter(QObject *obj, QEvent *event);
private slots:

    void currentRowChanged(QModelIndex current, QModelIndex previous);
    //menu actions
    void beforePopup(QPoint pos);
    void addShareAction();
    void removeShareAction();
    void openFileManagerAction();
    //settings
    void showSettings();
    void hideSettings();
    void applySettings();
    void animationCompleted();
    void settingsClicked();
    void animateStatus();
    void proxyChecked(int checked);
    void advancedClick();
    void switchAdvancedView();

    void cancelAdvanced();
    void okAdvanced();
public slots:
    void itemClicked(QModelIndex index);
    void showCloud();
    void showStorage();
    void linkStorage();
    void shareAdded(QString share, QString path);
    void shareRemoved(QString share);
    void showShare(QString share);
    void parseUrl(QString url);
    void stateChanged(OnMyDisk::ConnectionState status,  QString statusMessage); //state as int for js
    void storageIdUpdated(QString deviceId, QString linkId, QString validTo);
    void settingsLoaded(const OnMyDiskSettings& settings );
    void writeSettings(const OnMyDiskSettings& settings );

signals:
    void addShare(QString sharePath);
    void removeShare(QString sharePath);
    void settingsChanged(const OnMyDiskSettings& settings );
    void signOut();
    void quit();
};

#endif // DESKTOPBACKEND_H
