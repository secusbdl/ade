#include "ui/SidebarModel.h"
#include <QTimeZone>
#include <QtTest>
using namespace ade;
namespace {
SidebarEntry entry(QString id, qint64 created, qint64 last, QString path = {}) {
    SidebarEntry e;
    e.conversation.id = id;
    e.conversation.title = id;
    e.conversation.created = created;
    e.conversation.lastMessageAt = last;
    e.conversation.workspace = path;
    if (!path.isEmpty()) {
        SidebarMount m;
        m.path = path;
        e.mounts.append(m);
    }
    return e;
}
QStringList agents(const SidebarProjection& p) {
    QStringList ids;
    for (const auto& r : p.rows)
        if (r.kind == SidebarRow::Agent)
            ids.append(r.agentId);
    return ids;
}
QStringList headers(const SidebarProjection& p) {
    QStringList names;
    for (const auto& r : p.rows)
        if (r.kind == SidebarRow::Header)
            names.append(r.title);
    return names;
}
} // namespace
class SidebarModelTest : public QObject {
    Q_OBJECT
  private slots:
    void localCalendarDays_data() {
        QTest::addColumn<int>("days");
        QTest::addColumn<QString>("expected");
        for (const auto& pair : QList<QPair<int, QString>>{{-1, "Today"},
                                                           {0, "Today"},
                                                           {1, "Yesterday"},
                                                           {2, "Last 7 days"},
                                                           {7, "Last 7 days"},
                                                           {8, "Last 30 days"},
                                                           {30, "Last 30 days"},
                                                           {31, "Older"}})
            QTest::newRow(qPrintable(QString::number(pair.first))) << pair.first << pair.second;
    }
    void localCalendarDays() {
        QFETCH(int, days);
        QFETCH(QString, expected);
        const QTimeZone zone("America/New_York");
        QVERIFY(zone.isValid());
        const QDateTime now(QDate(2026, 11, 2), QTime(0, 15), zone);
        const auto timestamp = QDateTime(now.date().addDays(-days), QTime(23, 55), zone).toMSecsSinceEpoch();
        QCOMPARE(SidebarModel::ageLabel(timestamp, now), expected);
        QCOMPARE(SidebarModel::ageLabel(0, now), QString("Today"));
    }
    void stableNewestOrderAndPinPriority() {
        SidebarProjectionOptions o;
        o.pinnedIds = {"a", "b", "a", "missing"};
        QList<SidebarEntry> es{entry("a", 10, 10), entry("b", 30, 30), entry("c", 40, 40),
                               entry("d", 40, 40)};
        const auto p = SidebarModel::project(es, o);
        QCOMPARE(agents(p), QStringList({"a", "b", "c", "d"}));
        QCOMPARE(p.pinnedCount, 2);
    }
    void zeroActivityStillSortsByCreation() {
        SidebarProjectionOptions o;
        const auto p = SidebarModel::project({entry("empty", 100, 0), entry("old", 10, 10)}, o);
        QCOMPARE(agents(p), QStringList({"empty", "old"}));
        QCOMPARE(headers(p).first(), QString("Today"));
    }
    void currentBeyondFirstPageRemainsVisible() {
        SidebarProjectionOptions o;
        o.currentId = "0";
        QList<SidebarEntry> es;
        for (int i = 0; i < 35; ++i)
            es.append(entry(QString::number(i), i, i));
        const auto p = SidebarModel::project(es, o);
        QCOMPARE(agents(p).size(), 11);
        QVERIFY(agents(p).contains("0"));
        QCOMPARE(p.rows.last().kind, SidebarRow::ShowMore);
        o.visibleCount = 30;
        QCOMPARE(agents(SidebarModel::project(es, o)).size(), 31);
    }
    void newSessionChatsRaiseVisibleFloor() {
        SidebarProjectionOptions o;
        o.sessionStartedAt = 10;
        QList<SidebarEntry> es;
        for (int i = 0; i < 25; ++i)
            es.append(entry(QString::number(i), i, i));
        QCOMPARE(agents(SidebarModel::project(es, o)).size(), 15);
    }
    void searchHasNoEmptyGroupsOrArchivedLeaks() {
        SidebarProjectionOptions o;
        o.query = " NEEDLE ";
        o.pinnedIds = {"needle-pin"};
        auto archived = entry("needle-archived", 200, 200);
        archived.conversation.archived = true;
        const auto p = SidebarModel::project(
            {entry("other", 100, 100), entry("needle-pin", 1, 1), entry("needle", 2, 2), archived}, o);
        QCOMPARE(agents(p), QStringList({"needle-pin", "needle"}));
        QCOMPARE(p.pinnedCount, 1);
        o.query = "absent";
        QVERIFY(SidebarModel::project({archived, entry("a", 0, 0)}, o).rows.isEmpty());
    }
    void workspaceKeepsPinnedDuplicatesAndDistinctBasenames() {
        SidebarProjectionOptions o;
        o.mode = "workspace";
        o.pinnedIds = {"pinned"};
        const auto p = SidebarModel::project({entry("pinned", 20, 20, "C:/one/project"),
                                              entry("other", 10, 10, "D:/two/project"), entry("loose", 1, 1)},
                                             o);
        QCOMPARE(agents(p).count("pinned"), 2);
        QCOMPARE(headers(p), QStringList({"No workspace", "project", "project"}));
        QCOMPARE(p.workspaceKeys, QStringList({"dir:C:/one/project", "dir:D:/two/project"}));
    }
    void manualWorkspaceOrderAndNewGroups() {
        SidebarProjectionOptions o;
        o.mode = "workspace";
        o.workspaceOrder = {"dir:B:/b", "dir:A:/a", "stale"};
        const auto p = SidebarModel::project(
            {entry("a", 3, 3, "A:/a"), entry("b", 2, 2, "B:/b"), entry("c", 1, 1, "C:/c")}, o);
        QCOMPARE(p.workspaceKeys, QStringList({"dir:C:/c", "dir:B:/b", "dir:A:/a"}));
    }
    void collapseNeverHidesGlobalPin() {
        SidebarProjectionOptions o;
        o.mode = "workspace";
        o.pinnedIds = {"p"};
        o.collapsedKeys = {"dir:C:/repo"};
        const auto p = SidebarModel::project({entry("p", 10, 10, "C:/repo")}, o);
        QCOMPARE(agents(p), QStringList({"p"}));
        QVERIFY(p.rows.last().collapsed);
    }
    void gitRootThenWorktreeAgeAndEmptyWorktrees() {
        SidebarProjectionOptions o;
        o.mode = "workspace";
        auto a = entry("a", 10, 10);
        SidebarMount root;
        root.path = "C:/repo";
        root.git = true;
        root.repositoryId = "r";
        root.mainPath = root.path;
        root.branch = "main";
        a.mounts = {root};
        auto b = entry("b", 9, 9);
        auto work = root;
        work.path = "C:/work/older";
        work.worktree = true;
        work.branch = "feature";
        b.mounts = {work};
        auto recent = work;
        recent.path = "C:/work/newer";
        recent.branch = "new";
        recent.createdAt = 20;
        work.createdAt = 10;
        o.repositoryWorktrees["r"] = {recent, work, root};
        const auto p = SidebarModel::project({b, a}, o);
        QCOMPARE(headers(p), QStringList({"repo", "Root (main)", "newer (new)", "older (feature)"}));
        QCOMPARE(agents(p), QStringList({"a", "b"}));
    }
};
QTEST_GUILESS_MAIN(SidebarModelTest)
#include "test_sidebar_model.moc"
