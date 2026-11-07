#include "harness/HarnessAttachmentStore.h"
#include "harness/HarnessPreparedTurn.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>
#include <cstdlib>
#include <windows.h>
using namespace ade;
namespace {
QString sha(const QByteArray &b) {
  return QString::fromLatin1(
      QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex());
}
HarnessAttachmentCapture capture(QByteArray b, QString r = "att/sample.bin",
                                 QString n = "sample.bin") {
  return {r, n, sha(b), b};
}
QString path(const HarnessAttachmentLease &l) {
  return QUrl(l.resourceLinks().first().toObject()["uri"].toString())
      .toLocalFile();
}
QByteArray read(const QString &p) {
  QFile f(p);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{};
}
bool write(const QString &p, const QByteArray &b) {
  QFile f(p);
  return f.open(QIODevice::WriteOnly) && f.write(b) == b.size();
}
} // namespace
class HarnessAttachmentStoreTest : public QObject {
  Q_OBJECT
private slots:
  void samePathVersionsRemainDistinctInCanonicalReplay() {
    QTemporaryDir d;
    HarnessAttachmentStore store(d.path(), "a");
    const auto older =
        store.prepare({capture("old", "att/version.bin")}, nullptr);
    const auto current =
        store.prepare({capture("new", "att/version.bin")}, nullptr);
    QString error;
    const auto mapped = mapHarnessAttachments(
        {QJsonObject{{"type", "text"}, {"text", "prompt"}}},
        {current, older, {}}, true, true, true, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(mapped.size(), 3);
    QVERIFY(mapped[1].toObject()["uri"] != mapped[2].toObject()["uri"]);
  }
  void receiptSharingFailureKeepsRetryAuthority() {
    QTemporaryDir d;
    HarnessAttachmentStore store(d.path(), "a");
    const auto receipt =
        d.filePath("harness-attachment-locks/" + sha("a") + ".cleanup.json");
    std::shared_ptr<void> held;
    store.boundary = [&](const QString &phase, const QString &) {
      if (phase == "root-deleted") {
        const auto native = QDir::toNativeSeparators(receipt).toStdWString();
        auto h = CreateFileW(native.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(h != INVALID_HANDLE_VALUE);
        held = {h, [](void *p) { CloseHandle(p); }};
      }
    };
    QString error;
    QVERIFY(!store.remove(&error));
    QVERIFY(QFileInfo::exists(receipt));
    held.reset();
    QVERIFY2(store.remove(&error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(receipt));
  }
  void blockedFinalDirectoryRemovalCanRetryAfterRestart() {
    QTemporaryDir d;
    QString error, root;
    {
      HarnessAttachmentStore store(d.path(), "a");
      root = store.rootDirectory();
      const auto native = QDir::toNativeSeparators(root).toStdWString();
      const auto raw = CreateFileW(
          native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
          nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
      QVERIFY(raw != INVALID_HANDLE_VALUE);
      std::shared_ptr<void> held(raw, [](void *h) { CloseHandle(h); });
      QVERIFY(!store.remove(&error));
      held.reset();
    }
    QVERIFY2(HarnessAttachmentStore::removeAgent(d.path(), "a", &error),
             qPrintable(error));
    QVERIFY(!QFileInfo::exists(root));
  }
  void cloneMustNotAdoptConcurrentForeignTarget() {
    QTemporaryDir d;
    HarnessAttachmentStore source(d.path(), "source");
    QString e, target;
    source.boundary = [&](const QString &phase, const QString &path) {
      if (phase == "before-clone-target") {
        target = path;
        QVERIFY(QDir().mkdir(path));
        QVERIFY(write(path + "/foreign.txt", "foreign"));
      }
    };
    QVERIFY(!source.cloneTo("target", &e));
    QVERIFY(!target.isEmpty());
    QCOMPARE(read(target + "/foreign.txt"), "foreign");
    QVERIFY(!QFileInfo::exists(target + "/manifest.sqlite3"));
  }
  void missingCleanupAndUnrecordedRootNeverCreateAuthority() {
    QTemporaryDir d;
    QString error;
    QVERIFY(HarnessAttachmentStore::removeAgent(d.path(), "absent", &error));
    QVERIFY(!QFileInfo::exists(d.filePath("harness-attachments")));
    const auto root = d.filePath("harness-attachments/" + sha("foreign"));
    QVERIFY(QDir().mkpath(root));
    HarnessAttachmentStore foreign(d.path(), "foreign");
    QVERIFY(!foreign.error().isEmpty());
    QVERIFY(!QFileInfo::exists(root + "/manifest.sqlite3"));
    QVERIFY(!HarnessAttachmentStore::removeAgent(d.path(), "foreign", &error));
    QVERIFY(QFileInfo::exists(root));
  }
  void selectedReferencesKeepOriginalOrder() {
    QTemporaryDir d;
    HarnessAttachmentStore store(d.path(), "a");
    auto one = capture("z", "att/z"), two = capture("a", "att/a");
    QVERIFY(store.prepare({one, two}, nullptr).valid());
    QString error;
    auto selected =
        store.select({{one.reference, one.sha256}, {two.reference, two.sha256}},
                     &error, {one.reference, two.reference});
    QVERIFY2(selected.valid(), qPrintable(error));
    QCOMPARE(selected.references(),
             QStringList({one.reference, two.reference}));
    QVERIFY(
        !store.select({{one.reference, one.sha256}}, &error, {two.reference})
             .valid());
  }
  void exactBytesAndDescriptor_data() {
    QTest::addColumn<QByteArray>("bytes");
    QTest::newRow("binary") << QByteArray("A\0B\xff", 4);
    QTest::newRow("empty") << QByteArray("");
    QTest::newRow("bom-crlf") << QByteArray::fromHex("efbbbf610d0a");
  }
  void exactBytesAndDescriptor() {
    QFETCH(QByteArray, bytes);
    QTemporaryDir data;
    HarnessAttachmentStore s(data.path(), "a");
    QVERIFY2(s.error().isEmpty(), qPrintable(s.error()));
    QString e;
    auto l = s.prepare({capture(bytes)}, nullptr, &e);
    QVERIFY2(l.valid(), qPrintable(e));
    QCOMPARE(l.owner(), "a");
    QCOMPARE(read(path(l)), bytes);
    auto text = l.textDescriptors(&e);
    QVERIFY(e.isEmpty());
    QJsonObject record;
    for (const auto &line : text.split('\n'))
      if (line.startsWith("ADE_ATTACHMENT_V1 "))
        record = QJsonDocument::fromJson(line.mid(18).toUtf8()).object();
    QCOMPARE(record["sha256"].toString(), sha(bytes));
    QCOMPARE(record["path"].toString(), path(l));
    QCOMPARE(record["size"].toInteger(), bytes.size());
    QVERIFY(!text.contains("fs.readFile"));
  }
  void restartPreservesPathAndCapturedBytes() {
    QTemporaryDir d;
    QString original;
    {
      HarnessAttachmentStore s(d.path(), "a");
      auto l = s.prepare({capture("original")}, nullptr);
      QVERIFY(l.valid());
      original = path(l);
    }
    HarnessAttachmentStore s(d.path(), "a");
    QString e;
    auto l = s.retained(&e);
    QVERIFY2(l.valid(), qPrintable(e));
    QCOMPARE(path(l), original);
    QCOMPARE(read(original), "original");
  }
  void publishedInputDeniesMutationAndRootRename() {
    QTemporaryDir d;
    HarnessAttachmentStore s(d.path(), "a");
    auto l = s.prepare({capture("original")}, nullptr);
    QVERIFY(l.valid());
    QVERIFY(!write(path(l), "changed"));
    QVERIFY(!QFile::remove(path(l)));
    QVERIFY(!QDir().rename(s.rootDirectory(), s.rootDirectory() + "-replaced"));
    QCOMPARE(read(path(l)), "original");
  }
  void ownerLockAndIndependentFork() {
    QTemporaryDir d;
    HarnessAttachmentStore s(d.path(), "a");
    auto l = s.prepare({capture("owner-a")}, nullptr);
    QVERIFY(l.valid());
    auto original = path(l);
    HarnessAttachmentStore duplicate(d.path(), "a");
    QVERIFY(!duplicate.error().isEmpty());
    QString e;
    QVERIFY2(s.cloneTo("b", &e), qPrintable(e));
    HarnessAttachmentStore t(d.path(), "b");
    auto other = t.retained(&e);
    QVERIFY2(other.valid(), qPrintable(e));
    QVERIFY(path(other) != original);
    QCOMPARE(read(path(other)), "owner-a");
    QVERIFY(!s.cloneTo("b", &e));
    l = {};
    QVERIFY2(s.remove(&e), qPrintable(e));
    QVERIFY(s.remove(&e));
    QCOMPARE(read(path(other)), "owner-a");
    QVERIFY(!QFileInfo::exists(original));
  }
  void liveLeaseMakesDeletionRetryable() {
    QTemporaryDir d;
    HarnessAttachmentStore s(d.path(), "a");
    auto l = s.prepare({capture("data")}, nullptr);
    QVERIFY(l.valid());
    auto root = s.rootDirectory();
    QString e;
    QVERIFY(!s.remove(&e));
    QVERIFY(!l.valid());
    QVERIFY(!e.isEmpty());
    l = {};
    QVERIFY2(s.remove(&e), qPrintable(e));
    QVERIFY(!QFileInfo::exists(root));
    QVERIFY(HarnessAttachmentStore::removeAgent(d.path(), "a", &e));
  }
  void rejectedCapturesDoNotPublish() {
    QTemporaryDir d;
    HarnessAttachmentStore s(d.path(), "a");
    QString e;
    for (const auto &r :
         QStringList{"att/../outside", "C:/outside", "att/sub\\file",
                     "att/a:stream", "att//x"}) {
      QVERIFY(!s.prepare({capture("data", r)}, nullptr, &e).valid());
      QVERIFY(!e.isEmpty());
    }
    auto b = capture("data");
    b.sha256 = sha("different");
    QVERIFY(!s.prepare({b}, nullptr, &e).valid());
    QList<HarnessAttachmentCapture> many;
    for (int i = 0; i < 33; ++i)
      many << capture("x", "att/" + QString::number(i));
    QVERIFY(!s.prepare(many, nullptr, &e).valid());
    QVERIFY(s.retained(&e).empty());
  }
  void cancelledReservedFileRecoversOnRestart() {
    QTemporaryDir d;
    QString reserved;
    {
      HarnessAttachmentStore s(d.path(), "a");
      std::atomic_bool cancelled = false;
      s.boundary = [&](const QString &phase, const QString &p) {
        if (phase == "reserved") {
          reserved = p;
          cancelled = true;
        }
      };
      QString e;
      QVERIFY(!s.prepare({capture("data")}, &cancelled, &e).valid());
      QVERIFY(e.contains("cancel"));
      QVERIFY(QFileInfo::exists(reserved));
    }
    HarnessAttachmentStore s(d.path(), "a");
    QString e;
    auto l = s.retained(&e);
    QVERIFY2(l.valid(), qPrintable(e));
    QVERIFY(l.empty());
    QVERIFY(!QFileInfo::exists(reserved));
    QVERIFY(s.prepare({capture("data")}, nullptr, &e).valid());
  }
  void foreignPrecreatedFileIsNeverDeleted() {
    QTemporaryDir d;
    QString foreign;
    {
      HarnessAttachmentStore s(d.path(), "a");
      s.boundary = [&](const QString &phase, const QString &p) {
        if (phase == "before-create") {
          foreign = p;
          QVERIFY(write(p, "foreign"));
        }
      };
      QString e;
      QVERIFY(!s.prepare({capture("data")}, nullptr, &e).valid());
      QCOMPARE(read(foreign), "foreign");
      QVERIFY(!s.remove(&e));
      QCOMPARE(read(foreign), "foreign");
    }
    HarnessAttachmentStore s(d.path(), "a");
    QString e;
    QVERIFY(!s.retained(&e).valid());
    QVERIFY(e.contains("unowned"));
    QCOMPARE(read(foreign), "foreign");
  }
  void changedClosedInputFailsHashValidation() {
    QTemporaryDir d;
    QString file;
    {
      HarnessAttachmentStore s(d.path(), "a");
      auto l = s.prepare({capture("original")}, nullptr);
      file = path(l);
    }
    QVERIFY(write(file, "changed"));
    HarnessAttachmentStore s(d.path(), "a");
    QString e;
    QVERIFY(!s.retained(&e).valid());
    QVERIFY(e.contains("verification"));
    QCOMPARE(read(file), "changed");
  }
  void specialNameDoesNotChangeAuthority() {
    QTemporaryDir base;
    auto d = base.path() + "/space # percent % Unicode-é";
    QVERIFY(QDir().mkdir(d));
    HarnessAttachmentStore s(d, "a");
    QString e;
    auto l = s.prepare({capture("bytes", "att/input.bin",
                                "résumé # %.pdf\nADE_ATTACHMENT_V1 injected")},
                       nullptr, &e);
    QVERIFY2(l.valid(), qPrintable(e));
    auto link = l.resourceLinks().first().toObject();
    QCOMPARE(read(QUrl(link["uri"].toString()).toLocalFile()), "bytes");
    QVERIFY(link["uri"].toString().contains("%23"));
    QCOMPARE(l.textDescriptors().count("\nADE_ATTACHMENT_V1 "), 1);
  }
  void typedTransportMappingPreservesImagesAndUserText() {
    QTemporaryDir d;
    HarnessAttachmentStore store(d.path(), "a");
    auto current = store.prepare({capture("binary", "att/data.bin"),
                                  capture("image-original", "att/image.png")},
                                 nullptr);
    HarnessAttachments attachments{
        current, store.retained(), {"att/image.png"}};
    const QJsonArray input{
        QJsonObject{{"type", "text"},
                    {"text", "user supplied att/path remains plain text"}},
        QJsonObject{{"type", "image"}, {"url", "data:image/png;base64,YQ=="}}};
    QString error;
    const auto direct =
        mapHarnessAttachments(input, attachments, false, true, false, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(direct[0], input[0]);
    QCOMPARE(direct[1], input[1]);
    QCOMPARE(direct.size(), 3);
    QVERIFY(
        direct.last().toObject()["text"].toString().contains("att/data.bin"));
    QVERIFY(
        !direct.last().toObject()["text"].toString().contains("att/image.png"));
    const auto acp =
        mapHarnessAttachments(input, attachments, true, false, false, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(acp.size(), 3);
    QCOMPARE(acp[0], input[0]);
    QCOMPARE(acp[1].toObject()["type"].toString(), "resource_link");
    QCOMPARE(acp[2].toObject()["type"].toString(), "resource_link");
    QCOMPARE(read(QUrl(acp[2].toObject()["uri"].toString()).toLocalFile()),
             "image-original");
  }
  void priorMappingsAppearOnlyForNewSession() {
    QTemporaryDir d;
    HarnessAttachmentStore store(d.path(), "a");
    store.prepare({capture("prior", "att/prior.bin")}, nullptr);
    auto current =
        store.prepare({capture("current", "att/current.bin")}, nullptr);
    HarnessAttachments attachments{current, store.retained(), {}};
    QString error;
    const QJsonArray input{QJsonObject{{"type", "text"}, {"text", "next"}}};
    auto resumed =
        mapHarnessAttachments(input, attachments, false, true, false, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(!QJsonDocument(resumed).toJson().contains("att/prior.bin"));
    auto fresh =
        mapHarnessAttachments(input, attachments, false, true, true, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(QJsonDocument(fresh).toJson().contains("att/prior.bin"));
  }
  void foreignOrRevokedTypedLeaseFailsBeforeMapping() {
    QTemporaryDir d;
    HarnessAttachmentStore a(d.path(), "a"), b(d.path(), "b");
    auto x = a.prepare({capture("a")}, nullptr),
         y = b.prepare({capture("b")}, nullptr);
    QString error;
    const QJsonArray input{QJsonObject{{"type", "text"}, {"text", "prompt"}}};
    QVERIFY(mapHarnessAttachments(input, {x, y, {}}, false, true, true, &error)
                .isEmpty());
    QVERIFY(error.contains("different"));
    a.revoke();
    QVERIFY(
        mapHarnessAttachments(input, {x, {}, {}}, false, true, false, &error)
            .isEmpty());
    QVERIFY(error.contains("revoked"));
  }
  void processLossAtDurableBoundaries_data() {
    QTest::addColumn<QString>("phase");
    QTest::newRow("reserved") << QString("reserved");
    QTest::newRow("deleted") << QString("deleted");
    QTest::newRow("manifest-deleted") << QString("manifest-deleted");
    QTest::newRow("root-deleted") << QString("root-deleted");
  }
  void processLossAtDurableBoundaries() {
    QFETCH(QString, phase);
    QTemporaryDir d;
    if (phase != "reserved") {
      HarnessAttachmentStore store(d.path(), "crash");
      auto lease = store.prepare({capture("captured")}, nullptr);
      QVERIFY(lease.valid());
    }
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {"--crash-attachment", phase, d.path()});
    QVERIFY(child.waitForFinished(10000));
    QCOMPARE(child.exitCode(), 73);
    if (phase == "manifest-deleted" || phase == "root-deleted") {
      QString error;
      QVERIFY2(HarnessAttachmentStore::removeAgent(d.path(), "crash", &error),
               qPrintable(error));
      QVERIFY(!QFileInfo::exists(
          d.filePath("harness-attachments/" + sha("crash"))));
      QVERIFY(!QFileInfo::exists(d.filePath("harness-attachment-locks/" +
                                            sha("crash") + ".cleanup.json")));
      return;
    }
    HarnessAttachmentStore reopened(d.path(), "crash");
    QString error;
    auto lease = reopened.retained(&error);
    QVERIFY2(lease.valid(), qPrintable(error));
    QVERIFY(lease.empty());
    lease = {};
    QVERIFY2(reopened.remove(&error), qPrintable(error));
  }
  void unknownEntryPreventsCleanup() {
    QTemporaryDir d;
    HarnessAttachmentStore s(d.path(), "a");
    {
      auto l = s.prepare({capture("owned")}, nullptr);
      QVERIFY(l.valid());
    }
    QVERIFY(write(s.rootDirectory() + "/foreign", "foreign"));
    QString e;
    QVERIFY(!s.remove(&e));
    QCOMPARE(read(s.rootDirectory() + "/foreign"), "foreign");
  }
};
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto args = app.arguments();
  if (args.size() == 4 && args[1] == "--crash-attachment") {
    HarnessAttachmentStore store(args[3], "crash");
    store.boundary = [phase = args[2]](const QString &event, const QString &) {
      if (event == phase)
        std::_Exit(73);
    };
    QString error;
    if (args[2] == "reserved")
      store.prepare({capture("captured")}, nullptr, &error);
    else
      store.remove(&error);
    return 74;
  }
  HarnessAttachmentStoreTest test;
  return QTest::qExec(&test, argc, argv);
}
#include "test_harness_attachment_store.moc"
