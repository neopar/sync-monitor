/*
 * Copyright 2014 Canonical Ltd.
 *
 * This file is part of sync-monitor.
 *
 * sync-monitor is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * contact-service-app is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "sync-daemon.h"
#include "sync-account.h"
#include "sync-queue.h"
#include "sync-dbus.h"
#include "sync-i18n.h"
#include "eds-helper.h"
#include "notify-message.h"
#include "provider-template.h"
#include "sync-network.h"

#include <QtCore/QDebug>
#include <QtCore/QTimer>

using namespace Accounts;


#define DAEMON_SYNC_TIMEOUT     1000 * 60 // one minute
#define SYNC_MONITOR_ICON_PATH  "/usr/share/icons/ubuntu-mobile/actions/scalable/reload.svg"

SyncDaemon::SyncDaemon()
    : QObject(0),
      m_manager(0),
      m_eds(0),
      m_dbusAddaptor(0),
      m_syncing(false),
      m_aboutToQuit(false),
      m_currentAccount(0)
{
    m_provider = new ProviderTemplate();
    m_provider->load();

    m_syncQueue = new SyncQueue();
    m_offlineQueue = new SyncQueue();
    m_networkStatus = new SyncNetwork(this);
    connect(m_networkStatus, SIGNAL(onlineChanged(bool)), SLOT(onOnlineStatusChanged(bool)));

    m_timeout = new QTimer(this);
    m_timeout->setInterval(DAEMON_SYNC_TIMEOUT);
    m_timeout->setSingleShot(true);
    connect(m_timeout, SIGNAL(timeout()), SLOT(continueSync()));
}

SyncDaemon::~SyncDaemon()
{
    quit();
    delete m_timeout;
    delete m_syncQueue;
    delete m_offlineQueue;
    delete m_networkStatus;
}

void SyncDaemon::setupAccounts()
{
    if (m_manager) {
        return;
    }
    qDebug() << "Loading accounts...";

    m_manager = new Manager(this);
    Q_FOREACH(const AccountId &accountId, m_manager->accountList()) {
        addAccount(accountId, false);
    }
    connect(m_manager,
            SIGNAL(accountCreated(Accounts::AccountId)),
            SLOT(addAccount(Accounts::AccountId)));
    connect(m_manager,
            SIGNAL(accountRemoved(Accounts::AccountId)),
            SLOT(removeAccount(Accounts::AccountId)));
    Q_EMIT accountsChanged();
}

void SyncDaemon::setupTriggers()
{
    m_eds = new EdsHelper(this);
    connect(m_eds, &EdsHelper::dataChanged,
            this, &SyncDaemon::onDataChanged);
}

void SyncDaemon::onDataChanged(const QString &serviceName, const QString &sourceName)
{
    if (sourceName.isEmpty()) {
        syncAll(serviceName);
    } else {
        Q_FOREACH(SyncAccount *acc, m_accounts.values()) {
            if (acc->displayName() == sourceName) {
                sync(acc, serviceName);
                return;
            }
        }
    }
}

void SyncDaemon::onOnlineStatusChanged(bool isOnline)
{
    Q_EMIT isOnlineChanged(isOnline);
    if (isOnline) {
        qDebug() << "Device is online sync pending changes" << m_offlineQueue->count();
        m_syncQueue->push(m_offlineQueue->values());
        m_offlineQueue->clear();
        if (!m_syncing && !m_syncQueue->isEmpty()) {
            sync(true);
        } else {
            qDebug() << "No change to sync";
        }
    } else {
        qDebug() << "Device is offline cancel active syncs";
        cancel();
    }
    // make accounts availabel or not based on online status
    Q_EMIT accountsChanged();
}

void SyncDaemon::syncAll(const QString &serviceName, bool runNow)
{
    Q_FOREACH(SyncAccount *acc, m_accounts.values()) {
        if (serviceName.isEmpty()) {
            sync(acc, QString(), runNow);
        } else if (acc->availableServices().contains(serviceName)) {
            sync(acc, serviceName, runNow);
        }
    }
}

void SyncDaemon::cancel(const QString &serviceName)
{
    Q_FOREACH(SyncAccount *acc, m_accounts.values()) {
        if (serviceName.isEmpty()) {
            cancel(acc);
        } else if (acc->availableServices().contains(serviceName)) {
            cancel(acc, serviceName);
        }
    }
}

void SyncDaemon::sync(bool runNow)
{
    m_syncing = true;
    if (runNow) {
        m_timeout->stop();
        continueSync();
    } else {
        // wait some time for new sync requests
        m_timeout->start();
    }
}

void SyncDaemon::continueSync()
{
    if (!m_networkStatus->isOnline()) {
        qDebug() << "Device is offline we will skip the sync.";
        m_offlineQueue->push(m_syncQueue->values());
        m_syncQueue->clear();
        m_currentAccount = 0;
        m_syncing = false;
        Q_EMIT done();
        return;
    }

    // flush any change in EDS
    m_eds->flush();

    // freeze notifications during the sync, to save some CPU
    m_eds->freezeNotify();

    // sync the next service on the queue
    if (!m_aboutToQuit && !m_syncQueue->isEmpty()) {
        m_currentServiceName = m_syncQueue->popNext(&m_currentAccount);
    } else {
        m_currentAccount = 0;
    }

    if (m_currentAccount) {
        m_currentAccount->sync(m_currentServiceName);
    } else {
        m_currentAccount = 0;
        m_currentServiceName.clear();
        m_syncing = false;
        // The sync has done, unblock notifications
        m_eds->unfreezeNotify();
        Q_EMIT done();
    }
}

bool SyncDaemon::registerService()
{
    if (!m_dbusAddaptor) {
        QDBusConnection connection = QDBusConnection::sessionBus();
        if (connection.interface()->isServiceRegistered(SYNCMONITOR_SERVICE_NAME)) {
            qWarning() << "SyncMonitor service already registered";
            return false;
        } else if (!connection.registerService(SYNCMONITOR_SERVICE_NAME)) {
            qWarning() << "Could not register service!" << SYNCMONITOR_SERVICE_NAME;
            return false;
        }

        m_dbusAddaptor = new SyncDBus(connection, this);
        if (!connection.registerObject(SYNCMONITOR_OBJECT_PATH, this))
        {
            qWarning() << "Could not register object!" << SYNCMONITOR_OBJECT_PATH;
            delete m_dbusAddaptor;
            m_dbusAddaptor = 0;
            return false;
        }
    }
    return true;
}

void SyncDaemon::run()
{
    setupAccounts();
    setupTriggers();

    // export dbus interface
    registerService();
}

bool SyncDaemon::isPending() const
{
    // there is a sync request on the buffer
    return (m_syncQueue && (m_syncQueue->count() > 0));
}

bool SyncDaemon::isSyncing() const
{
    // the sync is happening right now
    return (m_syncing && (m_currentAccount != 0));
}

QStringList SyncDaemon::availableServices() const
{
    // TODO: check for all providers
    return m_provider->supportedServices("google");
}

QStringList SyncDaemon::enabledServices() const
{
    QSet<QString> services;
    QStringList available = availableServices();
    Q_FOREACH(SyncAccount *syncAcc, m_accounts) {
        Q_FOREACH(const QString &service, syncAcc->enabledServices()) {
            if (available.contains(service)) {
                services << service;
            }
        }
    }
    return services.toList();
}

bool SyncDaemon::isOnline() const
{
    return m_networkStatus->isOnline();
}

void SyncDaemon::addAccount(const AccountId &accountId, bool startSync)
{
    Account *acc = m_manager->account(accountId);
    qDebug() << "Found account:" << acc->displayName();
    if (!acc) {
        qWarning() << "Fail to retrieve accounts:" << m_manager->lastError().message();
    } else if (m_provider->contains(acc->providerName())) {
        SyncAccount *syncAcc = new SyncAccount(acc,
                                               m_provider->settings(acc->providerName()),
                                               this);
        m_accounts.insert(accountId, syncAcc);
        connect(syncAcc, SIGNAL(syncStarted(QString, bool)),
                         SLOT(onAccountSyncStarted(QString, bool)));
        connect(syncAcc, SIGNAL(syncFinished(QString, bool, QString)),
                         SLOT(onAccountSyncFinished(QString, bool, QString)));
        connect(syncAcc, SIGNAL(syncError(QString, int)),
                         SLOT(onAccountSyncError(QString, int)));
        connect(syncAcc, SIGNAL(enableChanged(QString, bool)),
                         SLOT(onAccountEnableChanged(QString, bool)));
        connect(syncAcc, SIGNAL(configured(QString)),
                         SLOT(onAccountConfigured(QString)), Qt::DirectConnection);
        if (startSync) {
            sync(syncAcc, QString(), true);
        }
        Q_EMIT accountsChanged();
    }
}

void SyncDaemon::sync(SyncAccount *syncAcc, const QString &serviceName, bool runNow)
{
    qDebug() << "syn requested for account:" << syncAcc->displayName() << serviceName;

    // check if the service is already in the sync queue or is the current operation
    if (m_syncQueue->contains(syncAcc, serviceName) ||
        (m_currentAccount == syncAcc) && (serviceName.isEmpty() || (serviceName == m_currentServiceName))) {
        qDebug() << "Account aready in the queue";
    } else {
        qDebug() << "Pushed into queue";
        m_syncQueue->push(syncAcc, serviceName);
        // if not syncing start a full sync
        if (!m_syncing) {
            sync(runNow);
            Q_EMIT syncAboutToStart();
            return;
        }
    }

    // immediately request, force sync to start
    if (runNow && !isSyncing()) {
        sync(runNow);
        Q_EMIT syncAboutToStart();
    }
}

void SyncDaemon::cancel(SyncAccount *syncAcc, const QString &serviceName)
{
    m_syncQueue->remove(syncAcc, serviceName);
    syncAcc->cancel(serviceName);
    if (m_currentAccount == syncAcc) {
        qDebug() << "Current sync canceled";
        m_currentAccount = 0;
    }
    Q_EMIT syncError(syncAcc, serviceName, "canceled");
}

void SyncDaemon::removeAccount(const AccountId &accountId)
{
    SyncAccount *syncAcc = m_accounts.take(accountId);
    if (syncAcc) {
        cancel(syncAcc);

        NotifyMessage *notify = new NotifyMessage(true, this);
        notify->setProperty("ACCOUNT", QVariant::fromValue<QObject*>(qobject_cast<QObject*>(syncAcc)));
        connect(notify, SIGNAL(questionRejected()), SLOT(removeAccountSource()));
        connect(notify, SIGNAL(messageClosed()), SLOT(destroyAccount()));
        notify->askYesOrNo(_("Synchronization"),
                           QString(_("Account %1 removed. Do you want to keep the account data?"))
                                .arg(syncAcc->displayName()),
                           SYNC_MONITOR_ICON_PATH);

    }
    Q_EMIT accountsChanged();
}

void SyncDaemon::removeAccountSource()
{
    QObject *sender = QObject::sender();
    QObject *accObj = sender->property("ACCOUNT").value<QObject*>();
    SyncAccount *acc = qobject_cast<SyncAccount*>(accObj);
    Q_ASSERT(acc);
    m_eds->removeSource("", acc->displayName());
}

void SyncDaemon::destroyAccount()
{
    QObject *sender = QObject::sender();
    QObject *acc = sender->property("ACCOUNT").value<QObject*>();
    Q_ASSERT(acc);
    acc->deleteLater();
}

void SyncDaemon::onAccountSyncStarted(const QString &serviceName, bool firstSync)
{
    SyncAccount *acc = qobject_cast<SyncAccount*>(QObject::sender());
    if (firstSync) {
        NotifyMessage *notify = new NotifyMessage(true, this);
        notify->show(_("Synchronization"),
                     QString(_("Start sync:  %1 (%2)"))
                         .arg(acc->displayName())
                         .arg(serviceName),
                     acc->iconName(serviceName));
    }
    m_syncElapsedTime.restart();
    qDebug() << QString("[%3] Start sync:  %1 (%2)")
                .arg(acc->displayName())
                .arg(serviceName)
                .arg(QDateTime::currentDateTime().toString(Qt::SystemLocaleShortDate));
    Q_EMIT syncStarted(acc, serviceName);
}

void SyncDaemon::onAccountSyncFinished(const QString &serviceName, const bool firstSync, const QString &status)
{
    SyncAccount *acc = qobject_cast<SyncAccount*>(QObject::sender());
    QString errorMessage = SyncAccount::statusDescription(status);

    if (firstSync && errorMessage.isEmpty()) {
        NotifyMessage *notify = new NotifyMessage(true, this);
        notify->show(_("Synchronization"),
                     QString(_("Sync done: %1 (%2)"))
                         .arg(acc->displayName())
                         .arg(serviceName),
                     acc->iconName(serviceName));
    } else if (!errorMessage.isEmpty()) {
        NotifyMessage *notify = new NotifyMessage(true, this);
        notify->show(_("Synchronization"),
                     QString(_("Fail to sync %1 (%2).\n%3"))
                         .arg(acc->displayName())
                         .arg(serviceName)
                         .arg(errorMessage),
                     acc->iconName(serviceName));
    }

    qDebug() << QString("[%6] Sync done: %1 (%2) Status: %3 Error: %4 Duration: %5s")
                .arg(acc->displayName())
                .arg(serviceName)
                .arg(status)
                .arg(errorMessage.isEmpty() ? "None" : errorMessage)
                .arg((m_syncElapsedTime.elapsed() < 1000 ? 1  : m_syncElapsedTime.elapsed() / 1000))
                .arg(QDateTime::currentDateTime().toString(Qt::SystemLocaleShortDate));

    Q_EMIT syncFinished(acc, serviceName);

    // sync next account
    continueSync();
}

void SyncDaemon::onAccountSyncError(const QString &serviceName, int errorCode)
{
    SyncAccount *acc = qobject_cast<SyncAccount*>(QObject::sender());

    NotifyMessage *notify = new NotifyMessage(true, this);
    notify->show(_("Synchronization"),
                 QString(_("Sync error account: %1, %2, %3"))
                     .arg(acc->displayName())
                     .arg(serviceName)
                     .arg(errorCode),
                 acc->iconName(serviceName));

    Q_EMIT syncError(acc, serviceName, QString(errorCode));
    // sync next account
    continueSync();
}

void SyncDaemon::onAccountEnableChanged(const QString &serviceName, bool enabled)
{
    SyncAccount *acc = qobject_cast<SyncAccount*>(QObject::sender());
    if (enabled) {
        sync(acc, serviceName, true);
    } else {
        cancel(acc, serviceName);
    }
    Q_EMIT accountsChanged();
}

void SyncDaemon::onAccountConfigured(const QString &serviceName)
{
    SyncAccount *acc = qobject_cast<SyncAccount*>(QObject::sender());
    m_eds->createSource(serviceName, acc->displayName());
}

void SyncDaemon::quit()
{
    m_aboutToQuit = true;

    if (m_dbusAddaptor) {
        delete m_dbusAddaptor;
        m_dbusAddaptor = 0;
    }

    if (m_eds) {
        delete m_eds;
        m_eds = 0;
    }

    // cancel all sync operation
    while(m_syncQueue->count()) {
        SyncAccount *acc = m_syncQueue->popNext();
        acc->cancel();
        acc->wait();
        delete acc;
    }

    while(m_offlineQueue->count()) {
        SyncAccount *acc = m_syncQueue->popNext();
        acc->cancel();
        acc->wait();
        delete acc;
    }

    if (m_manager) {
        delete m_manager;
        m_manager = 0;
    }

    if (m_networkStatus) {
        delete m_networkStatus;
    }
}
