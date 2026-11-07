#include "ui/MarkdownParts.h"
#include "editor/CodeEditor.h"
#include "ui/AppFonts.h"
#include "ui/QuestionPanel.h"
#include "ui/QuestionToolPart.h"
#include "ui/ConversationView.h"
#include <QPointer>
#include <QJsonArray>
#include <QApplication>
#include <QClipboard>
#include <QPushButton>
#include <QSignalSpy>
#include <QTextBlock>
#include <QTextFragment>
#include <QtTest>

using namespace ade;
namespace {
struct Anchor { QString text, href; };
QList<Anchor> anchors(const QWidget &view) {
    QList<Anchor> result;
    for (auto *part : view.findChildren<TranscriptText *>())
        for (auto block = part->document()->begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment();
                if (fragment.isValid() && fragment.charFormat().isAnchor())
                    result.append({fragment.text(), fragment.charFormat().anchorHref()});
            }
    return result;
}
QString plain(const MarkdownParts &view) {
    QString result;
    for (auto *part : view.findChildren<TranscriptText *>()) result += part->toPlainText();
    return result;
}
}
class MarkdownFindings : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { AppFonts::load(); }
    void richLinks_data() {
        QTest::addColumn<QString>("source"); QTest::addColumn<QString>("href"); QTest::addColumn<bool>("streaming");
        QTest::newRow("path-empty") << QString("Edit [](path:ws1/src/a.ts)") << QString("path:ws1/src/a.ts") << false;
        QTest::newRow("tab-empty") << QString("See [](tab:t_1)") << QString("tab:t_1") << false;
        QTest::newRow("color-empty") << QString("Use [](color:rgba(0, 0, 0))") << QString("color:rgba%280%2C%200%2C%200%29") << false;
        QTest::newRow("color-nonempty") << QString("Use [black](color:rgba(0, 0, 0))") << QString("color:rgba%280%2C%200%2C%200%29") << false;
        QTest::newRow("nested-path-parentheses") << QString("Edit [](path:ws1/src/a(b(c)).ts)") << QString("path:ws1/src/a%28b%28c%29%29.ts") << false;
        QTest::newRow("nested-ordinary-parentheses") << QString("[docs](https://example.invalid/a(b(c)))") << QString("https://example.invalid/a(b(c))") << false;
        QTest::newRow("incomplete-path") << QString("See [](path:ws1/sr") << QString("path:incomplete:ws1/sr") << true;
        QTest::newRow("incomplete-tab") << QString("See [](tab:t_1") << QString("tab:t_1") << true;
        QTest::newRow("incomplete-color") << QString("Use [](color:#123") << QString("color:#123") << true;
    }
    void richLinks() {
        QFETCH(QString, source); QFETCH(QString, href); QFETCH(bool, streaming);
        MarkdownParts view; view.updateText(source, streaming);
        const auto found = anchors(view);
        QCOMPARE(found.size(), 1);
        QVERIFY(!found.first().text.isEmpty());
        QCOMPARE(QUrl(found.first().href), QUrl(href));
    }
    void richLinkTitle_data() {
        QTest::addColumn<QString>("source");
        for (const auto *value : {"[](path:ws1/a.ts \"Title\")", "[x](path:ws1/a.ts 'Title')", "[x](path:ws1/a.ts (Title))", "[](<path:ws1/a.ts> \"Title\")", "[](<path:ws1/a.ts>)"})
            QTest::newRow(value) << QString(value);
    }
    void richLinkTitle() {
        QFETCH(QString, source);
        MarkdownParts view; view.updateText(source, false);
        const auto found=anchors(view); QCOMPARE(found.size(),1);
        QCOMPARE(found.first().href,QString("path:ws1/a.ts"));
        QCOMPARE(found.first().text,QString("a.ts"));
    }
    void actualQuestionLabelsShareRichRendering() {
        const QJsonObject field{{"questionId","answer"},{"type","input"},{"label","Open [](path:ws1/a.ts)"}};
        const QJsonObject form{{"id","question"},{"title","Review"},{"currentStep",0},{"steps",QJsonArray{QJsonObject{{"fields",QJsonArray{field}}}}}};
        QuestionPanel panel; panel.setQuestion("chat",form);
        const auto pending=anchors(panel); QCOMPARE(pending.size(),1); QCOMPARE(pending.first().text,QString("a.ts"));
        const QJsonObject part{{"name","askUserQuestions"},{"state","output-available"},{"input",form},{"output",QJsonObject{{"answers",QJsonObject{{"answer","yes"}}}}}};
        QuestionToolPart completed; completed.updatePart(part);
        const auto answered=anchors(completed); QCOMPARE(answered.size(),1); QCOMPARE(answered.first().text,QString("a.ts"));
        QSignalSpy activated(&completed,&QuestionToolPart::linkActivated);
        auto *label=completed.findChild<TranscriptText *>("questionAnswerLabel"); QVERIFY(label);
        QVERIFY(QMetaObject::invokeMethod(label,"anchorClicked",Q_ARG(QUrl,QUrl(answered.first().href))));
        QCOMPARE(activated.size(),1); QCOMPARE(activated.first().first().toUrl(),QUrl("path:ws1/a.ts"));
        QCOMPARE(panel.draftAnswers().value("answer").toString(),QString());
    }
    void supportedAliasActivation_data() {
        QTest::addColumn<QString>("url");
        for (const auto *url : {"path:ws1/a.ts", "tab:t_1", "color:red", "shell:session_1", "att:blob.txt",
                                "workspace:ws1/a.ts", "wsfile:ws1/a.ts", "mention:file:ws1/a.ts"})
            QTest::newRow(url) << QString(url);
    }
    void supportedAliasActivation() {
        QFETCH(QString, url);
        MarkdownParts view; view.updateText("[link](" + url + ")", false);
        auto *browser = view.findChild<TranscriptText *>(); QVERIFY(browser);
        QSignalSpy links(&view, &MarkdownParts::linkActivated);
        QVERIFY(QMetaObject::invokeMethod(browser, "anchorClicked", Q_ARG(QUrl, QUrl(url))));
        QCOMPARE(links.size(), 1); QCOMPARE(links.first().first().toUrl(), QUrl(url));
    }
    void referenceAcrossFence() {
        MarkdownParts view;
        view.updateText("See [docs][1].\n\n```\nx\n```\n\n[1]: https://example.invalid/docs\n", false);
        const auto found = anchors(view);
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().text, QString("docs"));
        QCOMPARE(QUrl(found.first().href), QUrl("https://example.invalid/docs"));
    }
    void indentedFenceCopy() {
        MarkdownParts view;
        view.updateText("1. Install:\n   ```sh\n   npm i\n   ```\n2. Run\n", false);
        auto *copy = view.findChild<QPushButton *>("copyCode"); QVERIFY(copy);
        copy->click(); QCOMPARE(QApplication::clipboard()->text(), QString("npm i\n"));
    }
    void inlineBacktickRunIsNotFence() {
        MarkdownParts view; view.updateText("```js``` text\nordinary after\n", false);
        QVERIFY(view.findChildren<CodeEditor *>().isEmpty());
        QVERIFY(plain(view).contains("ordinary after"));
    }
    void stoppedUnclosedFenceCopy() {
        MarkdownParts view; view.updateText("```js\nx", false);
        auto *copy = view.findChild<QPushButton *>("copyCode"); QVERIFY(copy);
        QVERIFY(!copy->isHidden()); copy->click(); QCOMPARE(QApplication::clipboard()->text(), QString("x"));
    }
    void referenceDefinitionsStayScopedToProse() {
        MarkdownParts view;
        view.updateText("See [first] and [second].\n\n```txt\n[first]: https://wrong.invalid\n```\n\n[first]: https://one.invalid\n[second]: https://two.invalid \"Title\"\n", false);
        const auto found = anchors(view);
        QCOMPARE(found.size(), 2);
        QCOMPARE(found[0].href, QString("https://one.invalid"));
        QCOMPARE(found[1].href, QString("https://two.invalid"));
        QCOMPARE(view.findChild<CodeEditor *>()->sourceText(), QString("[first]: https://wrong.invalid\n"));
    }
    void duplicateReferencesUseFirstDefinition() {
        MarkdownParts view;
        view.updateText("[doc]: https://first.invalid\n\n```\nx\n```\n\n[doc]: https://second.invalid\n\nSee [doc].\n", false);
        const auto found = anchors(view); QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().href, QString("https://first.invalid"));
    }
    void referenceCannotInterruptParagraph() {
        MarkdownParts view;
        view.updateText("Paragraph\n[docs]: https://invalid.example\n\n```\nx\n```\n\n[docs]\n", false);
        const auto found=anchors(view);
        for(const auto &anchor:found)QVERIFY2(anchor.text!="docs",qPrintable(anchor.text+" -> "+anchor.href));
        QCOMPARE(view.findChildren<TranscriptText *>().last()->toPlainText().trimmed(),QString("[docs]"));
    }
    void multilineReferenceAcrossFence() {
        MarkdownParts view;
        view.updateText("See [docs].\n\n```\nx\n```\n\n[docs]:\n <https://docs.invalid>\n \"A title\"\n", false);
        const auto found = anchors(view); QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().href, QString("https://docs.invalid"));
    }
    void literalCodeAndImagesAreNotRewritten() {
        MarkdownParts view;
        view.updateText("`[](path:ws1/a.ts)`\n\n![diagram](path:att/a.png)\n\n```txt\n[](color:rgba(0, 0, 0))\n```", false);
        QVERIFY(anchors(view).isEmpty());
        QCOMPARE(view.findChild<CodeEditor *>()->sourceText(), QString("[](color:rgba(0, 0, 0))\n"));
        auto *browser = view.findChild<TranscriptText *>(); QVERIFY(browser);
        QVERIFY(browser->containsImage(QUrl("path:att/a.png")));
        QVERIFY(browser->toPlainText().contains("[](path:ws1/a.ts)"));
        QVERIFY(browser->document()->resource(QTextDocument::ImageResource,QUrl("path:att/a.png")).isNull());
    }
    void partialToCompleteKeepsWidgetAndIdentity() {
        MarkdownParts view; view.updateText("See [](path:ws1/a", true);
        auto *first = view.findChild<TranscriptText *>(); QVERIFY(first);
        QSignalSpy activated(&view, &MarkdownParts::linkActivated);
        view.updateText("See [](path:ws1/a.ts)", false);
        QCOMPARE(view.findChild<TranscriptText *>(), first);
        const auto found = anchors(view); QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().text, QString("a.ts"));
        QVERIFY(QMetaObject::invokeMethod(first,"anchorClicked",Q_ARG(QUrl,QUrl(found.first().href))));
        QCOMPARE(activated.size(), 1);
        QCOMPARE(activated.first().first().toUrl(),QUrl("path:ws1/a.ts"));
        const auto updates = first->updateCount();
        view.updateText("See [](path:ws1/a.ts)", false);
        QCOMPARE(first->updateCount(), updates);
    }
    void unfinishedCopyIsHiddenOnlyWhileStreaming() {
        MarkdownParts view; view.updateText("```js\nx", true);
        auto *copy = view.findChild<QPushButton *>("copyCode"); QVERIFY(copy); QVERIFY(copy->isHidden());
        auto *code = view.findChild<CodeEditor *>(); QVERIFY(code);
        view.updateText("```js\nx", false);
        QCOMPARE(view.findChild<CodeEditor *>(), code);
        QVERIFY(!copy->isHidden());
    }
    void untrustedSchemesRemainBlocked() {
        MarkdownParts view; view.updateText("text", false);
        auto *browser = view.findChild<TranscriptText *>(); QVERIFY(browser);
        QSignalSpy activated(&view,&MarkdownParts::linkActivated);
        for(const auto *url:{"javascript:alert(1)","file:///C:/private.txt","data:text/html,x","unknown:value"})
            QVERIFY(QMetaObject::invokeMethod(browser,"anchorClicked",Q_ARG(QUrl,QUrl(url))));
        QCOMPARE(activated.size(), 0);
    }
    void sharedTextKeepsOriginalAndLiteralCode() {
        const QString source="Literal `[](path:ws1/a.ts)`\n\n```txt\n[](tab:t_1)\n```\n\nOpen [](path:ws1/b.ts \"Title\")";
        TranscriptText text; text.setSource(source);
        QCOMPARE(text.source(),source);
        QVERIFY(text.toPlainText().contains("[](path:ws1/a.ts)"));
        QVERIFY(text.toPlainText().contains("[](tab:t_1)"));
        int count=0;
        for(auto block=text.document()->begin();block.isValid();block=block.next())
            for(auto it=block.begin();!it.atEnd();++it)if(it.fragment().charFormat().isAnchor()) {
                ++count; QCOMPARE(it.fragment().charFormat().anchorHref(),QString("path:ws1/b.ts"));
            }
        QCOMPARE(count,1);
        text.setSource(source,false); QCOMPARE(text.toPlainText(),source); QCOMPARE(text.source(),source);
    }
    void partialBeforeFenceIsNotInvented() {
        MarkdownParts view; view.updateText("[](path:ws1/partial\n```txt\ncode\n```",false);
        QVERIFY(anchors(view).isEmpty());
        auto *text=view.findChild<TranscriptText*>();QVERIFY(text);
        text->setSource("[](path:ws1/partial",true,false);QVERIFY(!text->toHtml().contains("path:incomplete:"));
        text->setSource("[](path:ws1/partial",true,true);QVERIFY(text->toHtml().contains("path:incomplete:"));
    }
    void richLinksRetainRawGroupedOwnerAndChat() {
        const auto row=[](const QString& id,const QString& part,const QString& source){return QJsonObject{{"id",id},{"role","assistant"},{"parts",QJsonArray{QJsonObject{{"id",part},{"type","text"},{"text",source},{"state","done"}}}}};};
        ConversationView view;
        const QJsonArray rows{row("first","first-text","Earlier"),row("second","second-text","Open [](path:ws1/a.ts)")};
        view.setConversation("chat-a",rows,false);
        QSignalSpy links(&view,&ConversationView::scopedLinkActivated);
        QPointer<TranscriptText> target;
        for(auto *text:view.findChildren<TranscriptText*>())if(text->source().contains("path:ws1"))target=text;
        QVERIFY(target);QVERIFY(QMetaObject::invokeMethod(target,"anchorClicked",Q_ARG(QUrl,QUrl("path:ws1/a.ts"))));
        QCOMPARE(links.size(),1);QCOMPARE(links[0][0].toString(),QString("chat-a"));QCOMPARE(links[0][1].toString(),QString("second"));QCOMPARE(links[0][2].toString(),QString("second-text"));
        view.setConversation("chat-b",rows,false);QVERIFY(target.isNull());
        for(auto *text:view.findChildren<TranscriptText*>())if(text->source().contains("path:ws1"))target=text;
        QVERIFY(target);QVERIFY(QMetaObject::invokeMethod(target,"anchorClicked",Q_ARG(QUrl,QUrl("path:ws1/a.ts"))));
        QCOMPARE(links.size(),2);QCOMPARE(links[1][0].toString(),QString("chat-b"));QCOMPARE(links[1][1].toString(),QString("second"));
    }
    void malformedStreamingInputStaysBounded() {
        MarkdownParts view;
        const auto source = QString(50000,'[')+"\n"+QString("[]()").repeated(10000);
        view.updateText(source,true);
        QVERIFY(anchors(view).isEmpty());
        QVERIFY(plain(view).contains(QString(100,'[')));
    }
};
QTEST_MAIN(MarkdownFindings)
#include "test_markdown_findings.moc"
