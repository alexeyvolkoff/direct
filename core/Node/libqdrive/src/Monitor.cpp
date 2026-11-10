/**
 * \author Andreas Misje
 * \date 2014
 * \copyright MIT
 */

#include "QDrive.h"
#include "Monitor.h"
#include "Drive.h"
#include "Partition.h"
#include "FileSystem.h"
#include "BlockDevice.h"
using namespace QDrive;
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMetaType>
#include <QDebug>

Monitor::Monitor(QObject *parent)
	: QObject(parent)
{
    qDBusRegisterMetaType<InterfaceList>();
    qDBusRegisterMetaType<QDBusObjectPath>();
    qRegisterMetaType<InterfaceList>("QDrive::Monitor::InterfaceList");

	QDBusConnection sys = QDBusConnection::systemBus();
    if (!sys.isConnected()) {
        qWarning() << "DBus system bus is not connected. Drive monitoring disabled.";
        return;
    }

    bool connectionOK;
    connectionOK = sys.connect(UDisks2::service(), UDisks2::path(),
            Interface::ObjectManager(), "InterfacesAdded",  this, SLOT(
                checkAddedInterfaces(const QDBusObjectPath &,  const QDrive::Monitor::InterfaceList&)));

    if (!connectionOK) {
        qWarning() << "Could not connect InterfacesAdded signal (UDisks2 may not be running).";
        return;
    }

	connectionOK = sys.connect(UDisks2::service(), UDisks2::path(), 
            Interface::ObjectManager(), "InterfacesRemoved", this,  SLOT(
                checkRemovedInterfaces(const QDBusObjectPath &, const QStringList &)));

    if (!connectionOK) {
        qWarning() << "Could not connect InterfacesRemoved signal.";
        return;
    }
}

void Monitor::checkAddedInterfaces(const QDBusObjectPath &path, const 
		QDrive::Monitor::InterfaceList &interfaces)
{
	QList<QString> interfaceNames = interfaces.keys();

	if (Drive::isDrive(interfaceNames))
		emit driveAdded(path);
	if (BlockDevice::isBlockDevice(interfaceNames))
		emit blockDeviceAdded(path);
	if (Partition::isPartition(interfaceNames))
		emit partitionAdded(path);
	if (FileSystem::isFileSystem(interfaceNames))
		emit fileSystemAdded(path);
}

void Monitor::checkRemovedInterfaces(const QDBusObjectPath &path, const 
		QStringList &interfaces)
{
	if (Drive::isDrive(interfaces))
		emit driveRemoved(path);
	if (FileSystem::isFileSystem(interfaces))
		emit fileSystemRemoved(path);
	if (Partition::isPartition(interfaces))
		emit partitionRemoved(path);
	if (BlockDevice::isBlockDevice(interfaces))
		emit blockDeviceRemoved(path);
}
