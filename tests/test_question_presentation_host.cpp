#include "ui/MainWindow.h"
#include "ui/ConversationView.h"
#include "ui/MarkdownParts.h"
#include "ui/SessionStore.h"
#include "ui/AppStyle.h"
#include "attachments/AttachmentStore.h"
#include "editor/EditorWidget.h"
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
QJsonArray history(const QString& href){
 const QJsonObject form{{"title","Open a report"},{"steps",QJsonArray{QJsonObject{{"fields",QJsonArray{QJsonObject{{"questionId","q"},{"type","input"},{"label","[Report]("+href+")"}}}}}}}};
 const auto args=QString::fromUtf8(QJsonDocument(form).toJson(QJsonDocument::Compact));
 const QJsonObject part{{"id","question-part"},{"type","tool"},{"name","askUserQuestions"},{"toolCallId","question-call"},{"inputText",args},{"input",form},{"state","output-available"}};
 const QJsonObject display{{"version",1},{"id","question-row"},{"parts",QJsonArray{part}}};
 const QJsonObject call{{"id","question-call"},{"type","function"},{"function",QJsonObject{{"name","askUserQuestions"},{"arguments",args}}}};
 const auto answer=QString::fromUtf8(QJsonDocument(QJsonObject{{"answers",QJsonObject{{"q","Open"}}},{"cancelled",false}}).toJson(QJsonDocument::Compact));
 return {QJsonObject{{"role","assistant"},{"content",""},{"tool_calls",QJsonArray{call}},{"metadata",QJsonObject{{"display",display}}}},QJsonObject{{"role","tool"},{"tool_call_id","question-call"},{"content",answer}}};
}
bool select(MainWindow& window,const QString& id){auto* list=window.findChild<QListWidget*>("agentList");for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole)==id){list->setCurrentRow(i);return true;}return false;}
bool seed(const QString& data,const QString& href,const QString& workspace={}){QList<Conversation> chats;for(const auto& id:{"first","second"}){Conversation chat;chat.id=id;chat.title=id;chat.titleLocked=true;chat.workspace=workspace;chat.messages=history(href);chats.append(chat);}return SessionStore(data).save(chats);}
bool clickLabel(MainWindow& window,const QUrl& href){auto* view=window.findChild<ConversationView*>("conversationView");auto* label=view?view->findChild<TranscriptText*>("questionAnswerLabel"):nullptr;return label&&QMetaObject::invokeMethod(label,"anchorClicked",Q_ARG(QUrl,href));}
}
class QuestionPresentationHostTest:public QObject {
 Q_OBJECT
private slots:
 void initTestCase(){initializeAppStyle(*qApp);}
 void ownedQuestionLabelOpensExactOwnerAttachment(){QTemporaryDir data;AttachmentStore store(data.path());const auto ref=store.write("first","report.txt","OWNED QUESTION REPORT\n");QVERIFY(!ref.path.isEmpty());const auto foreign=store.agentDirectory("second",nullptr,true);QFile decoy(foreign+'/'+ref.path.mid(4));QVERIFY(decoy.open(QIODevice::WriteOnly));decoy.write("FOREIGN REPORT");decoy.close();QVERIFY(seed(data.path(),"path:"+ref.path));MainWindow window(data.path());QVERIFY(select(window,"first"));QVERIFY(clickLabel(window,QUrl("path:"+ref.path)));auto* text=window.findChild<QPlainTextEdit*>("transcriptAttachmentText");QVERIFY2(text,qPrintable(window.findChild<QLabel*>("sessionStatus")->text()));QVERIFY(text->isReadOnly());QCOMPARE(text->toPlainText(),QString("OWNED QUESTION REPORT\n"));QVERIFY(window.findChildren<EditorWidget*>().isEmpty());}
 void foreignBlobAndUnboundWorkspaceAreRejected(){QTemporaryDir data,workspace;AttachmentStore store(data.path());const auto foreign=store.write("second","foreign.txt","FOREIGN QUESTION REPORT");QVERIFY(!foreign.path.isEmpty());QFile file(workspace.filePath("untrusted.txt"));QVERIFY(file.open(QIODevice::WriteOnly));file.write("UNTRUSTED WORKSPACE");file.close();QVERIFY(seed(data.path(),"path:"+foreign.path,workspace.path()));MainWindow window(data.path());QVERIFY(select(window,"first"));auto* tabs=window.findChild<QTabWidget*>("contentTabs");const auto count=tabs->count();QVERIFY(clickLabel(window,QUrl("path:"+foreign.path)));QCOMPARE(tabs->count(),count);QVERIFY(!window.findChild<QPlainTextEdit*>("transcriptAttachmentText"));QVERIFY(clickLabel(window,QUrl("path:ws1/untrusted.txt")));QCOMPARE(tabs->count(),count);QVERIFY(window.findChildren<EditorWidget*>().isEmpty());QVERIFY(!window.findChild<QLabel*>("sessionStatus")->text().isEmpty());}
 void staleChatRowAndPartCannotOpen(){QTemporaryDir data;AttachmentStore store(data.path());const auto ref=store.write("first","report.txt","OWNED");QVERIFY(seed(data.path(),"path:"+ref.path));MainWindow window(data.path());QVERIFY(select(window,"first"));auto* view=window.findChild<ConversationView*>("conversationView");auto* tabs=window.findChild<QTabWidget*>("contentTabs");const auto count=tabs->count();emit view->scopedLinkActivated("first","missing-row","question-part",QUrl("path:"+ref.path));emit view->scopedLinkActivated("first","question-row","missing-part",QUrl("path:"+ref.path));QCOMPARE(tabs->count(),count);QVERIFY(select(window,"second"));emit view->scopedLinkActivated("first","question-row","question-part",QUrl("path:"+ref.path));QCOMPARE(tabs->count(),count);QVERIFY(!window.findChild<QPlainTextEdit*>("transcriptAttachmentText"));}
};
QTEST_MAIN(QuestionPresentationHostTest)
#include "test_question_presentation_host.moc"
