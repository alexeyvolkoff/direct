#include "version.h"
#include "desktopbackend.h"
#include "ui_manager.h"


#include <QStandardPaths>
#include <QStatusTipEvent>
#include <QPainter>
#include <QDesktopWidget>
#include <QDebug>
#include <QDateTime>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QDebug>
#include <QFileDialog>
#include <QDesktopServices>
#include <QMessageBox>


#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif






//Link list model

FilSystemLinksModel::FilSystemLinksModel(QObject *parent):  QStandardItemModel(parent)
{

}

void FilSystemLinksModel::addFolderItem()
{
    QIcon icon(":/resource/add-icon.png");
    QIcon folder = m_provider.icon(QFileIconProvider::Folder);
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
             painter.drawPixmap(
                QRectF(0, 0, 32, 32), folder.pixmap(32, 32),
                QRectF(0, 0, 32, 32));
             painter.drawPixmap(
                QRectF(16, 16, 16, 16), icon.pixmap(16, 16),
                QRectF(0, 0, 16, 16));

    QIcon combined = QIcon(pixmap);
    QStandardItem *item = new QStandardItem(combined, tr("Add folder"));

    QFlags<Qt::ItemFlag> flags = item->flags();
    flags |= Qt::ItemIsSelectable;
    flags |= Qt::ItemIsEnabled;
    flags &=  ~Qt::ItemIsEditable;
    item->setFlags(flags);
    item->setToolTip(tr("Add folder to your storage"));
    item->setStatusTip(tr("Add folder to your storage"));

    QList <QStandardItem *>row;
    row << item;
    appendRow(row);
}


void FilSystemLinksModel::addLinkItem()
{

    QIcon icon(":/icons/states/icon_online.png");
    QIcon folder = m_provider.icon(QFileIconProvider::Folder);
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
             painter.drawPixmap(
                QRectF(0, 0, 32, 32), folder.pixmap(32, 32),
                QRectF(0, 0, 32, 32));
             painter.drawPixmap(
                QRectF(16, 16, 16, 16), icon.pixmap(16, 16),
                QRectF(0, 0, 16, 16));

    QIcon combined = QIcon(pixmap);
    QStandardItem *item = new QStandardItem(combined, tr("Browse files"));

    QFlags<Qt::ItemFlag> flags = item->flags();
    flags |= Qt::ItemIsSelectable;
    flags |= Qt::ItemIsEnabled;
    flags &=  ~Qt::ItemIsEditable;
    item->setFlags(flags);
    item->setToolTip(tr("Browse your storage online"));
    item->setStatusTip(tr("Browse your storage online"));

    QList <QStandardItem *>row;
    row << item;
    appendRow(row);
}

void FilSystemLinksModel::addPath(const QString &path)
{
    QString shareName = QFileInfo(path).fileName();
    QStandardItem *item = new QStandardItem(m_provider.icon(QFileInfo(path)), shareName);

    QFlags<Qt::ItemFlag> flags = item->flags();
    flags |= Qt::ItemIsSelectable;
    flags |= Qt::ItemIsEnabled;
    flags &=  ~Qt::ItemIsEditable;
    item->setFlags(flags);
    item->setData(path, BR_Path);
    item->setToolTip(tr("Open %1").arg(shareName));
    item->setStatusTip(tr("Open %1").arg(shareName));

    QList <QStandardItem *>row;
    row << item;
    int idx = m_loaded ? rowCount() -1 : rowCount();
    insertRow(idx,  row);
}

void FilSystemLinksModel::removePath(const QString &path)
{
    QModelIndexList list = findPath(path);

    if (! list.isEmpty())
    {
        removeRow(list[0].row());
    }

}

QModelIndexList FilSystemLinksModel::findPath(const QString &path)
{
    return match(index(0, 0, QModelIndex()), BR_Path, path, 1, Qt::MatchFlags( Qt::MatchExactly | Qt::MatchRecursive )); // get list of matches;
}


QVariant FilSystemLinksModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if ((section == 0) && (role == Qt::DisplayRole))
    {
		return tr("Cloud folders");
    } else
        return QStandardItemModel::headerData(section, orientation, role);
}


int FilSystemLinksModel::columnCount(const QModelIndex &parent) const
{
        Q_UNUSED(parent)
        return 1;
}


//File Manager

DesktopBackend::DesktopBackend(QWidget *parent) :
    QFrame(parent), ui(new Ui::ManagerDialog),  ui_settings(new Ui::SettingsWidget),
    ui_advanced(new Ui::AdvancedSettings),
    m_sharesModel( parent)
{

    ui->setupUi(this);
    ui_settings->setupUi(ui->settingsWidget);
    ui_advanced->setupUi(ui->advancedWidget);
    ui->advancedWidget->hide();
	m_resetNeeded = false;

    createTrayIcon();


#ifndef Q_OS_MAC
    //Window frame with a single close button
    setWindowFlags((Qt::Window | Qt::WindowSystemMenuHint | Qt::CustomizeWindowHint | Qt::WindowCloseButtonHint) & ~Qt::WindowContextHelpButtonHint & ~Qt::WindowMaximizeButtonHint & ~Qt::WindowMinimizeButtonHint);
#endif
	setWindowTitle(QString("On My Disk - %1").arg(QHostInfo::localHostName()));

    ui_advanced->portEdit->setValidator( new QIntValidator(0, 9999, this) );
    ui_advanced->passwordEdit->setEchoMode(QLineEdit::Password);
    ui_advanced->renewCheckBox->setChecked(false);
    ui_settings->passwordEdit->setEchoMode(QLineEdit::Password);


    connect(ui_advanced->proxyCheckbox, &QCheckBox::stateChanged, this, &DesktopBackend::proxyChecked);
    proxyChecked(false);

    ui->imgLabel->setPixmap(QPixmap(":icons/states/icon_disconnected.png"));
    m_status = OnMyDisk::Disconnected;
    m_statusMessage = "";
    m_step = 1;
    m_loginRequired = true;
    connect(&m_statusTimer, &QTimer::timeout, this, &DesktopBackend::animateStatus);

    // hook up events
    qApp->installEventFilter(this);

    //settings
    connect(ui_settings->applyButton, &QPushButton::clicked, this, &DesktopBackend::applySettings);

    //menu actions
    connect(ui->actionAddShare, &QAction::triggered, this,  &DesktopBackend::addShareAction);
    connect(ui->actionRemoveShare, &QAction::triggered, this,  &DesktopBackend::removeShareAction);
    connect(ui->actionOpenCloud, &QAction::triggered, this,  &DesktopBackend::showCloud);
    connect(ui->actionOpenFileManager, &QAction::triggered, this,  &DesktopBackend::openFileManagerAction);
    connect(ui->actionQuit, &QAction::triggered, this,  &DesktopBackend::quit);
    connect(ui->filesView, &QTreeView::customContextMenuRequested, this,   &DesktopBackend::beforePopup);

    ui->filesView->setContextMenuPolicy(Qt::CustomContextMenu);

    //Menu
    m_popupMenu.addAction(ui->actionAddShare);
    m_popupMenu.addAction(ui->actionRemoveShare);
    m_popupMenu.addSeparator();
    m_popupMenu.addAction(ui->actionOpenFileManager);
    m_popupMenu.addAction(ui->actionOpenCloud);
    m_popupMenu.addSeparator();
    m_popupMenu.addAction(ui->actionQuit);

    //add  actions to main widget
    addAction(ui->actionAddShare);
    addAction(ui->actionOpenCloud);
    addAction(ui->actionOpenFileManager);
    addAction(ui->actionAddShare);
    addAction(ui->actionRemoveShare);
    addAction(ui->actionQuit);


    //Look and feel
    QFile styleFile(":/resource/style.qss");
    styleFile.open(QFile::ReadOnly);
    //read
    QByteArray data = styleFile.readAll();
    styleFile.close();
    //parse
    QString style = QString::fromUtf8(data);
    style.replace("@highlight", palette().color(QPalette::Highlight).name());
    style.replace("@inactive", palette().color(QPalette::Mid).name());
    style.replace("@hover", palette().color(QPalette::Midlight).name());
    //apply
    setStyleSheet(style);

    m_settingsVisible = false;
    ui->settingsWidget->setVisible(false);
    ui->versionLabel->setText(tr("Version: %1").arg(APP_VERSION));

    //main view

     ui->filesView->setModel(&m_sharesModel);
     ui->filesView->setAnimated(true);
     ui->filesView->setIndentation(0);
     ui->filesView->header()->setFixedHeight(32);
     ui->filesView->header()->setVisible(false);
     ui->filesView->setIconSize(QSize(32,32));
     m_sharesModel.addFolderItem();

     ui->filesView->selectionModel()->select(m_sharesModel.index(0,0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
     ui->filesView->selectionModel()->setCurrentIndex(m_sharesModel.index(0,0), QItemSelectionModel::SelectCurrent | QItemSelectionModel::Rows);


     ui->settingsButton->setCheckable(true);
     ui->settingsButton->setToolTip(tr("Settings"));
     ui->settingsButton->setStatusTip(tr("Settings"));
     connect(ui->settingsButton, &QPushButton::clicked, this, &DesktopBackend::settingsClicked);
     //connect(ui->statusLabel, &QLabel::cl, this, &DesktopBackend::settingsClicked);
     connect(ui_settings->advancedButton, &QPushButton::clicked, this, &DesktopBackend::advancedClick);
     connect(ui_advanced->okButton, &QPushButton::clicked, this, &DesktopBackend::okAdvanced);
     connect(ui_advanced->cancelButton, &QPushButton::clicked, this, &DesktopBackend::cancelAdvanced);

     //shares view
     connect(ui->filesView, &QTreeView::doubleClicked, this, &DesktopBackend::itemClicked);
     connect(ui->filesView->selectionModel(), &QItemSelectionModel::currentRowChanged, this, &DesktopBackend::currentRowChanged);


     heightAnimation = new QPropertyAnimation(ui->settingsWidget, "geometry");
     opacityEffect =  new QGraphicsOpacityEffect(this);
     ui->settingsWidget->setGraphicsEffect(opacityEffect);

     //show/hide animation
     opacityAnimation = new QPropertyAnimation(this);
     opacityAnimation->setTargetObject(opacityEffect);
     opacityAnimation->setPropertyName("opacity");
     //heightAnimation->setEasingCurve(QEasingCurve::InOutCubic);
     //opacityAnimation->setEasingCurve(QEasingCurve::InOutCubic);
     opacityEffect->setOpacity(0.001);
     connect(heightAnimation, &QPropertyAnimation::finished, this, &DesktopBackend::animationCompleted);


     //Default look and feel
     QRect r = geometry();
     if (r.top() == 0 || r.left() == 0)
     {
        const QSize availableSize = QApplication::desktop()->availableGeometry(this).size();
        move(availableSize.width( ) / 4, availableSize.height() / 4);
        resize(availableSize / 2);
     }

     //settings
     restoreSettings();

}


void DesktopBackend::animateStatus()
{
    ui->imgLabel->setPixmap(QPixmap(QString(":icons/states/icon_wait_0%1.png").arg(m_step )));
    if (++m_step > 4 ) m_step = 1;
}


void DesktopBackend::proxyChecked(int checked)
{
    ui_advanced->hostEdit->setVisible(checked == Qt::Checked);
    ui_advanced->hostLabel->setVisible(checked == Qt::Checked);
    ui_advanced->portEdit->setVisible(checked == Qt::Checked);
    ui_advanced->portLabel->setVisible(checked == Qt::Checked);
    ui_advanced->userEdit->setVisible(checked == Qt::Checked);
    ui_advanced->userLabel->setVisible(checked == Qt::Checked);
    ui_advanced->passwordEdit->setVisible(checked == Qt::Checked);
    ui_advanced->passwordLabel->setVisible(checked == Qt::Checked);
}



void DesktopBackend::advancedClick()
{
    switchAdvancedView();
}

void DesktopBackend::switchAdvancedView()
{
    
    QRect g( ui->settingsWidget->geometry());
    heightAnimation->setStartValue(g);
    g.setHeight(50);

    heightAnimation->setEndValue(g);
    opacityAnimation->setStartValue(1.0);
    opacityAnimation->setEndValue(0.001);
    opacityAnimation->setDuration(150);
    heightAnimation->setDuration(150);
    opacityEffect->setOpacity(1.0);
 
    m_advancedVisible = true;
    opacityAnimation->start();
    heightAnimation->start();
}


void DesktopBackend::animationCompleted()
{
    if (m_advancedVisible) {
         ui->settingsWidget->hide();
         ui->advancedWidget->show();
    } else
    if (m_settingsVisible)
    {
        ui->settingsWidget->updateGeometry();
    }
    else
    {
       ui->advancedWidget->hide();
       ui->settingsWidget->hide();
       ui->filesView->show();

       m_advancedVisible = false;
    }

    ui->settingsButton->setChecked(m_settingsVisible);
}



void DesktopBackend::currentRowChanged(QModelIndex current, QModelIndex previous)
{
    Q_UNUSED(previous)
    if (m_selectedIndex != current)
    {
        m_selectedIndex  = current;
    }
}



void DesktopBackend::keyPressEvent(QKeyEvent *event)
{
    switch (event->key())
    {
        case  Qt::Key_Return:
        {
            if (ui->settingsButton->isChecked())
            {
                applySettings();
            }
            else
            {
                if (m_selectedIndex.isValid())
                {
                    itemClicked(m_selectedIndex);
                }
            }
            break;
        }

        case  Qt::Key_Escape:
        {
            if (m_settingsVisible)
            {
                hideSettings();
            }
            break;
        }
    }

    QWidget::keyPressEvent(event);

}


void DesktopBackend::itemClicked(QModelIndex index)
{
     if (index.row() == 0)
         addShareAction();
     else if (index.row() == index.model()->rowCount() - 1)
         openCloudUrl("");
     else
         showShare(index.data().toString());
}




void DesktopBackend::openCloudUrl(QString path)
{
    QString url;
    if (path.isEmpty())
        url =  QString("https://" + m_omdSettings.server());
    else
        url = path;

    QDesktopServices::openUrl(QUrl(url));
}

void DesktopBackend::loaded()
{
    m_sharesModel.setLoaded(true);
    m_sharesModel.addLinkItem();
}


void DesktopBackend::showCloud()
{
     if (m_selectedIndex.row() > 0 && m_selectedIndex.row() < m_selectedIndex.model()->rowCount() )
        showShare(m_selectedIndex.data().toString());
     else
        openCloudUrl("");

}

void DesktopBackend::showStorage()
{
    show();
    activateWindow();
    raise();
}

void DesktopBackend::linkStorage()
{
    QString linkUrl = QString("https://%1/link/%2").arg(m_omdSettings.server(), m_omdSettings.storageId());
    openCloudUrl(linkUrl);
}

void DesktopBackend::stateChanged(OnMyDisk::ConnectionState status, QString statusMessage)
{
    m_statusMessage = statusMessage;
    m_status = status;
    ui->statusLabel->setText(m_statusMessage);

    switch (m_status)
    {
          case OnMyDisk::Authorization:
          case OnMyDisk::Connecting:
          {
              m_trayIcon.setIcon(QPixmap(":icons/states/icon_wait_01.png"));
              m_storageAction->setIcon(QPixmap(":icons/states/icon_wait_01.png"));
              ui->imgLabel->setPixmap(QPixmap(":icons/states/icon_wait_01.png"));
              m_statusTimer.start(300);
              break;
          }
          case OnMyDisk::AuthorizationError: {
              m_resetNeeded =  true;
              ui->imgLabel->setPixmap(QPixmap(":/resource/fa-status-warning.png"));
              m_trayIcon.setIcon(QPixmap(":/resource/fa-status-warning.png"));
              m_storageAction->setIcon(QPixmap(":/resource/fa-status-warning.png"));
              m_statusTimer.stop();
              m_loginRequired = true;
              show();
              showSettings();
              break;
          }
          case OnMyDisk::ConnectionError:
          {
              ui->imgLabel->setPixmap(QPixmap(":/resource/fa-status-warning.png"));
              m_trayIcon.setIcon(QPixmap(":/resource/fa-status-warning.png"));
              m_storageAction->setIcon(QPixmap(":/resource/fa-status-warning.png"));
              m_statusTimer.stop();
              m_loginRequired = true;
              show();
              showSettings();
              break;
          }
          case OnMyDisk::Disconnected:
          {
              ui->imgLabel->setPixmap(QPixmap(":icons/states/icon_disconnected.png"));
              m_trayIcon.setIcon(QPixmap(":icons/states/icon_disconnected.png"));
              m_storageAction->setIcon(QPixmap(":icons/states/icon_disconnected.png"));
              m_statusTimer.stop();
			  m_loginRequired = true;
			  m_resetNeeded =  false;
			  updateButtons();
              break;
          }

          case OnMyDisk::Connected:
          {
              ui->imgLabel->setPixmap(QPixmap(":icons/states/icon_online.png"));
              m_trayIcon.setIcon(QPixmap(":icons/states/icon_online.png"));
              m_storageAction->setIcon(QPixmap(":icons/states/icon_online.png"));

              m_statusTimer.stop();

              if (m_loginRequired)
                  openCloudUrl("");

              m_loginRequired = false;
			  m_resetNeeded =  false;
			  hideSettings();

              break;
          }

          //default: break;
    }
    m_trayIcon.setToolTip(QString("%1: %2").arg(APP_NAME, m_statusMessage));
    m_storageAction->setText(QString("%1: %2").arg(APP_NAME, m_statusMessage));

   // qDebug() << m_statusMessage;
    updateButtons();

}

void DesktopBackend::storageIdUpdated(QString deviceId, QString linkId, QString validTo)
{
   Q_UNUSED(linkId)
   m_omdSettings.setStorageId(deviceId);
   m_omdSettings.setLinkId(linkId);
   m_omdSettings.setValidTo(validTo);
   ui_settings->storageEdit->setText(m_omdSettings.storageId());
   ui_settings->validtoValueLabel->setText(validTo);
   //m_loginRequired = false;
   updateButtons();

}

void DesktopBackend::settingsLoaded(const OnMyDiskSettings &settings)
{
  m_omdSettings = settings;
  //basic
  ui_settings->accountEdit->setText(m_omdSettings.user());
  ui_settings->storageEdit->setText(m_omdSettings.storageId());
  ui_settings->passwordEdit->setText("");
  ui_settings->accountEdit->setReadOnly(true);
  ui_advanced->autostartCheckBox->setChecked(m_omdSettings.autostart());
  ui_advanced->proxyCheckbox->setChecked(m_omdSettings.useProxy());
  ui_advanced->serverEdit->setText(m_omdSettings.server());
  ui_advanced->cloudPortEdit->setText(QString::number(m_omdSettings.port()));

  //proxy
  ui_advanced->proxyCheckbox->setChecked(m_omdSettings.useProxy());
  ui_advanced->hostEdit->setText(m_omdSettings.proxyHost());
  ui_advanced->portEdit->setText(QString::number(m_omdSettings.proxyPort()));
  ui_advanced->userEdit->setText(m_omdSettings.proxyUser());
  ui_advanced->passwordEdit->setText(m_omdSettings.proxyPassword());

  ui_advanced->renewCheckBox->setChecked(false);

  m_loginRequired = (m_omdSettings.user() == "");

  setAutostart(m_omdSettings.autostart());

  updateButtons();

}


void DesktopBackend::settingsClicked()
{
    if (!m_settingsVisible)
        showSettings();
    else
        hideSettings();
}


void DesktopBackend::updateButtons()
{
    ui_settings->accountEdit->setReadOnly(! m_loginRequired);

	ui_settings->validtoLabel->setVisible(!m_resetNeeded && m_omdSettings.storageId() != "");
	ui_settings->validtoValueLabel->setVisible(!m_resetNeeded && m_omdSettings.storageId() != "");
    ui_settings->storageEdit->setVisible(m_omdSettings.storageId() != "");
    ui_settings->storageLabel->setVisible(m_omdSettings.storageId() != "");
	ui_settings->passwordEdit->setVisible(m_resetNeeded || m_omdSettings.storageId() == "");
	ui_settings->passwordLabel->setVisible(m_resetNeeded || m_omdSettings.storageId() == "");
    ui_settings->passwordEdit->setText("");


    ui_settings->resetPasswordLabel->setText(QString("<a href=\"https://%1/reset\" style=\"color: white;text-decoration:none;\" target=_new>Reset password</a>").arg(m_omdSettings.server()));
    ui_settings->createAccountLabel->setText(QString("<a href=\"https://%1/register\" style=\"color: white;text-decoration:none;\" target=_new>Create an account</a>").arg(m_omdSettings.server()));
    ui_settings->resetPasswordLabel->setVisible(m_loginRequired);
    ui_settings->createAccountLabel->setVisible(m_loginRequired);

    ui_settings->applyButton->setText(m_loginRequired ? tr("Sign in") : tr("Folders"));
    ui_settings->applyButton->setToolTip(m_loginRequired ? tr("Sign in to your account") : tr("Manage shared folders"));
    ui_settings->applyButton->setStatusTip(ui_settings->applyButton->toolTip());

}


void DesktopBackend::beforePopup(QPoint pos)
{
    QTreeView* view = static_cast <QTreeView*> (sender());

    updateActions();
    //show/hide menu items according to selected index
    m_popupMenu.popup(view->viewport()->mapToGlobal(pos));
}


void DesktopBackend::updateActions()
{

     //popup menu
    ui->actionAddShare->setVisible(m_selectedIndex.row() > 0);

    ui->actionRemoveShare->setVisible(m_selectedIndex.isValid() && m_selectedIndex.row() > 0  && m_selectedIndex.row() < m_selectedIndex.model()->rowCount() - 1);
    ui->actionRemoveShare->setToolTip(tr("Remove %1").arg( m_selectedIndex.data().toString()));
    ui->actionRemoveShare->setStatusTip(ui->actionRemoveShare->toolTip());

    ui->actionOpenCloud->setVisible(true);
    ui->actionOpenCloud->setToolTip(tr("Browse files online"));
    ui->actionOpenCloud->setStatusTip(ui->actionOpenCloud->toolTip());

    ui->actionOpenFileManager->setVisible(m_selectedIndex.isValid() && m_selectedIndex.row() > 0 && m_selectedIndex.row() < m_selectedIndex.model()->rowCount() - 1);
    ui->actionOpenFileManager->setToolTip(tr("Open %1 in file manager").arg(m_selectedIndex.data().toString()));
    ui->actionOpenFileManager->setStatusTip(ui->actionOpenFileManager->toolTip());

}

void DesktopBackend::writeSettings(const OnMyDiskSettings &settings)
{
    Q_UNUSED(settings)
    m_userSettings.setValue("geometry", saveGeometry());
    m_userSettings.sync();
}

void DesktopBackend::restoreSettings()
{
    restoreGeometry(m_userSettings.value("geometry").toByteArray());
}


//shares management

void DesktopBackend::shareAdded(QString share, QString path)
{
    if (! m_shares.contains(share))
    {
       m_sharesModel.addPath(path);
       m_shares[share] = path;

       QModelIndexList list = m_sharesModel.findPath(path);
       if (! list.isEmpty())
       {
           ui->filesView->selectionModel()->clear();
           ui->filesView->selectionModel()->select(list[0], QItemSelectionModel::Select | QItemSelectionModel::Rows);
           ui->filesView->selectionModel()->setCurrentIndex(list[0], QItemSelectionModel::SelectCurrent | QItemSelectionModel::Rows);
       }

       emit m_sharesModel.layoutChanged();

    }
}

void DesktopBackend::shareRemoved(QString share)
{
    if (m_shares.contains(share))
    {
        QString path = m_shares[share];
        m_sharesModel.removePath(path);
        m_shares.remove(share);

        m_selectedIndex = ui->filesView->selectionModel()->currentIndex();

        emit m_sharesModel.layoutChanged();
    }
}

void DesktopBackend::showShare(QString share)
{
    QString shareUrl = QString("https://%1/?item=%2/%3").arg(m_omdSettings.server(), QHostInfo::localHostName(), share);
    qDebug() << "Open URL" << shareUrl;
    openCloudUrl(shareUrl);
}

void DesktopBackend::showDevice()
{
    QString shareUrl = QString("https://%1/%2").arg(m_omdSettings.server(), QHostInfo::localHostName());
    openCloudUrl(shareUrl);
}


void DesktopBackend::parseUrl(QString url)
{
    qDebug() << "Parsing command from URL" << url;
    if (url.startsWith("omd://"))
    {
        QString cmd = url.mid(6);
        qDebug() << "Command:" << cmd;

        if (cmd.startsWith("add"))
        {
            showStorage();
            hideSettings();
            addShareAction();
        } else
        if (cmd.startsWith("show"))
        {
            showStorage();
            hideSettings();
        } else if (cmd.startsWith("settings"))
        {
            showStorage();
            showSettings();
        }

    } else
        qCritical() << "Invalid url scheme:" << url;
}




bool DesktopBackend::eventFilter(QObject *obj, QEvent *event)
{
    switch (event->type())
    {
        case QEvent::StatusTip:
        {
            QStatusTipEvent *statusEvent = static_cast<QStatusTipEvent *>(event);
            QString statusText = statusEvent->tip();
            if (statusText != "")
                ui->statusLabel->setText(statusText);
            else
                ui->statusLabel->setText(m_statusMessage);
            event->accept();
            return true;
        }
        case QEvent::ToolTip:
        {
            event->ignore();
            return true;
        }
        case QEvent::MouseButtonDblClick:
        {
         if (obj == ui->toolbar)
         {
            event->ignore();
            showCloud();
            return true;
         } else
         {
            event->accept();
            return false;
         }
        }
        default: return QWidget::eventFilter(obj, event);
    }

}


#if 0

void FileManager::setStatus(ServiceState state, QString statusMessage)
{
  m_statusMessage = statusMessage;
  m_state = state;
  ui->statusLabel->setText(m_statusMessage);
  m_spiffyGif.stop();

  switch (m_state)
  {
        case ServiceState::CertNeeded:
        {
            show();
            showSettings();
            ui->imgLabel->setPixmap(QPixmap(":/resource/fa-status-warning.png"));
            break;
        }
        case ServiceState::Connecting:
        {
            ui->imgLabel->setMovie(&m_spiffyGif);
            m_spiffyGif.start();
            break;
        }
        case ServiceState::ConnectionError:
        {
            show();
            ui->imgLabel->setPixmap(QPixmap(":/resource/fa-status-error.png"));
            break;
        }
        case ServiceState::Connected:
        {
            ui->statusLabel->setText(tr("Connected."));
            ui->imgLabel->setPixmap(QPixmap(":/resource/fa-status-ok.png"));
            hideSettings();

            break;
        }

        default: break;
  }
 // qDebug() << m_statusMessage;
  updateButtons();
}

#endif

//Menu actions

void DesktopBackend::addShareAction()
{
 // QFileDialog d;
  QString path =  QFileDialog::getExistingDirectory(nullptr, tr("Select folder"), QStandardPaths::writableLocation(QStandardPaths::HomeLocation), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
  if (path != "")
  {
      emit addShare(path);
  }
}


void DesktopBackend::removeShareAction()
{
    if (m_selectedIndex.data(BR_Path).toString() != "") {
        emit removeShare(m_selectedIndex.data().toString());
    }
}


void DesktopBackend::openFileManagerAction()
{
    openFileManager(m_selectedIndex.data(BR_Path).toString());
}


void DesktopBackend::openFileManager(QString path)
{

#ifdef Q_OS_WIN
   QUrl url = QUrl(QString("file:///%1").arg(path));
#else
   QUrl url = QUrl(QString("file://%1").arg(path));
#endif
   QDesktopServices::openUrl(url);
}



void DesktopBackend::showSettings()
{
    if (m_advancedVisible) {
        ui->advancedWidget->hide();
        m_advancedVisible = false;
        m_settingsVisible = false;
    }

    if (!m_settingsVisible)
    {
        ui_settings->mainWidget->setEnabled(true);
        ui_settings->mainWidget->show();
        ui->settingsWidget->show();

        QRect g( ui->settingsWidget->geometry());
        heightAnimation->setStartValue(g);
        g.setHeight(this->height() - 46);
        heightAnimation->setEndValue(g);
        opacityAnimation->setStartValue(0.001);
        opacityAnimation->setEndValue(1.0);
        opacityAnimation->setDuration(300);
        heightAnimation->setDuration(300);
        opacityEffect->setOpacity(0.001);

        updateButtons();

        m_settingsVisible = true;

        opacityAnimation->start();
        heightAnimation->start();

        ui->filesView->setVisible(false);
        //ui->bannerWidget->setVisible(false);
    }

}


void DesktopBackend::cancelAdvanced()
{
 ui_advanced->renewCheckBox->setChecked(false);
 if (m_status == OnMyDisk::Connected)
    hideSettings();
 else
    showSettings();
}

void DesktopBackend::okAdvanced()
{
 if (m_omdSettings.server() != ui_advanced->serverEdit->text() || m_omdSettings.port() != ui_advanced->cloudPortEdit->text().toInt()
    ||  ui_advanced->renewCheckBox->isChecked() || m_omdSettings.useProxy() != ui_advanced->proxyCheckbox->isChecked())
	m_resetNeeded =  true;
 m_omdSettings.setServer(ui_advanced->serverEdit->text());
 m_omdSettings.setPort(ui_advanced->cloudPortEdit->text().toInt());
 m_omdSettings.setAutostart(ui_advanced->autostartCheckBox->isChecked());
 m_omdSettings.setUseProxy( ui_advanced->proxyCheckbox->isChecked());
 m_omdSettings.setProxyHost(ui_advanced->hostEdit->text());
 m_omdSettings.setProxyPort(ui_advanced->portEdit->text().toInt());
 m_omdSettings.setProxyUser(ui_advanced->userEdit->text());
 m_omdSettings.setProxyPassword(ui_advanced->passwordEdit->text());
 m_omdSettings.setResetId(m_resetNeeded);

 ui_advanced->renewCheckBox->setChecked(false);
 if (m_resetNeeded) {
    showSettings();
    emit settingsChanged(m_omdSettings);
 } else
    hideSettings();
}


void DesktopBackend::hideSettings()
{
    if (m_advancedVisible) {
        ui->advancedWidget->hide();
        m_settingsVisible = true;
        m_advancedVisible = false;
    }
    if (m_settingsVisible)
    {
        ui->settingsWidget->show();

        QRect g( ui->settingsWidget->geometry());
        heightAnimation->setStartValue(g);
        g.setHeight(50);

        heightAnimation->setEndValue(g);
        opacityAnimation->setStartValue(1.0);
        opacityAnimation->setEndValue(0.001);
        opacityAnimation->setDuration(150);
        heightAnimation->setDuration(150);
        opacityEffect->setOpacity(1.0);

        updateButtons();

        m_settingsVisible = false;

        opacityAnimation->start();
        heightAnimation->start();

        ui->filesView->setVisible(false);
    }
}




void DesktopBackend::applySettings()
{
    //basic
    m_resetNeeded = ui_advanced->renewCheckBox->isChecked();
    if (m_resetNeeded)
        m_loginRequired = true;
    m_omdSettings.setUser(ui_settings->accountEdit->text());
    m_omdSettings.setPassword(ui_settings->passwordEdit->text());
    m_omdSettings.setResetId(ui_advanced->renewCheckBox->isChecked());
    setAutostart(m_omdSettings.autostart());

    ui_advanced->renewCheckBox->setChecked(false);
    if (m_loginRequired) {
        m_omdSettings.setStorageId("");
        emit settingsChanged(m_omdSettings);
    }
    else {
        //emit signOut();
        hideSettings();
    }

    m_omdSettings.setPassword("");
}





void DesktopBackend::sysTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if ( reason == QSystemTrayIcon::DoubleClick)
    {
        emit showStorage();
    }
}

void DesktopBackend::animateIcon()
{
    m_trayIcon.setIcon(QPixmap(QString(":icons/states/icon_wait_0%1.png").arg(m_step)));
    m_storageAction->setIcon(QPixmap(QString(":icons/states/icon_wait_0%1.png").arg(m_step )));
    if (++m_step > 4 ) m_step = 1;
}


void DesktopBackend::createTrayIcon()
{

    m_step = 1;
    connect(&m_statusTimer, SIGNAL(timeout()), this, SLOT(animateIcon()));

    m_trayIcon.setToolTip(QString("%1: %2").arg(APP_NAME, m_statusMessage));
    m_storageAction = new QAction(QString("%1: %2").arg(APP_NAME, m_statusMessage), this);
    connect(m_storageAction, &QAction::triggered, this,  &DesktopBackend::showStorage);
    m_trayIconMenu.addAction(m_storageAction);
    m_trayIconMenu.addSeparator();
    m_trayIconMenu.addAction(ui->actionOpenCloud);
    m_trayIconMenu.addSeparator();
    m_trayIconMenu.addAction(ui->actionQuit);

    m_trayIcon.setContextMenu(&m_trayIconMenu);
    connect(&m_trayIcon, &QSystemTrayIcon::activated, this, &DesktopBackend::sysTrayActivated);

    m_trayIcon.setIcon(QPixmap(":icons/states/icon_disconnected.png"));
    m_storageAction->setIcon(QPixmap(":icons/states/icon_disconnected.png"));

    showTrayIcon();
}

void DesktopBackend::showTrayIcon()
{
    if (QSystemTrayIcon::isSystemTrayAvailable())
    {
        m_trayIcon.show();
    } else
       QTimer::singleShot(5000, this, &DesktopBackend::showTrayIcon);
}


void DesktopBackend::setAutostart(bool autostart)
{

    //platform-specific location
#if defined(Q_OS_WIN)

    QSettings settings("HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    if (autostart)
    {
        QString appFile =  QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        QString value = QString("\"%1\" -t").arg(appFile);
        settings.setValue(APP_GENERIC_NAME, value);
    }
    else
    {
        settings.remove(APP_GENERIC_NAME);
    }


#elif defined(Q_OS_OSX)
   // create startup launchd entry
   QSettings plist(QDir::homePath() + QString("/Library/LaunchAgents/com.%1.agent.plist").arg(APP_GENERIC_NAME), QSettings::NativeFormat);
   if (autostart)
   {
        plist.setValue("Label", QString("com.%1.agent").arg(APP_GENERIC_NAME));
        //plist.setValue("KeepAlive", true);
        plist.setValue("RunAtLoad", true);
        plist.sync();
        //TODO try on Mac, check chmod("~/Library/LaunchAgents", 700);
   } else
   {
       QFile f(plist.fileName());
       f.remove();
   }
#elif defined (Q_OS_LINUX)
   // create startup desktop entry

   QString desktop_entry = "[Desktop Entry]\n"
                  "Type=Application\n"
                  "Name=On My Disk\n"
                  "GenericName=onmydisk\n"
                  "Comment=Share your files online\n"
                  "Exec=%1 -t\n"
                  "Categories=Network;\n"
                  "Icon=onmydisk\n";

   QFile entry_file(QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + QString("/.config/autostart/%1.desktop").arg( APP_GENERIC_NAME));
   if (autostart)
   {
       entry_file.open(QFile::WriteOnly | QFile::Text);
       entry_file.write(desktop_entry.arg(QCoreApplication::applicationFilePath()).toUtf8());
       entry_file.close();

   } else
   {
       entry_file.remove();
   }
#endif

}



DesktopBackend::~DesktopBackend()
{
    m_trayIcon.hide();
    delete ui;
}
