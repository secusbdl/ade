#include "ui/MainWindow.h"
#include "ui/Composer.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include "ui/AppStyle.h"
#include "core/ProviderStore.h"
#include <QtTest>
#include <QApplication>
#include <QFile>
#include <QFrame>
#include <QJsonDocument>
#include <QListWidget>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QToolButton>
using namespace ade;
namespace {
bool put(const QString &path,const QByteArray &bytes){QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();}
void expose(MainWindow &w){w.setAttribute(Qt::WA_DontShowOnScreen);w.resize(1000,700);w.show();w.findChild<Composer*>()->setFocus();}
bool seed(const QString &data,const QString &workspace){QList<Conversation> chats;for(const auto &id:QStringList{"first","second"}){Conversation c;c.id=id;c.title=id;c.titleLocked=true;c.workspace=workspace;c.providerId="fixture";c.model="fixture-model";chats.append(c);}return SessionStore(data).save(chats);}
bool select(MainWindow &w,const QString &id){auto *list=w.findChild<QListWidget*>("agentList");for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()==id){list->setCurrentItem(list->item(i));return true;}return false;}
void type(Composer *c,const QString &text){c->setFocus();c->setPlainText(text);c->moveCursor(QTextCursor::End);}
int choice(Composer *c,const QString &id){auto *list=c->findChild<QListWidget*>("composerCompletionList");for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()==id)return i;return -1;}
void activate(Composer *c,const QString &id){auto *list=c->findChild<QListWidget*>("composerCompletionList");const int row=choice(c,id);if(row<0)return;list->setCurrentRow(row);QTest::keyClick(c,Qt::Key_Return);}
}
class ComposerHostTest:public QObject {
    Q_OBJECT
    QTemporaryDir home;
    QByteArray previousProfile;
private slots:
    void initTestCase(){previousProfile=qgetenv("USERPROFILE");qputenv("USERPROFILE",home.path().toUtf8());initializeAppStyle(*qobject_cast<QApplication*>(qApp));}
    void cleanupTestCase(){qputenv("USERPROFILE",previousProfile);}
    void fileSelectionCapturesAttachmentAndPersistsDraft(){
        QTemporaryDir data,workspace;QVERIFY(put(workspace.filePath("src/file (1).txt"),"fixture"));QVERIFY(seed(data.path(),workspace.path()));
        MainWindow w(data.path());expose(w);auto *c=w.findChild<Composer*>();type(c,"Please inspect @file");
        QTRY_VERIFY_WITH_TIMEOUT(choice(c,"file:ws1/src/file (1).txt")>=0,5000);activate(c,"file:ws1/src/file (1).txt");
        QVERIFY(c->toPlainText().contains("[](path:ws1/src/file%20(1).txt)"));
        const auto saved=SessionStore(data.path()).load();QCOMPARE(saved.first().draft,c->toPlainText());QCOMPARE(saved.first().draftAttachments.size(),1);QCOMPARE(saved.first().draftAttachments.first().toObject()["path"].toString(),QString("ws1/src/file (1).txt"));
    }
    void workspaceChoiceHasPhysicalIdentityAndNoAttachment(){
        QTemporaryDir data,workspace;QVERIFY(seed(data.path(),workspace.path()));MainWindow w(data.path());expose(w);auto *c=w.findChild<Composer*>();type(c,"@");
        QSignalSpy selected(c,&Composer::completionActivated);QTRY_VERIFY_WITH_TIMEOUT(choice(c,"workspace:ws1")>=0,5000);activate(c,"workspace:ws1");QCOMPARE(selected.size(),1);QVERIFY(!selected[0][2].toJsonObject()["workspaceIdentity"].toString().isEmpty());
        QCOMPARE(c->toPlainText(),QString("[](path:ws1/) "));const auto saved=SessionStore(data.path()).load();QVERIFY(saved.first().draftAttachments.isEmpty());QCOMPARE(saved.first().draft,c->toPlainText());
    }
    void staleSearchDoesNotAppearInAnotherChat(){
        QTemporaryDir data,workspace;QVERIFY(put(workspace.filePath("file.txt"),"fixture"));QVERIFY(seed(data.path(),workspace.path()));MainWindow w(data.path());expose(w);auto *c=w.findChild<Composer*>();
        QSignalSpy requested(c,&Composer::completionRequested);type(c,"@file");QTRY_VERIFY(!requested.isEmpty());QVERIFY(select(w,"second"));QTest::qWait(150);QCOMPARE(c->toPlainText(),QString());QVERIFY(!c->findChild<QFrame*>("composerCompletionPopup")->isVisible());
    }
    void saveConflictPreservesTokenAndAttachmentList(){
        QTemporaryDir data,workspace;QVERIFY(put(workspace.filePath("file.txt"),"fixture"));QVERIFY(seed(data.path(),workspace.path()));MainWindow w(data.path());expose(w);auto *c=w.findChild<Composer*>();type(c,"@file");QTRY_VERIFY_WITH_TIMEOUT(choice(c,"file:ws1/file.txt")>=0,5000);
        SessionStore external(data.path());auto saved=external.load();saved[0].title="External title";QVERIFY(external.save(saved));activate(c,"file:ws1/file.txt");QCOMPARE(c->toPlainText(),QString("@file"));const auto after=SessionStore(data.path()).load();QCOMPARE(after[0].title,QString("External title"));QVERIFY(after[0].draftAttachments.isEmpty());
    }
    void skillsRespectInvocabilityAndDisabledPreferences(){
        QTemporaryDir data,workspace;QVERIFY(put(workspace.filePath(".stagewise/skills/visible/SKILL.md"),"---\nname: picker-fixture\ndescription: A fixture skill\n---\nFixture instruction."));QVERIFY(put(workspace.filePath(".stagewise/skills/hidden/SKILL.md"),"---\nname: picker-hidden\ndescription: Hidden fixture\nuser-invocable: false\n---\nHidden."));QVERIFY(seed(data.path(),workspace.path()));MainWindow w(data.path());expose(w);auto *c=w.findChild<Composer*>();type(c,"/picker");QTRY_VERIFY_WITH_TIMEOUT(choice(c,"skill:picker-fixture")>=0,5000);QCOMPARE(choice(c,"skill:picker-hidden"),-1);
        activate(c,"skill:picker-fixture");QCOMPARE(c->toPlainText(),QString("[/picker-fixture](slash:skill%3Apicker-fixture) "));
        auto *settings=w.findChild<SettingsPage*>();auto options=settings->preference("skillOptions").toObject();options["disabledSkills"]=QJsonArray{"picker-fixture"};QVERIFY(settings->setPreference("skillOptions",options));type(c,"/picker");QTest::qWait(250);QCOMPARE(choice(c,"skill:picker-fixture"),-1);
    }
    void slashInvocationReachesActualTurnContext(){
        QTemporaryDir data,workspace;QVERIFY(put(workspace.filePath(".stagewise/skills/probe/SKILL.md"),"---\nname: picker-context\ndescription: Context fixture\n---\nPICKER_CONTEXT_SENTINEL"));QVERIFY(seed(data.path(),workspace.path()));
        QTcpServer server;QVERIFY(server.listen(QHostAddress::LocalHost));QList<QJsonObject> requests;
        connect(&server,&QTcpServer::newConnection,&server,[&]{while(auto *socket=server.nextPendingConnection()){connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);connect(socket,&QTcpSocket::readyRead,&server,[&,socket]{if(socket->property("done").toBool())return;const auto bytes=socket->property("bytes").toByteArray()+socket->readAll();socket->setProperty("bytes",bytes);const int end=bytes.indexOf("\r\n\r\n");if(end<0)return;int size=0;for(const auto &line:bytes.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))size=line.mid(15).trimmed().toInt();if(bytes.size()<end+4+size)return;socket->setProperty("done",true);requests.append(QJsonDocument::fromJson(bytes.mid(end+4,size)).object());socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\ndata: [DONE]\n\n");socket->disconnectFromHost();});}});
        ProviderConfig provider;provider.id="fixture";provider.name="Fixture";provider.model="fixture-model";provider.apiKey="local-fixture-key";provider.baseUrl=QString("http://127.0.0.1:%1/v1").arg(server.serverPort());QVERIFY(ProviderStore::save(data.filePath("providers.json"),{provider}));
        MainWindow w(data.path());expose(w);auto *c=w.findChild<Composer*>();type(c,"/picker-context");QTRY_VERIFY_WITH_TIMEOUT(choice(c,"skill:picker-context")>=0,5000);activate(c,"skill:picker-context");w.findChild<QToolButton*>("sendButton")->click();QTRY_VERIFY_WITH_TIMEOUT(!requests.isEmpty(),5000);QVERIFY(QJsonDocument(requests.first()).toJson().contains("<invoked_skill"));QVERIFY(QJsonDocument(requests.first()).toJson().contains("PICKER_CONTEXT_SENTINEL"));
    }
};
QTEST_MAIN(ComposerHostTest)
#include "test_composer_host.moc"
