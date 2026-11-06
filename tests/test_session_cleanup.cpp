#include "ui/SessionStore.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <stdexcept>

using namespace ade;
namespace {
Conversation chat(const QString& id){Conversation result;result.id=id;result.title="Fixture "+id;return result;}
bool put(const QString& path,const QByteArray& bytes){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();}
QByteArray bytes(const QString& path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();}
QString owned(const QString& directory,const QString& id){return directory+"/owned-"+id+".bin";}
bool junction(const QString& path,const QString& target){QProcess process;process.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(path),QDir::toNativeSeparators(target)});return process.waitForFinished(5000)&&process.exitCode()==0;}
bool process(SessionStore& store,const QList<Conversation>& sessions,const SessionStore::Cleanup& callback,QString* error){
#ifdef ADE_SESSION_CLEANUP_BEFORE
    // Reconstructed old MainWindow::retryConversationCleanup sequence. Actual
    // current SessionStore I/O is used; only the absent lock span is reproduced.
    const auto pending=store.pendingCleanup();if(pending.isEmpty())return true;
    if(!store.save(sessions,error))return false;
    QStringList remaining,errors;
    for(const auto& id:pending){QString failure;if(!callback(id,store.directory(),&failure)){remaining.append(id);errors.append(failure);}}
    if(remaining!=pending&&!store.saveWithCleanup(sessions,remaining,error))errors.append(*error);
    if(error)*error=errors.join("; ");
    return errors.isEmpty();
#else
    return store.processPendingCleanup(sessions,callback,error);
#endif
}
bool seed(SessionStore& store,const QString& directory,const QStringList& deleted,QString* error){
    QList<Conversation> initial{chat("keep")};for(const auto& id:deleted){initial.append(chat(id));if(!put(owned(directory,id),"owned bytes"))return false;}
    return store.save(initial,error)&&store.saveWithCleanup({chat("keep")},deleted,error);
}
}
class SessionCleanupTests:public QObject {
    Q_OBJECT
private slots:
    void legacyMissingCleanupFieldLoadsWithoutChangingHistory_data(){
        QTest::addColumn<bool>("populated");
        QTest::newRow("empty-legacy-store")<<false;
        QTest::newRow("existing-legacy-chat")<<true;
    }
    void legacyMissingCleanupFieldLoadsWithoutChangingHistory(){
        QFETCH(bool,populated);QTemporaryDir data;QString error;
        const QJsonArray history{QJsonObject{{"role","user"},{"content","Preserve the original fixture message."}},
            QJsonObject{{"role","assistant"},{"content","Preserved reply"},{"metadata",QJsonObject{{"fixture","history-metadata"}}}}};
        QJsonArray records;
        if(populated)records.append(QJsonObject{{"id","legacy"},{"title","Existing fixture"},{"draft","Unsent fixture draft"},{"messages",history}});
        const auto original=QJsonDocument(QJsonObject{{"schemaVersion",1},{"sessions",records}}).toJson();
        QVERIFY(put(data.filePath("sessions.json"),original));SessionStore store(data.path());
        auto loaded=store.load(&error);
        QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(loaded.size(),populated?1:0);QVERIFY(store.pendingCleanup().isEmpty());
        if(populated){QCOMPARE(loaded.first().messages,history);QCOMPARE(loaded.first().draft,QString("Unsent fixture draft"));}
        QCOMPARE(bytes(data.filePath("sessions.json")),original);
        int cleanups=0;
        QVERIFY(process(store,loaded,[&](const QString&,const QString&,QString*){++cleanups;return true;},&error));QCOMPARE(cleanups,0);
        QCOMPARE(bytes(data.filePath("sessions.json")),original);
        loaded.append(chat("new-chat"));QVERIFY2(store.save(loaded,&error),qPrintable(error));
        const auto saved=QJsonDocument::fromJson(bytes(data.filePath("sessions.json"))).object();
        QVERIFY(saved.value("pendingCleanup").isArray());QVERIFY(saved.value("pendingCleanup").toArray().isEmpty());
        SessionStore restarted(data.path());const auto after=restarted.load(&error);
        QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(after.size(),populated?2:1);
        if(populated){QCOMPARE(after.first().messages,history);QCOMPARE(after.first().draft,QString("Unsent fixture draft"));}
    }
    void invalidCleanupFieldStillFailsClosed_data(){
        QTest::addColumn<QJsonValue>("pending");
        QTest::newRow("null")<<QJsonValue(QJsonValue::Null);
        QTest::newRow("boolean")<<QJsonValue(false);
        QTest::newRow("number")<<QJsonValue(0);
        QTest::newRow("string")<<QJsonValue("");
        QTest::newRow("object")<<QJsonValue(QJsonObject{});
        QJsonArray tooMany;for(int i=0;i<10001;++i)tooMany.append("gone-"+QString::number(i));
        QTest::newRow("too-many-records")<<QJsonValue(tooMany);
    }
    void invalidCleanupFieldStillFailsClosed(){
        QFETCH(QJsonValue,pending);QTemporaryDir data;QString error;
        const QJsonArray records{QJsonObject{{"id","keep"},{"title","Retained fixture"},{"messages",QJsonArray{}}}};
        const auto original=QJsonDocument(QJsonObject{{"schemaVersion",1},{"sessions",records},{"pendingCleanup",pending}}).toJson();
        QVERIFY(put(data.filePath("sessions.json"),original));SessionStore store(data.path());
        QVERIFY(store.load(&error).isEmpty());QVERIFY(error.contains("cleanup records are invalid"));
        QVERIFY(!store.save({chat("replacement")},&error));QVERIFY(!error.isEmpty());
        int cleanups=0;QVERIFY(!process(store,{},[&](const QString&,const QString&,QString*){++cleanups;return true;},&error));
        QCOMPARE(cleanups,0);QCOMPARE(bytes(data.filePath("sessions.json")),original);
    }
    void concurrentWriterCannotResurrectChatDuringCleanup(){
        QTemporaryDir data;SessionStore first(data.path());QString error;QVERIFY2(seed(first,data.path(),{"gone"},&error),qPrintable(error));
        bool called=false,competingSave=true;QString competingError;
        const bool completed=process(first,{chat("keep")},[&](const QString& id,const QString& directory,QString*){
            called=true;SessionStore second(data.path());auto retained=second.load(&competingError);retained.append(chat(id));competingSave=second.saveWithCleanup(retained,{},&competingError);
            return QFile::remove(owned(directory,id));
        },&error);
        QVERIFY(called);
        const auto diagnostic=QString("concurrent writer saved=%1; deleted blob still exists=%2; cleanup error=%3").arg(competingSave).arg(QFileInfo::exists(owned(data.path(),"gone"))).arg(error);
        QVERIFY2(!competingSave,qPrintable(diagnostic));QVERIFY(!competingError.isEmpty());QVERIFY2(completed,qPrintable(error));
        SessionStore check(data.path());const auto retained=check.load(&error);QCOMPARE(retained.size(),1);QCOMPARE(retained.first().id,"keep");QVERIFY(check.pendingCleanup().isEmpty());QVERIFY(!QFileInfo::exists(owned(data.path(),"gone")));
    }
    void staleRevisionCannotDeleteFiles(){
        QTemporaryDir data;SessionStore stale(data.path());QString error;QVERIFY(seed(stale,data.path(),{"gone"},&error));SessionStore writer(data.path());auto current=writer.load(&error);current[0].title="Concurrent edit";QVERIFY(writer.save(current,&error));int calls=0;
        QVERIFY(!process(stale,{chat("keep")},[&](const QString&,const QString&,QString*){++calls;return true;},&error));QCOMPARE(calls,0);QVERIFY(!error.isEmpty());QVERIFY(QFileInfo::exists(owned(data.path(),"gone")));
        SessionStore check(data.path());QCOMPARE(check.load(&error).first().title,"Concurrent edit");QCOMPARE(check.pendingCleanup(),QStringList{"gone"});
    }
    void partialCleanupSurvivesRestartAndRetriesOnlyFailures(){
        QTemporaryDir data;QString error;QStringList called;
        {SessionStore store(data.path());QVERIFY(seed(store,data.path(),{"a","b","c"},&error));
            QVERIFY(!process(store,{chat("keep")},[&](const QString& id,const QString& directory,QString* failure){called.append(id);if(id=="b"){*failure="Fixture cleanup failed";return false;}return QFile::remove(owned(directory,id));},&error));QVERIFY(error.contains("Fixture cleanup failed"));QCOMPARE(store.pendingCleanup(),QStringList{"b"});}
        QCOMPARE(called,QStringList({"a","b","c"}));QVERIFY(!QFileInfo::exists(owned(data.path(),"a")));QVERIFY(QFileInfo::exists(owned(data.path(),"b")));QVERIFY(!QFileInfo::exists(owned(data.path(),"c")));
        SessionStore restarted(data.path());const auto sessions=restarted.load(&error);QCOMPARE(restarted.pendingCleanup(),QStringList{"b"});called.clear();
        QVERIFY(process(restarted,sessions,[&](const QString& id,const QString& directory,QString*){called.append(id);return QFile::remove(owned(directory,id));},&error));QCOMPARE(called,QStringList{"b"});QVERIFY(restarted.pendingCleanup().isEmpty());
        QVERIFY(process(restarted,sessions,[&](const QString&,const QString&,QString*){called.append("unexpected");return true;},&error));QCOMPARE(called,QStringList{"b"});
    }
    void exceptionKeepsDurableCleanupMarker(){
        QTemporaryDir data;SessionStore store(data.path());QString error;QVERIFY(seed(store,data.path(),{"gone"},&error));
        QVERIFY(!process(store,{chat("keep")},[](const QString&,const QString&,QString*)->bool{throw std::runtime_error("fixture exception");},&error));QVERIFY(!error.isEmpty());QVERIFY(QFileInfo::exists(owned(data.path(),"gone")));
        SessionStore restarted(data.path());const auto sessions=restarted.load(&error);QCOMPARE(restarted.pendingCleanup(),QStringList{"gone"});QVERIFY(process(restarted,sessions,[](const QString& id,const QString& directory,QString*){return QFile::remove(owned(directory,id));},&error));
    }
    void resurrectedCandidateRejectedBeforeCleanup(){
        QTemporaryDir data;SessionStore store(data.path());QString error;QVERIFY(seed(store,data.path(),{"gone"},&error));const auto before=bytes(data.filePath("sessions.json"));int calls=0;
        QVERIFY(!process(store,{chat("keep"),chat("GONE")},[&](const QString&,const QString&,QString*){++calls;return true;},&error));QCOMPARE(calls,0);QVERIFY(!error.isEmpty());QCOMPARE(bytes(data.filePath("sessions.json")),before);QVERIFY(QFileInfo::exists(owned(data.path(),"gone")));
    }
    void invalidIds_data(){
        QTest::addColumn<QStringList>("retained");QTest::addColumn<QStringList>("pending");
        QTest::newRow("retained-empty")<<QStringList{QString()}<<QStringList{};
        QTest::newRow("retained-traversal")<<QStringList{"../escape"}<<QStringList{};
        QTest::newRow("retained-space")<<QStringList{"bad id"}<<QStringList{};
        QTest::newRow("retained-unicode")<<QStringList{QString::fromUtf8("é")}<<QStringList{};
        QTest::newRow("retained-too-long")<<QStringList{QString(129,'a')}<<QStringList{};
        QTest::newRow("retained-duplicate")<<QStringList{"chat","chat"}<<QStringList{};
        QTest::newRow("retained-case-alias")<<QStringList{"Chat","chat"}<<QStringList{};
        QTest::newRow("cleanup-empty")<<QStringList{"keep"}<<QStringList{QString()};
        QTest::newRow("cleanup-traversal")<<QStringList{"keep"}<<QStringList{"../gone"};
        QTest::newRow("cleanup-too-long")<<QStringList{"keep"}<<QStringList{QString(129,'b')};
        QTest::newRow("cleanup-duplicate")<<QStringList{"keep"}<<QStringList{"gone","gone"};
        QTest::newRow("cleanup-case-alias")<<QStringList{"keep"}<<QStringList{"Gone","gone"};
        QTest::newRow("cleanup-retained-alias")<<QStringList{"Keep"}<<QStringList{"KEEP"};
    }
    void invalidIds(){
        QFETCH(QStringList,retained);QFETCH(QStringList,pending);QTemporaryDir data;SessionStore store(data.path());QString error;QVERIFY(store.save({chat("baseline")},&error));const auto before=bytes(data.filePath("sessions.json"));QList<Conversation> sessions;QJsonArray records;
        for(const auto& id:retained){sessions.append(chat(id));records.append(QJsonObject{{"id",id},{"title","Fixture"},{"messages",QJsonArray{}}});}
        QVERIFY(!store.saveWithCleanup(sessions,pending,&error));QVERIFY(!error.isEmpty());QCOMPARE(bytes(data.filePath("sessions.json")),before);
        const auto corrupt=QJsonDocument(QJsonObject{{"schemaVersion",1},{"sessions",records},{"pendingCleanup",QJsonArray::fromStringList(pending)}}).toJson();QVERIFY(put(data.filePath("sessions.json"),corrupt));SessionStore invalid(data.path());QVERIFY(invalid.load(&error).isEmpty());QVERIFY(!error.isEmpty());int calls=0;
        QVERIFY(!process(invalid,{},[&](const QString&,const QString&,QString*){++calls;return true;},&error));QCOMPARE(calls,0);QCOMPARE(bytes(data.filePath("sessions.json")),corrupt);
    }
    void nonStringCleanupRecordFailsClosed(){
        QTemporaryDir data;QJsonObject payload{{"schemaVersion",1},{"sessions",QJsonArray{}},{"pendingCleanup",QJsonArray{"valid-first",42}}};const auto original=QJsonDocument(payload).toJson();QVERIFY(put(data.filePath("sessions.json"),original));SessionStore store(data.path());QString error;QVERIFY(store.load(&error).isEmpty());QVERIFY(!error.isEmpty());int calls=0;QVERIFY(!process(store,{},[&](const QString&,const QString&,QString*){++calls;return true;},&error));QCOMPARE(calls,0);QCOMPARE(bytes(data.filePath("sessions.json")),original);
    }
    void ordinarySaveCannotRebindAnIdenticalRevisionAlias(){
#ifdef Q_OS_WIN
        QTemporaryDir fixture;const auto original=fixture.filePath("original"),foreign=fixture.filePath("foreign"),alias=fixture.filePath("alias");QVERIFY(QDir().mkpath(original));QVERIFY(QDir().mkpath(foreign));QVERIFY(junction(alias,original));const auto cleanup=qScopeGuard([&]{QDir().rmdir(alias);});
        SessionStore store(alias);QString error;QVERIFY(seed(store,original,{"gone"},&error));const auto document=bytes(original+"/sessions.json");QVERIFY(put(foreign+"/sessions.json",document));QVERIFY(put(owned(foreign,"gone"),"foreign"));QVERIFY(QDir().rmdir(alias));QVERIFY(junction(alias,foreign));
        QVERIFY2(!store.save({chat("keep")},&error),"An ordinary save must not adopt a retargeted directory even when its JSON revision is identical.");int calls=0;QVERIFY(!process(store,{chat("keep")},[&](const QString&,const QString&,QString*){++calls;return true;},&error));QCOMPARE(calls,0);QCOMPARE(bytes(foreign+"/sessions.json"),document);QCOMPARE(bytes(owned(foreign,"gone")),QByteArray("foreign"));QVERIFY(QFileInfo::exists(owned(original,"gone")));
#else
        QSKIP("Windows junction root fixture");
#endif
    }
    void rootAlias_data(){QTest::addColumn<QString>("mode");QTest::newRow("unchanged")<<QString("unchanged");QTest::newRow("retarget-before")<<QString("before");QTest::newRow("retarget-during")<<QString("during");}
    void rootAlias(){
#ifdef Q_OS_WIN
        QFETCH(QString,mode);QTemporaryDir fixture;const auto original=fixture.filePath("original"),foreign=fixture.filePath("foreign"),alias=fixture.filePath("alias");QVERIFY(QDir().mkpath(original));QVERIFY(QDir().mkpath(foreign));QVERIFY(junction(alias,original));const auto cleanup=qScopeGuard([&]{QDir().rmdir(alias);});
        SessionStore store(alias);QString error;QVERIFY(seed(store,original,{"gone"},&error));QVERIFY(put(foreign+"/sessions.json",bytes(original+"/sessions.json")));QVERIFY(put(owned(foreign,"gone"),"foreign bytes"));
        const auto retarget=[&]{return QDir().rmdir(alias)&&junction(alias,foreign);};if(mode=="before")QVERIFY(retarget());int calls=0;QString received;bool changed=true;
        const bool okay=process(store,{chat("keep")},[&](const QString& id,const QString& directory,QString*){++calls;received=directory;if(mode=="during")changed=retarget();return QFile::remove(owned(directory,id));},&error);
        QVERIFY2(okay==(mode!="before"),qPrintable(error+"; Qt canonical alias="+QFileInfo(alias).canonicalFilePath()+"; actual="+original));QCOMPARE(calls,mode=="before"?0:1);QVERIFY(changed);QCOMPARE(bytes(owned(foreign,"gone")),QByteArray("foreign bytes"));
        if(mode=="before")QVERIFY(QFileInfo::exists(owned(original,"gone")));
        else {QCOMPARE(received,QFileInfo(original).canonicalFilePath());QVERIFY(!QFileInfo::exists(owned(original,"gone")));SessionStore check(original);check.load(&error);QVERIFY(check.pendingCleanup().isEmpty());}
#else
        QSKIP("Windows junction containment fixture");
#endif
    }
};
QTEST_GUILESS_MAIN(SessionCleanupTests)
#include "test_session_cleanup.moc"
