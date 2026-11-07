#include "ClaudeStream.h"
#include <QJsonDocument>
#include <QtTest>
using namespace ade;
namespace {
QJsonObject event(QJsonObject value, QString parent = {}) {
  return {{"type", "stream_event"},
          {"parent_tool_use_id", parent},
          {"event", value}};
}
QJsonObject start(QString message, QString parent = {}) {
  return event(
      {{"type", "message_start"}, {"message", QJsonObject{{"id", message}}}},
      parent);
}
QJsonObject block(int index, QJsonObject value, QString parent = {}) {
  return event({{"type", "content_block_start"},
                {"index", index},
                {"content_block", value}},
               parent);
}
QJsonObject stop(int index, QString parent = {}) {
  return event({{"type", "content_block_stop"}, {"index", index}}, parent);
}
QJsonObject final(QString message, QJsonArray content, QString uuid = {},
                  QString parent = {}) {
  QJsonObject value{
      {"type", "assistant"},
      {"parent_tool_use_id", parent},
      {"message", QJsonObject{{"id", message}, {"content", content}}}};
  if (!uuid.isEmpty())
    value["uuid"] = uuid;
  return value;
}
QJsonObject text(QString value) { return {{"type", "text"}, {"text", value}}; }
QJsonObject thought(QString value) {
  return {{"type", "thinking"},
          {"thinking", value},
          {"signature", "SYNTHETIC_SIGNATURE_NOT_DISPLAY"}};
}
QJsonObject tool(QString id, QString path) {
  return {{"type", "tool_use"},
          {"id", id},
          {"name", "Read"},
          {"input", QJsonObject{{"file_path", path}}}};
}
QJsonObject result(QString id, QJsonValue content) {
  return {{"type", "user"},
          {"message", QJsonObject{{"content", QJsonArray{QJsonObject{
                                                  {"type", "tool_result"},
                                                  {"tool_use_id", id},
                                                  {"content", content}}}}}}};
}
QStringList snapshotTexts(const ClaudeStream &parser) {
  QStringList result;
  for (const auto &value : parser.snapshot())
    if (value.toObject()["type"] == "textSnapshot")
      result.append(value.toObject()["text"].toString());
  return result;
}
} // namespace
class ClaudeFollowupsStreamTest : public QObject {
  Q_OBJECT
private slots:
  void contradictionsAndAmbiguousFramesReject_data() {
    QTest::addColumn<QString>("kind");
    for (const auto *kind :
         {"changed-uuid", "changed-tool-name", "foreign-tool-owner",
          "contradictory-type", "ambiguous-no-uuid", "ambiguous-new-uuid",
          "ambiguous-identical", "ambiguous-replay-new-uuid"})
      QTest::newRow(kind) << QString(kind);
  }
  void contradictionsAndAmbiguousFramesReject() {
    QFETCH(QString, kind);
    ClaudeStream parser;
    parser.reset("turn");
    QJsonArray updates;
    QString error;
    QVERIFY(parser.consume(start("message"), &updates, &error));
    if (kind == "changed-tool-name" || kind == "foreign-tool-owner") {
      const auto item = tool("owned-tool", "file");
      QVERIFY(parser.consume(block(0, item), &updates, &error));
      auto changed = item;
      if (kind == "changed-tool-name")
        changed["name"] = "Write";
      QVERIFY(!parser.consume(
          final(kind == "foreign-tool-owner" ? "other" : "message", {changed},
                "event", kind == "foreign-tool-owner" ? "child" : QString()),
          &updates, &error));
    } else {
      QVERIFY(parser.consume(block(0, text("first")), &updates, &error));
      if (kind == "ambiguous-new-uuid" || kind == "ambiguous-identical") {
        QVERIFY(parser.consume(
            block(1, text(kind == "ambiguous-identical" ? "first" : "second")),
            &updates, &error));
        QVERIFY(!parser.consume(
            final("message",
                  {text(kind == "ambiguous-identical" ? "first"
                                                      : "second corrected")},
                  "new-final"),
            &updates, &error));
      } else if (kind == "contradictory-type") {
        QVERIFY(!parser.consume(
            final("message", {thought("different type")}, "event"), &updates,
            &error));
      } else {
        QVERIFY(parser.consume(final("message", {text("first")}, "event"),
                               &updates, &error));
        if (kind == "ambiguous-no-uuid" ||
            kind == "ambiguous-replay-new-uuid") {
          QVERIFY(parser.consume(
              block(1, text(kind == "ambiguous-no-uuid" ? "first" : "second")),
              &updates, &error));
          QVERIFY(!parser.consume(final("message", {text("first")},
                                        kind == "ambiguous-no-uuid"
                                            ? QString{}
                                            : QString("new-final")),
                                  &updates, &error));
        } else
          QVERIFY(!parser.consume(final("message", {text("changed")}, "event"),
                                  &updates, &error));
      }
    }
    QVERIFY(!error.isEmpty());
  }
  void outOfOrderToolFinalsMatchExactIds() {
    ClaudeStream parser;
    parser.reset("turn");
    QJsonArray updates;
    QString error;
    QVERIFY(parser.consume(start("message"), &updates, &error));
    const auto first = tool("tool-a", "a"), second = tool("tool-b", "b");
    QVERIFY(parser.consume(block(0, first), &updates, &error));
    QVERIFY(parser.consume(block(1, second), &updates, &error));
    QVERIFY(
        parser.consume(final("message", {second}, "second"), &updates, &error));
    QVERIFY(
        parser.consume(final("message", {first}, "first"), &updates, &error));
    QVERIFY(
        parser.consume(final("message", {first, second}, "complete-snapshot"),
                       &updates, &error));
    QCOMPARE(parser.snapshot().size(), 2);
    QCOMPARE(parser.snapshot()[0].toObject()["itemId"].toString(),
             QString("tool/tool-a"));
    QCOMPARE(parser.snapshot()[1].toObject()["itemId"].toString(),
             QString("tool/tool-b"));
  }
  void assistantOnlySplitBlocksAndReset() {
    ClaudeStream parser;
    parser.reset("turn-a");
    QJsonArray updates;
    QString error;
    QVERIFY(parser.consume(final("same", {text("first")}, "first"), &updates,
                           &error));
    QVERIFY(parser.consume(final("same", {text("second")}, "second"), &updates,
                           &error));
    const auto count = updates.size();
    QVERIFY(parser.consume(final("same", {text("first")}, "first"), &updates,
                           &error));
    QCOMPARE(updates.size(), count);
    QCOMPARE(snapshotTexts(parser), (QStringList{"first", "second"}));
    parser.reset("turn-b");
    updates = {};
    QVERIFY(parser.consume(final("same", {text("replacement")}, "first"),
                           &updates, &error));
    QCOMPARE(snapshotTexts(parser), QStringList{"replacement"});
    QCOMPARE(parser.snapshot().first().toObject()["turnId"].toString(),
             QString("turn-b"));
  }
  void trailingNewlineIsNotAnImageMimeLabel() {
    ClaudeStream parser;
    parser.reset("turn");
    QJsonArray updates;
    QString error;
    QVERIFY(parser.consume(final("tool", {tool("image", "input")}), &updates,
                           &error));
    const QJsonObject image{
        {"type", "image"},
        {"source", QJsonObject{{"media_type", "image/png\n"}}}};
    QVERIFY(
        parser.consume(result("image", QJsonArray{image}), &updates, &error));
    const auto bytes = QJsonDocument(parser.snapshot()).toJson();
    QVERIFY(bytes.contains("preview unavailable"));
    QVERIFY(!bytes.contains("image/png"));
  }
  void imageMetadataAndBudgetsStayBounded() {
    ClaudeStream parser;
    parser.reset("turn");
    QJsonArray updates;
    QString error;
    QVERIFY(parser.consume(final("tool", {tool("image", "input")}), &updates,
                           &error));
    const QJsonObject malicious{
        {"type", "image"},
        {"source", QJsonObject{{"media_type", "image/png\nUNTRUSTED_LABEL"},
                               {"url", "https://untrusted.invalid/private"},
                               {"data", "SYNTHETIC_SECRET_BYTES"}}}};
    QVERIFY(parser.consume(result("image", QJsonArray{malicious}), &updates,
                           &error));
    const auto bytes = QJsonDocument(parser.snapshot()).toJson();
    QVERIFY(bytes.contains("preview unavailable"));
    QVERIFY(!bytes.contains("UNTRUSTED_LABEL"));
    QVERIFY(!bytes.contains("untrusted.invalid"));
    QVERIFY(!bytes.contains("SYNTHETIC_SECRET_BYTES"));
    parser.reset("turn-b");
    updates = {};
    QVERIFY(parser.consume(final("tool", {tool("image", "input")}), &updates,
                           &error));
    QJsonArray flood;
    for (int index = 0; index < 20001; ++index)
      flood.append(QJsonObject{{"type", "image"}});
    QVERIFY(!parser.consume(result("image", flood), &updates, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(QJsonDocument(parser.snapshot()).toJson().size() < 2048);
  }
  void recordedShapeSeparateThinkingAndTextFinals() {
    ClaudeStream parser;
    parser.reset("synthetic-turn");
    QJsonArray updates;
    QString error;
    QVERIFY(parser.consume(start("message"), &updates, &error));
    QVERIFY(parser.consume(block(0, thought("SYNTHETIC_REASONING")), &updates,
                           &error));
    QVERIFY(parser.consume(stop(0), &updates, &error));
    QVERIFY(parser.consume(
        final("message", {thought("SYNTHETIC_REASONING")}, "final-0"), &updates,
        &error));
    QVERIFY(parser.consume(block(1, text("useful reply")), &updates, &error));
    QVERIFY(parser.consume(stop(1), &updates, &error));
    QVERIFY2(parser.consume(final("message", {text("useful reply")}, "final-1"),
                            &updates, &error),
             qPrintable(error));
    QCOMPARE(snapshotTexts(parser), QStringList{"useful reply"});
    QVERIFY(!QJsonDocument(parser.snapshot())
                 .toJson()
                 .contains("SYNTHETIC_SIGNATURE_NOT_DISPLAY"));
  }
  void recordedShapeMultipleToolsKeepTheirIds() {
    ClaudeStream parser;
    parser.reset("turn");
    QJsonArray updates;
    QString error;
    QVERIFY(parser.consume(start("message"), &updates, &error));
    QVERIFY(parser.consume(block(0, thought("synthetic")), &updates, &error));
    QVERIFY(parser.consume(stop(0), &updates, &error));
    QVERIFY(parser.consume(final("message", {thought("synthetic")}, "a"),
                           &updates, &error));
    for (int index = 1; index < 4; ++index) {
      const auto item = tool("tool-" + QString::number(index),
                             "file-" + QString::number(index));
      QVERIFY(parser.consume(block(index, item), &updates, &error));
      QVERIFY(parser.consume(stop(index), &updates, &error));
      QVERIFY2(parser.consume(
                   final("message", {item}, "event-" + QString::number(index)),
                   &updates, &error),
               qPrintable(error));
      QVERIFY(parser.consume(result("tool-" + QString::number(index),
                                    "output-" + QString::number(index)),
                             &updates, &error));
    }
    QSet<QString> ids;
    for (const auto &value : parser.snapshot())
      if (value.toObject()["type"] == "tool") {
        ids.insert(value.toObject()["itemId"].toString());
        QCOMPARE(value.toObject()["state"].toString(),
                 QString("output-available"));
      }
    QCOMPARE(ids, (QSet<QString>{"tool/tool-1", "tool/tool-2", "tool/tool-3"}));
  }
  void sameTypeBlocksAndReplayRemainDistinct_data() {
    QTest::addColumn<bool>("identical");
    QTest::newRow("different") << false;
    QTest::newRow("identical") << true;
  }
  void sameTypeBlocksAndReplayRemainDistinct() {
    QFETCH(bool, identical);
    ClaudeStream parser;
    parser.reset("turn");
    QJsonArray updates;
    QString error;
    QVERIFY(parser.consume(start("message"), &updates, &error));
    const auto first = text("first"),
               second = text(identical ? "first" : "second");
    QVERIFY(parser.consume(block(0, first), &updates, &error));
    QVERIFY(parser.consume(stop(0), &updates, &error));
    const auto a = final("message", {first}, "event-a");
    QVERIFY(parser.consume(a, &updates, &error));
    QVERIFY(parser.consume(block(1, second), &updates, &error));
    QVERIFY(parser.consume(stop(1), &updates, &error));
    const auto beforeReplay = updates.size();
    QVERIFY(parser.consume(a, &updates, &error));
    QCOMPARE(updates.size(), beforeReplay);
    const auto b = final("message", {second}, "event-b");
    QVERIFY2(parser.consume(b, &updates, &error), qPrintable(error));
    const auto after = updates.size();
    QVERIFY(parser.consume(b, &updates, &error));
    QCOMPARE(updates.size(), after);
    QCOMPARE(snapshotTexts(parser),
             (QStringList{"first", identical ? "first" : "second"}));
  }
  void interleavedParentMessageIdsNeverMerge() {
    ClaudeStream parser;
    parser.reset("turn");
    QJsonArray updates;
    QString error;
    QVERIFY(parser.consume(start("same"), &updates, &error));
    QVERIFY(parser.consume(block(0, text("root one")), &updates, &error));
    QVERIFY(parser.consume(start("same", "child"), &updates, &error));
    QVERIFY(
        parser.consume(block(0, text("child one"), "child"), &updates, &error));
    QVERIFY(
        parser.consume(final("same", {text("child one")}, "child-a", "child"),
                       &updates, &error));
    QVERIFY(parser.consume(final("same", {text("root one")}, "root-a"),
                           &updates, &error));
    QVERIFY(
        parser.consume(block(1, text("child two"), "child"), &updates, &error));
    QVERIFY(parser.consume(block(1, text("root two")), &updates, &error));
    QVERIFY2(parser.consume(final("same", {text("root two")}, "root-b"),
                            &updates, &error),
             qPrintable(error));
    QVERIFY2(
        parser.consume(final("same", {text("child two")}, "child-b", "child"),
                       &updates, &error),
        qPrintable(error));
    QCOMPARE(snapshotTexts(parser),
             (QStringList{"root one", "child one", "child two", "root two"}));
  }
  void imageToolResultDoesNotAbortUsefulText() {
    ClaudeStream parser;
    parser.reset("turn");
    QJsonArray updates;
    QString error;
    QVERIFY(parser.consume(
        final("tool-message", {tool("image-read", "fixture.png")}), &updates,
        &error));
    const QJsonObject image{
        {"type", "image"},
        {"source", QJsonObject{{"type", "base64"},
                               {"media_type", "image/png"},
                               {"data", "SYNTHETIC_IMAGE_BYTES_NOT_DISPLAY"}}}};
    QVERIFY2(parser.consume(result("image-read",
                                   QJsonArray{text("useful tool text"), image}),
                            &updates, &error),
             qPrintable(error));
    QVERIFY(parser.consume(final("answer", {text("useful answer after image")}),
                           &updates, &error));
    const auto bytes = QJsonDocument(parser.snapshot()).toJson();
    QVERIFY(bytes.contains("useful tool text"));
    QVERIFY(bytes.contains("image/png"));
    QVERIFY(bytes.contains("useful answer after image"));
    QVERIFY(!bytes.contains("SYNTHETIC_IMAGE_BYTES_NOT_DISPLAY"));
    QCOMPARE(parser.snapshot()[0].toObject()["state"].toString(),
             QString("output-available"));
  }
};
QTEST_GUILESS_MAIN(ClaudeFollowupsStreamTest)
#include "test_claude_followups_stream.moc"
