#include "ui/MarkdownParts.h"
#include "ui/AppFonts.h"
#include "links/TranscriptLinkResolver.h"
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextFragment>
#include <QtTest>
using namespace ade;
class MarkdownLinkScope : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { AppFonts::load(); }
    void renderedLinkUsesExactScopedDestination_data() {
        QTest::addColumn<QString>("scheme");
        for(const auto *scheme:{"path","workspace","wsfile","mention:file"})QTest::newRow(scheme)<<QString(scheme);
    }
    void renderedLinkUsesExactScopedDestination() {
        QFETCH(QString,scheme);
        QTemporaryDir data,root,other;
        QFile file(root.filePath("a(b).txt")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("OWNED"); file.close();
        FileReadSnapshotStore store(data.path());
        AttachmentStore attachments(data.path());
        QString setupError;
        QVERIFY(!attachments.agentDirectory("owner",&setupError,true).isEmpty());
        TranscriptLinkOptions options; options.agentId="owner";options.dataDirectory=data.path();
        options.lease=store.acquire("owner");options.mounts={{"ws1",root.path()}};
        QString error;const auto scope=TranscriptLinkResolver::create(options,&error);QVERIFY2(scope.isCurrent("owner"),qPrintable(error));
        TranscriptLinkProvenance provenance;provenance.mountBindings=scope.mountBindings();
        MarkdownParts view;view.updateText("See []("+scheme+":ws1/a(b).txt)",false);
        auto *browser=view.findChild<TranscriptText*>();QVERIFY(browser);
        QString href;
        for(auto block=browser->document()->begin();block.isValid();block=block.next())
            for(auto it=block.begin();!it.atEnd();++it)if(it.fragment().charFormat().isAnchor())href=it.fragment().charFormat().anchorHref();
        QVERIFY(!href.isEmpty());QCOMPARE(TranscriptLinkResolver::parse(href).path,QString("ws1/a(b).txt"));
        QString owner="owner";TranscriptLinkResolver::Action action;
        connect(&view,&MarkdownParts::linkActivated,&view,[&](const QUrl& url){action=TranscriptLinkResolver::resolve(scope,owner,url.toString(QUrl::FullyEncoded),provenance,nullptr,&error);});
        QVERIFY(QMetaObject::invokeMethod(browser,"anchorClicked",Q_ARG(QUrl,QUrl(href))));
        QCOMPARE(action.kind,TranscriptLinkResolver::Kind::File);QCOMPARE(action.bytes,QByteArray("OWNED"));
        owner="foreign";
        QVERIFY(QMetaObject::invokeMethod(browser,"anchorClicked",Q_ARG(QUrl,QUrl(href))));
        QCOMPARE(action.kind,TranscriptLinkResolver::Kind::Invalid);
        owner="owner";provenance.mountBindings={};
        QVERIFY(QMetaObject::invokeMethod(browser,"anchorClicked",Q_ARG(QUrl,QUrl(href))));
        QCOMPARE(action.kind,TranscriptLinkResolver::Kind::Invalid);
    }
};
QTEST_MAIN(MarkdownLinkScope)
#include "test_markdown_link_scope.moc"
