#include "core/QueuedMessages.h"
#include "ui/AppFonts.h"
#include "ui/MarkdownParts.h"
#include "ui/MessageQueuePanel.h"
#include "ui/QuestionPanel.h"
#include <QCheckBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
QJsonObject pending() {
  QJsonArray fields{
      QJsonObject{{"questionId", "name"},
                  {"label", "**Project name**"},
                  {"type", "input"},
                  {"required", true},
                  {"minLength", 2}},
      QJsonObject{{"questionId", "mode"},
                  {"label", "Mode"},
                  {"type", "radio-group"},
                  {"options", QJsonArray{QJsonObject{{"value", "fast"},
                                                     {"label", "Fast"}}}},
                  {"defaultValue", "fast"}},
      QJsonObject{{"questionId", "private"},
                  {"label", "Private project"},
                  {"type", "checkbox"},
                  {"defaultValue", true}},
      QJsonObject{{"questionId", "password"},
                  {"label", "Secret fixture"},
                  {"type", "input"},
                  {"inputType", "password"}},
      QJsonObject{
          {"questionId", "targets"},
          {"label", "Targets"},
          {"type", "checkbox-group"},
          {"options",
           QJsonArray{QJsonObject{{"value", "desktop"}, {"label", "Desktop"}},
                      QJsonObject{{"value", "mobile"}, {"label", "Mobile"}}}}}};
  return {{"id", "phase-token"},
          {"title", "Project details"},
          {"description", "Choose how the project should start."},
          {"currentStep", 0},
          {"answers", QJsonObject{}},
          {"steps", QJsonArray{QJsonObject{{"fields", fields}}}}};
}
} // namespace
class QuestionPanelTest : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    const auto fonts = AppFonts::load();
    QVERIFY(fonts.fullyLoaded());
    QFont font(fonts.body);
    font.setPixelSize(14);
    QApplication::setFont(font);
  }
  void validatesExplicitSubmissionAndCollectsTypedValues() {
    QuestionPanel panel;
    panel.resize(500, 500);
    panel.setQuestion("chat", pending());
    panel.show();
    QSignalSpy submitted(&panel, &QuestionPanel::submitStep),
        draft(&panel, &QuestionPanel::draftChanged);
    auto *submit = panel.findChild<QPushButton *>("questionSubmit");
    QVERIFY(submit);
    submit->click();
    QVERIFY(submitted.isEmpty());
    auto *name = panel.findChild<QLineEdit *>("answer_name");
    QVERIFY(name);
    name->setText("Desktop app");
    QVERIFY(!draft.isEmpty());
    QCOMPARE(draft.last()[0].toString(), "chat");
    panel.findChild<QRadioButton *>("answer_mode_other")->click();
    panel.findChild<QLineEdit *>("answer_mode_otherText")
        ->setText("Custom mode");
    panel.findChild<QCheckBox *>("answer_targets_desktop")->setChecked(true);
    QCOMPARE(panel.findChild<QLineEdit *>("answer_password")->echoMode(),
             QLineEdit::Password);
    submit->click();
    QCOMPARE(submitted.size(), 1);
    const auto answer = submitted[0][3].toJsonObject();
    QCOMPARE(answer["name"], "Desktop app");
    QCOMPARE(answer["mode"], "__other__:Custom mode");
    QCOMPARE(answer["private"], true);
    QCOMPARE(answer["targets"], QJsonArray{"desktop"});
    QCOMPARE(submitted[0][1].toString(), "phase-token");
  }
  void cancellationPreservesCurrentDraftAndContext() {
    QuestionPanel panel;
    panel.setQuestion("first", pending());
    QSignalSpy cancelled(&panel, &QuestionPanel::cancelled);
    panel.findChild<QLineEdit *>("answer_name")->setText("unfinished");
    panel.findChild<QPushButton *>("questionCancel")->click();
    QCOMPARE(cancelled.size(), 1);
    QCOMPARE(cancelled[0][0].toString(), "first");
    QCOMPARE(cancelled[0][2].toJsonObject()["name"], "unfinished");
    auto next = pending();
    next["id"] = "new-token";
    panel.setQuestion("second", next);
    panel.setError("first", "phase-token", "stale error");
    QCOMPARE(panel.token(), "new-token");
    QCOMPARE(panel.draftAnswers()["name"], "");
    panel.setQuestion("second", {});
    QVERIFY(panel.isHidden());
  }
  void callbackMayDeletePanel() {
    auto *panel = new QuestionPanel;
    panel->setQuestion("chat", pending());
    QPointer<QuestionPanel> guard(panel);
    connect(panel, &QuestionPanel::cancelled, panel, [panel] { delete panel; });
    panel->findChild<QPushButton *>("questionCancel")->click();
    QVERIFY(!guard);
  }
  void queueControlsRetainScopedIds() {
    MessageQueuePanel panel;
    const QJsonArray queue{QueuedMessages::create("first"),
                           QueuedMessages::create("second")};
    const auto id = queue[1].toObject()["id"].toString();
    panel.setQueue("chat", queue, false);
    QSignalSpy edited(&panel, &MessageQueuePanel::editRequested),
        moved(&panel, &MessageQueuePanel::moveRequested),
        sent(&panel, &MessageQueuePanel::sendNowRequested),
        removed(&panel, &MessageQueuePanel::removeRequested),
        resumed(&panel, &MessageQueuePanel::resumeRequested);
    panel.findChild<QPushButton *>("queueEdit_" + id)->click();
    panel.findChild<QPlainTextEdit *>("queueText_" + id)
        ->setPlainText("changed\nmultiline");
    panel.findChild<QPushButton *>("queueSave_" + id)->click();
    QCOMPARE(edited[0][0].toString(), "chat");
    QCOMPARE(edited[0][1].toString(), id);
    QCOMPARE(edited[0][2].toString(), "changed\nmultiline");
    panel.findChild<QPushButton *>("queueUp_" + id)->click();
    QCOMPARE(moved[0][2].toInt(), 0);
    panel.findChild<QPushButton *>("queueSend_" + id)->click();
    QCOMPARE(sent[0][1].toString(), id);
    panel.findChild<QPushButton *>("queueRemove_" + id)->click();
    QCOMPARE(removed[0][1].toString(), id);
    panel.findChild<QPushButton *>("queueResume")->click();
    QCOMPARE(resumed[0][0].toString(), "chat");
  }
  void nativeFormRendersAllControls() {
    QuestionPanel panel;
    panel.setQuestion("render", pending());
    panel.resize(520, 530);
    panel.show();
    QTest::qWait(30);
    const auto image = panel.grab().toImage();
    QVERIFY(!image.isNull());
    QVERIFY(image.width() >= 500);
    QVERIFY(panel.findChild<QPushButton *>("questionSubmit")->isVisible());
    QVERIFY(panel.findChild<QLineEdit *>("answer_name")->isVisible());
    const auto path = qEnvironmentVariable("ADE_QUESTION_RENDER");
    if (!path.isEmpty())
      QVERIFY(image.save(path));
  }
  void markdownCannotLoadLocalImageBytes() {
    QTemporaryDir dir;
    QImage sentinel(31, 29, QImage::Format_ARGB32);
    sentinel.fill(QColor(251, 1, 249));
    const auto path = dir.filePath("secret.png");
    QVERIFY(sentinel.save(path));
    auto question = pending();
    question["description"] =
        "![fixture](" + QUrl::fromLocalFile(path).toString() + ")";
    QuestionPanel panel;
    panel.setQuestion("chat", question);
    panel.resize(500, 530);
    panel.show();
    QTest::qWait(30);
    bool inspected = false;
    for (auto *text : panel.findChildren<TranscriptText *>())
      if (text->source().contains("secret.png")) {
        inspected = true;
        const auto image = panel.grab().toImage();
        int matching = 0;
        for (int y = 0; y < image.height(); ++y)
          for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y) == QColor(251, 1, 249))
              ++matching;
        QCOMPARE(matching, 0);
        QVERIFY(!text->document()
                     ->resource(QTextDocument::ImageResource,
                                QUrl::fromLocalFile(path))
                     .isValid());
      }
    QVERIFY(inspected);
  }
};
QTEST_MAIN(QuestionPanelTest)
#include "test_question_panel.moc"
