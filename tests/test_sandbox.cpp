#include "sandbox/SandboxHost.h"
#include "core/ToolExecutor.h"
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace ade;

struct SandboxResult {
    bool arrived = false;
    bool error = true;
    QString raw;
    QJsonObject json;
    QString value() const { return json.value("result").toString(); }
};

class SandboxTests final : public QObject {
    Q_OBJECT
private:
    std::unique_ptr<QTemporaryDir> temporary;
    std::unique_ptr<SandboxHost> host;
    std::unique_ptr<ToolExecutor> files;
    int nextId = 0;
    SandboxResult run(const QString& script, const QString& requestedId = {}) {
        const QString id = requestedId.isEmpty() ? QStringLiteral("call-%1").arg(++nextId) : requestedId;
        QSignalSpy spy(host.get(), &SandboxHost::completed);
        host->execute(id, "executeSandboxJs", {{"explanation", "Test native sandbox"}, {"script", script}});
        QElapsedTimer elapsed; elapsed.start();
        while (elapsed.elapsed() < 8000) {
            for (const auto& arguments : spy) if (arguments.at(0).toString() == id) {
                const QString raw = arguments.at(1).toString();
                return {true, arguments.at(2).toBool(), raw, QJsonDocument::fromJson(raw.toUtf8()).object()};
            }
            spy.wait(100);
        }
        return {false, true, "No completion within eight seconds", {}};
    }
private slots:
    void init() {
        temporary = std::make_unique<QTemporaryDir>();
        QVERIFY(temporary->isValid());
        host = std::make_unique<SandboxHost>();
        host->setWorkspace(temporary->path());
        host->setArtifactDirectory(temporary->filePath("attachments"));
        host->setExecutionLimits(2000, 3000, 5000);
        files = std::make_unique<ToolExecutor>();
        QVERIFY(files->setWorkspace(temporary->path()));
        connect(host.get(), &SandboxHost::fileRequest, files.get(), &ToolExecutor::execute);
        connect(files.get(), &ToolExecutor::completed, host.get(), &SandboxHost::completeFileRequest);
    }
    void cleanup() { host.reset(); files.reset(); temporary.reset(); }

    void asyncPersistentGlobalsAndAgentIsolation() {
        const auto first = run("globalThis.count=40; await new Promise(r=>setTimeout(r,15)); return ++globalThis.count;");
        QVERIFY2(first.arrived && !first.error, qPrintable(first.raw)); QCOMPARE(first.value(), "41");
        QCOMPARE(run("return ++globalThis.count;").value(), "42");
        host->setAgentId("other");
        QCOMPARE(run("return typeof globalThis.count;").value(), "undefined");
        host->setAgentId("default");
        QCOMPARE(run("return globalThis.count;").value(), "42");
        host->resetAgent("default");
        QCOMPARE(run("return typeof globalThis.count;").value(), "undefined");
    }

    void outputReturnConsoleAndTimers() {
        QSignalSpy output(host.get(), &SandboxHost::output);
        QSignalSpy console(host.get(), &SandboxHost::consoleMessage);
        const auto result = run("console.warn('diagnostic'); API.output('first'); API.output({a:1}); "
            "const v=await new Promise(r=>setTimeout(r,10,7)); return {value:v};");
        QVERIFY2(!result.error, qPrintable(result.raw));
        QCOMPARE(result.value(), "first\n{\"a\":1}\n{\"value\":7}");
        QCOMPARE(output.count(), 2); QCOMPARE(console.count(), 1);
        QCOMPARE(console.first().at(1).toString(), "warn");
        QCOMPARE(run("let n=0; await new Promise(r=>{const t=setInterval(()=>{if(++n===3){clearInterval(t);r();}},5);}); return n;").value(), "3");
        QCOMPARE(run("return null;").value(), QString());
        QCOMPARE(run("API.output(undefined);API.output('one','ignored');return ()=>{};").value(), "\none");
    }

    void javascriptErrorsAndUnsupportedApisAreExplicit() {
        const auto error = run("globalThis.survives=1; API.output('partial'); throw new Error('intentional');");
        QVERIFY(error.arrived && error.error); QVERIFY(error.raw.contains("intentional"));
        QCOMPARE(error.json.value("output").toString(), "partial");
        QCOMPARE(run("return globalThis.survives;").value(), "1");
        for (const auto& script : {"require('child_process');", "await importModule('x');", "require('events');"}) {
            const auto result = run(script);
            QVERIFY2(result.arrived && result.error && result.raw.contains("not implemented"), qPrintable(result.raw));
        }
        const auto noNetwork=run("await fetch('http://127.0.0.1/');");
        QVERIFY(noNetwork.error); QVERIFY(noNetwork.raw.contains("No network broker"));
        const auto syntax = run("const = ;"); QVERIFY(syntax.error); QVERIFY(syntax.raw.contains("SyntaxError"));
        QCOMPARE(run("return [typeof window, typeof document, process.env.SECRET_TEST_ONLY, typeof require('fs').promises.readFile];").value(), "[\"undefined\",\"undefined\",null,\"function\"]");
    }

    void cdpPromisesCorrelateAndReject() {
        connect(host.get(), &SandboxHost::browserRequest, host.get(), [this](const QString& id, const QString& tab, const QString& method, const QJsonObject& params) {
            QCOMPARE(tab, "t_test");
            if (method == "Test.error") { host->completeBrowserRequest(id, "controlled failure", true); return; }
            if (method == "Test.invalid") { host->completeBrowserRequest(id, "not JSON", false); return; }
            const int value = params.value("value").toInt();
            QTimer::singleShot(value == 1 ? 35 : 5, host.get(), [this, id, value] {
                host->completeBrowserRequest(id, QStringLiteral("{\"result\":%1}").arg(value), false);
            });
        });
        const auto result = run("const x=await Promise.all([API.sendCDP('t_test','Test.get',{value:1}),API.sendCDP('t_test','Test.get',{value:2})]); return x.map(v=>v.result);");
        QVERIFY2(!result.error, qPrintable(result.raw)); QCOMPARE(result.value(), "[1,2]");
        const auto error = run("return await API.sendCDP('t_test','Test.error');");
        QVERIFY(error.error); QVERIFY(error.raw.contains("controlled failure"));
        const auto invalid = run("return await API.sendCDP('t_test','Test.invalid');");
        QVERIFY(invalid.error); QVERIFY(invalid.raw.contains("invalid JSON"));
    }

    void mountedFilesPreserveUtf8BytesAndScope() {
        const QString content = QString(QChar(0xfeff)) + QString::fromUtf8("Hello\r\nArabic: \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7");
        const auto encoded = QJsonDocument(QJsonArray{content}).toJson(QJsonDocument::Compact);
        const QString literal = QString::fromUtf8(encoded.mid(1, encoded.size()-2));
        const auto result = run("await fsPromises.writeFile('ws1/a.txt'," + literal + "); return await require('node:fs/promises').readFile('w1/a.txt','utf8');");
        QVERIFY2(!result.error, qPrintable(result.raw)); QCOMPARE(result.value(), content);
        QFile saved(temporary->filePath("a.txt")); QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), content.toUtf8());
        QCOMPARE(run("return (await fsPromises.readFile('a.txt')).toString();").value(), content);
        QCOMPARE(run("return await new Promise((resolve,reject)=>fs.readFile('a.txt','utf8',(e,v)=>e?reject(e):resolve(v)));").value(), content);
        QFile binary(temporary->filePath("bad.bin")); QVERIFY(binary.open(QIODevice::WriteOnly)); binary.write(QByteArray::fromHex("fffe80")); binary.close();
        QVERIFY(run("return await fsPromises.readFile('bad.bin','utf8');").error);
        QVERIFY(run("await fsPromises.writeFile('../escape.txt','bad');").error);
        QVERIFY(run("return await fsPromises.readFile('ws2/a.txt','utf8');").error);
        const auto binaryCopy=run("await fsPromises.writeFile('invalid.bin',await fsPromises.readFile('bad.bin'));await fsPromises.writeFile('incomplete.bin',new Uint8Array([65,226,130]));return [(await fsPromises.readFile('invalid.bin')).toString('base64'),Array.from(await fsPromises.readFile('incomplete.bin'))];");
        QVERIFY2(!binaryCopy.error,qPrintable(binaryCopy.raw));QCOMPARE(binaryCopy.value(),"[\"//6A\",[65,226,130]]");
        QVERIFY(run("return await fsPromises.readFile('incomplete.bin','utf8');").error);
        QFile copied(temporary->filePath("invalid.bin"));QVERIFY(copied.open(QIODevice::ReadOnly));QCOMPARE(copied.readAll(),QByteArray::fromHex("fffe80"));
        QVERIFY(run("await fsPromises.writeFile('a.txt','bad',{flag:'a'});").error);
        const auto bufferWrite=run("await fsPromises.writeFile('bom.txt',Buffer.from([239,187,191,65,13,10]));return await fsPromises.readFile('bom.txt','utf8');");
        QVERIFY2(!bufferWrite.error,qPrintable(bufferWrite.raw));
        QCOMPARE(bufferWrite.value().toUtf8(), QByteArray::fromHex("efbbbf410d0a"));
    }

    void appsMountWithoutWorkspaceAndBinaryCallbacks() {
        const auto appRoot=temporary->filePath("owned-apps");QVERIFY(QDir().mkpath(appRoot));
        QVERIFY(files->setAppsDirectory(appRoot));host->setAppsDirectory(appRoot);
        files->setWorkspace({});host->setWorkspace({});
        const auto result=run("await fsPromises.mkdir('apps/demo/assets',{recursive:true});"
            "await fsPromises.writeFile('apps/demo/index.html','<!doctype html><body>Authored</body>');"
            "await new Promise((resolve,reject)=>fs.writeFile('apps/demo/assets/data.bin',new Uint8Array([0,255,128,65]).buffer,e=>e?reject(e):resolve()));"
            "const bytes=await new Promise((resolve,reject)=>fs.readFile('apps/demo/assets/data.bin',(e,v)=>e?reject(e):resolve(v)));"
            "return {bytes:Array.from(bytes),files:(await fsPromises.readdir('apps/demo',{recursive:true})).sort()};");
        QVERIFY2(result.arrived&&!result.error,qPrintable(result.raw));
        const auto value=QJsonDocument::fromJson(result.value().toUtf8()).object();
        QCOMPARE(value["bytes"].toArray(),QJsonArray({0,255,128,65}));
        QCOMPARE(value["files"].toArray(),QJsonArray({"assets","assets/data.bin","index.html"}));
        QFile asset(appRoot+"/demo/assets/data.bin");QVERIFY(asset.open(QIODevice::ReadOnly));QCOMPARE(asset.readAll(),QByteArray::fromHex("00ff8041"));
        QVERIFY(run("await fsPromises.mkdir('apps/demo');").error);
        QVERIFY(run("await fsPromises.mkdir('apps/missing/deep');").error);
        QVERIFY(run("await fsPromises.writeFile('apps/missing/deep.txt','must not create parents');").error);
        QVERIFY(run("await fsPromises.writeFile('apps/missing/deep.bin',Buffer.from([0]));").error);
        const auto created=run("const first=await fsPromises.mkdir('apps/new/deep',{recursive:true});const repeated=await fsPromises.mkdir('apps/new/deep',{recursive:true});return [first,typeof repeated];");
        QVERIFY2(!created.error,qPrintable(created.raw));const auto directories=QJsonDocument::fromJson(created.value().toUtf8()).array();QCOMPARE(directories[0].toString(),appRoot+"/new");QCOMPARE(directories[1].toString(),"undefined");
        QVERIFY(run("await fsPromises.writeFile('relative.txt','no workspace');").error);
        QVERIFY(run("await fsPromises.writeFile('ws1/no.txt','no workspace');").error);
        QVERIFY(run("await fsPromises.writeFile('apps/../escape.txt','escape');").error);
        QVERIFY(run("await fsPromises.readdir('apps',{withFileTypes:true});").error);
        QVERIFY(!QFileInfo::exists(temporary->filePath("escape.txt")));
    }

    void binaryReadsAreNeverSilentlyTruncated() {
        QByteArray bytes(200000,Qt::Uninitialized);for(int i=0;i<bytes.size();++i)bytes[i]=static_cast<char>(i%256);
        QFile input(temporary->filePath("large.bin"));QVERIFY(input.open(QIODevice::WriteOnly));QCOMPARE(input.write(bytes),bytes.size());input.close();
        const auto result=run("const bytes=await fsPromises.readFile('large.bin');await fsPromises.writeFile('copy.bin',bytes);return [bytes.length,bytes[65536],bytes[199999]];");
        QVERIFY2(!result.error,qPrintable(result.raw));QCOMPARE(result.value(),"[200000,0,63]");
        QFile copy(temporary->filePath("copy.bin"));QVERIFY(copy.open(QIODevice::ReadOnly));QCOMPARE(copy.readAll(),bytes);
        QVERIFY(input.open(QIODevice::WriteOnly));QVERIFY(input.resize(4*1024*1024+1));input.close();
        const auto excessive=run("return await fsPromises.readFile('large.bin');");QVERIFY(excessive.error);QVERIFY(excessive.raw.contains("4 MiB"));
        QCOMPARE(run("return 42;").value(),"42");
    }

    void attachmentsAndBasicBuffer() {
        QSignalSpy attachment(host.get(), &SandboxHost::attachmentCreated);
        const auto result = run("const b=Buffer.from('hello ✓'); return await API.createAttachment('../../sample.txt',b);");
        QVERIFY2(!result.error, qPrintable(result.raw)); QVERIFY(result.value().endsWith("sample.txt")); QVERIFY(!result.value().contains('/'));
        QCOMPARE(attachment.count(), 1);
        const QString path = attachment.first().at(1).toString();
        QVERIFY(path.startsWith(temporary->filePath("attachments")));
        QFile saved(path); QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), QString::fromUtf8("hello ✓").toUtf8());
        QCOMPARE(run("return [Buffer.from('aGVsbG8=','base64').toString(),Buffer.alloc(0).toString(),btoa('hi'),atob('aGk=')];").value(), "[\"hello\",\"\",\"aGk=\",\"hi\"]");
        QVERIFY(run("await API.createAttachment('bad.txt','%%%');").error);
    }

    void cancellationInterruptsSynchronousLoopAndResetsContext() {
        host->setExecutionLimits(5000, 6000, 7000);
        connect(host.get(), &SandboxHost::output, host.get(), [this](const QString& id, const QString&) { host->cancel(id); });
        QElapsedTimer elapsed; elapsed.start();
        const auto result = run("globalThis.poison=1; API.output('started'); while(true){};");
        QVERIFY2(result.arrived && result.error, qPrintable(result.raw)); QVERIFY(result.raw.contains("canceled"));
        QVERIFY(elapsed.elapsed() < 3000);
        QCOMPARE(run("return typeof globalThis.poison;").value(), "undefined");
    }

    void cancellationCleansBrokersAndIgnoresLateReply() {
        QString request;
        QSignalSpy cancellations(host.get(), &SandboxHost::brokerCancellationRequested);
        connect(host.get(), &SandboxHost::browserRequest, host.get(), [this, &request](const QString& id, const QString&, const QString&, const QJsonObject&) { request=id; host->cancel("reused"); });
        const auto result = run("return await API.sendCDP('t','Test.wait');", "reused");
        QVERIFY(result.error); QVERIFY(!request.isEmpty()); QCOMPARE(cancellations.count(), 1);
        host->completeBrowserRequest(request, "{\"late\":true}", false);
        QCOMPARE(run("return 'fresh';", "reused").value(), "fresh");
    }

    void synchronousAndAsyncTimeLimits() {
        host->setExecutionLimits(60, 2000, 3000);
        const auto sync = run("globalThis.poison=1; while(true){};");
        QVERIFY2(sync.arrived && sync.error && sync.raw.contains("synchronous time limit"), qPrintable(sync.raw));
        QCOMPARE(run("return typeof globalThis.poison;").value(), "undefined");
        host->setExecutionLimits(1000, 100, 2000);
        const auto idle = run("return await new Promise(()=>{});");
        QVERIFY2(idle.error && idle.raw.contains("inactivity"), qPrintable(idle.raw));
        host->setExecutionLimits(1000, 140, 2000);
        const auto heartbeat = run("for(let i=0;i<5;i++){await new Promise(r=>setTimeout(r,45));API.output('tick');}return 'done';");
        QVERIFY2(!heartbeat.error && heartbeat.value().endsWith("done"), qPrintable(heartbeat.raw));
        host->setExecutionLimits(1000, 140, 200);
        const auto hard = run("while(true){await new Promise(r=>setTimeout(r,20));API.output('tick');}");
        QVERIFY2(hard.error && hard.raw.contains("hard time limit"), qPrintable(hard.raw));
    }

    void asyncContinuationInfiniteLoopIsBounded() {
        host->setExecutionLimits(60, 1000, 2000);
        const auto result = run("await Promise.resolve(); while(true){};");
        QVERIFY2(result.arrived && result.error && result.raw.contains("synchronous time limit"), qPrintable(result.raw));
        QCOMPARE(run("return 42;").value(), "42");
    }

    void memoryLimitAndOutputLimitRecover() {
        host->setExecutionLimits(2000, 3000, 5000, 8ull*1024*1024);
        const auto memory = run("globalThis.large=[]; for(let i=0;i<1000000;i++)globalThis.large.push({i,text:'x'.repeat(100)});return 'unexpected';");
        QVERIFY2(memory.arrived && memory.error, qPrintable(memory.raw));
        QVERIFY2(memory.raw.contains("memory", Qt::CaseInsensitive), qPrintable(memory.raw));
        QCOMPARE(run("return typeof globalThis.large;").value(), "undefined");
        const auto excessive = run("return 'x'.repeat(65537);");
        QVERIFY(excessive.error); QVERIFY(excessive.raw.contains("65536"));
        const auto messages = run("for(let i=0;i<2000;i++)API.output('');");
        QVERIFY(messages.error); QVERIFY(messages.raw.contains("1024"));
        QCOMPARE(run("return 9;").value(), "9");
    }

    void oversizedBrokerResponseFailsWithoutWorkerCrash() {
        host->setExecutionLimits(2000,3000,5000,8ull*1024*1024);
        connect(host.get(),&SandboxHost::browserRequest,host.get(),[this](const QString& id,const QString&,const QString&,const QJsonObject&){
            const QString value = QLatin1Char('"') + QString(12*1024*1024, QLatin1Char('x')) + QLatin1Char('"');
            host->completeBrowserRequest(id,value,false);
        });
        const auto result=run("return await API.sendCDP('t','Test.large');");
        QVERIFY2(result.arrived&&result.error&&result.raw.contains("memory"),qPrintable(result.raw));
        QCOMPARE(run("return 42;").value(),"42");
    }

    void independentAgentsAndDuplicateBusyCalls() {
        QSignalSpy spy(host.get(), &SandboxHost::completed);
        host->execute("slow", "executeSandboxJs", {{"explanation","Wait"},{"script","await new Promise(r=>setTimeout(r,80));return 'a';"}});
        host->execute("busy", "executeSandboxJs", {{"explanation","Wait"},{"script","return 'bad';"}});
        QCOMPARE(spy.count(), 1); QCOMPARE(spy.first().at(0).toString(), "busy"); QVERIFY(spy.first().at(2).toBool());
        host->setAgentId("second");
        const auto result = run("return 'b';"); QCOMPARE(result.value(), "b");
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 3, 3000);
    }

    void destructionDuringInfiniteWorkAndMissingWorker() {
        QSignalSpy output(host.get(), &SandboxHost::output);
        host->setExecutionLimits(5000,6000,7000);
        host->execute("destroy", "executeSandboxJs", {{"explanation","Wait"},{"script","API.output('ready');while(true){};"}});
        QVERIFY(output.wait(3000));
        QElapsedTimer elapsed; elapsed.start(); host.reset(); QVERIFY(elapsed.elapsed() < 2000);
        host = std::make_unique<SandboxHost>();
        host->setWorkerExecutable(temporary->filePath("missing-worker.exe"));
        const auto failed = run("return 1;"); QVERIFY(failed.arrived && failed.error); QVERIFY(failed.raw.contains("Cannot start"));
    }

    void reentrantDestructionFromSignals() {
        for (const auto& script : {"return 1;", "API.output('delete');await new Promise(()=>{});", "return await API.sendCDP('t','Test.delete');"}) {
            auto* doomed = new SandboxHost;
            QPointer<SandboxHost> guard(doomed);
            if (QString(script).startsWith("return 1")) connect(doomed,&SandboxHost::completed,this,[doomed]{delete doomed;});
            else if (QString(script).startsWith("API.output")) connect(doomed,&SandboxHost::output,this,[doomed]{delete doomed;});
            else connect(doomed,&SandboxHost::browserRequest,this,[doomed]{delete doomed;});
            doomed->execute("delete","executeSandboxJs",{{"explanation","Destroy"},{"script",script}});
            QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(),3000);
        }
        auto* missing = new SandboxHost;
        QPointer<SandboxHost> guard(missing);
        missing->setWorkerExecutable(temporary->filePath("missing.exe"));
        connect(missing,&SandboxHost::completed,this,[missing]{delete missing;});
        missing->execute("missing","executeSandboxJs",{{"explanation","Destroy"},{"script","return 1;"}});
        QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(),3000);
    }

    void failedStartupObserverCanDestroyProtectedHostStorage() {
#ifdef Q_OS_WIN
        void* storage=VirtualAlloc(nullptr,sizeof(SandboxHost),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        QVERIFY(storage);
        QPointer<SandboxHost> doomed=new(storage) SandboxHost;
        const auto cleanup=qScopeGuard([&]{if(doomed)doomed.data()->~SandboxHost();VirtualFree(storage,0,MEM_RELEASE);});
#else
        QPointer<SandboxHost> doomed=new SandboxHost;
        const auto cleanup=qScopeGuard([&]{delete doomed.data();});
#endif
        doomed->setWorkerExecutable(temporary->filePath("missing-protected-worker.exe"));
        int completed=0;
        connect(doomed,&SandboxHost::completed,this,[&](const QString& id,const QString& output,bool error){
            ++completed;QCOMPARE(id,"missing-protected");QVERIFY(error);QVERIFY(output.contains("Cannot start"));
#ifdef Q_OS_WIN
            doomed.data()->~SandboxHost();
            DWORD oldProtection=0;
            QVERIFY(VirtualProtect(storage,sizeof(SandboxHost),PAGE_NOACCESS,&oldProtection));
#else
            delete doomed.data();
#endif
        });
        // QProcess::start can report failure synchronously. The observer destroys
        // the host before execute() regains control from ensureProcess().
        doomed->execute("missing-protected","executeSandboxJs",{{"explanation","Fail worker startup"},{"script","return 1;"}});
        QTRY_VERIFY_WITH_TIMEOUT(doomed.isNull(),3000);QCOMPARE(completed,1);
        QCoreApplication::processEvents();QCOMPARE(completed,1);
    }

    void startupFailureReplacementDoesNotSendAnObsoleteExecution() {
        const auto worker=QCoreApplication::applicationDirPath()+"/ADE-sandbox-worker.exe";
        host->setWorkerExecutable(temporary->filePath("missing-replaced-worker.exe"));
        QSignalSpy results(host.get(),&SandboxHost::completed),console(host.get(),&SandboxHost::consoleMessage);
        bool replaced=false;
        connect(host.get(),&SandboxHost::completed,host.get(),[&](const QString& id,const QString&,bool error){
            if(id!="reused-startup"||replaced)return;
            QVERIFY(error);replaced=true;host->setWorkerExecutable(worker);
            host->execute(id,"executeSandboxJs",{{"explanation","Replace failed startup"},{"script","globalThis.replacementState=42;return 'replacement';"}});
        });
        host->execute("reused-startup","executeSandboxJs",{{"explanation","Start obsolete script"},{"script","console.log('OBSOLETE_STARTUP_EXECUTED');globalThis.obsolete=true;return 'obsolete';"}});
        QTRY_COMPARE_WITH_TIMEOUT(results.count(),2,4000);QVERIFY(replaced);QVERIFY(results.first()[2].toBool());QVERIFY(!results.last()[2].toBool());
        QCOMPARE(QJsonDocument::fromJson(results.last()[1].toString().toUtf8()).object()["result"].toString(),"replacement");
        const auto retained=run("return globalThis.replacementState;");QVERIFY2(!retained.error,qPrintable(retained.raw));QCOMPARE(retained.value(),"42");
        for(const auto& row:console)QVERIFY(!row[2].toString().contains("OBSOLETE_STARTUP_EXECUTED"));
    }

    void cancelAllObserverCanDestroyProtectedHostWithTwoAgents() {
#ifdef Q_OS_WIN
        void* storage=VirtualAlloc(nullptr,sizeof(SandboxHost),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);QVERIFY(storage);
        QPointer<SandboxHost> doomed=new(storage) SandboxHost;
        const auto cleanup=qScopeGuard([&]{if(doomed)doomed.data()->~SandboxHost();VirtualFree(storage,0,MEM_RELEASE);});
#else
        QPointer<SandboxHost> doomed=new SandboxHost;
        const auto cleanup=qScopeGuard([&]{delete doomed.data();});
#endif
        QSignalSpy started(doomed,&SandboxHost::output);int resets=0;
        doomed->setAgentId("first");doomed->execute("first-call","executeSandboxJs",{{"explanation","Wait in first agent"},{"script","API.output('ready');await new Promise(()=>{});"}});
        doomed->setAgentId("second");doomed->execute("second-call","executeSandboxJs",{{"explanation","Wait in second agent"},{"script","API.output('ready');await new Promise(()=>{});"}});
        QTRY_COMPARE_WITH_TIMEOUT(started.count(),2,4000);
        connect(doomed,&SandboxHost::contextReset,this,[&](const QString&){
            ++resets;
#ifdef Q_OS_WIN
            doomed.data()->~SandboxHost();DWORD oldProtection=0;
            QVERIFY(VirtualProtect(storage,sizeof(SandboxHost),PAGE_NOACCESS,&oldProtection));
#else
            delete doomed.data();
#endif
        });
        doomed->cancelAll();QVERIFY(doomed.isNull());QCOMPARE(resets,1);
        QCoreApplication::processEvents();QCOMPARE(resets,1);
    }

    void persistentEventsCanSendBackgroundAppMessages() {
        QString listener;QStringList calls;QSignalSpy unsubscribed(host.get(),&SandboxHost::eventUnsubscriptionRequested);
        connect(host.get(),&SandboxHost::eventSubscriptionRequested,host.get(),[&listener](const QString& id,const QString& agent,const QString& kind,const QJsonObject& args){QCOMPARE(agent,"default");QCOMPARE(kind,"cdp");QCOMPARE(args["event"].toString(),"Runtime.bindingCalled");listener=id;});
        connect(host.get(),&SandboxHost::appRequest,host.get(),[this,&calls](const QString& id,const QString& agent,const QString& operation,const QJsonObject& args){QCOMPARE(agent,"default");QCOMPARE(operation,"send");QCOMPARE(args["appId"].toString(),"app");calls.append(id);QTimer::singleShot(80,host.get(),[this,id]{host->completeAppRequest(id,QJsonValue(QJsonValue::Null),{});});});
        const auto setup=run("globalThis.events=[];globalThis.ack=false;globalThis.unsubscribe=API.onCDPEvent('t','Runtime.bindingCalled',e=>{events.push(e);API.sendMessage('app',e,{pluginId:'p'}).then(()=>globalThis.ack=true);});return 'registered';");
        QVERIFY2(!setup.error,qPrintable(setup.raw));QVERIFY(!listener.isEmpty());
        host->deliverEvent(listener,QJsonObject{{"payload","after-IIFE"}});QTRY_COMPARE_WITH_TIMEOUT(calls.size(),1,3000);
        QCOMPARE(run("return events[0].payload;").value(),"after-IIFE");QTest::qWait(120);QCOMPARE(run("return ack;").value(),"true");
        QCOMPARE(run("unsubscribe();return 'off';").value(),"off");QCOMPARE(unsubscribed.count(),1);host->deliverEvent(listener,QJsonObject{{"payload","late"}});QTest::qWait(30);QCOMPARE(run("return events.length;").value(),"1");
    }

    void eventCallbacksAreBoundedAndQueueOverflowUnsubscribes() {
        QStringList listeners;QSignalSpy resets(host.get(),&SandboxHost::contextReset),unsubscribed(host.get(),&SandboxHost::eventUnsubscriptionRequested);
        connect(host.get(),&SandboxHost::eventSubscriptionRequested,host.get(),[&listeners](const QString& id){listeners.append(id);});
        QVERIFY(!run("API.onMessage('a',()=>{throw new Error('listener error');});API.onMessage('b',()=>{globalThis.safe=42;});return 1;").error);
        QCOMPARE(listeners.size(),2);host->deliverEvent(listeners[0],1);host->deliverEvent(listeners[1],2);QTest::qWait(50);QCOMPARE(run("return safe;").value(),"42");QCOMPARE(resets.count(),0);
        QVERIFY(!run("API.onMessage('loop',()=>{while(true){}});return 1;").error);host->deliverEvent(listeners.last(),1);QTRY_COMPARE_WITH_TIMEOUT(resets.count(),1,3000);QCOMPARE(unsubscribed.count(),3);QCOMPARE(run("return typeof safe;").value(),"undefined");
        listeners.clear();unsubscribed.clear();QVERIFY(!run("globalThis.n=0;API.onMessage('queue',()=>n++);return 1;").error);QCOMPARE(listeners.size(),1);
        for(int i=0;i<100;++i)host->deliverEvent(listeners.first(),i);
        QCOMPARE(unsubscribed.count(),1);QTest::qWait(50);QVERIFY(run("return n;").value().toInt()<=65);
    }

    void listenerIdentitiesAreIsolatedAcrossAgentsAndReset() {
        QHash<QString,QString> listeners;connect(host.get(),&SandboxHost::eventSubscriptionRequested,host.get(),[&listeners](const QString& id,const QString& agent){listeners[agent]=id;});
        QCOMPARE(run("globalThis.value='a';API.onMessage('app',e=>globalThis.value=e);return 1;").value(),"1");host->setAgentId("second");QCOMPARE(run("globalThis.value='b';API.onMessage('app',e=>globalThis.value=e);return 2;").value(),"2");
        QVERIFY(listeners["default"]!=listeners["second"]);host->deliverEvent(listeners["default"],"only-a");QTest::qWait(50);QCOMPARE(run("return value;").value(),"b");host->setAgentId("default");QCOMPARE(run("return value;").value(),"only-a");
        const auto old=listeners["default"];host->resetAgent("default");QCOMPARE(run("globalThis.value='fresh';API.onMessage('app',e=>globalThis.value=e);return 3;").value(),"3");host->deliverEvent(old,"stale");QTest::qWait(30);QCOMPARE(run("return value;").value(),"fresh");
    }

    void idleAttachmentsPersistAndEventsUseOnlyTheActiveSameAgentTool() {
        QString listener;
        connect(host.get(),&SandboxHost::eventSubscriptionRequested,host.get(),[&listener](const QString& id){listener=id;});
        QSignalSpy attachments(host.get(),&SandboxHost::attachmentCreated);
        QVERIFY(!run("globalThis.saved='';API.onMessage('attachment',()=>API.createAttachment('event.txt',Buffer.from('event bytes')).then(name=>{globalThis.saved=name;if(globalThis.release){const done=release;globalThis.release=null;done();}}));return 1;").error);
        QVERIFY(!listener.isEmpty());
        host->deliverEvent(listener,1);
        QTRY_COMPARE_WITH_TIMEOUT(attachments.count(),1,3000);
        QVERIFY(attachments.first()[0].toString().isEmpty());
        QFile saved(attachments.first()[1].toString());QVERIFY(saved.open(QIODevice::ReadOnly));QCOMPARE(saved.readAll(),QByteArray("event bytes"));saved.close();
        const auto idleName=run("return saved;");QVERIFY2(!idleName.error,qPrintable(idleName.raw));QCOMPARE(idleName.value(),QFileInfo(attachments.first()[1].toString()).fileName());
        connect(host.get(),&SandboxHost::output,host.get(),[this,&listener](const QString& id){if(id=="active-event"||id=="other-agent")host->deliverEvent(listener,2);});
        const auto active=run("const waiting=new Promise(r=>globalThis.release=r);API.output('waiting');await waiting;return saved;","active-event");
        QVERIFY2(active.arrived&&!active.error,qPrintable(active.raw));QCOMPARE(attachments.count(),2);QCOMPARE(attachments.last()[0].toString(),"active-event");
        host->setAgentId("other");
        const auto other=run("API.output('other agent');await new Promise(r=>setTimeout(r,120));return 'done';","other-agent");
        QVERIFY2(other.arrived&&!other.error,qPrintable(other.raw));QTRY_COMPARE_WITH_TIMEOUT(attachments.count(),3,1000);QVERIFY(attachments.last()[0].toString().isEmpty());
    }

    void attachmentNotificationPrecedesResultAndJunctionStorageRejects() {
        QStringList order;connect(host.get(),&SandboxHost::attachmentCreated,host.get(),[&order]{order.append("attachment");});connect(host.get(),&SandboxHost::completed,host.get(),[&order]{order.append("completed");});
        QVERIFY(!run("return await API.createAttachment('ordered.txt',Buffer.from('ok'));").error);QCOMPARE(order,QStringList({"attachment","completed"}));
#ifdef Q_OS_WIN
        const auto outside=temporary->filePath("outside"),link=temporary->filePath("artifact-link");QVERIFY(QDir().mkdir(outside));
        QProcess process;process.start("cmd.exe",{"/d","/c","mklink","/J",QDir::toNativeSeparators(link),QDir::toNativeSeparators(outside)});QVERIFY(process.waitForFinished(5000));QCOMPARE(process.exitCode(),0);
        host->setArtifactDirectory(link+"/nested");const auto denied=run("return await API.createAttachment('escape.txt',Buffer.from('bad'));");
        const bool removed=RemoveDirectoryW(QDir::toNativeSeparators(link).toStdWString().c_str());QVERIFY(removed);QVERIFY(denied.error);QVERIFY(denied.raw.contains("junction"));QVERIFY(QDir(outside).entryList(QDir::AllEntries|QDir::NoDotAndDotDot).isEmpty());
#endif
    }
};

QTEST_GUILESS_MAIN(SandboxTests)
#include "test_sandbox.moc"
