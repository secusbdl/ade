#include "ClaudeStream.h"
#include <QJsonDocument>
#include <QtTest>
using namespace ade;
namespace {
QJsonObject stream(const QJsonObject &event, const QString &parent = {}) {
  return {{"type", "stream_event"},
          {"parent_tool_use_id", parent},
          {"event", event}};
}
QJsonObject start(const QString &id, const QString &parent = {}) {
  return stream(
      {{"type", "message_start"}, {"message", QJsonObject{{"id", id}}}},
      parent);
}
QJsonObject block(int index, const QJsonObject &value,
                  const QString &parent = {}) {
  return stream({{"type", "content_block_start"},
                 {"index", index},
                 {"content_block", value}},
                parent);
}
QJsonObject delta(int index, const QJsonObject &value,
                  const QString &parent = {}) {
  return stream(
      {{"type", "content_block_delta"}, {"index", index}, {"delta", value}},
      parent);
}
QJsonObject final(const QString &id, const QJsonArray &content,
                  const QString &parent = {}) {
  return {{"type", "assistant"},
          {"parent_tool_use_id", parent},
          {"message", QJsonObject{{"id", id}, {"content", content}}}};
}
QJsonObject result(const QString &id, const QJsonValue &content,
                   bool error = false, const QString &parent = {}) {
  return {
      {"type", "user"},
      {"parent_tool_use_id", parent},
      {"message",
       QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "tool_result"},
                                                      {"tool_use_id", id},
                                                      {"content", content},
                                                      {"is_error", error}}}}}}};
}
} // namespace
class ClaudeStreamTest : public QObject {
  Q_OBJECT
private slots:
  void streamedReasoningTextToolAndFinalSnapshotAreOrderedOnce() {
    ClaudeStream parser;
    parser.reset("turn-one");
    QJsonArray output;
    QString error;
    auto accept = [&](const QJsonObject &event) {
      return parser.consume(event, &output, &error);
    };
    QVERIFY(accept(start("m1")));
    QVERIFY(accept(block(0, {{"type", "thinking"}, {"thinking", ""}})));
    QVERIFY(
        accept(delta(0, {{"type", "thinking_delta"}, {"thinking", "Reason"}})));
    QVERIFY(accept(delta(0, {{"type", "signature_delta"},
                             {"signature", "SIGNED_PRIVATE_MARKER"}})));
    QVERIFY(accept(block(1, {{"type", "text"}, {"text", ""}})));
    QVERIFY(accept(delta(1, {{"type", "text_delta"}, {"text", "Hello 😀"}})));
    QVERIFY(accept(block(2, {{"type", "tool_use"},
                             {"id", "t1"},
                             {"name", "Read"},
                             {"input", QJsonObject{}}})));
    QVERIFY(accept(delta(
        2, {{"type", "input_json_delta"}, {"partial_json", "{\"file_"}})));
    QVERIFY(accept(delta(2, {{"type", "input_json_delta"},
                             {"partial_json", "path\":\"a.txt\"}"}})));
    QVERIFY(accept(stream({{"type", "content_block_stop"}, {"index", 2}})));
    const auto before = output.size();
    QVERIFY(accept(final(
        "m1", {QJsonObject{{"type", "thinking"},
                           {"thinking", "Reason"},
                           {"signature", "SIGNED_PRIVATE_MARKER"}},
               QJsonObject{{"type", "text"}, {"text", "Hello 😀"}},
               QJsonObject{{"type", "tool_use"},
                           {"id", "t1"},
                           {"name", "Read"},
                           {"input", QJsonObject{{"file_path", "a.txt"}}}}})));
    // Final snapshot closes text/reasoning without re-emitting their contents
    // or restarting the tool.
    QCOMPARE(output.size() - before, 2);
    QVERIFY(accept(result("t1", "FILE_CONTENT")));
    const auto completedCount = output.size();
    QVERIFY(accept(result("t1", "FILE_CONTENT")));
    QCOMPARE(output.size(), completedCount);
    QString text, reasoning;
    QStringList order;
    for (const auto &value : output) {
      auto item = value.toObject();
      QCOMPARE(item.value("turnId").toString(), QString("turn-one"));
      if (item.value("type") == "text")
        text += item.value("text").toString();
      if (item.value("type") == "reasoning")
        reasoning += item.value("text").toString();
      const auto id = item.value("itemId").toString();
      if (!order.contains(id))
        order.append(id);
    }
    QCOMPARE(text, QString::fromUtf8("Hello 😀"));
    QCOMPARE(reasoning, QString("Reason"));
    QCOMPARE(order.size(), 3);
    QCOMPARE(output.last().toObject().value("state").toString(),
             QString("output-available"));
    QCOMPARE(output.last()
                 .toObject()
                 .value("arguments")
                 .toObject()
                 .value("file_path")
                 .toString(),
             QString("a.txt"));
    QVERIFY(!QJsonDocument(output).toJson().contains("SIGNED_PRIVATE_MARKER"));
    QVERIFY(parser.finish("completed").isEmpty());
  }
  void assistantOnlyAndCorrectedSnapshotUseSameStablePart() {
    ClaudeStream parser;
    parser.reset("turn");
    QJsonArray output;
    QString error;
    QVERIFY(parser.consume(start("message"), &output, &error));
    QVERIFY(parser.consume(block(0, {{"type", "text"}, {"text", "par"}}),
                           &output, &error));
    const auto id = output.last().toObject().value("itemId");
    QVERIFY(parser.consume(
        final("message", {QJsonObject{{"type", "text"}, {"text", "complete"}}}),
        &output, &error));
    QCOMPARE(output[output.size() - 2].toObject().value("type").toString(),
             QString("textSnapshot"));
    QCOMPARE(output[output.size() - 2].toObject().value("itemId"), id);
    QCOMPARE(output[output.size() - 2].toObject().value("text").toString(),
             QString("complete"));
    const auto size = output.size();
    QVERIFY(parser.consume(
        final("second",
              {QJsonObject{{"type", "text"}, {"text", "second round"}}}),
        &output, &error));
    QCOMPARE(output.size() - size, 3);
    QVERIFY(output.last().toObject().value("itemId") != id);
  }
  void parentChannelsAndPrefixMessageNamesRemainSeparate() {
    ClaudeStream parser;
    parser.reset("turn");
    QJsonArray output;
    QString error;
    QVERIFY(parser.consume(start("a"), &output, &error));
    QVERIFY(parser.consume(block(0, {{"type", "text"}, {"text", "first"}}),
                           &output, &error));
    QVERIFY(parser.consume(start("a:0", "child"), &output, &error));
    QVERIFY(
        parser.consume(block(0, {{"type", "text"}, {"text", "child"}}, "child"),
                       &output, &error));
    QVERIFY(
        parser.consume(stream({{"type", "message_stop"}}), &output, &error));
    QVERIFY(parser.consume(
        delta(0, {{"type", "text_delta"}, {"text", " continues"}}, "child"),
        &output, &error));
    QVERIFY(output[1].toObject().value("itemId") !=
            output[3].toObject().value("itemId"));
    QCOMPARE(output.last().toObject().value("text").toString(),
             QString(" continues"));
  }
  void hiddenInternalToolsRemainAssociatedWithoutDisplay() {
    ClaudeStream parser;
    parser.reset("turn");
    QJsonArray output;
    QString error;
    QVERIFY(
        parser.consume(final("hidden", {QJsonObject{{"type", "tool_use"},
                                                    {"id", "internal"},
                                                    {"name", "EnterPlanMode"},
                                                    {"input", QJsonObject{}}}}),
                       &output, &error));
    QVERIFY(
        parser.consume(result("internal", "hidden result"), &output, &error));
    QVERIFY(output.isEmpty());
  }
  void childTextCannotSuppressRootResultFallback() {
    ClaudeStream parser;
    parser.reset("turn");
    QJsonArray output;
    QString error;
    QVERIFY(parser.consume(
        final("child-message",
              {QJsonObject{{"type", "text"}, {"text", "CHILD_ONLY"}}},
              "subagent"),
        &output, &error));
    QVERIFY(parser.consume(
        {{"type", "result"}, {"subtype", "success"}, {"result", "ROOT_RESULT"}},
        &output, &error));
    const auto snapshot = parser.snapshot();
    QStringList texts;
    for (const auto &value : snapshot)
      if (value.toObject().value("type") == "textSnapshot")
        texts.append(value.toObject().value("text").toString());
    QCOMPARE(texts, (QStringList{"CHILD_ONLY", "ROOT_RESULT"}));
  }
  void interruptedReasoningAndToolAreClosedWithoutInventedOutput() {
    ClaudeStream parser;
    parser.reset("turn");
    QJsonArray output;
    QString error;
    QVERIFY(parser.consume(start("m"), &output, &error));
    QVERIFY(parser.consume(
        block(0, {{"type", "thinking"}, {"thinking", "partial"}}), &output,
        &error));
    QVERIFY(parser.consume(block(1, {{"type", "tool_use"},
                                     {"id", "t"},
                                     {"name", "Read"},
                                     {"input", QJsonObject{}}}),
                           &output, &error));
    const auto closed = parser.finish("interrupted");
    QCOMPARE(closed.size(), 2);
    QCOMPARE(closed[0].toObject().value("state").toString(),
             QString("canceled"));
    QCOMPARE(closed[1].toObject().value("state").toString(),
             QString("canceled"));
    QVERIFY(parser.finish("interrupted").isEmpty());
  }
  void malformedAndUnsupportedFrames_data() {
    QTest::addColumn<QString>("kind");
    for (const auto *kind :
         {"missing-message", "unknown-index", "changed-type", "duplicate-tool",
          "orphan-result", "generated-image", "result-document", "bad-json",
          "changed-result", "empty-part-cap", "text-cap"})
      QTest::newRow(kind) << QString(kind);
  }
  void malformedAndUnsupportedFrames() {
    QFETCH(QString, kind);
    ClaudeStream parser;
    parser.reset("turn");
    QJsonArray output;
    QString error;
    if (kind == "missing-message") {
      QVERIFY(!parser.consume(
          delta(0, {{"type", "text_delta"}, {"text", "unowned"}}), &output,
          &error));
      return;
    }
    QVERIFY(parser.consume(start("m"), &output, &error));
    bool accepted = true;
    if (kind == "unknown-index")
      accepted = parser.consume(
          delta(0, {{"type", "text_delta"}, {"text", "unowned"}}), &output,
          &error);
    else if (kind == "orphan-result")
      accepted = parser.consume(result("unknown", "secret"), &output, &error);
    else if (kind == "generated-image")
      accepted = parser.consume(
          final("m",
                {QJsonObject{{"type", "image"}, {"source", QJsonObject{}}}}),
          &output, &error);
    else if (kind == "changed-type") {
      QVERIFY(parser.consume(block(0, {{"type", "text"}, {"text", ""}}),
                             &output, &error));
      accepted = parser.consume(
          final("m",
                {QJsonObject{{"type", "thinking"}, {"thinking", "different"}}}),
          &output, &error);
    } else if (kind == "text-cap") {
      QVERIFY(parser.consume(block(0, {{"type", "text"}, {"text", ""}}),
                             &output, &error));
      accepted = parser.consume(
          delta(0, {{"type", "text_delta"},
                    {"text", QString(4 * 1024 * 1024 + 1, 'x')}}),
          &output, &error);
    } else if (kind == "empty-part-cap") {
      for (int i = 0; i < 20000; ++i) {
        QVERIFY(parser.consume(block(i, {{"type", "text"}, {"text", ""}}),
                               &output, &error));
        output = {};
      }
      QVERIFY(parser.consume(start("next"), &output, &error));
      accepted = parser.consume(block(0, {{"type", "text"}, {"text", ""}}),
                                &output, &error);
    } else {
      QVERIFY(parser.consume(block(0, {{"type", "tool_use"},
                                       {"id", "t"},
                                       {"name", "Read"},
                                       {"input", QJsonObject{}}}),
                             &output, &error));
      if (kind == "duplicate-tool")
        accepted = parser.consume(block(1, {{"type", "tool_use"},
                                            {"id", "t"},
                                            {"name", "Read"},
                                            {"input", QJsonObject{}}}),
                                  &output, &error);
      else if (kind == "result-document")
        accepted = parser.consume(
            result("t", QJsonArray{QJsonObject{{"type", "document"}}}), &output,
            &error);
      else if (kind == "changed-result") {
        QVERIFY(parser.consume(result("t", "original"), &output, &error));
        accepted = parser.consume(result("t", "changed"), &output, &error);
      } else if (kind == "bad-json") {
        QVERIFY(parser.consume(
            delta(0, {{"type", "input_json_delta"}, {"partial_json", "{"}}),
            &output, &error));
        accepted = parser.consume(
            stream({{"type", "content_block_stop"}, {"index", 0}}), &output,
            &error);
      }
    }
    QVERIFY(!accepted);
    QVERIFY(!error.isEmpty());
  }
};
QTEST_GUILESS_MAIN(ClaudeStreamTest)
#include "test_claude_stream.moc"
