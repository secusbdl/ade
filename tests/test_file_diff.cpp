#include "core/AgentEngine.h"
#include "core/ProviderProtocol.h"
#include "mutations/FileMutationDiff.h"
#include "mutations/FileMutationJournal.h"
#include "ui/ConversationView.h"
#include "ui/FileDiffView.h"
#include <QFile>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class FileDiffTest : public QObject {
  Q_OBJECT
private slots:
  void journalDiffIsDisplayOnly_data() {
    QTest::addColumn<QString>("protocol");
    for (const auto *value : {"openai", "responses", "anthropic", "google"})
      QTest::newRow(value) << QString(value);
  }
  void journalDiffIsDisplayOnly() {
    QFETCH(QString, protocol);
    QTemporaryDir data, workspace;
    QFile file(workspace.filePath("file.txt"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("UI_ONLY_OLD_TEXT\n");
    file.close();
    FileMutationJournal journal(data.path(), "chat");
    QString error;
    QVERIFY(journal.bindMount("ws1", workspace.path(), true, &error));
    auto result =
        journal.executor()({"call",
                            "write",
                            workspace.path(),
                            {},
                            {{"path", "file.txt"}, {"content", "new text\n"}}},
                           nullptr);
    QVERIFY2(!result.error, qPrintable(result.output));
    const auto diff = result.metadata["fileDiff"].toObject();
    QVERIFY(validMutationDiff(diff));
    QCOMPARE(diff["files"].toArray().size(), 1);
    QVERIFY(!result.output.contains("UI_ONLY_OLD_TEXT"));
    const QJsonObject call{
        {"id", "call"},
        {"type", "function"},
        {"function", QJsonObject{{"name", "write"}, {"arguments", "{}"}}}};
    const QJsonArray history{
        QJsonObject{{"role", "user"}, {"content", "Write fixture"}},
        QJsonObject{{"role", "assistant"},
                    {"content", ""},
                    {"tool_calls", QJsonArray{call}}},
        QJsonObject{{"role", "tool"},
                    {"tool_call_id", "call"},
                    {"content", result.output},
                    {"metadata", result.metadata}}};
    auto rows = AgentEngine::displayHistory(history);
    QCOMPARE(rows.last()
                 .toObject()["parts"]
                 .toArray()
                 .last()
                 .toObject()["fileDiff"]
                 .toObject(),
             diff);
    ProviderConfig provider{"fixture", "Fixture", "http://127.0.0.1:1/v1",
                            "fixture", "model",   protocol};
    const auto wire =
        QJsonDocument(ProviderProtocol::prepare(provider, history, {}, {}).body)
            .toJson();
    QVERIFY(!wire.contains("UI_ONLY_OLD_TEXT"));
    QVERIFY(!wire.contains("fileDiff"));
    ConversationView view;
    view.resize(350, 500);
    view.setConversation("chat", rows, false);
    auto *preview = view.findChild<QPlainTextEdit *>("fileDiffText");
    QVERIFY(preview);
    QVERIFY(preview->toPlainText().contains("-UI_ONLY_OLD_TEXT"));
    QVERIFY(preview->toPlainText().contains("+new text"));
    QVERIFY(preview->isReadOnly());
    auto cursor = preview->textCursor();
    cursor.setPosition(0);
    cursor.setPosition(4, QTextCursor::KeepAnchor);
    preview->setTextCursor(cursor);
    view.setConversation("chat", rows, true);
    QCOMPARE(preview->textCursor().selectedText(), "@@ -");
  }
  void creationDeletionAndInvalidBytesAreDistinct() {
    const auto metadata = mutationDiffMetadata(
        {{"created", {}, "", false, true},
         {"deleted", "old", {}, true, false},
         {"binary", QByteArray::fromHex("00ff"), "x", true, true},
         {"truncatedUtf8", QByteArray::fromHex("e282"), "x", true, true},
         {"tooLarge", QByteArray(600000, 'a'), "x", true, true}});
    QVERIFY(validMutationDiff(metadata));
    QCOMPARE(metadata["files"].toArray().size(), 2);
    QCOMPARE(metadata["omitted"].toInt(), 3);
    const auto created = metadata["files"].toArray()[0].toObject();
    QVERIFY(created["before"].isNull());
    QVERIFY(created["after"].isString());
    FileDiffView view;
    view.setDiff(metadata);
    QCOMPARE(view.findChildren<QPlainTextEdit*>().first()->toPlainText(),QString("Empty file created."));
  }
  void lineDiffIsMinimalAndBounded() {
    const auto preview = FileDiffView::unifiedPreview("a\nb\nc\n", "a\nB\nc\n");
    QVERIFY(preview.contains("-b\n+B"));
    QVERIFY(!preview.contains("-a"));
    QVERIFY(!preview.contains("+c"));
    QVERIFY(FileDiffView::unifiedPreview("a", "a\n")
                .contains("Final newline added"));
    const auto large = FileDiffView::unifiedPreview(
        QString("a\n").repeated(3000), QString("b\n").repeated(3000));
    QVERIFY(large.count('\n') <= 1201);
    QVERIFY(large.contains("truncated"));
  }
  void malformedImportedDiffDoesNotRender() {
    FileDiffView view;
    auto diff = mutationDiffMetadata({{"x", "before", "after", true, true}});
    view.setDiff(diff);
    QCOMPARE(view.findChildren<QPlainTextEdit *>().size(), 1);
    diff["files"] =
        QJsonArray{QJsonObject{{"path", "x"},
                               {"before", QString(128 * 1024 + 1, 'a')},
                               {"after", ""}}};
    view.setDiff(diff);
    QVERIFY(view.isHidden());
    QVERIFY(view.findChildren<QPlainTextEdit *>().isEmpty());
  }
};
QTEST_MAIN(FileDiffTest)
#include "test_file_diff.moc"
