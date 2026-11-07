#include "attachments/AttachmentStore.h"
#include "core/AgentEngine.h"
#include "runtime/AgentToolHost.h"
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class AcpOriginalHostTest : public QObject {
 Q_OBJECT
private slots:
 void currentImageUsesOriginalBytes() {
  QTemporaryDir data;
  auto log=data.filePath("wire");
  qputenv("ADE_ATTACHMENT_LOG",log.toUtf8());
  qputenv("ADE_ATTACHMENT_ROOT",data.path().toUtf8());
  qputenv("ADE_ATTACHMENT_IMAGE_CAPABILITY","1");
  ProviderConfig p;p.id="opencode";p.kind="opencode";p.protocol="acp";p.model="model-a";
  p.harnessExecutable=QCoreApplication::applicationDirPath()+"/attachment_acp_fixture.exe";
  AgentEngine engine;engine.setProvider(p);AgentToolHost host(&engine,nullptr,"a",data.path());
  QImage image(16,16,QImage::Format_RGB32);image.fill(Qt::red);image.setText("OriginalMetadata","Must survive byte-exact transport");
  QByteArray original;QBuffer buffer(&original);buffer.open(QIODevice::WriteOnly);QVERIFY(image.save(&buffer,"PNG"));buffer.close();
  AttachmentStore source(data.path());QString error;auto ref=source.write("a","original.png",original,&error);QVERIFY2(!ref.isEmpty(),qPrintable(error));
  auto m=host.captureAttachments({ref.toJson()},&error);QVERIFY2(!m.isEmpty(),qPrintable(error));
  QSignalSpy done(&engine,&AgentEngine::finished);
  QVERIFY(engine.submitWithAttachments("Describe",m["attachments"].toArray(),m["pathReferences"].toObject(),m["pathMetadata"].toObject()));
  QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,15000);QVERIFY(engine.lastTurnSucceeded());
  QFile wire(log);QVERIFY(wire.open(QIODevice::ReadOnly));QByteArray delivered;QString mime;
  for(const auto &line:wire.readAll().split('\n')) {
   auto frame=QJsonDocument::fromJson(line).object()["frame"].toObject();
   if(frame["method"]!="session/prompt")continue;
   for(const auto &v:frame["params"].toObject()["prompt"].toArray()) {auto o=v.toObject();if(o["type"]=="image"){delivered=QByteArray::fromBase64(o["data"].toString().toLatin1());mime=o["mimeType"].toString();}}
  }
  QCOMPARE(mime,QString("image/png"));QCOMPARE(delivered,original);
 }
 void cleanup(){qunsetenv("ADE_ATTACHMENT_LOG");qunsetenv("ADE_ATTACHMENT_ROOT");qunsetenv("ADE_ATTACHMENT_IMAGE_CAPABILITY");}
};
QTEST_MAIN(AcpOriginalHostTest)
#include "test_acp_original_host.moc"
