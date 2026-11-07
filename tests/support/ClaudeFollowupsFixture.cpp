#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <cstdio>
#include <iostream>
namespace {
void send(QJsonObject row) {
  const auto bytes = QJsonDocument(row).toJson(QJsonDocument::Compact) + '\n';
  for (qsizetype i = 0; i < bytes.size(); i += 7) {
    const auto part = bytes.mid(i, 7);
    fwrite(part.data(), 1, size_t(part.size()), stdout);
    fflush(stdout);
  }
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto mode = qEnvironmentVariable("ADE_CLAUDE_FOLLOWUPS");
  QString session;
  for (const auto &a : app.arguments())
    if (a.startsWith("--session-id=") || a.startsWith("--resume="))
      session = a.mid(a.indexOf('=') + 1);
  int turn = 0;
  std::string line;
  while (std::getline(std::cin, line)) {
    const auto row =
        QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
    if (row.value("type") == "control_request") {
      const QJsonObject reply{{"subtype", "success"},
                              {"request_id", row.value("request_id")},
                              {"response", QJsonObject{}}};
      send({{"type", "control_response"}, {"response", reply}});
      if (row.value("request").toObject().value("subtype") == "interrupt")
        send({{"type", "result"},
              {"subtype", "success"},
              {"session_id", session},
              {"result", "LATE_CANCEL_RESULT"}});
      continue;
    }
    if (row.value("type") != "user")
      continue;
    ++turn;
    const QString message = "message-" + QString::number(turn);
    auto init = QJsonObject{
        {"type", "system"}, {"subtype", "init"}, {"session_id", session}};
    if (mode == "telemetry-init" || mode == "telemetry-precedence")
      init["model"] = "cli-init-model";
    send(init);
    auto stream = [&](QJsonObject value, QString parent = QString{}) {
      send({{"type", "stream_event"},
            {"session_id", session},
            {"parent_tool_use_id", parent},
            {"event", value}});
    };
    auto begin = [&](QString parent = QString{}) {
      stream({{"type", "message_start"},
              {"message", QJsonObject{{"id", message}}}},
             parent);
    };
    auto block = [&](int index, QJsonObject value) {
      stream({{"type", "content_block_start"},
              {"index", index},
              {"content_block", value}});
      stream({{"type", "content_block_stop"}, {"index", index}});
    };
    auto final = [&](QJsonObject value, QString suffix,
                     QString model = QString{}, QString parent = QString{},
                     QString owner = QString{}) {
      auto m = QJsonObject{{"id", message}, {"content", QJsonArray{value}}};
      if (!model.isEmpty())
        m["model"] = model;
      send({{"type", "assistant"},
            {"session_id", owner.isEmpty() ? session : owner},
            {"parent_tool_use_id", parent},
            {"uuid", message + suffix},
            {"message", m}});
    };
    auto text = [](QString t) {
      return QJsonObject{{"type", "text"}, {"text", t}};
    };
    auto textBlock = [&](int index, QString value) {
      stream({{"type", "content_block_start"},
              {"index", index},
              {"content_block", text("")}});
      stream({{"type", "content_block_delta"},
              {"index", index},
              {"delta", QJsonObject{{"type", "text_delta"}, {"text", value}}}});
      stream({{"type", "content_block_stop"}, {"index", index}});
    };
    begin();
    if (mode == "active-budget") {
      stream({{"type", "content_block_start"},
              {"index", 0},
              {"content_block", text("")}});
      for (int i = 0; i < 40; ++i) {
        stream(
            {{"type", "content_block_delta"},
             {"index", 0},
             {"delta", QJsonObject{{"type", "text_delta"}, {"text", "tick"}}}});
        QThread::msleep(75);
      }
    } else if (mode == "split-final") {
      auto thought = QJsonObject{{"type", "thinking"},
                                 {"thinking", "SYNTHETIC_REASONING"},
                                 {"signature", "SYNTHETIC_SIGNATURE"}};
      block(0, thought);
      final(thought, "-thought");
      for (int i = 1; i < 3; ++i) {
        auto tool = QJsonObject{
            {"type", "tool_use"},
            {"id", "tool-" + QString::number(i)},
            {"name", "Read"},
            {"input",
             QJsonObject{{"file_path", "fixture-" + QString::number(i)}}}};
        block(i, tool);
        final(tool, "-tool" + QString::number(i));
      }
      textBlock(3, "USEFUL_COMPLETION");
      final(text("USEFUL_COMPLETION"), "-text");
    } else if (mode == "image-result") {
      auto tool =
          QJsonObject{{"type", "tool_use"},
                      {"id", "image-tool"},
                      {"name", "Read"},
                      {"input", QJsonObject{{"file_path", "synthetic.png"}}}};
      block(0, tool);
      final(tool, "-tool");
      const QJsonObject source{{"type", "base64"},
                               {"media_type", "image/png"},
                               {"data", "SYNTHETIC_IMAGE_BYTES"}};
      const QJsonArray output{
          text("USEFUL_TOOL_TEXT"),
          QJsonObject{{"type", "image"}, {"source", source}}};
      const QJsonObject result{{"type", "tool_result"},
                               {"tool_use_id", "image-tool"},
                               {"content", output}};
      send({{"type", "user"},
            {"session_id", session},
            {"message", QJsonObject{{"content", QJsonArray{result}}}}});
      textBlock(1, "USEFUL_COMPLETION");
      final(text("USEFUL_COMPLETION"), "-text");
    } else {
      if (mode == "telemetry-child")
        final(text("CHILD"), "-child", "child-model", "child-tool");
      if (mode == "telemetry-foreign")
        final(text("FOREIGN"), "-foreign", "foreign-model", {},
              "00000000-0000-0000-0000-000000000001");
      QString model;
      if (mode == "telemetry-assistant" || mode == "telemetry-precedence" ||
          mode == "telemetry-conflict" ||
          (mode == "telemetry-reset" && turn == 1))
        model = "claude-fixture-concrete";
      if (mode == "telemetry-invalid")
        model = "model\nUNTRUSTED";
      if (mode == "telemetry-trailing-newline")
        model = "model\n";
      textBlock(0, "USEFUL_COMPLETION");
      final(text("USEFUL_COMPLETION"), "-text", model);
      if (mode == "telemetry-conflict") {
        block(1, text("SECOND"));
        final(text("SECOND"), "-second", "different-root-model");
      }
    }
    send({{"type", "result"},
          {"subtype", "success"},
          {"session_id", session},
          {"result", "USEFUL_COMPLETION"}});
  }
}
