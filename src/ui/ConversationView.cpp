#include "ConversationView.h"
#include "MarkdownParts.h"
#include "FileDiffView.h"
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
bool exploratory(const QJsonObject &part){
    if(part["type"]=="reasoning")return true;
    return part["type"]=="tool"&&QStringList{"glob","grepSearch","read","ls","searchInLibraryDocs","listLibraryDocs","executeSandboxJs","readConsoleLogs","getLintingDiagnostics"}.contains(part["name"].toString());
}
QString partPlain(const QJsonObject &part){
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

class PartWidget final : public QWidget {
public:
    PartWidget(const QString &key,const QString &messageId,const QString &type,const Callbacks &callbacks,QWidget *parent):QWidget(parent),type(type),messageId(messageId),callbacks(callbacks) {
        setObjectName("part_"+key);setProperty("partKey",key);setProperty("partType",type);
        auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(4);
        if(type=="text") {markdown=new MarkdownParts(this);layout->addWidget(markdown);connect(markdown,&MarkdownParts::linkActivated,this,[this](const QUrl &url){const auto callback=this->callbacks.link;const auto message=this->messageId;const auto part=this->partId;callback(message,part,url);});}
        else if(type=="reasoning") {
            disclosure=new Disclosure(key,callbacks,this);layout->addWidget(disclosure);
            markdown=new MarkdownParts(disclosure->body);disclosure->bodyLayout->addWidget(markdown);
            connect(markdown,&MarkdownParts::linkActivated,this,[this](const QUrl &url){const auto callback=this->callbacks.link;const auto message=this->messageId;const auto part=this->partId;callback(message,part,url);});
        } else if(type=="tool") {
            disclosure=new Disclosure(key,callbacks,this);layout->addWidget(disclosure);
            preview=new CodeEditor(disclosure->body);preview->setObjectName("toolCodePreview");preview->setReadOnly(true);preview->setFrameShape(QFrame::NoFrame);preview->setLineWrapMode(QPlainTextEdit::NoWrap);preview->hide();disclosure->bodyLayout->addWidget(preview);
            inputSection=new Disclosure(key+"/arguments",callbacks,disclosure->body);inputSection->trigger->setObjectName("toolArgumentsToggle");disclosure->bodyLayout->addWidget(inputSection);
            input=new TranscriptText(inputSection->body);input->setObjectName("toolInput");inputSection->bodyLayout->addWidget(input);
            auto *copyInput=new QPushButton("Copy arguments",inputSection->body);copyInput->setObjectName("copyToolArguments");inputSection->bodyLayout->addWidget(copyInput,0,Qt::AlignRight);
            outputSection=new Disclosure(key+"/result",callbacks,disclosure->body);outputSection->trigger->setObjectName("toolResultToggle");disclosure->bodyLayout->addWidget(outputSection);
            output=new TranscriptText(outputSection->body);output->setObjectName("toolOutput");outputSection->bodyLayout->addWidget(output);
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
    void updatePart(const QJsonObject &part,bool last,bool busy) {
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
            callId=part["toolCallId"].toString();const auto name=part["name"].toString("Tool");
            QString prefix;
            if(state=="output-available")prefix="Completed · ";
            else if(state=="output-error")prefix="Error · ";
            else if(state=="output-denied")prefix="Skipped · ";
            else if(state=="canceled")prefix="Canceled · ";
            else if(state=="approval-requested")prefix="Approval required · ";
            else if(state=="input-streaming")prefix="Preparing · ";
            else if(state=="running")prefix="Running · ";
            else prefix="Ready · ";
            const auto diagnostic=part["errorText"].toString();
            auto inputText=part["inputText"].toString();if(inputText.isEmpty()&&part["input"].isObject())inputText=QString::fromUtf8(QJsonDocument(part["input"].toObject()).toJson(QJsonDocument::Indented));
            auto arguments=part["input"].toObject();if(arguments.isEmpty())arguments=QJsonDocument::fromJson(inputText.toUtf8()).object();
            const bool compactRead=name=="read"&&state!="approval-requested";
            disclosure->compact(compactRead);
            QString title=prefix+name+((state=="output-error"&&!diagnostic.isEmpty())?" · "+diagnostic:QString());
            if((name=="executeSandboxJs"||name=="executeShellCommand")&&!arguments["explanation"].toString().isEmpty())title=(state=="output-available"?QString():prefix)+arguments["explanation"].toString();
            if(compactRead){auto args=part["input"].toObject();if(args.isEmpty())args=QJsonDocument::fromJson(inputText.toUtf8()).object();const auto path=callbacks.displayPath(args["path"].toString());if(state=="output-available")title="Read "+path;else if(state=="output-error")title="Error · "+(diagnostic.isEmpty()?QString("Could not read ")+path:diagnostic);else if(!settled(state))title=path.isEmpty()?QStringLiteral("Reading file..."):"Reading "+path+"...";}
            disclosure->title(title);disclosure->policy(active||last||state=="approval-requested");
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
            inputSection->title(inputText.size()>65536?QStringLiteral("Arguments · first 65,536 characters"):QStringLiteral("Arguments"));inputSection->policy(script.isEmpty());
            rawOutput=part["output"].toString(part["errorText"].toString());output->setSource(rawOutput.right(65536),false);outputSection->setVisible(!rawOutput.isEmpty());
            fileDiff->setDiff(part["fileDiff"].toObject());
            QString resultLabel=state=="output-error"?"Error":active?"Live output":"Result";
            if(rawOutput.size()>65536)resultLabel+=" · last 65,536 characters";
            outputSection->title(resultLabel);outputSection->policy(true);
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
    MarkdownParts *markdown=nullptr;Disclosure *disclosure=nullptr;TranscriptText *input=nullptr,*output=nullptr,*error=nullptr;
    Disclosure *inputSection=nullptr,*outputSection=nullptr;CodeEditor *preview=nullptr;
    FileDiffView* fileDiff=nullptr;
    QWidget *approvalBar=nullptr;QPushButton *approve=nullptr,*deny=nullptr;
};

class ExplorationGroup final : public QWidget {
public:
    ExplorationGroup(const QString &key,const QString &messageId,const Callbacks &callbacks,QWidget *parent):QWidget(parent),key(key),messageId(messageId),callbacks(callbacks) {
        auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);
        disclosure=new Disclosure(key,callbacks,this);layout->addWidget(disclosure);setObjectName("explorationGroup");
    }
    void updateParts(const QJsonArray &parts,bool last,bool busy) {
        QSet<QString> keep;bool active=false;int completed=0;
        for(qsizetype index=0;index<parts.size();++index){
            const auto part=parts[index].toObject();const auto id=part["id"].toString();keep.insert(id);
            auto *widget=widgets.value(id);if(widget&&widget->property("partType")!=part["type"].toString()){delete widgets.take(id);widget=nullptr;}if(!widget){widget=new PartWidget(key+"/"+id,messageId,part["type"].toString(),callbacks,disclosure->body);widgets.insert(id,widget);}
            if(disclosure->bodyLayout->indexOf(widget)!=index){disclosure->bodyLayout->removeWidget(widget);disclosure->bodyLayout->insertWidget(index,widget);}
            widget->updatePart(part,last&&index==parts.size()-1,busy);
            active=active||!settled(part["state"].toString());if(part["type"]=="tool"&&part["state"]=="output-available")++completed;
        }
        const auto ids=widgets.keys();for(const auto &id:ids)if(!keep.contains(id))delete widgets.take(id);
        disclosure->title(busy&&last&&active?QStringLiteral("Exploring..."):completed>0?QString("Explored · %1 %2").arg(completed).arg(completed==1?"operation":"operations"):QStringLiteral("Explored the codebase"));
        disclosure->policy(last);disclosure->inlineOnly(parts.size()==1);disclosure->scroll->setLimit(384);
    }
private:
    QString key,messageId;Callbacks callbacks;Disclosure *disclosure;QHash<QString,PartWidget*> widgets;
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
    void updateRow(const QJsonObject &row,bool busy,bool lastRow) {
        const bool active=busy&&lastRow;
        if(initialized&&data==row&&lastBusy==active&&lastPosition==lastRow)return;
        initialized=true;lastBusy=active;lastPosition=lastRow;
        data=row;const auto parts=row["parts"].toArray();QSet<QString> keep;int position=0;qsizetype index=0;
        while(index<parts.size()) {
            const auto part=parts[index].toObject();const bool group=role=="assistant"&&exploratory(part);const QString key=(group?"group/":"part/")+part["id"].toString();
            QJsonArray grouped;if(group){while(index<parts.size()&&exploratory(parts[index].toObject()))grouped.append(parts[index++]);}else ++index;
            keep.insert(key);auto *widget=items.value(key);if(widget&&!group&&widget->property("partType")!=part["type"].toString()){delete items.take(key);widget=nullptr;}
            if(!widget){widget=group?static_cast<QWidget*>(new ExplorationGroup(id+"/"+key,id,callbacks,body)):static_cast<QWidget*>(new PartWidget(id+"/"+key,id,part["type"].toString(),callbacks,body));items.insert(key,widget);}
            if(layout->indexOf(widget)!=position){layout->removeWidget(widget);layout->insertWidget(position,widget);}++position;
            if(group)static_cast<ExplorationGroup*>(widget)->updateParts(grouped,lastRow&&index==parts.size(),busy&&lastRow);
            else static_cast<PartWidget*>(widget)->updatePart(part,lastRow&&index==parts.size(),busy&&lastRow);
        }
        const auto keys=items.keys();for(const auto &key:keys)if(!keep.contains(key))delete items.take(key);
        chips->update(row["attachments"].toArray());measure();
    }
protected:
    void resizeEvent(QResizeEvent *event) override{QWidget::resizeEvent(event);measure();}
    void changeEvent(QEvent *event) override{QWidget::changeEvent(event);if(event->type()==QEvent::PaletteChange)colors();}
private:
    void colors(){if(role=="user")body->setStyleSheet("QFrame#messageBody{background:"+QString(darkFor(this)?"#2c313b":"#f1f3f6")+";border:1px solid "+QString(darkFor(this)?"#3c424c":"#dfe3e8")+";border-radius:8px;}");}
    void measure(){if(role!="user")return;int desired=72;for(const auto &value:data["parts"].toArray())for(const auto &line:value.toObject()["text"].toString().split('\n'))desired=qMax(desired,fontMetrics().horizontalAdvance(line.left(1000))+24);if(!data["attachments"].toArray().isEmpty())desired=qMax(desired,250);body->setMaximumWidth(qMin(qMax(60,width()-16),qMin(736,desired)));}
    QString id,role;Callbacks callbacks;QFrame *body;QVBoxLayout *layout;AttachmentChips *chips;QHash<QString,QWidget*> items;QJsonObject data;bool initialized=false,lastBusy=false,lastPosition=false;
};
} // namespace

struct ConversationView::Private {
    struct ScrollState { bool follow=true;QString anchor;int offset=0,absolute=0;QHash<QString,bool> disclosures; };
    ConversationView *q;QScrollArea *scroll;QWidget *content,*tail;QVBoxLayout *layout;QToolButton *jump;QLabel *activity;
    QHash<QString,MessageRow*> rows;QHash<QString,ScrollState> states;QHash<QString,QString> attachmentNames;QStringList order;QString id;QJsonArray messages;bool busy=false;
    bool updating=false,pending=false;quint64 epoch=0,contextEpoch=0;int previousValue=0;QTimer stallTimer,scrollEnd;QElapsedTimer lastChange;
    struct ImageRequest {quint64 epoch;QString messageId,partId;QUrl url;QPointer<MarkdownParts> target;};
    QHash<quint64,ImageRequest> imageRequests;QSet<QByteArray> requestedImages;quint64 imageRequestSequence=0;qint64 imageBytes=0;
    explicit Private(ConversationView *view):q(view){}
    ScrollState capture() const {
        auto state=states.value(id);auto *bar=scroll->verticalScrollBar();state.absolute=bar->value();state.anchor.clear();
        for(const auto &key:order){const auto *row=rows.value(key);if(row&&row->y()+row->height()>bar->value()){state.anchor=key;state.offset=bar->value()-row->y();break;}}
        return state;
    }
    void remember(){if(!id.isEmpty())states[id]=capture();}
    void clearRows(){imageRequests.clear();requestedImages.clear();imageBytes=0;qDeleteAll(rows);rows.clear();order.clear();}
    void scheduleScroll(bool force=false){
        if(pending&&!force)return;
        pending=true;const auto generation=++epoch;const auto owner=id;const auto anchor=states.value(id);
        const QPointer<ConversationView> self(q);
        QTimer::singleShot(0,q,[this,generation,owner,anchor,self]{
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
            QTimer::singleShot(0,q,[this,generation,owner,anchor,self]{
                if(generation!=epoch||owner!=id)return;
                auto *bar=scroll->verticalScrollBar();
                if(states.value(id).follow)bar->setValue(bar->maximum());
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
    void updateJump(){auto *bar=scroll->verticalScrollBar();jump->setVisible(bar->maximum()-bar->value()>2);}
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
        if(!d->updating&&!d->id.isEmpty()){if(value<d->previousValue&&d->scroll->verticalScrollBar()->maximum()-value>100)d->states[d->id].follow=false;d->scrollEnd.start();}
        d->previousValue=value;d->updateJump();
    });
    connect(bar,&QScrollBar::sliderReleased,this,[this]{if(d->scroll->verticalScrollBar()->maximum()-d->scroll->verticalScrollBar()->value()<=100)d->states[d->id].follow=true;});
    d->scrollEnd.setSingleShot(true);d->scrollEnd.setInterval(120);connect(&d->scrollEnd,&QTimer::timeout,this,[this]{if(!d->id.isEmpty()&&d->scroll->verticalScrollBar()->maximum()-d->scroll->verticalScrollBar()->value()<=100)d->states[d->id].follow=true;});
    d->content->installEventFilter(this);d->scroll->viewport()->installEventFilter(this);d->updateMargins();qApp->installEventFilter(this);
    d->stallTimer.setInterval(250);connect(&d->stallTimer,&QTimer::timeout,this,[this]{if(d->busy&&d->lastChange.isValid()&&d->lastChange.elapsed()>=5000){d->activity->setText("Still on it…");d->activity->show();}});
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
        for(const auto &value:self->d->messages){const auto message=value.toObject();if(message["id"]!=messageId)continue;for(const auto &entry:message["parts"].toArray()){const auto part=entry.toObject();if(part["id"]==partId&&(part["type"]=="text"||part["type"]=="reasoning")){emit self->scopedLinkActivated(id,messageId,partId,url);if(self&&self->d->id==id&&self->d->contextEpoch==contextEpoch)emit self->linkActivated(url);return;}}}
    };
    callbacks.approval=[self,id,contextEpoch](const QString &messageId,const QString &partId,const QString &call,bool allowed){
        if(!self||self->d->id!=id||self->d->contextEpoch!=contextEpoch||!self->d->busy)return;
        for(const auto &value:self->d->messages){const auto message=value.toObject();if(message["id"]!=messageId)continue;for(const auto &entry:message["parts"].toArray()){const auto part=entry.toObject();if(part["id"]==partId&&part["type"]=="tool"&&part["toolCallId"]==call&&part["state"]=="approval-requested"){emit self->approvalDecided(id,messageId,partId,call,allowed);return;}}}
    };
    callbacks.image=[self,id,contextEpoch](const QString &messageId,const QString &partId,const QUrl &url,MarkdownParts *target){
        if(!self||self->d->id!=id||self->d->contextEpoch!=contextEpoch||!target)return;
        if(self->d->requestedImages.size()>=32)return;
        const auto key=QJsonDocument(QJsonArray{messageId,partId,url.toString(QUrl::FullyEncoded)}).toJson(QJsonDocument::Compact);
        if(self->d->requestedImages.contains(key))return;
        self->d->requestedImages.insert(key);const auto requestId=++self->d->imageRequestSequence;
        self->d->imageRequests.insert(requestId,{contextEpoch,messageId,partId,url,target});
        emit self->imageRequested(requestId,id,messageId,partId,url);
    };
    callbacks.disclosure=[this,id](const QString &key)->std::optional<bool>{const auto state=d->states.value(id);const auto it=state.disclosures.constFind(key);if(it==state.disclosures.cend())return {};return *it;};
    callbacks.remember=[this,id](const QString &key,bool expanded){d->states[id].disclosures[key]=expanded;};
    callbacks.displayPath=[this](const QString &raw){if(d->attachmentNames.contains(raw))return d->attachmentNames.value(raw);QString path=raw;path.replace('\\','/');for(const auto &prefix:{QString("att/"),QString("plugins/"),QString("apps/")})if(path.startsWith(prefix))return path.mid(prefix.size());path.remove(QRegularExpression("^(?:ws1|w[0-9a-f]{1,8})/"));return path;};
    QSet<QString> keep;d->order.clear();int position=0;
    for(qsizetype index=0;index<messages.size();++index){
        const auto row=messages[index].toObject();const auto key=row["id"].toString();const auto role=row["role"].toString();
        if(key.isEmpty()||keep.contains(key)||role=="system"||role=="tool")continue;
        keep.insert(key);d->order.append(key);auto *widget=d->rows.value(key);
        if(!widget){widget=new MessageRow(key,role,callbacks,d->content);d->rows.insert(key,widget);}
        if(d->layout->indexOf(widget)!=position){d->layout->removeWidget(widget);d->layout->insertWidget(position,widget);}++position;widget->updateRow(row,busy,index==messages.size()-1);
    }
    const auto keys=d->rows.keys();for(const auto &key:keys)if(!keep.contains(key))delete d->rows.take(key);
    d->scheduleScroll(switched);
}
void ConversationView::clear(){d->remember();++d->epoch;++d->contextEpoch;d->pending=false;d->updating=true;d->scrollEnd.stop();d->clearRows();d->messages={};d->id.clear();d->busy=false;d->activity->hide();d->stallTimer.stop();d->tail->setFixedHeight(0);d->jump->hide();d->updating=false;}
QString ConversationView::toPlainText() const {QStringList text;for(const auto &value:d->messages){const auto row=value.toObject();for(const auto &part:row["parts"].toArray())text.append(partPlain(part.toObject()));for(const auto &attachment:row["attachments"].toArray()){const auto ref=attachment.toObject();text.append("Attachment: "+ref["originalFileName"].toString(ref["path"].toString()));}}return text.join('\n');}
QString ConversationView::conversationId() const{return d->id;}
bool ConversationView::followsOutput() const{return d->states.value(d->id).follow;}
bool ConversationView::provideImage(quint64 requestId,const QImage &image,const QString &error){
    const auto it=d->imageRequests.find(requestId);if(it==d->imageRequests.end())return false;
    const auto request=*it;d->imageRequests.erase(it);
    if(request.epoch!=d->contextEpoch||!request.target)return false;
    bool present=false;for(const auto &value:d->messages){const auto row=value.toObject();if(row["id"]!=request.messageId)continue;for(const auto &entry:row["parts"].toArray()){const auto part=entry.toObject();if(part["id"]==request.partId&&(part["type"]=="text"||part["type"]=="reasoning"))present=true;}}
    if(!present)return false;
    if(!error.isEmpty()||image.isNull()){request.target->setProperty("imageError",error.left(512));return false;}
    if(image.width()>16384||image.height()>16384||image.sizeInBytes()>64*1024*1024)return false;
    const auto bounded=image.width()>2048||image.height()>2048?image.scaled(2048,2048,Qt::KeepAspectRatio,Qt::SmoothTransformation):image;
    if(d->imageBytes+bounded.sizeInBytes()>64*1024*1024)return false;
    QList<QPointer<TranscriptText>> texts;for(auto *text:request.target->findChildren<TranscriptText*>())if(text->containsImage(request.url))texts.append(text);
    if(texts.isEmpty())return false;
    d->imageBytes+=bounded.sizeInBytes();const QPointer<ConversationView> self(this);const auto context=d->contextEpoch;
    for(const auto &text:texts){if(text)text->setTrustedImage(request.url,bounded);if(!self||context!=self->d->contextEpoch)return true;}
    return true;
}
void ConversationView::scrollToBottom(){if(d->id.isEmpty())return;d->states[d->id].follow=true;d->scheduleScroll(true);}
bool ConversationView::eventFilter(QObject *object,QEvent *event){
    if(object==d->scroll->viewport()&&event->type()==QEvent::Resize)d->updateMargins();
    if(object==d->content&&event->type()==QEvent::LayoutRequest&&!d->id.isEmpty())d->scheduleScroll();
    if(event->type()==QEvent::MouseButtonPress||event->type()==QEvent::KeyPress){auto *widget=qobject_cast<QWidget*>(object);if(widget&&(widget==this||isAncestorOf(widget))&&!d->pending)d->remember();}
    if(event->type()==QEvent::Wheel){auto *widget=qobject_cast<QWidget*>(object);if(widget&&(widget==this||isAncestorOf(widget))){auto *wheel=static_cast<QWheelEvent*>(event);if(wheel->angleDelta().y()>0||wheel->pixelDelta().y()>0){d->states[d->id].follow=false;++d->epoch;d->pending=false;d->updating=false;}else if(d->scroll->verticalScrollBar()->maximum()-d->scroll->verticalScrollBar()->value()<=100)d->states[d->id].follow=true;}}
    return QWidget::eventFilter(object,event);
}
void ConversationView::resizeEvent(QResizeEvent *event){if(!d->id.isEmpty()&&!d->pending)d->remember();QWidget::resizeEvent(event);d->jump->move(width()-45,height()-43);if(!d->id.isEmpty())d->scheduleScroll();}
void ConversationView::changeEvent(QEvent *event){QWidget::changeEvent(event);if(event->type()==QEvent::FontChange)d->updateMargins();if(event->type()==QEvent::PaletteChange){d->jump->setStyleSheet("QToolButton{border:1px solid "+QString(darkFor(this)?"#505762":"#d5dae2")+";border-radius:15px;background:"+QString(darkFor(this)?"#2c313b":"#ffffff")+";}");for(auto *markdown:findChildren<MarkdownParts*>())markdown->refreshAppearance();}}
} // namespace ade
