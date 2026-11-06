#include "git/GitService.h"
#include "ui/AppStyle.h"
#include "ui/MainWindow.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include "ui/SidebarNavigation.h"
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QProcess>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using namespace ade;
namespace {
Conversation chat(QString id, QString workspace = {}) {
    Conversation s;
    s.id = id;
    s.title = id;
    s.workspace = workspace;
    s.created = s.updated = s.lastMessageAt = QDateTime::currentMSecsSinceEpoch();
    return s;
}
QStringList ids(QListWidget* list) {
    QStringList r;
    for (int i = 0; i < list->count(); ++i)
        if (!list->item(i)->data(Qt::UserRole).toString().isEmpty())
            r.append(list->item(i)->data(Qt::UserRole).toString());
    return r;
}
QStringList groups(QListWidget* list) {
    QStringList r;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->data(SidebarNavigation::KindRole) == SidebarRow::Header)
            r.append(list->item(i)->text());
    return r;
}
bool initRepo(const QString& path) {
    QProcess process;
    process.setWorkingDirectory(path);
    process.start("git", {"init", "-b", "main"});
    return process.waitForFinished(10000) && process.exitCode() == 0;
}
} // namespace
class SidebarNavigationTest : public QObject {
    Q_OBJECT
  private slots:
    void removedGitServiceDropsCachedLabels() {
        QTemporaryDir profile, repo;
        QVERIFY(initRepo(repo.path()));
        GitService service;
        GitWorkspaceContext context;
        context.trustedRoots = {repo.path()};
        service.setWorkspaceContext(context);
        SettingsPage settings(profile.path());
        QListWidget list;
        QToolButton grouping;
        QLabel label;
        SidebarNavigation nav(&list, &grouping, &label, &settings);
        nav.refresh({chat("one", repo.path())}, {}, "one", {});
        QVERIFY(nav.setGroupingMode("workspace"));
        nav.setGitService(&service);
        QTRY_VERIFY_WITH_TIMEOUT(groups(&list).contains("Root (main)"), 10000);
        nav.setGitService(nullptr);
        QVERIFY(!groups(&list).contains("Root (main)"));
        QCOMPARE(ids(&list), QStringList({"one"}));
    }
    void replacedGitServiceInspectsTheCurrentWorkspace() {
        QTemporaryDir profile, repo;
        QVERIFY(initRepo(repo.path()));
        GitService first, second;
        GitWorkspaceContext context;
        context.trustedRoots = {repo.path()};
        first.setWorkspaceContext(context);
        second.setWorkspaceContext(context);
        SettingsPage settings(profile.path());
        QListWidget list;
        QToolButton grouping;
        QLabel label;
        SidebarNavigation nav(&list, &grouping, &label, &settings);
        nav.refresh({chat("one", repo.path())}, {}, "one", {});
        QVERIFY(nav.setGroupingMode("workspace"));
        nav.setGitService(&first);
        nav.workspaceContextChanged();
        QSignalSpy completed(&second, &GitService::completed);
        nav.setGitService(&second);
        nav.workspaceContextChanged();
        QTRY_VERIFY_WITH_TIMEOUT(completed.size() > 0, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(groups(&list).contains("Root (main)"), 10000);
    }
    void topologyRefreshRejectsTheRetiredInspection() {
        QTemporaryDir profile, repo;
        QVERIFY(initRepo(repo.path()));
        GitService service;
        GitWorkspaceContext context;
        context.trustedRoots = {repo.path()};
        service.setWorkspaceContext(context);
        SettingsPage settings(profile.path());
        QListWidget list;
        QToolButton grouping;
        QLabel label;
        SidebarNavigation nav(&list, &grouping, &label, &settings);
        nav.refresh({chat("one", repo.path())}, {}, "one", {});
        QVERIFY(nav.setGroupingMode("workspace"));
        quint64 retired = 0;
        bool observed = false, stalePublished = false;
        connect(&service, &GitService::completed, &nav, [&](quint64 id, const GitResult& result) {
            if (!retired && result.ok && !result.snapshot.repoRoot.isEmpty()) {
                retired = id;
                emit service.repositoryTopologyChanged(result.snapshot.commonGitDir, 1);
            }
        });
        nav.setGitService(&service);
        connect(&service, &GitService::completed, &nav, [&](quint64 id, const GitResult&) {
            if (id == retired) {
                observed = true;
                stalePublished = groups(&list).contains("Root (main)");
            }
        });
        nav.workspaceContextChanged();
        QTRY_VERIFY_WITH_TIMEOUT(observed, 10000);
        QVERIFY(!stalePublished);
        QTRY_VERIFY_WITH_TIMEOUT(groups(&list).contains("Root (main)"), 10000);
    }
    void groupsAndPinsPersistAcrossRestart() {
        QTemporaryDir data;
        QList<Conversation> sessions = {chat("one", "C:/fixture/a"), chat("two", "D:/fixture/b"),
                                        chat("three")};
        QVERIFY(SessionStore(data.path()).save(sessions));
        {
            MainWindow window(data.path());
            auto* nav = window.findChild<SidebarNavigation*>();
            QVERIFY(nav);
            auto* list = window.findChild<QListWidget*>("agentList");
            QCOMPARE(groups(list), QStringList({"Today"}));
            QVERIFY(nav->setPinned("two", true));
            QVERIFY(nav->setPinned("one", true));
            QCOMPARE(ids(list).mid(0, 2), QStringList({"one", "two"}));
            QVERIFY(nav->movePinned("one", 1));
            QCOMPARE(ids(list).mid(0, 2), QStringList({"two", "one"}));
            QVERIFY(nav->setGroupingMode("workspace"));
            QVERIFY(groups(list).contains("No workspace"));
            QCOMPARE(ids(list).count("one"), 2);
        }
        MainWindow reopened(data.path());
        auto* nav = reopened.findChild<SidebarNavigation*>();
        QVERIFY(nav->isPinned("one"));
        QVERIFY(nav->isPinned("two"));
        QCOMPARE(reopened.findChild<SettingsPage*>()->preference("sidebar.agentListGroupingMode").toString(),
                 QString("workspace"));
        QCOMPARE(ids(reopened.findChild<QListWidget*>("agentList")).mid(0, 2), QStringList({"two", "one"}));
    }
    void saveFailureKeepsOrderingAndSelection() {
        QTemporaryDir data;
        QVERIFY(SessionStore(data.path()).save({chat("one"), chat("two")}));
        MainWindow window(data.path());
        auto* nav = window.findChild<SidebarNavigation*>();
        auto* list = window.findChild<QListWidget*>("agentList");
        const auto before = ids(list);
        const auto selected = window.currentSessionId();
        QFile conflict(data.filePath("preferences.json"));
        QVERIFY(conflict.open(QIODevice::WriteOnly));
        conflict.write("externally changed");
        conflict.close();
        QVERIFY(!nav->setPinned("two", true));
        QVERIFY(!nav->setGroupingMode("workspace"));
        QCOMPARE(ids(list), before);
        QCOMPARE(window.currentSessionId(), selected);
        QVERIFY(!nav->isPinned("two"));
    }
    void activityMigrationAndInvalidPreservation() {
        QTemporaryDir data;
        auto original = chat("legacy");
        original.updated = 12345;
        original.messages = {QJsonObject{{"role", "user"}, {"content", "old"}}};
        QVERIFY(SessionStore(data.path()).save({original}));
        QFile file(data.filePath("sessions.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto object = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        auto rows = object["sessions"].toArray();
        auto record = rows[0].toObject();
        record.remove("lastMessageAt");
        rows[0] = record;
        object["sessions"] = rows;
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(object).toJson());
        file.close();
        QString error;
        QCOMPARE(SessionStore(data.path()).load(&error).first().lastMessageAt, qint64(12345));
        QVERIFY(error.isEmpty());
        record["lastMessageAt"] = -1;
        rows[0] = record;
        object["sessions"] = rows;
        const auto invalid = QJsonDocument(object).toJson();
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(invalid);
        file.close();
        SessionStore store(data.path());
        QVERIFY(store.load(&error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(!store.save({}, &error));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), invalid);
    }
    void pinContextMenuAndArchiveUnpin() {
        QTemporaryDir data;
        QVERIFY(SessionStore(data.path()).save({chat("one"), chat("two")}));
        MainWindow window(data.path());
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        auto* list = window.findChild<QListWidget*>("agentList");
        QListWidgetItem* target = nullptr;
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->data(Qt::UserRole) == "two")
                target = list->item(i);
        QVERIFY(target);
        emit list->customContextMenuRequested(list->visualItemRect(target).center());
        auto* menu = window.findChild<QMenu*>("conversationMenu");
        QVERIFY(menu);
        auto* pin = menu->findChild<QAction*>("pinConversation");
        QVERIFY(pin);
        pin->trigger();
        menu->close();
        auto* nav = window.findChild<SidebarNavigation*>();
        QVERIFY(nav->isPinned("two"));
        target = nullptr;
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->data(Qt::UserRole) == "two")
                target = list->item(i);
        emit list->customContextMenuRequested(list->visualItemRect(target).center());
        const auto menus = window.findChildren<QMenu*>("conversationMenu");
        menu = menus.last();
        QVERIFY(menu->findChild<QAction*>("unpinConversation"));
        menu->findChild<QAction*>("archiveConversation")->trigger();
        menu->close();
        QVERIFY(!nav->isPinned("two"));
        QVERIFY(!ids(list).contains("two"));
    }
};
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    initializeAppStyle(app);
    SidebarNavigationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_sidebar_navigation.moc"
