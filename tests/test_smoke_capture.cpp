#include "core/AgentEngine.h"
#include "ui/AppStyle.h"
#include "ui/SmokeTranscriptCapture.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class SmokeCaptureTest final:public QObject {
    Q_OBJECT
private slots:
    void recursiveRedactionPreservesTypes(){
        const QString secret="synthetic-key-only";
        const QJsonObject original{{"nested",QJsonArray{QJsonObject{{"text","prefix "+secret+" suffix"},{secret,true}},3,false,QJsonValue::Null}},{"ordinary","unchanged"}};
        const auto clean=redactSmokeEvidence(original,{secret}).toObject();
        QVERIFY(!QJsonDocument(clean).toJson().contains(secret.toUtf8()));
        QCOMPARE(clean["ordinary"].toString(),QString("unchanged"));
        const auto nested=clean["nested"].toArray();QCOMPARE(nested.at(1).toInt(),3);QCOMPARE(nested.at(2).toBool(),false);QVERIFY(nested.at(3).isNull());
        QVERIFY(QJsonDocument(original).toJson().contains(secret.toUtf8()));
    }
    void actualWidgetFramesAreBoundedAndHashed(){
        QTemporaryDir directory;QVERIFY(directory.isValid());AgentEngine engine;
        SmokeTranscriptCapture capture(&engine,directory.path(),{"synthetic-key-only"});
        engine.setMessages(QJsonArray{QJsonObject{{"role","user"},{"content","Inspect this test"}}});
        QTest::qWait(40);
        engine.setMessages(QJsonArray{QJsonObject{{"role","user"},{"content","Inspect this test"}},QJsonObject{{"role","assistant"},{"content","Native **Markdown** with synthetic-key-only redacted."}}});
        QTest::qWait(40);
        const auto report=capture.finish();QCOMPARE(report["error"].toString(),QString());
        const auto frames=report["frames"].toArray();QVERIFY(frames.size()>=3);QVERIFY(frames.size()<=8);
        for(const auto& entry:frames){const auto frame=entry.toObject();const auto path=directory.filePath(frame["path"].toString());
            const QImage image(path);QVERIFY(!image.isNull());QCOMPARE(image.size(),QSize(1000,800));
            QSet<QRgb> colors;for(int y=0;y<image.height();y+=2)for(int x=0;x<image.width();x+=2)colors.insert(image.pixel(x,y));
            QVERIFY2(colors.size()>8,"A milestone frame must include rendered content, not an unsettled blank widget.");
            QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex()),frame["sha256"].toString());
        }
        QCOMPARE(capture.finish(),report);
        engine.setMessages({});QTest::qWait(30);QCOMPARE(capture.finish()["frames"],report["frames"]);
    }
    void nonemptyOutputDirectoryIsPreserved(){
        QTemporaryDir directory;QFile existing(directory.filePath("keep.txt"));QVERIFY(existing.open(QIODevice::WriteOnly));existing.write("preserve");existing.close();
        AgentEngine engine;SmokeTranscriptCapture capture(&engine,directory.path(),{});
        const auto report=capture.finish();QVERIFY(!report["error"].toString().isEmpty());QVERIFY(report["frames"].toArray().isEmpty());
        QVERIFY(existing.open(QIODevice::ReadOnly));QCOMPARE(existing.readAll(),QByteArray("preserve"));
    }
};
int main(int argc,char** argv){qputenv("QT_QPA_PLATFORM","offscreen");QApplication app(argc,argv);initializeAppStyle(app);SmokeCaptureTest test;return QTest::qExec(&test,argc,argv);}
#include "test_smoke_capture.moc"
