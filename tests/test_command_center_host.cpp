#include "ui/MainWindow.h"
#include "ui/CommandCenter.h"
#include "ui/Composer.h"
#include "ui/SettingsPage.h"
#include "ui/SidebarNavigation.h"
#include "ui/SessionStore.h"
#include "ui/AppStyle.h"
#include "editor/EditorWidget.h"
#include "editor/CodeEditor.h"
#include "terminal/TerminalWidget.h"
#include "runtime/AgentToolHost.h"
#include <QJsonDocument>
#include <QPushButton>
#include <QAction>
#include <QApplication>
#include <QFile>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>
#include <QScopeGuard>
#include <cstddef>
#include <cstring>
#include <QtTest>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
using namespace ade;
namespace {
void put(const QString& file,const QByteArray& text){QDir().mkpath(QFileInfo(file).absolutePath());QFile output(file);QVERIFY(output.open(QIODevice::WriteOnly));QCOMPARE(output.write(text),text.size());}
bool seed(const QString& data,const QString& workspace){QList<Conversation> list;for(const auto& id:QStringList{"first","second"}){Conversation c;c.id=id;c.title=id+" chat";c.titleLocked=true;c.workspace=workspace;list.append(c);}return SessionStore(data).save(list);}
void expose(MainWindow& window){window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1000,700);window.show();window.activateWindow();QApplication::setActiveWindow(&window);window.findChild<Composer*>()->setFocus();QCoreApplication::processEvents();QCOMPARE(QApplication::activeWindow(),&window);QVERIFY(window.findChild<Composer*>()->hasFocus());}
CommandCenter* open(MainWindow& window,const QString& mode="global"){
    // Complete the previous dialog's queued hide/deactivation before setting
    // the offscreen platform's active window for this new keyboard action.
    QCoreApplication::processEvents();
    auto* composer=window.findChild<Composer*>();QWidget* target=composer&&composer->isVisible()?static_cast<QWidget*>(composer):static_cast<QWidget*>(&window);
    window.activateWindow();QApplication::setActiveWindow(&window);target->setFocus();
    if(QApplication::activeWindow()!=&window||!target->isVisible()||(target==composer&&!composer->hasFocus())){auto* focus=QApplication::focusWidget();qWarning()<<"Command shortcut focus gate"<<"activeWindowMatch"<<(QApplication::activeWindow()==&window)<<"targetVisible"<<target->isVisible()<<"targetIsComposer"<<(target==composer)<<"composerFocus"<<composer->hasFocus()<<"focusName"<<(focus?focus->objectName():QString())<<"focusClass"<<(focus?focus->metaObject()->className():"none");QTest::qFail("Fixture shortcut target must be visible in the active window",__FILE__,__LINE__);return nullptr;}
    QTest::keyClick(target,mode=="files"?Qt::Key_P:Qt::Key_K,Qt::ControlModifier);
    auto* center=window.findChild<CommandCenter*>();
    if(!center||!center->isVisible()){QTest::qFail("The real shortcut must open a visible command center",__FILE__,__LINE__);return nullptr;}
    return center;
}
QListWidgetItem* row(CommandCenter* center,const QString& key,const QString& value){if(!center)return nullptr;auto* list=center->findChild<QListWidget*>("commandResults");for(int index=0;index<list->count();++index)if(list->item(index)->data(Qt::UserRole).toJsonObject()[key].toString()==value)return list->item(index);return nullptr;}
void activate(CommandCenter* center,QListWidgetItem* item){QVERIFY(center);QVERIFY(item);center->findChild<QListWidget*>("commandResults")->setCurrentItem(item);QTest::keyClick(center->findChild<QLineEdit*>("commandQuery"),Qt::Key_Return);}
bool renameIdleFixtureRoot(const QString& source,const QString& destination){
#ifdef Q_OS_WIN
    HANDLE handle=INVALID_HANDLE_VALUE;DWORD failure=ERROR_SUCCESS;
    // This is a readiness barrier, not a retry of the mutation. Git processes
    // and scanners can temporarily pin the fixture root. Acquiring DELETE
    // waits for those non-delete-sharing users, then blocks new such readers
    // until this one same-handle rename finishes.
    const bool ready=QTest::qWaitFor([&]{handle=CreateFileW(reinterpret_cast<LPCWSTR>(source.utf16()),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);failure=handle==INVALID_HANDLE_VALUE?GetLastError():ERROR_SUCCESS;return handle!=INVALID_HANDLE_VALUE;},5000);
    if(!ready){qWarning()<<"Fixture root remained busy before rename"<<failure;return false;}
    const auto close=qScopeGuard([&]{CloseHandle(handle);});
    const auto target=QDir::toNativeSeparators(QFileInfo(destination).absoluteFilePath());
    QByteArray storage(qsizetype(offsetof(FILE_RENAME_INFO,FileName))+(target.size()+1)*qsizetype(sizeof(wchar_t)),0);
    auto* info=reinterpret_cast<FILE_RENAME_INFO*>(storage.data());info->ReplaceIfExists=FALSE;info->RootDirectory=nullptr;info->FileNameLength=DWORD(target.size()*sizeof(wchar_t));std::memcpy(info->FileName,target.utf16(),info->FileNameLength);
    if(!SetFileInformationByHandle(handle,FileRenameInfo,info,DWORD(storage.size()))){qWarning()<<"Fixture same-handle rename failed"<<GetLastError();return false;}
    return true;
#else
    return QDir().rename(source,destination);
#endif
}
}
class CommandHostTest:public QObject{
Q_OBJECT
private slots:
void initTestCase(){initializeAppStyle(*qobject_cast<QApplication*>(qApp));}
void shortcutSearchSelectPreservesDraft(){QTemporaryDir data,workspace;QVERIFY(seed(data.path(),workspace.path()));MainWindow window(data.path());expose(window);window.findChild<Composer*>()->setPlainText("raw draft  ");auto* center=open(window);QVERIFY(center);QTRY_VERIFY(center->isVisible());QVERIFY(row(center,"agentId","second"));activate(center,row(center,"agentId","second"));QCOMPARE(window.currentSessionId(),QString("second"));QVERIFY(!center->isVisible());const auto stored=SessionStore(data.path()).load();QCOMPARE(stored[0].draft,QString("raw draft  "));}
void settingsActionUsesRealRoute(){QTemporaryDir data,workspace;QVERIFY(seed(data.path(),workspace.path()));MainWindow window(data.path());expose(window);auto* center=open(window);QVERIFY(center);QVERIFY(row(center,"route","models-providers"));activate(center,row(center,"route","models-providers"));auto* settings=window.findChild<SettingsPage*>();QCOMPARE(settings->currentSection(),QString("models-providers"));QVERIFY(settings->isVisible());}
void pinPersistsAndConflictRollsBack(){QTemporaryDir data,workspace;QVERIFY(seed(data.path(),workspace.path()));MainWindow window(data.path());expose(window);auto* center=open(window);QVERIFY(center);auto* results=center->findChild<QListWidget*>("commandResults");results->setCurrentItem(row(center,"agentId","second"));center->findChild<QToolButton*>("commandPin")->click();auto* sidebar=window.findChild<SidebarNavigation*>();QVERIFY(sidebar->isPinned("second"));QVERIFY(center->isVisible());SettingsPage external(data.path());auto prefs=external.preference("sidebar").toObject();prefs["pinnedAgentIds"]=QJsonArray{"first"};QVERIFY(external.setPreference("sidebar",prefs));results->setCurrentItem(row(center,"agentId","second"));center->findChild<QToolButton*>("commandPin")->click();QVERIFY(sidebar->isPinned("second"));}
void fileSelectionOpensActualEditor(){QTemporaryDir data,workspace;put(workspace.filePath("src/example.txt"),"line one\nline two\n");QVERIFY(seed(data.path(),workspace.path()));MainWindow window(data.path());expose(window);auto* center=open(window,"files");QVERIFY(center);center->findChild<QLineEdit*>("commandQuery")->setText("example");QTRY_VERIFY_WITH_TIMEOUT(row(center,"relativePath","src/example.txt"),5000);activate(center,row(center,"relativePath","src/example.txt"));auto* editor=window.findChild<EditorWidget*>();QVERIFY(editor);QCOMPARE(editor->editor()->toPlainText(),QString("line one\nline two\n"));}
void folderSelectionRevealsActualTree(){QTemporaryDir data,workspace;put(workspace.filePath("nested/example.txt"),"ok");QVERIFY(seed(data.path(),workspace.path()));MainWindow window(data.path());expose(window);auto* center=open(window,"files");QVERIFY(center);center->findChild<QLineEdit*>("commandQuery")->setText("nested");QTRY_VERIFY_WITH_TIMEOUT(row(center,"relativePath","nested"),5000);activate(center,row(center,"relativePath","nested"));auto* tree=window.findChild<QTreeView*>();QVERIFY(tree);QVERIFY(tree->isVisible());QTRY_VERIFY(tree->currentIndex().isValid());}
void staleRootResultCannotOpenReplacement(){QTemporaryDir data,parent;const auto root=parent.filePath("workspace");QVERIFY(QDir().mkpath(root));put(root+"/example.txt","original");QVERIFY(seed(data.path(),root));MainWindow window(data.path());expose(window);auto* center=open(window,"files");QVERIFY(center);QSignalSpy requested(center,&CommandCenter::queryRequested);center->findChild<QLineEdit*>("commandQuery")->setText("example");const auto queryId=center->requestId();QTRY_VERIFY_WITH_TIMEOUT(!requested.isEmpty()&&requested.last()[0].toULongLong()==queryId&&requested.last()[2].toString()=="example",5000);QTRY_VERIFY_WITH_TIMEOUT(row(center,"relativePath","example.txt"),5000);QCOMPARE(center->requestId(),queryId);QVERIFY(renameIdleFixtureRoot(root,parent.filePath("previous")));put(root+"/example.txt","replacement");activate(center,row(center,"relativePath","example.txt"));QVERIFY(!window.findChild<EditorWidget*>());}
void terminalTargetSurvivesTabIndexChanges(){QTemporaryDir data,workspace;QVERIFY(seed(data.path(),workspace.path()));MainWindow window(data.path());expose(window);auto* tabs=window.findChild<QTabWidget*>("workspaceTabs");if(!tabs)tabs=window.findChild<QTabWidget*>();QVERIFY(tabs);auto* first=new TerminalWidget;auto* second=new TerminalWidget;tabs->addTab(first,"First terminal");tabs->addTab(second,"Second terminal");auto* center=open(window);QVERIFY(center);auto* selected=row(center,"title","Second terminal");QVERIFY(selected);tabs->removeTab(tabs->indexOf(first));delete first;activate(center,selected);QCOMPARE(tabs->currentWidget(),second);QVERIFY(second->isVisible());}
void ownedShellOpensReusesAndSurvivesViewClose(){
    QTemporaryDir data,workspace;QVERIFY(seed(data.path(),workspace.path()));MainWindow window(data.path());expose(window);
    for(auto* item:window.findChildren<QAction*>())if(item->text()=="File changes and undo"){item->trigger();break;}
    auto* host=window.findChild<AgentToolHost*>();QVERIFY(host);QVERIFY(host->setWorkspace(workspace.path()));
    auto* shells=host->shells();QSignalSpy done(shells,&ShellService::completed);shells->execute("create-fixture","createShellSession",{{"cwd","ws1"}});
    QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,15000);QVERIFY2(!done[0][2].toBool(),qPrintable(done[0][1].toString()));
    const auto id=QJsonDocument::fromJson(done[0][1].toString().toUtf8()).object()["session_id"].toString();QVERIFY(!id.isEmpty());
    auto* center=open(window);QVERIFY(center);auto* selected=row(center,"shellId",id);QVERIFY(selected);activate(center,selected);
    auto* tabs=window.findChild<QTabWidget*>();QVERIFY(tabs);auto* page=tabs->currentWidget();QCOMPARE(page->property("shellSessionId").toString(),id);
    auto* terminal=page->findChild<TerminalWidget*>("agentShellTerminal");QVERIFY(terminal);QCOMPARE(terminal->terminalModel(),shells->sessionModel(id));
    auto* process=shells->sessionProcess(id);QVERIFY(process&&process->isRunning());
    const int count=tabs->count();center=open(window);QVERIFY(center);selected=row(center,"shellId",id);QVERIFY(selected);activate(center,selected);QCOMPARE(tabs->count(),count);QCOMPARE(tabs->currentWidget(),page);
    QPointer<QWidget> oldPage(page);emit tabs->tabCloseRequested(tabs->indexOf(page));QTRY_VERIFY(!oldPage);QVERIFY(process->isRunning());
    center=open(window);QVERIFY(center);selected=row(center,"shellId",id);QVERIFY(selected);activate(center,selected);page=tabs->currentWidget();QCOMPARE(page->property("shellSessionId").toString(),id);
    auto* stop=page->findChild<QPushButton*>("stopAgentShell");QVERIFY(stop);stop->click();QTRY_VERIFY(!process->isRunning());QVERIFY(!stop->isEnabled());
}
};
QTEST_MAIN(CommandHostTest)
#include "test_command_center_host.moc"
