#include "terminal/TerminalModel.h"
#include "terminal/TerminalWidget.h"
#include "terminal/ConPtyProcess.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QScrollBar>
#include <cstdio>
#include <string>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace ade;
namespace {
void fixturePrint(const QByteArray &bytes){DWORD count=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),bytes.constData(),DWORD(bytes.size()),&count,nullptr);}
int fixture(){
    SetConsoleCP(CP_UTF8);SetConsoleOutputCP(CP_UTF8);
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),ENABLE_VIRTUAL_TERMINAL_INPUT);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE),ENABLE_PROCESSED_OUTPUT|ENABLE_VIRTUAL_TERMINAL_PROCESSING|DISABLE_NEWLINE_AUTO_RETURN);
    fixturePrint("READY\r\n");QString pending;wchar_t bytes[256];DWORD length=0;
    // ReadConsoleW preserves surrogate pairs; legacy ReadFile console code-page
    // conversion can replace each UTF-16 surrogate even with CP_UTF8 selected.
    while(ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE),bytes,256,&length,nullptr)&&length){
        for(DWORD i=0;i<length;++i){const wchar_t c=bytes[i];if(c==3){fixturePrint("INTERRUPTED\r\n");pending.clear();continue;}
            if(c!='\r'&&c!='\n'){pending+=QChar(ushort(c));continue;}if(pending.isEmpty())continue;
            const QByteArray line=pending.toUtf8();
            if(line=="exit")return 17;
            if(line=="stall"){Sleep(INFINITE);return 0;}
            if(line=="size"){CONSOLE_SCREEN_BUFFER_INFO info{};GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE),&info);fixturePrint("SIZE:"+QByteArray::number(info.srWindow.Right-info.srWindow.Left+1)+"x"+QByteArray::number(info.srWindow.Bottom-info.srWindow.Top+1)+"\r\n");}
            else if(line=="child"){
                wchar_t path[32768];GetModuleFileNameW(nullptr,path,32768);std::wstring command=L"\""+std::wstring(path)+L"\" --descendant";STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
                if(CreateProcessW(path,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)){fixturePrint("CHILD:"+QByteArray::number(process.dwProcessId)+"\r\n");CloseHandle(process.hThread);CloseHandle(process.hProcess);}
            }else if(line=="flood"){for(int j=0;j<20000;++j)fixturePrint("0123456789abcdefghijklmnopqrstuvwxyz\r\n");fixturePrint("FLOOD-END\r\n");}
            else fixturePrint("ECHO:"+line+"\r\n");
            pending.clear();
        }
    }return 0;
}
QStringList fixtureCommand(){return {QCoreApplication::applicationFilePath(),"--terminal-fixture"};}
}
class TerminalTest final:public QObject{
    Q_OBJECT
private slots:
    void splitUnicodeAndControlSequences();
    void cursorEraseAlternateScreenAndColours();
    void boundedHistoryAndResize();
    void keyboardPasteAndProtocolReplies();
    void titleDirectoryAndFragmentLimits();
    void malformedSequencesStayBounded();
    void interactiveConPtyAndResize();
    void stoppingKillsOnlyOwnedProcessTree();
    void invalidStartsAreRecoverable();
    void highVolumeOutputAndImmediateStop();
    void nativeWidgetGeometryAndInput();
    void saturatedInputTeardownAndRestart();
    void powerShellPromptTracksWorkingDirectory();
    void tabAltGrAndAtomicClipboardInput();
    void emptyRepeatCombiningFloodAndGlobalReverse();
};
void TerminalTest::splitUnicodeAndControlSequences(){TerminalModel model({40,5});const auto bytes=QString::fromUtf8("A日本😀éZ").toUtf8();for(const char c:bytes)model.feed(QByteArray(1,c));QCOMPARE(model.lineText(0),QString::fromUtf8("A日本😀éZ"));model.feed("\x1b[");model.feed("38;2;12;34;");model.feed("56mQ");const auto cursor=model.cursor();QCOMPARE(model.cell(0,cursor.x()-1).foreground,QColor(12,34,56));}
void TerminalTest::cursorEraseAlternateScreenAndColours(){TerminalModel model({20,5});model.feed("abcdef\rXY\x1b[K");QCOMPARE(model.lineText(0),QString("XY"));model.feed("\x1b[2;3H\x1b[1;3;4;38;2;40;50;60;48;2;70;80;90mC");const auto cell=model.cell(1,2);QCOMPARE(cell.text,QString("C"));QVERIFY(cell.bold&&cell.italic&&cell.underline);QCOMPARE(cell.foreground,QColor(40,50,60));QCOMPARE(cell.background,QColor(70,80,90));model.feed("\x1b[?1049hALT");QVERIFY(model.alternateScreen());QVERIFY(model.plainText(false).contains("ALT"));model.feed("\x1b[?1049l");QVERIFY(!model.alternateScreen());QCOMPARE(model.lineText(0),QString("XY"));model.feed("\x1b[2J");QVERIFY(model.plainText(false).isEmpty());}
void TerminalTest::boundedHistoryAndResize(){TerminalModel model({20,3});for(int i=0;i<5100;++i)model.feed(QByteArray::number(i)+"\r\n");QCOMPARE(model.historyLines(),5000);QVERIFY(model.plainText().contains("5099"));QVERIFY(!model.lineText(0).contains("0\n"));QVERIFY(model.resize({30,6}));QCOMPARE(model.size(),QSize(30,6));QVERIFY(model.plainText().contains("5099"));QVERIFY(!model.resize({0,0}));model.feed("\x1b[3J");QCOMPARE(model.historyLines(),0);}
void TerminalTest::keyboardPasteAndProtocolReplies(){TerminalModel model;QByteArray output;connect(&model,&TerminalModel::outputReady,this,[&](const QByteArray &b){output+=b;});model.key(Qt::Key_C,Qt::ControlModifier,QString(QChar(3)));QCOMPARE(output,QByteArray(1,3));output.clear();model.key(Qt::Key_Up,Qt::NoModifier,{});QCOMPARE(output,QByteArray("\x1b[A"));output.clear();model.feed("\x1b[?1h");model.key(Qt::Key_Up,Qt::NoModifier,{});QCOMPARE(output,QByteArray("\x1bOA"));output.clear();model.key(0,Qt::NoModifier,QString::fromUtf8("😀"));QCOMPARE(output,QString::fromUtf8("😀").toUtf8());output.clear();model.feed("\x1b[?2004h");model.paste("a\nb");QCOMPARE(output,QByteArray("\x1b[200~a\rb\x1b[201~"));output.clear();model.feed("\x1b[3;4H\x1b[6n");QCOMPARE(output,QByteArray("\x1b[3;4R"));output.clear();model.feed("\x1b]11;?\x07");QVERIFY(output.startsWith("\x1b]11;rgb:"));}
void TerminalTest::titleDirectoryAndFragmentLimits(){TerminalModel model;QSignalSpy titles(&model,&TerminalModel::titleChanged),cwd(&model,&TerminalModel::workingDirectoryChanged);model.feed("\x1b]2;Native ");model.feed(QString::fromUtf8("日本").toUtf8()+"\x07");QCOMPARE(titles.count(),1);QCOMPARE(titles.first().first().toString(),QString::fromUtf8("Native 日本"));model.feed("\x1b]7;file:///D:/test%20workspace\x1b\\");QCOMPARE(cwd.count(),1);QCOMPARE(cwd.first().first().toString(),QString("D:/test workspace"));model.feed("\x1b]2;"+QByteArray(100000,'x')+"\x07OK");QVERIFY(model.plainText().endsWith("OK"));QVERIFY(titles.last().first().toString().size()<=4096);}
void TerminalTest::malformedSequencesStayBounded(){TerminalModel model({40,5});QElapsedTimer timer;timer.start();QByteArray many="\x1b[";for(int i=0;i<10000;++i)many+="1;";many+="31mAFTER";for(qsizetype i=0;i<many.size();i+=7)model.feed(many.mid(i,7));QCOMPARE(model.lineText(0),QString("AFTER"));model.feed("\x1b["+QByteArray(10000,'9')+"C!");QCOMPARE(model.cursor().x(),39);model.feed(QByteArray("\x1b]2",4)+QByteArray(1,0)+";name\x07");model.feed("\x1b]"+QByteArray(10000,'9')+";ignored\x07\r\nEND");QVERIFY(model.plainText().contains("END"));QVERIFY(timer.elapsed()<3000);}
void TerminalTest::interactiveConPtyAndResize(){QTemporaryDir scratch;ConPtyProcess process;TerminalModel model({80,24});connect(&process,&ConPtyProcess::outputReady,&model,&TerminalModel::feed);connect(&model,&TerminalModel::outputReady,&process,[&](const QByteArray &b){process.write(b);});QSignalSpy exited(&process,&ConPtyProcess::exited);QString error;QVERIFY2(process.start(scratch.path(),fixtureCommand(),{80,24},&error),qPrintable(error));QTRY_VERIFY_WITH_TIMEOUT(model.plainText().contains("READY"),5000);QVERIFY(process.write("hello\r"));QTRY_VERIFY_WITH_TIMEOUT(model.plainText().contains("ECHO:hello"),5000);QVERIFY(process.resize({93,31},&error));model.resize({93,31});QVERIFY(process.write("size\r"));QTRY_VERIFY_WITH_TIMEOUT(model.plainText().contains("SIZE:93x31"),5000);QVERIFY(process.write(QString::fromUtf8("日本😀\r").toUtf8()));QTRY_VERIFY2_WITH_TIMEOUT(model.plainText().contains(QString::fromUtf8("ECHO:日本😀")),qPrintable(model.plainText()),5000);QVERIFY(process.write(QByteArray(1,3)));QTRY_VERIFY_WITH_TIMEOUT(model.plainText().contains("INTERRUPTED"),5000);QVERIFY(process.write("exit\r"));QTRY_COMPARE_WITH_TIMEOUT(exited.count(),1,5000);QCOMPARE(exited.first().first().toUInt(),17u);QVERIFY(!process.isRunning());}
void TerminalTest::stoppingKillsOnlyOwnedProcessTree(){QTemporaryDir scratch;ConPtyProcess process;TerminalModel model({80,24});connect(&process,&ConPtyProcess::outputReady,&model,&TerminalModel::feed);QString error;QVERIFY2(process.start(scratch.path(),fixtureCommand(),{80,24},&error),qPrintable(error));QTRY_VERIFY_WITH_TIMEOUT(model.plainText().contains("READY"),5000);process.write("child\r");QRegularExpression pattern("CHILD:(\\d+)");QTRY_VERIFY_WITH_TIMEOUT(pattern.match(model.plainText()).hasMatch(),5000);const auto pid=pattern.match(model.plainText()).captured(1).toUInt();QVERIFY(pid!=GetCurrentProcessId());HANDLE child=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);QVERIFY(child);HANDLE parent=OpenProcess(SYNCHRONIZE,FALSE,process.processId());QVERIFY(parent);process.stop();QCOMPARE(WaitForSingleObject(child,2000),DWORD(WAIT_OBJECT_0));QCOMPARE(WaitForSingleObject(parent,2000),DWORD(WAIT_OBJECT_0));CloseHandle(child);CloseHandle(parent);QVERIFY(!process.isRunning());}
void TerminalTest::invalidStartsAreRecoverable(){ConPtyProcess process;QString error;QVERIFY(!process.start({}, {"Z:/missing/no.exe"},{80,24},&error));QVERIFY(!error.isEmpty());QVERIFY(!process.start({},fixtureCommand(),{0,24},&error));QVERIFY(process.start("Z:/missing/directory",fixtureCommand(),{80,24},&error));QCOMPARE(process.workingDirectory(),QDir::homePath());QVERIFY(!process.start({},fixtureCommand(),{80,24},&error));process.stop();QVERIFY(process.start({},fixtureCommand(),{80,24},&error));process.stop();}
void TerminalTest::highVolumeOutputAndImmediateStop(){ConPtyProcess process;QByteArray output;connect(&process,&ConPtyProcess::outputReady,this,[&](const QByteArray &b){output+=b;});QString error;QVERIFY2(process.start({},fixtureCommand(),{80,24},&error),qPrintable(error));QTRY_VERIFY_WITH_TIMEOUT(output.contains("READY"),5000);process.write("flood\r");QTRY_VERIFY_WITH_TIMEOUT(output.contains("FLOOD-END"),10000);QElapsedTimer timer;timer.start();process.stop();QVERIFY2(timer.elapsed()<5000,"ConPTY teardown blocked.");for(int i=0;i<4;++i){QVERIFY(process.start({},fixtureCommand(),{80,24},&error));process.stop();}QVERIFY(!process.isRunning());}
void TerminalTest::nativeWidgetGeometryAndInput(){TerminalWidget widget;widget.resize(800,450);widget.ensurePolished();QCoreApplication::processEvents();widget.setZoomPercent(100);QVERIFY(widget.terminalModel()->size().width()>20);widget.setZoomPercent(150);QCOMPARE(widget.zoomPercent(),150);QString error;QVERIFY2(widget.start({},&error,fixtureCommand()),qPrintable(error));QTRY_VERIFY_WITH_TIMEOUT(widget.terminalModel()->plainText().contains("READY"),5000);QTest::keyClicks(&widget,"from-widget");QTest::keyClick(&widget,Qt::Key_Return);QTRY_VERIFY_WITH_TIMEOUT(widget.terminalModel()->plainText().contains("ECHO:from-widget"),5000);widget.stop();}
void TerminalTest::saturatedInputTeardownAndRestart(){ConPtyProcess process;QByteArray output;connect(&process,&ConPtyProcess::outputReady,this,[&](const QByteArray &b){output+=b;});QSignalSpy exits(&process,&ConPtyProcess::exited);QString error;
    for(int i=0;i<5;++i){output.clear();QVERIFY2(process.start({},fixtureCommand(),{80,24},&error),qPrintable(error));QTRY_VERIFY_WITH_TIMEOUT(output.contains("READY"),5000);QCOMPARE(exits.count(),0);QVERIFY(process.write("stall\r"));QVERIFY(process.write(QByteArray(4*1024*1024-16,'x')));QElapsedTimer timer;timer.start();process.stop();QVERIFY(timer.elapsed()<5000);}
    output.clear();QVERIFY(process.start({},fixtureCommand(),{80,24},&error));QTRY_VERIFY_WITH_TIMEOUT(output.contains("READY"),5000);QCOMPARE(exits.count(),0);process.write("exit\r");QTRY_COMPARE_WITH_TIMEOUT(exits.count(),1,5000);QCOMPARE(exits.first().first().toUInt(),17u);
}
void TerminalTest::powerShellPromptTracksWorkingDirectory(){QTemporaryDir scratch;QDir().mkpath(scratch.path()+"/next 日本");ConPtyProcess process;TerminalModel model({100,30});connect(&process,&ConPtyProcess::outputReady,&model,&TerminalModel::feed);connect(&model,&TerminalModel::outputReady,&process,[&](const QByteArray &b){process.write(b);});QSignalSpy cwd(&model,&TerminalModel::workingDirectoryChanged);auto command=ConPtyProcess::defaultShell();QVERIFY(command.first().contains("powershell",Qt::CaseInsensitive)||command.first().contains("pwsh",Qt::CaseInsensitive));command.insert(1,"-NoProfile");QString error;QVERIFY2(process.start(scratch.path(),command,{100,30},&error),qPrintable(error));QTRY_VERIFY_WITH_TIMEOUT(!cwd.isEmpty(),10000);QCOMPARE(QDir::cleanPath(cwd.last().first().toString()),QDir::cleanPath(scratch.path()));process.write(QString::fromUtf8("Set-Location -LiteralPath './next 日本'\r").toUtf8());QTRY_VERIFY_WITH_TIMEOUT(cwd.last().first().toString().contains(QString::fromUtf8("next 日本")),10000);process.write("exit\r");QTRY_VERIFY_WITH_TIMEOUT(!process.isRunning(),5000);}
void TerminalTest::tabAltGrAndAtomicClipboardInput(){TerminalWidget widget;QSignalSpy input(widget.terminalModel(),&TerminalModel::outputReady);QTest::keyClick(&widget,Qt::Key_Tab);QCOMPARE(input.size(),1);QCOMPARE(input.takeFirst().first().toByteArray(),QByteArray("\t"));widget.terminalModel()->key(Qt::Key_Q,Qt::ControlModifier|Qt::AltModifier,"@");QCOMPARE(input.takeFirst().first().toByteArray(),QByteArray("@"));widget.terminalModel()->feed("\x1b[?2004h");widget.terminalModel()->paste("safe\x1b[201~\ntext");QCOMPARE(input.size(),1);QCOMPARE(input.first().first().toByteArray(),QByteArray("\x1b[200~safe[201~\rtext\x1b[201~"));}
void TerminalTest::emptyRepeatCombiningFloodAndGlobalReverse(){TerminalModel model({40,5});QElapsedTimer timer;timer.start();model.feed("\x1b[999b");QCOMPARE(model.cursor(),QPoint(0,0));model.feed("A");for(int i=0;i<10000;++i)model.feed(QByteArray::fromHex("cc81"));QVERIFY(model.cell(0,0).text.size()<=VTERM_MAX_CHARS_PER_CELL);model.feed("\r\nOK");QVERIFY(model.plainText().contains("OK"));const auto before=model.cell(0,0);model.feed("\x1b[?5h");QCOMPARE(model.cell(0,0).foreground,before.background);QCOMPARE(model.cell(0,0).background,before.foreground);QVERIFY(timer.elapsed()<3000);}
int main(int argc,char **argv){for(int i=1;i<argc;++i){if(std::string(argv[i])=="--terminal-fixture")return fixture();if(std::string(argv[i])=="--descendant"){Sleep(INFINITE);return 0;}}QApplication app(argc,argv);TerminalTest test;return QTest::qExec(&test,argc,argv);}
#include "test_terminal.moc"
