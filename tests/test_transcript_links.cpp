#include "links/TranscriptLinkResolver.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
using Resolver = TranscriptLinkResolver;
namespace {
bool put(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool junction(const QString& path, const QString& target) {
    return QProcess::execute("cmd", {"/c", "mklink", "/J", QDir::toNativeSeparators(path),
                                     QDir::toNativeSeparators(target)}) == 0;
}
struct Fixture {
    QTemporaryDir data, workspace;
    AttachmentStore store{data.path()};
    FileReadSnapshotStore snapshots{data.path()};
    FileReadSnapshotStore::Lease lease = snapshots.acquire("chat-a");
    AttachmentRef attachment = store.write("chat-a", "Original name.txt", "ATT_A");
    TranscriptLinkOptions options;
    Fixture() {
        options.agentId = "chat-a";
        options.dataDirectory = data.path();
        options.lease = lease;
        options.mounts = {{"ws1", workspace.path()}, {"w1", workspace.path()}};
        options.browserSessionId = "browser-now";
        options.liveTabIds = {"tab-1"};
    }
    Resolver::Scope scope(QString* error = nullptr) {
        return Resolver::create(options, error);
    }
};
} // namespace
class TranscriptLinksTest : public QObject {
    Q_OBJECT
  private slots:
    void parsesPinnedForms_data() {
        QTest::addColumn<QString>("href");
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("line");
        QTest::newRow("path") << QString("path:ws1/src/a%20b.ts:42?display=expanded&flag")
                              << QString("ws1/src/a b.ts") << QString("42");
        QTest::newRow("att-alias") << QString("att:blob.txt") << QString("att/blob.txt") << QString();
        QTest::newRow("workspace-alias")
            << QString("workspace:ws1/a.txt") << QString("ws1/a.txt") << QString();
        QTest::newRow("file-alias") << QString("wsfile:ws1/a.txt:0") << QString("ws1/a.txt") << QString("0");
        QTest::newRow("mention-alias")
            << QString("mention:file:ws1/a.txt") << QString("ws1/a.txt") << QString();
        QTest::newRow("unicode") << QString("path:ws1/%E6%97%A5.txt") << QString::fromUtf8("ws1/日.txt")
                                 << QString();
        QTest::newRow("incomplete") << QString("path:incomplete:ws1/a.txt") << QString("ws1/a.txt")
                                    << QString();
    }
    void parsesPinnedForms() {
        QFETCH(QString, href);
        QFETCH(QString, path);
        QFETCH(QString, line);
        QString error;
        const auto value = Resolver::parse(href, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(value.kind, Resolver::Kind::File);
        QCOMPARE(value.path, path);
        QCOMPARE(value.lineNumber, line);
        if (href.contains("display=")) {
            QCOMPARE(value.params["display"].toString(), "expanded");
            QCOMPARE(value.params["flag"].toString(), "true");
        }
    }
    void malformedLinks_data() {
        QTest::addColumn<QString>("href");
        const QStringList values = {"file:///C:/secret",   "javascript:alert(1)",
                                    "data:text/plain,x",   "shell:missing",
                                    "path:C:/secret",      "path:/absolute",
                                    "path:ws1/../outside", "path:ws1/%2e%2e/outside",
                                    "path:ws1/%5coutside", "path:ws1/a%00.txt",
                                    "path:ws1/%ff",        "path:ws1/%xz",
                                    "path:ws1/a:",         "path:ws1/a:stream",
                                    "path:ws1//a",         "path:ws1/CON",
                                    "path:ws1/a. ",        "path:att/other/name.txt",
                                    "path:att/a b.txt",    "tab:",
                                    "color:%00",           "https://user:password@example.test/"};
        int i = 0;
        for (const auto& value : values)
            QTest::newRow(qPrintable(QString::number(i++))) << value;
    }
    void malformedLinks() {
        QFETCH(QString, href);
        QString error;
        QCOMPARE(Resolver::parse(href, &error).kind, Resolver::Kind::Invalid);
        QVERIFY(!error.isEmpty());
    }
    void malformedUnicodeAndRelativeHostMountAreRejected() {
        QString error;
        QCOMPARE(Resolver::parse(QString("path:ws1/") + QChar(0xd800), &error).kind, Resolver::Kind::Invalid);
        Fixture f;
        f.options.mounts["ws1"] = "relative-root";
        QVERIFY(!f.scope(&error).isCurrent("chat-a"));
        QVERIFY(error.contains("absolute"));
    }
    void requiresKnownMessageWorkspaceBinding() {
        Fixture f;
        QVERIFY(put(f.workspace.filePath("a.txt"), "OWNED"));
        QString error;
        const auto scope = f.scope(&error);
        QVERIFY2(scope.isCurrent("chat-a"), qPrintable(error));
        QCOMPARE(Resolver::resolve(scope, "chat-a", "path:ws1/a.txt", {}, nullptr, &error).kind,
                 Resolver::Kind::Invalid);
        QVERIFY(error.contains("binding"));
        TranscriptLinkProvenance provenance;
        provenance.mountBindings = scope.mountBindings();
        auto opened = Resolver::resolve(scope, "chat-a", "path:ws1/a.txt:5?display=expanded", provenance,
                                        nullptr, &error);
        QCOMPARE(opened.kind, Resolver::Kind::File);
        QCOMPARE(opened.bytes, QByteArray("OWNED"));
        QVERIFY(!opened.readOnly);
        QVERIFY(opened.isCurrent("chat-a"));
        QCOMPARE(opened.lineNumber, "5");
        QTemporaryDir other;
        QVERIFY(put(other.filePath("a.txt"), "UNRELATED"));
        auto options = f.options;
        options.mounts["ws1"] = other.path();
        const auto changed = Resolver::create(options, &error);
        QCOMPARE(Resolver::resolve(changed, "chat-a", "path:ws1/a.txt", provenance, nullptr, &error).kind,
                 Resolver::Kind::Invalid);
        QVERIFY(error.contains("binding"));
    }
    void attachmentsUseOnlyFixedChatAndAreReadOnly() {
        Fixture f;
        const auto otherRoot = f.store.agentDirectory("chat-b", nullptr, true);
        const auto blob = f.attachment.path.mid(4);
        QVERIFY(put(otherRoot + '/' + blob, "ATT_B"));
        QString error;
        const auto scope = f.scope(&error);
        TranscriptLinkProvenance p;
        p.attachmentNames[f.attachment.path] = "Original name.txt";
        const auto opened =
            Resolver::resolve(scope, "chat-a", "path:" + f.attachment.path, p, nullptr, &error);
        QCOMPARE(opened.kind, Resolver::Kind::File);
        QCOMPARE(opened.bytes, QByteArray("ATT_A"));
        QVERIFY(opened.readOnly);
        QCOMPARE(opened.displayName, "Original name.txt");
        QCOMPARE(Resolver::resolve(scope, "chat-b", "path:" + f.attachment.path, p, nullptr, &error).kind,
                 Resolver::Kind::Invalid);
        auto options = f.options;
        options.agentId = "chat-b";
        QVERIFY(!Resolver::create(options, &error).isCurrent("chat-b"));
    }
    void tabAndColorRouting() {
        Fixture f;
        const auto scope = f.scope();
        QString error;
        TranscriptLinkProvenance p;
        p.browserSessionId = "browser-now";
        p.historicalTabUrls["tab-1"] = QUrl("https://original.example.test/a");
        auto action = Resolver::resolve(scope, "chat-a", "tab:tab-1", p, nullptr, &error);
        QCOMPARE(action.kind, Resolver::Kind::ActivateTab);
        QCOMPARE(action.tabId, "tab-1");
        p.browserSessionId = "browser-before";
        action = Resolver::resolve(scope, "chat-a", "tab:tab-1", p, nullptr, &error);
        QCOMPARE(action.kind, Resolver::Kind::ExternalUrl);
        QCOMPARE(action.url, QUrl("https://original.example.test/a"));
        p.historicalTabUrls.clear();
        QCOMPARE(Resolver::resolve(scope, "chat-a", "tab:tab-1", p, nullptr, &error).kind,
                 Resolver::Kind::Invalid);
        p.historicalTabUrls["tab-1"] = QUrl("file:///C:/secret");
        QCOMPARE(Resolver::resolve(scope, "chat-a", "tab:tab-1", p, nullptr, &error).kind,
                 Resolver::Kind::Invalid);
        action = Resolver::resolve(scope, "chat-a", "color:rgba%280%2C%2012%2C%2034%2C%20.5%29", {}, nullptr,
                                   &error);
        QCOMPARE(action.kind, Resolver::Kind::CopyColor);
        QCOMPARE(action.color, "rgba(0, 12, 34, .5)");
        QCOMPARE(Resolver::resolve(scope, "chat-a", "https://example.test/", {}, nullptr, &error).kind,
                 Resolver::Kind::ExternalUrl);
    }
    void revocationAndCancellation() {
        Fixture f;
        QString error;
        auto scope = f.scope();
        auto action = Resolver::resolve(scope, "chat-a", "path:" + f.attachment.path, {}, nullptr, &error);
        QVERIFY(action.isCurrent("chat-a"));
        const auto fresh = f.scope();
        scope.revoke();
        QVERIFY(!action.isCurrent("chat-a"));
        QVERIFY(fresh.isCurrent("chat-a"));
        QCOMPARE(Resolver::resolve(scope, "chat-a", "color:red", {}, nullptr, &error).kind,
                 Resolver::Kind::Invalid);
        std::atomic_bool canceled = true;
        QCOMPARE(Resolver::resolve(fresh, "chat-a", "path:" + f.attachment.path, {}, &canceled, &error).kind,
                 Resolver::Kind::Invalid);
        FileReadSnapshotStore::revoke(f.lease);
        QVERIFY(!fresh.isCurrent("chat-a"));
        auto reactivated = f.snapshots.acquire("chat-a");
        QVERIFY(reactivated.belongsTo("chat-a"));
        QVERIFY(!fresh.isCurrent("chat-a"));
    }
    void boundsBinaryAndDirectorySemantics() {
        Fixture f;
        const QByteArray binary = QByteArray::fromHex("000102ff504b0304");
        const auto blob = f.store.write("chat-a", "archive.zip", binary);
        QString error;
        const auto scope = f.scope();
        auto action = Resolver::resolve(scope, "chat-a", "path:" + blob.path, {}, nullptr, &error);
        QCOMPARE(action.bytes, binary);
        QCOMPARE(action.mimeType, "application/zip");
        TranscriptLinkProvenance p;
        p.mountBindings = scope.mountBindings();
        QCOMPARE(Resolver::resolve(scope, "chat-a", "path:ws1", p, nullptr, &error).kind,
                 Resolver::Kind::NoOp);
        QCOMPARE(Resolver::resolve(scope, "chat-a", "path:ws1/folder/", p, nullptr, &error).kind,
                 Resolver::Kind::NoOp);
        action = {};
        auto options = f.options;
        options.maxFileBytes = 3;
        const auto bounded = Resolver::create(options, &error);
        QCOMPARE(Resolver::resolve(bounded, "chat-a", "path:" + blob.path, {}, nullptr, &error).kind,
                 Resolver::Kind::Invalid);
        QVERIFY(error.contains("limit"));
    }
    void rejectsReplacedRootAndChildJunction() {
        Fixture f;
        QTemporaryDir outside;
        QVERIFY(put(outside.filePath("secret.txt"), "OUTSIDE"));
        QString error;
        const auto scope = f.scope();
        TranscriptLinkProvenance p;
        p.mountBindings = scope.mountBindings();
        QVERIFY(junction(f.workspace.filePath("alias"), outside.path()));
        QCOMPARE(Resolver::resolve(scope, "chat-a", "path:ws1/alias/secret.txt", p, nullptr, &error).kind,
                 Resolver::Kind::Invalid);
        const auto originalRoot = f.store.agentDirectory("chat-a");
        QVERIFY(QDir().rename(originalRoot, originalRoot + "-old"));
        QVERIFY(QDir().mkdir(originalRoot));
        QVERIFY(put(originalRoot + '/' + f.attachment.path.mid(4), "REPLACEMENT"));
        QCOMPARE(Resolver::resolve(scope, "chat-a", "path:" + f.attachment.path, {}, nullptr, &error).kind,
                 Resolver::Kind::Invalid);
        QVERIFY(error.contains("replaced"));
    }
    void fileHandleLoanPinsUntilEditorLoadFinishes() {
        Fixture f;
        QVERIFY(put(f.workspace.filePath("file.txt"), "INITIAL"));
        QString error;
        const auto scope = f.scope();
        TranscriptLinkProvenance p;
        p.mountBindings = scope.mountBindings();
        auto action = Resolver::resolve(scope, "chat-a", "path:ws1/file.txt", p, nullptr, &error);
        QCOMPARE(action.kind, Resolver::Kind::File);
        QVERIFY(!QFile::remove(action.canonicalPath));
        QFile reader(action.canonicalPath);
        QVERIFY(reader.open(QIODevice::ReadOnly));
        QCOMPARE(reader.readAll(), action.bytes);
        reader.close();
        action = {};
        QVERIFY(QFile::remove(f.workspace.filePath("file.txt")));
    }
};
QTEST_GUILESS_MAIN(TranscriptLinksTest)
#include "test_transcript_links.moc"
