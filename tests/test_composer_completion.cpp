#include "ui/Composer.h"
#include <QtTest>
#include <QApplication>
#include <QFrame>
#include <QListWidget>
#include <QPointer>

using namespace ade;
namespace {
QJsonObject item(QString id="skill:plan",QString insertion="[/Plan](slash:skill:plan)") {
    return {{"id",id},{"label","Plan"},{"description","Create a scoped plan"},{"group","Workspace"},{"kind","skill"},{"insertText",insertion}};
}
void show(Composer &c){c.resize(340,150);c.show();c.setFocus();c.setCompletionContext("chat");}
void type(Composer &c,const QString &value){c.setPlainText(value);c.moveCursor(QTextCursor::End);}
QFrame *popup(Composer &c){return c.findChild<QFrame*>("composerCompletionPopup");}
quint64 request(QSignalSpy &spy){return spy.last().at(1).toULongLong();}
}
class ComposerCompletionTest:public QObject {
    Q_OBJECT
private slots:
    void queryAndHostCommittedSelection(){
        Composer c;show(c);QSignalSpy queries(&c,&Composer::completionRequested),chosen(&c,&Composer::completionActivated),sent(&c,&Composer::submitted);
        type(c,"please /pl");QTRY_COMPARE(queries.size(),1);QCOMPARE(queries[0][0].toString(),QString("chat"));QCOMPARE(queries[0][2].value<QChar>(),QChar('/'));QCOMPARE(queries[0][3].toString(),QString("pl"));
        const auto id=request(queries);c.setCompletionResults(id,{item()});QVERIFY(popup(c)->isVisible());
        QTest::keyClick(&c,Qt::Key_Return);QCOMPARE(chosen.size(),1);QCOMPARE(sent.size(),0);QCOMPARE(c.toPlainText(),QString("please /pl"));
        QVERIFY(c.commitCompletion(id,"skill:plan"));QCOMPARE(c.toPlainText(),QString("please [/Plan](slash:skill:plan) "));QVERIFY(!popup(c)->isVisible());
        QTest::keyClick(&c,Qt::Key_Return);QCOMPARE(sent.size(),1);
    }
    void staleResultsAndCommitCannotCrossChats(){
        Composer c;show(c);QSignalSpy q(&c,&Composer::completionRequested);type(c,"@");QTRY_COMPARE(q.size(),1);const auto old=request(q);
        c.setCompletionContext("other");type(c,"@new");QTRY_COMPARE(q.size(),2);c.setCompletionResults(old,{item()});QVERIFY(!popup(c)->isVisible());QVERIFY(!c.commitCompletion(old,"skill:plan"));
        c.setCompletionResults(request(q),{item("file","[](path:ws1/file.txt)")});QVERIFY(c.commitCompletion(request(q),"file"));QCOMPARE(c.toPlainText(),QString("[](path:ws1/file.txt) "));
    }
    void textOrCursorChangeRevokesPendingChoice(){
        Composer c;show(c);QSignalSpy q(&c,&Composer::completionRequested);type(c,"@file");QTRY_COMPARE(q.size(),1);auto id=request(q);c.setCompletionResults(id,{item()});
        c.moveCursor(QTextCursor::Start);QVERIFY(!c.commitCompletion(id,"skill:plan"));QVERIFY(!popup(c)->isVisible());
        type(c,"@other");QTRY_COMPARE(q.size(),2);id=request(q);c.setCompletionResults(id,{item()});c.insertPlainText("x");QVERIFY(!c.commitCompletion(id,"skill:plan"));
    }
    void escapeDismissesBeforeStopping(){
        Composer c;show(c);QSignalSpy q(&c,&Composer::completionRequested),stop(&c,&Composer::stopRequested);type(c,"/");QTRY_COMPARE(q.size(),1);const auto id=request(q);c.setCompletionResults(id,{item()});
        QTest::keyClick(&c,Qt::Key_Escape);QCOMPARE(stop.size(),0);QVERIFY(!popup(c)->isVisible());c.setCompletionResults(id,{item()});QVERIFY(!popup(c)->isVisible());QTest::keyClick(&c,Qt::Key_Escape);QCOMPARE(stop.size(),1);
    }
    void arrowsTabAndShiftReturn(){
        Composer c;show(c);QSignalSpy q(&c,&Composer::completionRequested),chosen(&c,&Composer::completionActivated);type(c,"@");QTRY_COMPARE(q.size(),1);c.setCompletionResults(request(q),{item("a","[](path:ws1/a)"),item("b","[](path:ws1/b)")});
        QTest::keyClick(&c,Qt::Key_Down);QTest::keyClick(&c,Qt::Key_Tab);QCOMPARE(chosen.size(),1);QCOMPARE(chosen.last()[2].toJsonObject()["id"].toString(),QString("b"));
        QTest::keyClick(&c,Qt::Key_Return,Qt::ShiftModifier);QVERIFY(c.toPlainText().contains('\n'));QCOMPARE(chosen.size(),1);
    }
    void nonTriggerTokensAndSelection_data(){QTest::addColumn<QString>("value");for(const auto &v:QStringList{"a@example.com","https://example.com","folder/file","text","/@ambiguous"})QTest::newRow(qPrintable(v))<<v;}
    void nonTriggerTokensAndSelection(){
        QFETCH(QString,value);Composer c;show(c);QSignalSpy q(&c,&Composer::completionRequested);type(c,value);QTest::qWait(90);
        if(value=="/@ambiguous")QCOMPARE(q.size(),1);else QCOMPARE(q.size(),0);
        c.selectAll();QVERIFY(!popup(c)->isVisible());
    }
    void resultLimitsDuplicatesAndEmpty(){
        Composer c;show(c);QSignalSpy q(&c,&Composer::completionRequested);type(c,"@");QTRY_COMPARE(q.size(),1);QJsonArray items;for(int i=0;i<200;++i)items.append(item(QString::number(i),"[](path:ws1/a)"));items.prepend(QJsonObject{{"id","invalid"},{"label","Missing insertion"}});
        c.setCompletionResults(request(q),items);auto *list=c.findChild<QListWidget*>();QCOMPARE(list->count(),128);
        c.setCompletionResults(request(q),{item(),item()});QCOMPARE(list->count(),1);c.setCompletionResults(request(q),{},"No matching files");QCOMPARE(list->count(),1);QCOMPARE(list->item(0)->text(),QString("No matching files"));QVERIFY(!list->item(0)->flags().testFlag(Qt::ItemIsEnabled));
    }
    void activationMayDeleteComposer(){
        QPointer<Composer> c=new Composer;show(*c);QSignalSpy q(c,&Composer::completionRequested);type(*c,"/");QTRY_COMPARE(q.size(),1);c->setCompletionResults(request(q),{item()});
        connect(c,&Composer::completionActivated,c,[&]{delete c.data();});QTest::keyClick(c,Qt::Key_Return);QVERIFY(!c);
    }
    void requestingMaySwitchContext(){
        Composer c;show(c);connect(&c,&Composer::completionRequested,&c,[&](QString,quint64 id,QChar,QString){c.setCompletionContext("replacement");c.setCompletionResults(id,{item()});});type(c,"@");QTest::qWait(90);QVERIFY(!popup(c)->isVisible());QCOMPARE(c.toPlainText(),QString("@"));
    }
    void singleUndoRestoresTypedToken(){
        Composer c;show(c);QSignalSpy q(&c,&Composer::completionRequested);type(c,"prefix @file suffix");auto cursor=c.textCursor();cursor.setPosition(12);c.setTextCursor(cursor);QTRY_COMPARE(q.size(),1);const auto id=request(q);c.setCompletionResults(id,{item("f","[](path:ws1/file)")});QVERIFY(c.commitCompletion(id,"f"));c.undo();QCOMPARE(c.toPlainText(),QString("prefix @file suffix"));
    }
    void slashGroupsExpandWithoutInvokingACommand(){
        Composer c;show(c);QSignalSpy q(&c,&Composer::completionRequested),chosen(&c,&Composer::completionActivated);type(c,"/");QTRY_COMPARE(q.size(),1);
        QJsonArray entries;for(int i=0;i<5;++i)entries.append(item(QString::number(i),"[/Plan](slash:skill:plan)"));c.setCompletionResults(request(q),entries);
        auto *list=c.findChild<QListWidget*>();QCOMPARE(list->count(),5);QCOMPARE(list->item(0)->text(),QString("Skills"));QCOMPARE(list->item(4)->text(),QString("Show 2 more"));
        list->setCurrentRow(4);QTest::keyClick(&c,Qt::Key_Return);QCOMPARE(chosen.size(),0);QCOMPARE(list->count(),6);QCOMPARE(c.toPlainText(),QString("/"));
    }
    void focusLossRevokesAsynchronousResults(){
        Composer c;show(c);QSignalSpy q(&c,&Composer::completionRequested);type(c,"@");QTRY_COMPARE(q.size(),1);const auto id=request(q);
        QFocusEvent lost(QEvent::FocusOut,Qt::OtherFocusReason);QApplication::sendEvent(&c,&lost);c.setCompletionResults(id,{item()});QVERIFY(!popup(c)->isVisible());QVERIFY(!c.commitCompletion(id,"skill:plan"));
    }
    void textObserverCannotMoveReplacementChatCursor(){
        Composer c;show(c);QSignalSpy q(&c,&Composer::completionRequested);type(c,"@file");QTRY_COMPARE(q.size(),1);const auto id=request(q);c.setCompletionResults(id,{item()});bool replaced=false;
        connect(&c,&QPlainTextEdit::textChanged,&c,[&]{if(replaced)return;replaced=true;c.setCompletionContext("replacement");c.setPlainText("A different draft");auto cursor=c.textCursor();cursor.setPosition(2);c.setTextCursor(cursor);});
        QVERIFY(c.commitCompletion(id,"skill:plan"));QCOMPARE(c.toPlainText(),QString("A different draft"));QCOMPARE(c.textCursor().position(),2);
    }
};
QTEST_MAIN(ComposerCompletionTest)
#include "test_composer_completion.moc"
