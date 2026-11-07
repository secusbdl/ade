#include "ConversationView.h"
#include "harness/AcpToolPresentation.h"
#include "TranscriptGrouping.h"
#include "MarkdownParts.h"
#include "FileDiffView.h"
#include "TurnFileEdits.h"
#include "QuestionToolPart.h"
#include "editor/CodeEditor.h"
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QFrame>
#include <QFontInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QKeyEvent>
#include <QPointer>
#include <QPainter>
#include <QRegularExpression>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <functional>
#include <optional>

namespace ade {
namespace {
bool darkFor(const QWidget *widget){return widget->palette().color(QPalette::Window).lightness()<128;}
bool settled(const QString &state){return QStringList{"done","error","output-available","output-error","output-denied","canceled"}.contains(state);}
// Native terminal states include cancellation/errors in addition to the source UI states.
bool hasUnfinishedAssistant(const QJsonArray &messages) {
    if(messages.isEmpty())return false;
    const auto message=messages.last().toObject();
    if(message["role"]!="assistant")return false;
    // A visible human decision is not a stalled model stream. Inspect the
    // complete current row first: earlier reasoning may still be unfinished
    // while its question/approval waits. Historical turns do not suppress work.
    for(const auto &value:message["parts"].toArray()) {
        const auto part=value.toObject();
        if(part["type"]!="tool")continue;
        const auto state=part["state"].toString();
        if(state=="approval-requested"||
           (isNativeQuestionPart(part)&&(state=="running"||state=="input-available")))return false;
    }
    for(const auto &value:message["parts"].toArray()) {
        const auto part=value.toObject();const auto type=part["type"].toString();
        const auto state=part["state"].toString();
        if((type=="text"||type=="reasoning")&&!state.isEmpty()&&!settled(state))return true;
        if(type=="tool"&&!settled(state))return true;
    }
    return false;
}
QJsonObject acpEntry(const QJsonObject &part) {
    const auto mapped=part["acpPresentation"].toObject();
    return AcpToolPresentation::valid(mapped)?mapped["entries"].toArray().first().toObject():QJsonObject{};
}
QString visualToolName(const QJsonObject &part) {
    const auto entry=acpEntry(part);const auto kind=entry["kind"].toString();
    if(kind=="read")return "read";
    if(kind=="list")return "ls";
    if(kind=="search")return "grepSearch";
    if(kind=="execute")return "executeShellCommand";
    return entry["name"].toString(part["name"].toString());
}
bool exploratory(const QJsonObject &part){
    if(part["type"]=="reasoning")return true;
    return part["type"]=="tool"&&QStringList{"glob","grepSearch","read","ls","searchInLibraryDocs","listLibraryDocs","executeSandboxJs","readConsoleLogs","getLintingDiagnostics"}.contains(visualToolName(part));
}
QString partPlain(const QJsonObject &part){
    if(isNativeQuestionPart(part))return questionToolPlainText(part);
    if(part["type"]=="tool") {
        QString result=part["name"].toString();
        if(!part["inputText"].toString().isEmpty())result+='\n'+part["inputText"].toString();
        if(!part["output"].toString().isEmpty())result+='\n'+part["output"].toString();
        else if(!part["errorText"].toString().isEmpty())result+='\n'+part["errorText"].toString();
        return result;
    }
    return part["text"].toString(part["errorText"].toString());
}
struct Callbacks {
    std::function<void(const QString&,const QString&,const QUrl&)> link;
    std::function<void(const QString&,const QString&,const QString&,bool)> approval;
    std::function<std::optional<bool>(const QString&)> disclosure;
    std::function<void(const QString&,bool)> remember;
    std::function<QString(const QString&)> displayPath;
    std::function<void(const QString&,const QString&,const QUrl&,MarkdownParts*)> image;
    std::function<void(const QString&,const QStringList&,const QString&,bool)> turnFile;
    std::function<void(const QString&)> retryTurn;
};

class CompactScroll final : public QScrollArea {
public:
    explicit CompactScroll(QWidget *body,QWidget *parent):QScrollArea(parent) {
        setObjectName("toolContentScroll");setFrameShape(QFrame::NoFrame);setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);setWidget(body);body->setAutoFillBackground(false);viewport()->setAutoFillBackground(false);setAutoFillBackground(false);body->installEventFilter(this);
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);setFixedHeight(1);
    }
    void scheduleMeasure(){
        if(pending)return;
        pending=true;const bool follow=initialized&&verticalScrollBar()->maximum()-verticalScrollBar()->value()<=20;
        const QPointer<CompactScroll> self(this);
        QTimer::singleShot(0,this,[this,follow,self]{
            pending=false;widget()->layout()->activate();
            if(!self)return;
            setFixedHeight(qBound(1,widget()->sizeHint().height()+2,limit));
            if(!self)return;
            if(follow)verticalScrollBar()->setValue(verticalScrollBar()->maximum());
            if(!self)return;
            initialized=true;
        });
    }
    void setLimit(int value){limit=value;scheduleMeasure();}
protected:
    bool eventFilter(QObject *object,QEvent *event) override {
        if(object==widget()&&(event->type()==QEvent::LayoutRequest||event->type()==QEvent::Resize))scheduleMeasure();
        return QScrollArea::eventFilter(object,event);
    }
private:
    bool pending=false,initialized=false;int limit=256;
};

class Disclosure final : public QWidget {
public:
    Disclosure(const QString &key,const Callbacks &callbacks,QWidget *parent):QWidget(parent),key(key),callbacks(callbacks) {
        auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(3);
        trigger=new QToolButton(this);trigger->setObjectName("disclosure_"+key);trigger->setProperty("partKey",key);
        trigger->setCheckable(true);trigger->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);trigger->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);trigger->setMinimumWidth(0);
        trigger->setCursor(Qt::PointingHandCursor);trigger->setStyleSheet("QToolButton{text-align:left;border:0;padding:3px 0;background:transparent;} QToolButton:hover{background:rgba(128,128,128,16);}");
        body=new QWidget(this);body->setObjectName("disclosureBody");bodyLayout=new QVBoxLayout(body);bodyLayout->setContentsMargins(15,2,4,4);bodyLayout->setSpacing(6);
        scroll=new CompactScroll(body,this);layout->addWidget(trigger);layout->addWidget(scroll);
        manual=callbacks.disclosure(key);
        connect(trigger,&QToolButton::clicked,this,[this](bool value){if(compactOnly)return;manual=value;const QPointer<Disclosure> self(this);apply(value);if(self)this->callbacks.remember(this->key,value);});
        smallFont();
    }
    void policy(bool automatic){apply(manual.value_or(automatic));}
    void inlineOnly(bool value){trigger->setVisible(!value);if(value){scroll->show();bodyLayout->setContentsMargins(0,0,0,0);}else bodyLayout->setContentsMargins(15,2,4,4);}
    void title(const QString &title){fullTitle=title;trigger->setAccessibleName(title);fitTitle();}
    void compact(bool value){compactOnly=value;trigger->setCheckable(!value);trigger->setCursor(value?Qt::ArrowCursor:Qt::PointingHandCursor);trigger->setFocusPolicy(value?Qt::NoFocus:Qt::StrongFocus);if(value){QPixmap icon(12,12);icon.fill(Qt::transparent);QPainter p(&icon);p.setRenderHint(QPainter::Antialiasing);p.setPen(trigger->palette().color(QPalette::WindowText));p.drawEllipse(QRectF(1,3,10,6));p.setBrush(p.pen().color());p.drawEllipse(QPointF(6,6),1.3,1.3);trigger->setIcon(QIcon(icon));trigger->setIconSize({12,12});apply(false);}else trigger->setIcon({});}
    void apply(bool expanded){expanded=expanded&&!compactOnly;{const QSignalBlocker blocker(trigger);trigger->setChecked(expanded);}trigger->setArrowType(compactOnly?Qt::NoArrow:expanded?Qt::DownArrow:Qt::RightArrow);scroll->setVisible(expanded);scroll->scheduleMeasure();}
    QToolButton *trigger;QWidget *body;QVBoxLayout *bodyLayout;CompactScroll *scroll;
protected:
    void resizeEvent(QResizeEvent *event) override{QWidget::resizeEvent(event);fitTitle();}
    void changeEvent(QEvent *event) override{QWidget::changeEvent(event);if(event->type()==QEvent::FontChange){smallFont();fitTitle();}}
private:
    void smallFont(){auto face=font();face.setPixelSize(qMax(9,qRound(QFontInfo(font()).pixelSize()*12.0/14)));trigger->setFont(face);}
    void fitTitle(){trigger->setText(trigger->fontMetrics().elidedText(fullTitle,Qt::ElideRight,qMax(40,width()-26)));trigger->setToolTip(fullTitle);}
    QString key,fullTitle;Callbacks callbacks;std::optional<bool> manual;bool compactOnly=false;
};

// Reported ACP diffs are read-only display values, never journal snapshots.
// Each child retains its index identity beneath the original raw owner/part.
class ReportedDiffEntries final : public QWidget {
public:
    ReportedDiffEntries(const QString &key,const Callbacks &callbacks,QWidget *parent)
        :QWidget(parent),key(key),callbacks(callbacks) {
        setObjectName("acpReportedDiffs");layout=new QVBoxLayout(this);
        layout->setContentsMargins(0,0,0,0);layout->setSpacing(7);
        notice=new QLabel(this);notice->setObjectName("acpDiffNotice");notice->setWordWrap(true);notice->setTextFormat(Qt::PlainText);layout->addWidget(notice);
    }
    void update(QJsonObject mapped,QString state,bool last) {
        const QPointer<ReportedDiffEntries> self(this);const auto epoch=++generation;
        const auto current=[self,epoch]{return self&&self->generation==epoch;};
        const auto entries=mapped["entries"].toArray();QSet<QString> keep;
        for(const auto &value:entries) {
            const auto entry=value.toObject();const auto kind=entry["kind"].toString();
            if(kind!="write"&&kind!="edit"&&kind!="delete")continue;
            const auto id=entry["id"].toString();keep.insert(id);
            auto item=items.value(id);
            if(!item.disclosure) {
                item.disclosure=new Disclosure(key+"/acp-diff/"+id,callbacks,this);
                item.disclosure->setObjectName("acpReportedDiff_"+id);item.disclosure->setProperty("displayChildId",id);
                item.text=new CodeEditor(item.disclosure->body);item.text->setObjectName("acpReportedDiffText");item.text->setReadOnly(true);item.text->setFrameShape(QFrame::NoFrame);item.text->setLineWrapMode(QPlainTextEdit::NoWrap);
                item.disclosure->bodyLayout->addWidget(item.text);items.insert(id,item);
                layout->insertWidget(layout->count()-1,item.disclosure);
            }
            item.disclosure->setProperty("reportedPath",entry["path"]);
            if(!current())return;
            const auto path=callbacks.displayPath(entry["path"].toString());
            QString title=path.section('/',-1);
            if(kind=="delete")title="Deleted "+title;
            else if(kind=="write")title+=" · Added";
            if(entry["added"].toInt()>0)title+=QString(" +%1").arg(entry["added"].toInt());
            if(entry["removed"].toInt()>0)title+=QString(" -%1").arg(entry["removed"].toInt());
            if(!settled(state))title=(kind=="delete"?"Deleting ":kind=="write"?"Writing ":"Editing ")+path;
            item.disclosure->trigger->setProperty("reportedPath",entry["path"]);
            if(!current())return;
            if(state=="output-error")title=entry["errorText"].toString().isEmpty()?"Error editing "+path:entry["errorText"].toString();
            else if(state=="canceled"||state=="output-denied")title="Skipped · "+title;
            item.disclosure->title(title);if(!current())return;
            item.disclosure->trigger->setEnabled(state!="output-error");
            if(state=="output-error")item.disclosure->apply(false);else item.disclosure->policy(true);
            if(!current())return;
            Q_UNUSED(last);
            const QString text=entry["diffAvailable"].toBool()
                ?FileDiffView::unifiedPreview(entry["before"].toString(),entry["after"].toString())
                :QString("Reported diff unavailable: missing or oversized text.");
            if(item.text->sourceText()!=text) {
                const auto cursor=item.text->textCursor();item.text->setPlainText(text);
                if(!current())return;
                auto restored=item.text->textCursor();restored.setPosition(qMin(cursor.anchor(),text.size()));restored.setPosition(qMin(cursor.position(),text.size()),QTextCursor::KeepAnchor);item.text->setTextCursor(restored);if(!current())return;
            }
            item.text->setFileName("reported.diff");item.text->setDarkTheme(darkFor(this));
            item.text->setFixedHeight(qBound(40,item.text->blockCount()*item.text->fontMetrics().lineSpacing()+14,220));
        }
        for(const auto &id:items.keys())if(!keep.contains(id)){delete items.take(id).disclosure;if(!current())return;}
        QString message="Reported by coding agent · read-only preview";
        if(mapped["omitted"].toInt()>0)message+=QString(" · %1 additional diffs omitted").arg(mapped["omitted"].toInt());
        notice->setText(message);setVisible(!items.isEmpty());
    }
private:
    struct Item{Disclosure *disclosure=nullptr;CodeEditor *text=nullptr;};
    QString key;Callbacks callbacks;QVBoxLayout *layout;QLabel *notice;QHash<QString,Item> items;quint64 generation=0;
};

class PartWidget final : public QWidget {
public:
    PartWidget(const QString &key,const QString &messageId,const QString &type,const Callbacks &callbacks,QWidget *parent):QWidget(parent),type(type),messageId(messageId),callbacks(callbacks) {
        setObjectName("part_"+key);setProperty("partKey",key);setProperty("sourceMessageId",messageId);setProperty("partType",type);
        auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(4);
        if(type=="text") {markdown=new MarkdownParts(this);layout->addWidget(markdown);connect(markdown,&MarkdownParts::linkActivated,this,[this](const QUrl &url){const auto callback=this->callbacks.link;const auto message=this->messageId;const auto part=this->partId;callback(message,part,url);});}
        else if(type=="reasoning") {
            disclosure=new Disclosure(key,callbacks,this);layout->addWidget(disclosure);
            markdown=new MarkdownParts(disclosure->body);disclosure->bodyLayout->addWidget(markdown);
            connect(markdown,&MarkdownParts::linkActivated,this,[this](const QUrl &url){const auto callback=this->callbacks.link;const auto message=this->messageId;const auto part=this->partId;callback(message,part,url);});
        } else if(type=="tool") {
            questionPart=new QuestionToolPart(this);layout->addWidget(questionPart);questionPart->hide();
            connect(questionPart,&QuestionToolPart::linkActivated,this,[this](const QUrl& url){const auto callback=this->callbacks.link;const auto message=this->messageId;const auto part=this->partId;callback(message,part,url);});
            disclosure=new Disclosure(key,callbacks,this);layout->addWidget(disclosure);
            preview=new CodeEditor(disclosure->body);preview->setObjectName("toolCodePreview");preview->setReadOnly(true);preview->setFrameShape(QFrame::NoFrame);preview->setLineWrapMode(QPlainTextEdit::NoWrap);preview->hide();disclosure->bodyLayout->addWidget(preview);
            inputSection=new Disclosure(key+"/arguments",callbacks,disclosure->body);inputSection->trigger->setObjectName("toolArgumentsToggle");disclosure->bodyLayout->addWidget(inputSection);
            input=new TranscriptText(inputSection->body);input->setObjectName("toolInput");inputSection->bodyLayout->addWidget(input);
            auto *copyInput=new QPushButton("Copy arguments",inputSection->body);copyInput->setObjectName("copyToolArguments");inputSection->bodyLayout->addWidget(copyInput,0,Qt::AlignRight);
            outputSection=new Disclosure(key+"/result",callbacks,disclosure->body);outputSection->trigger->setObjectName("toolResultToggle");disclosure->bodyLayout->addWidget(outputSection);
            output=new TranscriptText(outputSection->body);output->setObjectName("toolOutput");outputSection->bodyLayout->addWidget(output);
            acpDiffs=new ReportedDiffEntries(key,callbacks,this);layout->insertWidget(0,acpDiffs);acpDiffs->hide();
            fileDiff=new FileDiffView(disclosure->body);disclosure->bodyLayout->insertWidget(1,fileDiff);fileDiff->hide();
            auto *copyOutput=new QPushButton("Copy result",outputSection->body);copyOutput->setObjectName("copyToolResult");outputSection->bodyLayout->addWidget(copyOutput,0,Qt::AlignRight);
            connect(copyInput,&QPushButton::clicked,this,[this]{const auto value=rawInput;QApplication::clipboard()->setText(value);});
            connect(copyOutput,&QPushButton::clicked,this,[this]{const auto value=rawOutput;QApplication::clipboard()->setText(value);});
            approvalBar=new QWidget(disclosure->body);auto *buttons=new QHBoxLayout(approvalBar);buttons->setContentsMargins(0,2,0,2);
            deny=new QPushButton("Deny",approvalBar);approve=new QPushButton("Approve",approvalBar);
            deny->setObjectName("denyTool");approve->setObjectName("approveTool");buttons->addStretch();buttons->addWidget(deny);buttons->addWidget(approve);
            disclosure->bodyLayout->addWidget(approvalBar);
            connect(deny,&QPushButton::clicked,this,[this]{decide(false);});connect(approve,&QPushButton::clicked,this,[this]{decide(true);});
        } else {
            error=new TranscriptText(this);error->setObjectName("transcriptError");layout->addWidget(error);
        }
        if(markdown)connect(markdown,&MarkdownParts::imageRequested,this,[this](const QUrl &url){const auto callback=this->callbacks.image;callback(this->messageId,this->partId,url,this->markdown);});
    }
    void updatePart(const QJsonObject part,bool last,bool busy) {
        const QPointer<PartWidget> self(this);
        if(initialized&&previousPart==part&&wasLast==last&&wasBusy==busy)return;
        const auto updateEpoch=++updateGeneration;
        initialized=true;previousPart=part;wasLast=last;wasBusy=busy;
        const auto state=part["state"].toString();const bool active=!settled(state)&&busy;
        partId=part["id"].toString();setProperty("partId",partId);setProperty("displayState",state);
        if(type=="text")markdown->updateText(part["text"].toString(),state=="streaming");
        else if(type=="reasoning") {
            QString label=state=="streaming"?QStringLiteral("Thinking..."):QStringLiteral("Thought");
            if(state=="done"&&part.contains("durationMs")&&part["durationMs"].toDouble()>0)label=QString("Thought for %1s").arg(qRound64(part["durationMs"].toDouble()/1000));
            disclosure->title(label);disclosure->policy(state=="streaming"||last);
            markdown->updateText(part["text"].toString(),state=="streaming");
            auto p=markdown->palette();auto foreground=palette().color(QPalette::Text);foreground.setAlphaF(0.75);p.setColor(QPalette::Text,foreground);markdown->setPalette(p);
        } else if(type=="tool") {
            callId=part["toolCallId"].toString();
            const bool question=isNativeQuestionPart(part);
            questionPart->setVisible(question);
            if(!self||updateGeneration!=updateEpoch)return;
            if(question){
                questionPart->updatePart(part);
                if(!self||updateGeneration!=updateEpoch)return;
                // The source question card has no raw JSON/copy disclosures.
                // Canonical results remain untouched; generic copy callbacks
                // cannot expose a previously rendered password value.
                rawInput.clear();rawOutput.clear();input->setSource({},false);
                if(!self||updateGeneration!=updateEpoch)return;
                output->setSource({},false);
                if(!self||updateGeneration!=updateEpoch)return;
                inputSection->hide();outputSection->hide();preview->hide();fileDiff->hide();acpDiffs->hide();
                const bool requested=state=="approval-requested";
                disclosure->setVisible(requested);
                if(!self||updateGeneration!=updateEpoch)return;
                if(requested){disclosure->compact(false);disclosure->title("Approval required · askUserQuestions");disclosure->policy(true);}
                if(previousState!=state)decisionPending=false;
                approvalBar->setVisible(requested);canDecide=requested&&!decisionPending&&!callId.isEmpty()&&busy;
                approve->setEnabled(canDecide);deny->setEnabled(canDecide);
                approve->setProperty("toolCallId",callId);deny->setProperty("toolCallId",callId);previousState=state;
                return;
            }
            disclosure->show();
            const auto mapped=part["acpPresentation"].toObject();
            const auto mappedEntry=acpEntry(part);const auto name=visualToolName(part);
            const bool hasMapped=!mappedEntry.isEmpty();const auto mappedKind=mappedEntry["kind"].toString();
            const bool hasDiff=hasMapped&&QStringList{"write","edit","delete"}.contains(mappedKind);
            QString prefix;
            if(state=="output-available")prefix="Completed · ";
            else if(state=="output-error")prefix="Error · ";
            else if(state=="output-denied")prefix="Skipped · ";
            else if(state=="canceled")prefix="Canceled · ";
            else if(state=="approval-requested")prefix="Approval required · ";
            else if(state=="input-streaming")prefix="Preparing · ";
            else if(state=="running")prefix="Running · ";
            else prefix="Ready · ";
            const auto diagnostic=part["errorText"].toString(mappedEntry["errorText"].toString());
            auto inputText=part["inputText"].toString();if(inputText.isEmpty()&&part["input"].isObject())inputText=QString::fromUtf8(QJsonDocument(part["input"].toObject()).toJson(QJsonDocument::Indented));
            auto arguments=part["input"].toObject();if(arguments.isEmpty())arguments=QJsonDocument::fromJson(inputText.toUtf8()).object();
            if(hasMapped) {
                if(mappedKind=="read"||mappedKind=="list")arguments=QJsonObject{{"path",mappedEntry["path"]}};
                else if(mappedKind=="execute")arguments=QJsonObject{{"command",mappedEntry["command"]},{"explanation",mappedEntry["explanation"]}};
            }
            const bool compactRead=name=="read"&&state!="approval-requested";
            const bool compactMapped=hasMapped&&(mappedKind=="list"||mappedKind=="search")&&state!="approval-requested";
            disclosure->compact(compactRead||compactMapped);

            QString title=prefix+name+((state=="output-error"&&!diagnostic.isEmpty())?" · "+diagnostic:QString());
            if((name=="executeSandboxJs"||name=="executeShellCommand")&&!arguments["explanation"].toString().isEmpty())title=(state=="output-available"?QString():prefix)+arguments["explanation"].toString();
            if(compactRead){const auto path=callbacks.displayPath(arguments["path"].toString());if(state=="output-available")title="Read "+path;else if(state=="output-error")title="Error · "+(diagnostic.isEmpty()?QString("Could not read ")+path:diagnostic);else if(!settled(state))title=path.isEmpty()?QStringLiteral("Reading file..."):"Reading "+path+"...";}
            if(mappedKind=="execute"&&state!="approval-requested") {
                const auto explanation=mappedEntry["explanation"].toString();const auto command=mappedEntry["command"].toString();
                if(state=="output-error")title=diagnostic.isEmpty()?"Error running: "+command:diagnostic;
                else if(state=="output-denied")title=explanation.isEmpty()?"Skipped command":explanation;
                else if(state=="canceled")title="Canceled · "+(explanation.isEmpty()?command:explanation);
                else if(state=="output-available") {
                    title=explanation.isEmpty()?"Ran command":explanation;
                    if(mappedEntry.contains("exitCode")&&mappedEntry["exitCode"].toDouble()!=0)title+=QString(" (%1)").arg(mappedEntry["exitCode"].toDouble());
                }else title=explanation.isEmpty()?"Running "+command:explanation;
            }
            if(compactMapped) {
                if(mappedKind=="list") {
                    const auto path=callbacks.displayPath(mappedEntry["path"].toString());
                    title=state=="output-available"?"Listed "+path:path.isEmpty()?"Listing directory...":"Listing "+path+"...";
                }else {
                    const auto query=mappedEntry["query"].toString();
                    title=state=="output-available"?(mappedEntry.contains("matches")?QString("Found %1 result%2").arg(mappedEntry["matches"].toInt()).arg(mappedEntry["matches"].toInt()==1?"":"s"):QString("Search results (preview truncated)")):query.isEmpty()?"Searching with grep...":"Searching for "+query+"...";
                    if(state=="output-available"&&!query.isEmpty())title+=" for \""+query+"\"";
                }
                if(state=="output-error")title="Error · "+diagnostic;
                else if(state=="canceled"||state=="output-denied")title="Skipped · "+title;
            }
            if(hasDiff&&state!="approval-requested")title="Raw tool details · "+part["name"].toString();
            disclosure->title(title);disclosure->policy(hasDiff?state=="approval-requested":active||last||state=="approval-requested");
            acpDiffs->update(hasDiff?mapped:QJsonObject{},state,last);
            if(!self||updateGeneration!=updateEpoch)return;

            const auto script=name=="executeSandboxJs"?arguments["script"].toString():name=="executeShellCommand"?arguments["command"].toString():QString();
            preview->setVisible(!script.isEmpty());
            if(!script.isEmpty()){
                const auto code=script.left(65536);const auto cursor=preview->textCursor();
                if(preview->sourceText()!=code){preview->setPlainText(code);auto restored=preview->textCursor();restored.setPosition(qMin(cursor.anchor(),code.size()));restored.setPosition(qMin(cursor.position(),code.size()),QTextCursor::KeepAnchor);preview->setTextCursor(restored);}
                preview->setFileName(name=="executeSandboxJs"?"tool.js":"tool.sh");preview->setDarkTheme(darkFor(this));
                preview->setFixedHeight(qBound(28,preview->blockCount()*preview->fontMetrics().lineSpacing()+14,180));
                preview->setToolTip(script.size()>65536?QStringLiteral("Preview shows the first 65,536 characters; copy arguments for the full input."):QString());
            }
            rawInput=inputText;input->setSource(inputText.left(65536),false);inputSection->setVisible(!inputText.isEmpty());
            inputSection->title(inputText.size()>65536?QStringLiteral("Arguments · first 65,536 characters"):QStringLiteral("Arguments"));inputSection->policy(script.isEmpty()&&!hasDiff);
            rawOutput=part["output"].toString(part["errorText"].toString());output->setSource(rawOutput.right(65536),false);outputSection->setVisible(!rawOutput.isEmpty());
            fileDiff->setDiff(part["fileDiff"].toObject());
            QString resultLabel=state=="output-error"?"Error":active?"Live output":"Result";
            if(rawOutput.size()>65536)resultLabel+=" · last 65,536 characters";
            outputSection->title(resultLabel);outputSection->policy(!hasDiff);
            if(previousState!=state)decisionPending=false;
            const bool requested=state=="approval-requested";approvalBar->setVisible(requested);
            canDecide=requested&&!decisionPending&&!callId.isEmpty()&&busy;
            approve->setEnabled(canDecide);deny->setEnabled(canDecide);
            approve->setProperty("toolCallId",callId);deny->setProperty("toolCallId",callId);previousState=state;
        } else {
            error->setSource(part["text"].toString(part["errorText"].toString()),false);
            auto p=error->palette();p.setColor(QPalette::Text,darkFor(this)?QColor("#f5a2a7"):QColor("#ad202e"));error->setPalette(p);
        }
    }
private:
    void decide(bool allowed){if(!canDecide||decisionPending||callId.isEmpty()||previousState!="approval-requested")return;decisionPending=true;canDecide=false;approve->setEnabled(false);deny->setEnabled(false);const auto id=callId;callbacks.approval(messageId,partId,id,allowed);}
    QString type,messageId,partId,callId,previousState,rawInput,rawOutput;Callbacks callbacks;bool decisionPending=false,canDecide=false;
    QJsonObject previousPart;bool initialized=false,wasLast=false,wasBusy=false;quint64 updateGeneration=0;
    MarkdownParts *markdown=nullptr;Disclosure *disclosure=nullptr;TranscriptText *input=nullptr,*output=nullptr,*error=nullptr;
    Disclosure *inputSection=nullptr,*outputSection=nullptr;CodeEditor *preview=nullptr;
    FileDiffView* fileDiff=nullptr;ReportedDiffEntries* acpDiffs=nullptr;QuestionToolPart* questionPart=nullptr;
    QWidget *approvalBar=nullptr;QPushButton *approve=nullptr,*deny=nullptr;
};

class ExplorationGroup final : public QWidget {
public:
    ExplorationGroup(const QString &key,const QString &messageId,const Callbacks &callbacks,QWidget *parent):QWidget(parent),key(key),messageId(messageId),callbacks(callbacks) {
        auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);
        disclosure=new Disclosure(key,callbacks,this);layout->addWidget(disclosure);setObjectName("explorationGroup");
    }
    void updateParts(QList<TranscriptPart> parts,bool last,bool busy) {
        const QPointer<ExplorationGroup> self(this);const auto epoch=++generation;
        QSet<QString> keep;bool active=false;bool reasoningOnly=true;int completed=0;
        for(qsizetype index=0;index<parts.size();++index){
            const auto& reference=parts[index];const auto part=reference.value;const auto id=reference.key();keep.insert(id);
            auto *widget=widgets.value(id);if(widget&&widget->property("partType")!=part["type"].toString()){delete widgets.take(id);widget=nullptr;}if(!widget){widget=new PartWidget(key+"/"+id,reference.messageId,part["type"].toString(),callbacks,disclosure->body);widgets.insert(id,widget);}
            const auto* item=disclosure->bodyLayout->itemAt(index);
            if(!item||item->widget()!=widget){disclosure->bodyLayout->removeWidget(widget);disclosure->bodyLayout->insertWidget(index,widget);}
            widget->updatePart(part,last&&index==parts.size()-1,busy);
            if(!self||generation!=epoch)return;
            reasoningOnly=reasoningOnly&&part["type"]=="reasoning";active=active||!settled(part["state"].toString());if(part["type"]=="tool"&&part["state"]=="output-available")++completed;
        }
        const auto ids=widgets.keys();for(const auto &id:ids)if(!keep.contains(id))delete widgets.take(id);
        disclosure->title(reasoningOnly?(busy&&last&&active?QStringLiteral("Thinking..."):QStringLiteral("Thought")):busy&&last&&active?QStringLiteral("Exploring..."):completed>0?QString("Explored · %1 %2").arg(completed).arg(completed==1?"operation":"operations"):QStringLiteral("Explored the codebase"));
        disclosure->policy(last);disclosure->inlineOnly(parts.size()==1);disclosure->scroll->setLimit(384);
    }
private:
    QString key,messageId;Callbacks callbacks;Disclosure *disclosure;QHash<QString,PartWidget*> widgets;quint64 generation=0;
};

class AttachmentChips final : public QWidget {
public:
    explicit AttachmentChips(QWidget *parent):QWidget(parent){setObjectName("attachmentChips");setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);}
    void update(const QJsonArray &refs) {
        if(refs==references)return;
        references=refs;qDeleteAll(labels);labels.clear();
        for(const auto &value:refs){const auto ref=value.toObject();QString name=ref["originalFileName"].toString();if(name.isEmpty())name=ref["path"].toString().section('/',-1).section('\\',-1);
            auto *label=new QLabel(name,this);label->setObjectName("attachmentChip");label->setProperty("attachmentPath",ref["path"].toString());label->setToolTip(ref["path"].toString());
            label->setTextFormat(Qt::PlainText);label->setTextInteractionFlags(Qt::TextSelectableByMouse);label->setStyleSheet("padding:4px 7px;border:1px solid rgba(128,128,128,70);border-radius:5px;");label->show();labels.append(label);
        }measure();setVisible(!refs.isEmpty());
    }
protected:
    void resizeEvent(QResizeEvent *event) override {QWidget::resizeEvent(event);measure();}
private:
    void measure(){int x=0,y=0,rowHeight=0;const int available=qMax(1,width());for(auto *label:labels){const int w=qMin(available,qMin(300,label->sizeHint().width()));const int h=label->sizeHint().height();if(x&&x+w>available){x=0;y+=rowHeight+5;rowHeight=0;}label->setGeometry(x,y,w,h);x+=w+5;rowHeight=qMax(rowHeight,h);}setFixedHeight(labels.isEmpty()?0:y+rowHeight);}
    QList<QLabel*> labels;QJsonArray references;
};

class MessageRow final : public QWidget {
public:
    MessageRow(const QString &id,const QString &role,const Callbacks &callbacks,QWidget *parent):QWidget(parent),id(id),role(role),callbacks(callbacks) {
        setObjectName("message_"+id);setProperty("messageId",id);setProperty("role",role);
        auto *outer=new QHBoxLayout(this);outer->setContentsMargins(0,0,0,0);outer->setSpacing(0);
        body=new QFrame(this);body->setObjectName("messageBody");layout=new QVBoxLayout(body);layout->setSpacing(7);
        layout->setContentsMargins(role=="user"?10:0,role=="user"?7:2,role=="user"?10:0,role=="user"?7:2);
        if(role=="user")outer->addStretch();
        outer->addWidget(body);
        chips=new AttachmentChips(body);layout->addWidget(chips);colors();
    }
    void fileSummary(bool visible,const QJsonObject& summary={},const QString& error={},const QString& sourceRow={}){
        if(!sourceRow.isEmpty())turnRow=sourceRow;
        if(!visible&&!turnFiles)return;
        if(!turnFiles){
            turnFiles=new TurnFileEdits(body);layout->addWidget(turnFiles);
            connect(turnFiles,&TurnFileEdits::fileRequested,this,[this](const QString& path,const QStringList& ids){callbacks.turnFile(turnRow,ids,path,false);});
            connect(turnFiles,&TurnFileEdits::reviewRequested,this,[this](const QStringList& ids){callbacks.turnFile(turnRow,ids,{},false);});
            connect(turnFiles,&TurnFileEdits::savedFileRequested,this,[this](const QString& path,const QStringList& ids){callbacks.turnFile(turnRow,ids,path,true);});
            connect(turnFiles,&TurnFileEdits::undoReviewRequested,this,[this](const QStringList& ids){callbacks.turnFile(turnRow,ids,{},true);});
            connect(turnFiles,&TurnFileEdits::retryRequested,this,[this]{callbacks.retryTurn(turnRow);});
            connect(turnFiles,&TurnFileEdits::folderExpansionChanged,this,[this](const QString& key,bool expanded){callbacks.remember(id+"/files/"+key,expanded);});
        }
        turnFiles->setVisible(visible);if(!visible)return;
        if(summary.isEmpty()&&error.isEmpty())turnFiles->setPending();else turnFiles->setSummary(summary,error);
        QSet<QString> collapsed;for(const auto& entry:summary["files"].toArray()){const auto path=entry.toObject()["path"].toString();QString key;const auto pieces=path.split('/');for(int i=0;i+1<pieces.size();++i){key+=(key.isEmpty()?QString():"/")+pieces[i];if(callbacks.disclosure(id+"/files/"+key)==false)collapsed.insert(key);}}
        turnFiles->setCollapsedFolders(collapsed);
    }
    void updateRow(TranscriptGroup row,bool busy,bool lastRow) {
        const QPointer<MessageRow> self(this);const auto epoch=++generation;
        const bool active=busy&&lastRow;
        if(initialized&&data==row&&lastBusy==active&&lastPosition==lastRow)return;
        initialized=true;lastBusy=active;lastPosition=lastRow;
        data=row;const auto& parts=row.parts;QSet<QString> keep;int position=0;qsizetype index=0;
        while(index<parts.size()) {
            const auto reference=parts[index];const auto part=reference.value;const bool group=role=="assistant"&&exploratory(part);const QString key=(group?"group/":"part/")+reference.key();
            QList<TranscriptPart> grouped;if(group){while(index<parts.size()&&exploratory(parts[index].value))grouped.append(parts[index++]);}else ++index;
            keep.insert(key);auto *widget=items.value(key);if(widget&&!group&&widget->property("partType")!=part["type"].toString()){delete items.take(key);widget=nullptr;}
            if(!widget){widget=group?static_cast<QWidget*>(new ExplorationGroup(id+"/"+key,id,callbacks,body)):static_cast<QWidget*>(new PartWidget(id+"/"+key,reference.messageId,part["type"].toString(),callbacks,body));items.insert(key,widget);}
            const auto* item=layout->itemAt(position);
            if(!item||item->widget()!=widget){layout->removeWidget(widget);layout->insertWidget(position,widget);}++position;
            if(group)static_cast<ExplorationGroup*>(widget)->updateParts(grouped,lastRow&&index==parts.size(),busy&&lastRow);
            else static_cast<PartWidget*>(widget)->updatePart(part,lastRow&&index==parts.size(),busy&&lastRow);
            if(!self||generation!=epoch)return;
        }
        const auto keys=items.keys();for(const auto &key:keys)if(!keep.contains(key))delete items.take(key);
        chips->update(row.attachments);measure();
    }
protected:
    void resizeEvent(QResizeEvent *event) override{QWidget::resizeEvent(event);measure();}
    void changeEvent(QEvent *event) override{QWidget::changeEvent(event);if(event->type()==QEvent::PaletteChange)colors();}
private:
    void colors(){if(role=="user")body->setStyleSheet("QFrame#messageBody{background:"+QString(darkFor(this)?"#2c313b":"#f1f3f6")+";border:1px solid "+QString(darkFor(this)?"#3c424c":"#dfe3e8")+";border-radius:8px;}");}
    void measure(){if(role!="user")return;int desired=72;for(const auto &value:data.parts)for(const auto &line:value.value["text"].toString().split('\n'))desired=qMax(desired,fontMetrics().horizontalAdvance(line.left(1000))+24);if(!data.first["attachments"].toArray().isEmpty())desired=qMax(desired,250);body->setMaximumWidth(qMin(qMax(60,width()-16),qMin(736,desired)));}
    QString id,role,turnRow;Callbacks callbacks;QFrame *body;QVBoxLayout *layout;AttachmentChips *chips;TurnFileEdits* turnFiles=nullptr;QHash<QString,QWidget*> items;TranscriptGroup data;bool initialized=false,lastBusy=false,lastPosition=false;quint64 generation=0;
};
} // namespace

struct ConversationView::Private {
    struct ScrollState { bool follow=true;QString anchor;int offset=0,absolute=0;QHash<QString,bool> disclosures; };
    ConversationView *q;QScrollArea *scroll;QWidget *content,*tail;QVBoxLayout *layout;QToolButton *jump;QLabel *activity;
    QHash<QString,MessageRow*> rows;QHash<QString,QString> rowGroups;QHash<QString,ScrollState> states;QHash<QString,QString> attachmentNames;QStringList order;QString id;QJsonArray messages;bool busy=false;
    bool updating=false,pending=false;quint64 epoch=0,contextEpoch=0;int previousValue=0;QTimer stallTimer,scrollEnd;QElapsedTimer lastChange;
    struct ImageRequest {quint64 epoch;QString messageId,partId;QUrl url;QPointer<TranscriptText> target;QByteArray key;};
    struct ImageBinding {quint64 requestId;QPointer<TranscriptText> target;qint64 bytes=0;};
    QHash<quint64,ImageRequest> imageRequests;QHash<QByteArray,ImageBinding> requestedImages;quint64 imageRequestSequence=0;qint64 imageBytes=0;
    struct TurnRequest {quint64 epoch;QString chat,row;QStringList ids;QByteArray key;};
    struct TurnSummary {QJsonObject value;QString error;};
    QHash<quint64,TurnRequest> turnRequests;QHash<QByteArray,TurnSummary> turnCache;QList<QByteArray> turnCacheOrder;quint64 turnSequence=0;
    QByteArray turnKey(const QStringList& ids)const{return QJsonDocument(QJsonArray{id,QJsonArray::fromStringList(ids)}).toJson(QJsonDocument::Compact);}
    explicit Private(ConversationView *view):q(view){}
    ScrollState capture() const {
        auto state=states.value(id);auto *bar=scroll->verticalScrollBar();state.absolute=bar->value();state.anchor.clear();
        for(const auto &key:order){const auto *row=rows.value(key);if(row&&row->y()+row->height()>bar->value()){state.anchor=key;state.offset=bar->value()-row->y();break;}}
        return state;
    }
    void remember(){if(!id.isEmpty())states[id]=capture();}
    void userScroll(){++epoch;pending=false;updating=false;scrollEnd.stop();}
    void clearRows(){imageRequests.clear();requestedImages.clear();imageBytes=0;turnRequests.clear();qDeleteAll(rows);rows.clear();rowGroups.clear();order.clear();}
    void scheduleScroll(bool force=false){
        if(pending&&!force)return;
        pending=true;const auto generation=++epoch;const auto owner=id;
        const QPointer<ConversationView> self(q);
        QTimer::singleShot(0,q,[this,generation,owner,self]{
            if(generation!=epoch||owner!=id)return;
            pending=false;updating=true;
            layout->activate();
            if(!self||generation!=self->d->epoch||owner!=self->d->id)return;
            int lastUser=-1;for(int i=0;i<order.size();++i)if(rows[order[i]]->property("role")=="user")lastUser=i;
            int occupied=0;if(lastUser>=0)for(int i=lastUser;i<order.size();++i)occupied+=rows[order[i]]->sizeHint().height()+layout->spacing();
            tail->setFixedHeight(lastUser>=0?qMax(0,scroll->viewport()->height()-occupied-18):0);
            if(!self||generation!=self->d->epoch||owner!=self->d->id)return;
            layout->activate();
            if(!self||generation!=self->d->epoch||owner!=self->d->id)return;
            QTimer::singleShot(0,q,[this,generation,owner,self]{
                if(generation!=epoch||owner!=id)return;
                auto *bar=scroll->verticalScrollBar();
                const auto anchor=states.value(id);
                if(anchor.follow)bar->setValue(bar->maximum());
                else if(rows.contains(anchor.anchor))bar->setValue(rows[anchor.anchor]->y()+anchor.offset);
                else bar->setValue(anchor.absolute);
                // valueChanged is public and may synchronously close this view
                // or switch its owner. Do not touch the old frame afterwards.
                if(!self||generation!=self->d->epoch||owner!=self->d->id)return;
                previousValue=bar->value();updating=false;updateJump();
            });
        });
    }
    void updateMargins(){
        const double scale=qMax(1,QFontInfo(q->font()).pixelSize())/14.0;
        const int available=scroll->viewport()->width();
        const int inset=qMax(qRound(6*scale),(available-qRound(768*scale))/2+qRound(6*scale));
        layout->setContentsMargins(inset,qRound(16*scale),inset,qRound(8*scale));
    }
    void updateJump(){auto *bar=scroll->verticalScrollBar();jump->setVisible(bar->maximum()-bar->value()>100);}
};

ConversationView::ConversationView(QWidget *parent):QWidget(parent),d(std::make_unique<Private>(this)) {
    setObjectName("conversationView");auto *outer=new QVBoxLayout(this);outer->setContentsMargins(0,0,0,0);outer->setSpacing(0);
    d->scroll=new QScrollArea(this);d->scroll->setObjectName("transcriptScroll");d->scroll->setWidgetResizable(true);d->scroll->setFrameShape(QFrame::NoFrame);d->scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    d->content=new QWidget;d->content->setObjectName("transcriptContent");d->layout=new QVBoxLayout(d->content);d->layout->setContentsMargins(27,16,12,8);d->layout->setSpacing(12);d->layout->setAlignment(Qt::AlignTop);
    d->activity=new QLabel(d->content);d->activity->setObjectName("transcriptActivity");d->activity->setProperty("muted",true);d->activity->hide();d->layout->addWidget(d->activity);
    d->tail=new QWidget(d->content);d->tail->setFixedHeight(0);d->layout->addWidget(d->tail);d->scroll->setWidget(d->content);d->content->setAutoFillBackground(false);d->scroll->viewport()->setAutoFillBackground(false);d->scroll->setAutoFillBackground(false);outer->addWidget(d->scroll);
    d->jump=new QToolButton(this);d->jump->setObjectName("jumpToBottom");d->jump->setText("↓");d->jump->setToolTip("Scroll to bottom");d->jump->setAccessibleName("Scroll to bottom");d->jump->setFixedSize(30,30);d->jump->hide();
    connect(d->jump,&QToolButton::clicked,this,&ConversationView::scrollToBottom);
    auto *bar=d->scroll->verticalScrollBar();connect(bar,&QScrollBar::rangeChanged,this,[this]{d->updateJump();});
    connect(bar,&QScrollBar::valueChanged,this,[this](int value){
        if(!d->updating&&!d->id.isEmpty()){if(value<d->previousValue&&d->scroll->verticalScrollBar()->maximum()-value>100)d->states[d->id].follow=false;d->remember();d->scrollEnd.start();}
        d->previousValue=value;d->updateJump();
    });
    connect(bar,&QScrollBar::sliderReleased,this,[this]{if(d->scroll->verticalScrollBar()->maximum()-d->scroll->verticalScrollBar()->value()<=100)d->states[d->id].follow=true;});
    d->scrollEnd.setSingleShot(true);d->scrollEnd.setInterval(120);connect(&d->scrollEnd,&QTimer::timeout,this,[this]{if(!d->id.isEmpty()&&d->scroll->verticalScrollBar()->maximum()-d->scroll->verticalScrollBar()->value()<=100)d->states[d->id].follow=true;});
    d->content->installEventFilter(this);d->scroll->viewport()->installEventFilter(this);d->updateMargins();qApp->installEventFilter(this);
    d->stallTimer.setInterval(250);connect(&d->stallTimer,&QTimer::timeout,this,[this]{if(d->busy&&hasUnfinishedAssistant(d->messages)&&d->lastChange.isValid()&&d->lastChange.elapsed()>=5000){d->activity->setText("Still on it…");d->activity->show();}});
}
ConversationView::~ConversationView(){qApp->removeEventFilter(this);d->stallTimer.stop();d->scrollEnd.stop();++d->epoch;for(auto *child:findChildren<QObject*>())disconnect(child,nullptr,this,nullptr);}

void ConversationView::setConversation(const QString &id,const QJsonArray &messages,bool busy) {
    const bool switched=d->id!=id;
    if(!switched&&d->messages==messages&&d->busy==busy)return;
    if(switched){d->remember();++d->epoch;++d->contextEpoch;d->pending=false;d->updating=true;d->scrollEnd.stop();d->clearRows();d->id=id;d->messages={};if(!d->states.contains(id))d->states.insert(id,{});}
    else if(!d->pending)d->remember();
    const bool changed=d->messages!=messages||d->busy!=busy;
    d->messages=messages;d->busy=busy;
    d->attachmentNames.clear();for(const auto &value:messages)for(const auto &attachment:value.toObject()["attachments"].toArray()){const auto ref=attachment.toObject();if(!ref["originalFileName"].toString().isEmpty())d->attachmentNames.insert(ref["path"].toString(),ref["originalFileName"].toString());}
    if(changed){d->lastChange.restart();d->activity->hide();}
    if(busy)d->stallTimer.start();else{d->stallTimer.stop();d->activity->hide();}
    const QPointer<ConversationView> self(this);
    Callbacks callbacks;
    const auto contextEpoch=d->contextEpoch;
    callbacks.link=[self,id,contextEpoch](const QString &messageId,const QString &partId,const QUrl &url){
        if(!self||self->d->id!=id||self->d->contextEpoch!=contextEpoch)return;
        for(const auto &value:self->d->messages){const auto message=value.toObject();if(message["id"]!=messageId)continue;for(const auto &entry:message["parts"].toArray()){const auto part=entry.toObject();if(part["id"]==partId&&(part["type"]=="text"||part["type"]=="reasoning"||isNativeQuestionPart(part))){emit self->scopedLinkActivated(id,messageId,partId,url);if(self&&self->d->id==id&&self->d->contextEpoch==contextEpoch)emit self->linkActivated(url);return;}}}
    };
    callbacks.approval=[self,id,contextEpoch](const QString &messageId,const QString &partId,const QString &call,bool allowed){
        if(!self||self->d->id!=id||self->d->contextEpoch!=contextEpoch||!self->d->busy)return;
        for(const auto &value:self->d->messages){const auto message=value.toObject();if(message["id"]!=messageId)continue;for(const auto &entry:message["parts"].toArray()){const auto part=entry.toObject();if(part["id"]==partId&&part["type"]=="tool"&&part["toolCallId"]==call&&part["state"]=="approval-requested"){emit self->approvalDecided(id,messageId,partId,call,allowed);return;}}}
    };
    callbacks.image=[self,id,contextEpoch](const QString &messageId,const QString &partId,const QUrl &url,MarkdownParts *target){
        if(!self||self->d->id!=id||self->d->contextEpoch!=contextEpoch||!target)return;
        // A raw part can survive regrouping while its document is replaced.
        // Deduplicate actual resource recipients, not the lifetime of the chat.
        QList<QPointer<TranscriptText>> texts;
        for(auto* text:target->findChildren<TranscriptText*>())if(text->containsImage(url))texts.append(text);
        for(const auto& text:texts){
            if(!self||self->d->id!=id||self->d->contextEpoch!=contextEpoch)return;
            if(!text||!text->containsImage(url))continue;
            const auto key=QJsonDocument(QJsonArray{messageId,partId,url.toString(QUrl::FullyEncoded),QString::number(reinterpret_cast<quintptr>(text.data()),16)}).toJson(QJsonDocument::Compact);
            if(self->d->requestedImages.contains(key))continue;
            if(self->d->requestedImages.size()>=32)return;
            const auto requestId=++self->d->imageRequestSequence;
            self->d->requestedImages.insert(key,{requestId,text,0});
            self->d->imageRequests.insert(requestId,{contextEpoch,messageId,partId,url,text,key});
            connect(text,&QObject::destroyed,self,[self,key,requestId,contextEpoch]{
                if(!self||self->d->contextEpoch!=contextEpoch)return;
                const auto binding=self->d->requestedImages.constFind(key);
                if(binding==self->d->requestedImages.cend()||binding->requestId!=requestId)return;
                self->d->imageBytes-=binding->bytes;
                self->d->requestedImages.remove(key);self->d->imageRequests.remove(requestId);
            });
            emit self->imageRequested(requestId,id,messageId,partId,url);
        }
    };
    callbacks.turnFile=[self,id,contextEpoch](const QString& row,const QStringList& ids,const QString& path,bool historical){
        if(!self||self->d->id!=id||self->d->contextEpoch!=contextEpoch)return;
        const auto projected=self->turnMutationIds(row);
        if(projected.isEmpty()||projected.size()!=ids.size()||QSet<QString>(projected.cbegin(),projected.cend())!=QSet<QString>(ids.cbegin(),ids.cend()))return;
        const auto key=self->d->turnKey(projected);const auto cached=self->d->turnCache.value(key);
        if(!cached.error.isEmpty()||!TurnFileEdits::validSummary(cached.value))return;
        if(path.isEmpty()){if(historical)emit self->turnUndoReviewRequested(id,row,projected);else emit self->turnFilesReviewRequested(id,row,projected);return;}
        for(const auto& file:cached.value["files"].toArray())if(file.toObject()["path"]==path){if(historical)emit self->savedTurnFileRequested(id,row,projected,path);else emit self->turnFileRequested(id,row,projected,path);return;}
    };
    callbacks.retryTurn=[self,id,contextEpoch](const QString& row){
        if(!self||self->d->id!=id||self->d->contextEpoch!=contextEpoch)return;
        const auto ids=self->turnMutationIds(row);if(ids.isEmpty())return;
        const auto key=self->d->turnKey(ids);self->d->turnCache.remove(key);self->d->turnCacheOrder.removeAll(key);
        for(const auto& request:self->d->turnRequests)if(request.key==key)return;
        if(auto* widget=self->d->rows.value(self->d->rowGroups.value(row)))widget->fileSummary(true,{}, {},row);
        const auto request=++self->d->turnSequence;self->d->turnRequests.insert(request,{contextEpoch,id,row,ids,key});
        emit self->turnFileEditsRequested(request,id,row,ids);
    };
    callbacks.disclosure=[this,id](const QString &key)->std::optional<bool>{const auto state=d->states.value(id);const auto it=state.disclosures.constFind(key);if(it==state.disclosures.cend())return {};return *it;};
    callbacks.remember=[this,id](const QString &key,bool expanded){d->states[id].disclosures[key]=expanded;};
    callbacks.displayPath=[this](const QString &raw){if(d->attachmentNames.contains(raw))return d->attachmentNames.value(raw);QString path=raw;path.replace('\\','/');for(const auto &prefix:{QString("att/"),QString("plugins/"),QString("apps/")})if(path.startsWith(prefix))return path.mid(prefix.size());path.remove(QRegularExpression("^(?:ws1|w[0-9a-f]{1,8})/"));return path;};
    const auto selections=TurnFileEdits::selections(messages);QList<quint64> requestIds;
    const auto groups=groupTranscriptRows(messages);
    d->rowGroups.clear();for(const auto& group:groups)for(const auto& source:group.messageIds)d->rowGroups.insert(source,group.first["id"].toString());
    QSet<QString> keep;d->order.clear();int position=0;
    for(qsizetype index=0;index<groups.size();++index){
        const auto& row=groups[index];const auto key=row.first["id"].toString();const auto role=row.first["role"].toString();
        const auto selectionRow=row.messageIds.last();
        if(key.isEmpty()||keep.contains(key)||role=="system"||role=="tool")continue;
        keep.insert(key);d->order.append(key);auto *widget=d->rows.value(key);
        if(widget&&widget->property("role").toString()!=role){delete d->rows.take(key);widget=nullptr;}
        if(!widget){widget=new MessageRow(key,role,callbacks,d->content);d->rows.insert(key,widget);}
        const auto* layoutItem=d->layout->itemAt(position);
        if(!layoutItem||layoutItem->widget()!=widget){d->layout->removeWidget(widget);d->layout->insertWidget(position,widget);}++position;widget->updateRow(row,busy,index==groups.size()-1);
        if(!self||self->d->id!=id||self->d->contextEpoch!=contextEpoch||self->d->messages!=messages)return;
        const bool show=selections.contains(selectionRow)&&!(busy&&index==groups.size()-1);
        if(!show)widget->fileSummary(false);
        else{
            const auto selection=selections.value(selectionRow);const auto cacheKey=d->turnKey(selection.mutationIds);
            if(selection.incomplete)widget->fileSummary(true,{},"This turn exceeds the saved-change selection limit; review changes from the File changes panel.",selectionRow);
            else if(d->turnCache.contains(cacheKey)){const auto cached=d->turnCache.value(cacheKey);widget->fileSummary(true,cached.value,cached.error,selectionRow);}
            else{
                widget->fileSummary(true,{}, {},selectionRow);bool pending=false;for(const auto& request:d->turnRequests)if(request.key==cacheKey&&request.row==selectionRow)pending=true;
                if(!pending){const auto request=++d->turnSequence;d->turnRequests.insert(request,{contextEpoch,id,selectionRow,selection.mutationIds,cacheKey});requestIds.append(request);}
            }
        }
    }
    const auto keys=d->rows.keys();for(const auto &key:keys)if(!keep.contains(key))delete d->rows.take(key);
    d->scheduleScroll(switched);
    for(const auto requestId:requestIds){if(!self||self->d->id!=id||self->d->contextEpoch!=contextEpoch)return;const auto request=self->d->turnRequests.value(requestId);if(request.chat==id&&self->turnMutationIds(request.row)==request.ids)emit self->turnFileEditsRequested(requestId,id,request.row,request.ids);}
}
void ConversationView::clear(){d->remember();++d->epoch;++d->contextEpoch;d->pending=false;d->updating=true;d->scrollEnd.stop();d->clearRows();d->messages={};d->id.clear();d->busy=false;d->activity->hide();d->stallTimer.stop();d->tail->setFixedHeight(0);d->jump->hide();d->updating=false;}
QString ConversationView::toPlainText() const {QStringList text;for(const auto &value:d->messages){const auto row=value.toObject();for(const auto &part:row["parts"].toArray())if(!AcpToolPresentation::suppressQuestionWrapperPart(part.toObject()))text.append(partPlain(part.toObject()));for(const auto &attachment:row["attachments"].toArray()){const auto ref=attachment.toObject();text.append("Attachment: "+ref["originalFileName"].toString(ref["path"].toString()));}}return text.join('\n');}
QString ConversationView::conversationId() const{return d->id;}
QStringList ConversationView::turnMutationIds(const QString& rowId)const{
    const auto selection=TurnFileEdits::selections(d->messages).value(rowId);
    if(selection.incomplete||(d->busy&&!d->messages.isEmpty()&&d->messages.last().toObject()["id"]==rowId))return {};
    return selection.mutationIds;
}
bool ConversationView::provideTurnFileEdits(quint64 requestId,const QJsonObject& summary,const QString& error){
    const auto it=d->turnRequests.find(requestId);if(it==d->turnRequests.end())return false;
    const auto request=*it;d->turnRequests.erase(it);
    if(request.epoch!=d->contextEpoch||request.chat!=d->id||turnMutationIds(request.row)!=request.ids)return false;
    QString issue=error;
    if(issue.isEmpty()){
        QSet<QString> returned;for(const auto& id:summary["mutationIds"].toArray())returned.insert(id.toString());
        const QSet<QString> expected(request.ids.cbegin(),request.ids.cend());
        if(!TurnFileEdits::validSummary(summary)||summary["agentId"]!=request.chat||returned!=expected)issue="Saved-change response does not match this chat and turn.";
    }
    const auto value=issue.isEmpty()?summary:QJsonObject{};d->turnCache.insert(request.key,{value,issue});d->turnCacheOrder.removeAll(request.key);d->turnCacheOrder.append(request.key);
    while(d->turnCacheOrder.size()>200)d->turnCache.remove(d->turnCacheOrder.takeFirst());
    if(auto* row=d->rows.value(d->rowGroups.value(request.row)))row->fileSummary(true,value,issue,request.row);
    d->scheduleScroll();return true;
}
bool ConversationView::followsOutput() const{return d->states.value(d->id).follow;}
bool ConversationView::provideImage(quint64 requestId,const QImage &image,const QString &error){
    const auto it=d->imageRequests.find(requestId);if(it==d->imageRequests.end())return false;
    const auto request=*it;d->imageRequests.erase(it);
    if(request.epoch!=d->contextEpoch||!request.target)return false;
    const auto binding=d->requestedImages.find(request.key);
    if(binding==d->requestedImages.end()||binding->requestId!=requestId||binding->target!=request.target)return false;
    bool present=false;for(const auto &value:d->messages){const auto row=value.toObject();if(row["id"]!=request.messageId)continue;for(const auto &entry:row["parts"].toArray()){const auto part=entry.toObject();if(part["id"]==request.partId&&(part["type"]=="text"||part["type"]=="reasoning"))present=true;}}
    if(!present||!request.target->containsImage(request.url))return false;
    if(!error.isEmpty()||image.isNull()){request.target->setProperty("imageError",error.left(512));return false;}
    if(image.width()>16384||image.height()>16384||image.sizeInBytes()>64*1024*1024)return false;
    const auto bounded=image.width()>2048||image.height()>2048?image.scaled(2048,2048,Qt::KeepAspectRatio,Qt::SmoothTransformation):image;
    if(d->imageBytes+bounded.sizeInBytes()>64*1024*1024)return false;
    binding->bytes=bounded.sizeInBytes();d->imageBytes+=binding->bytes;
    request.target->setTrustedImage(request.url,bounded);
    return true;
}
void ConversationView::scrollToBottom(){if(d->id.isEmpty())return;d->states[d->id].follow=true;d->scheduleScroll(true);}
bool ConversationView::eventFilter(QObject *object,QEvent *event){
    if(object==d->scroll->viewport()&&event->type()==QEvent::Resize)d->updateMargins();
    if(object==d->content&&event->type()==QEvent::LayoutRequest&&!d->id.isEmpty())d->scheduleScroll();
    auto *bar=d->scroll->verticalScrollBar();
    if(event->type()==QEvent::MouseButtonPress&&object==bar)d->userScroll();
    if(event->type()==QEvent::KeyPress&&(object==bar||object==d->scroll||object==d->scroll->viewport())){
        const auto key=static_cast<QKeyEvent*>(event)->key();
        if(key==Qt::Key_Up||key==Qt::Key_Down||key==Qt::Key_PageUp||key==Qt::Key_PageDown||key==Qt::Key_Home||key==Qt::Key_End)d->userScroll();
    }
    if(event->type()==QEvent::MouseButtonPress||event->type()==QEvent::KeyPress){auto *widget=qobject_cast<QWidget*>(object);if(widget&&(widget==this||isAncestorOf(widget))&&!d->pending)d->remember();}
    if(event->type()==QEvent::Wheel){auto *widget=qobject_cast<QWidget*>(object);if(widget&&(widget==this||isAncestorOf(widget))){auto *wheel=static_cast<QWheelEvent*>(event);if(wheel->angleDelta().y()>0||wheel->pixelDelta().y()>0){d->states[d->id].follow=false;d->userScroll();}else if(d->scroll->verticalScrollBar()->maximum()-d->scroll->verticalScrollBar()->value()<=100)d->states[d->id].follow=true;}}
    return QWidget::eventFilter(object,event);
}
void ConversationView::resizeEvent(QResizeEvent *event){if(!d->id.isEmpty()&&!d->pending)d->remember();QWidget::resizeEvent(event);d->jump->move(width()-45,height()-43);if(!d->id.isEmpty())d->scheduleScroll();}
void ConversationView::changeEvent(QEvent *event){
    const QPointer<ConversationView> self(this);QWidget::changeEvent(event);if(!self)return;
    if(event->type()==QEvent::FontChange)d->updateMargins();
    if(!self)return;
    if(event->type()==QEvent::PaletteChange){
        d->jump->setStyleSheet("QToolButton{border:1px solid "+QString(darkFor(this)?"#505762":"#d5dae2")+";border-radius:15px;background:"+QString(darkFor(this)?"#2c313b":"#ffffff")+";}");
        if(!self)return;
        QList<QPointer<MarkdownParts>> texts;for(auto* text:findChildren<MarkdownParts*>())texts.append(text);
        for(const auto& text:texts){if(text)text->refreshAppearance();if(!self)return;}
    }
    if(event->type()==QEvent::PaletteChange||event->type()==QEvent::FontChange){
        QList<QPointer<QuestionToolPart>> questions;for(auto* question:findChildren<QuestionToolPart*>())questions.append(question);
        for(const auto& question:questions){if(question)question->refreshAppearance();if(!self)return;}
    }
}
} // namespace ade
