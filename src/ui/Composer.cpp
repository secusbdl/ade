#include "Composer.h"
#include <QApplication>
#include <QFocusEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHideEvent>
#include <QListWidget>
#include <QPointer>
#include <QScreen>
#include <QSet>
#include <QVBoxLayout>

namespace ade {
void Composer::initializeCompletion() {
    m_completionPopup=new QFrame(this,Qt::ToolTip|Qt::FramelessWindowHint);
    m_completionPopup->setObjectName("composerCompletionPopup");
    m_completionPopup->setAttribute(Qt::WA_ShowWithoutActivating);
    m_completionPopup->setFocusPolicy(Qt::NoFocus);
    auto *layout=new QVBoxLayout(m_completionPopup);layout->setContentsMargins(4,4,4,4);
    m_completionList=new QListWidget(m_completionPopup);m_completionList->setObjectName("composerCompletionList");
    m_completionList->setFocusPolicy(Qt::NoFocus);m_completionList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_completionList->setFrameShape(QFrame::NoFrame);m_completionList->setMouseTracking(true);
    layout->addWidget(m_completionList);
    m_completionTimer=new QTimer(this);m_completionTimer->setSingleShot(true);m_completionTimer->setInterval(60);
    connect(this,&QPlainTextEdit::textChanged,this,&Composer::updateCompletion);
    connect(this,&QPlainTextEdit::cursorPositionChanged,this,&Composer::updateCompletion);
    connect(m_completionList,&QListWidget::itemClicked,this,[this](QListWidgetItem*){activateCompletion();});
    connect(m_completionTimer,&QTimer::timeout,this,[this]{
        if(!completionIsCurrent()||m_completionSuppressed)return;
        // All state is committed before observers can replace or delete us.
        emit completionRequested(m_completionChat,m_completionRequest,m_completionTrigger,m_completionQuery);
    });
}

void Composer::setCompletionContext(const QString &chatId) {
    if(m_completionChat==chatId)return;
    m_completionChat=chatId;invalidateCompletions();
}
void Composer::invalidateCompletions() {
    ++m_completionRequest;m_completionTimer->stop();m_completionPopup->hide();m_completionItems={};
    m_completionStart=m_completionEnd=-1;m_completionToken.clear();m_completionSuppressed=false;
    m_expandedCompletionGroups.clear();
}
bool Composer::completionIsCurrent() const {
    if(m_completionChat.isEmpty()||m_completionStart<0||m_completionEnd<0)return false;
    const auto cursor=textCursor();
    if(cursor.hasSelection()||cursor.position()!=m_completionEnd)return false;
    auto token=cursor;token.setPosition(m_completionStart);token.setPosition(m_completionEnd,QTextCursor::KeepAnchor);
    return token.selectedText()==m_completionToken;
}
void Composer::updateCompletion() {
    if(m_completionChat.isEmpty()||isReadOnly()){invalidateCompletions();return;}
    const auto cursor=textCursor();
    if(cursor.hasSelection()){invalidateCompletions();return;}
    auto before=cursor;const int end=cursor.position();const int begin=qMax(0,end-256);
    before.setPosition(begin);before.setPosition(end,QTextCursor::KeepAnchor);const auto prefix=before.selectedText();
    int start=prefix.size()-1;
    while(start>=0&&!prefix.at(start).isSpace()&&prefix.at(start)!=QChar(0x2029))--start;
    ++start;const auto token=prefix.mid(start);
    // Require a real token boundary, not an email, path segment or URL.
    if(token.isEmpty()||(token.front()!='/'&&token.front()!='@')||token.size()>201||
       (start==0&&begin>0)||token.contains(QChar(0x2028))){invalidateCompletions();return;}
    const int absoluteStart=begin+start;
    if(m_completionStart==absoluteStart&&m_completionEnd==end&&m_completionToken==token)return;
    if(m_completionStart!=absoluteStart||m_completionTrigger!=token.front())m_expandedCompletionGroups.clear();
    ++m_completionRequest;m_completionTimer->stop();m_completionPopup->hide();m_completionItems={};
    m_completionStart=absoluteStart;m_completionEnd=end;m_completionToken=token;
    m_completionTrigger=token.front();m_completionQuery=token.mid(1);m_completionSuppressed=false;
    m_completionTimer->start();
}
void Composer::setCompletionResults(quint64 requestId,const QJsonArray &items,const QString &emptyText) {
    if(requestId!=m_completionRequest||!completionIsCurrent()||m_completionSuppressed)return;
    m_completionItems={};QSet<QString> ids;
    for(const auto &value:items) {
        if(m_completionItems.size()>=128)break;
        const auto item=value.toObject();const auto id=item["id"].toString();const auto label=item["label"].toString();
        const auto insertion=item["insertText"].toString();
        if(id.isEmpty()||id.size()>2048||ids.contains(id)||label.isEmpty()||label.size()>200||
           insertion.isEmpty()||insertion.size()>4096)continue;
        ids.insert(id);m_completionItems.append(item);
    }
    renderCompletionItems(emptyText);
    m_completionPopup->setFont(font());m_completionPopup->setPalette(palette());
    m_completionPopup->setStyleSheet("QFrame#composerCompletionPopup{border:1px solid palette(mid);border-radius:6px;background:palette(base);} QListWidget{background:palette(base);color:palette(text);} QListWidget::item{padding:4px 7px;} QListWidget::item:selected{background:palette(highlight);color:palette(highlighted-text);}");
    positionCompletion();m_completionPopup->show();
}
void Composer::renderCompletionItems(const QString &emptyText) {
    m_completionList->clear();
    const auto append=[this](const QJsonObject &item){
        const auto id=item["id"].toString();const auto label=item["label"].toString();
        const auto description=item["description"].toString().left(400);
        const auto group=item["group"].toString().left(80);
        auto *row=new QListWidgetItem((m_completionTrigger=='/'?"/":"")+label+(description.isEmpty()?QString():"\n"+description),m_completionList);
        row->setData(Qt::UserRole,id);row->setToolTip((group.isEmpty()?QString():group+" · ")+label+(description.isEmpty()?QString():"\n"+description));
        row->setSizeHint(QSize(1,qMax(30,fontMetrics().lineSpacing()*(description.isEmpty()?1:2)+12)));
    };
    if(m_completionTrigger=='/') {
        QHash<QString,QJsonArray> groups;QStringList order{"builtin","workspace","plugin"};
        for(const auto &value:m_completionItems){auto group=value.toObject()["group"].toString().toLower();if(group=="global")group="workspace";if(group.isEmpty())group="builtin";groups[group].append(value);if(!order.contains(group))order.append(group);}
        for(const auto &group:order){const auto entries=groups.value(group);if(entries.isEmpty())continue;
            if(group!="builtin"){auto *header=new QListWidgetItem(group=="workspace"?"Skills":group=="plugin"?"Plugins":group,m_completionList);header->setFlags(Qt::NoItemFlags);header->setSizeHint(QSize(1,26));}
            const int count=group=="builtin"||m_expandedCompletionGroups.contains(group)?entries.size():qMin(3,entries.size());
            for(int index=0;index<count;++index)append(entries[index].toObject());
            if(count<entries.size()){auto *more=new QListWidgetItem(QString("Show %1 more").arg(entries.size()-count),m_completionList);more->setData(Qt::UserRole+1,group);more->setSizeHint(QSize(1,30));}
        }
    } else {
        // Mention providers already supply a combined relevance-ranked list.
        for(const auto &value:m_completionItems)append(value.toObject());
    }
    if(m_completionItems.isEmpty()) {
        auto *row=new QListWidgetItem(emptyText.isEmpty()?QStringLiteral("No matches"):emptyText.left(200),m_completionList);
        row->setFlags(Qt::NoItemFlags);row->setSizeHint(QSize(1,34));
    } else for(int row=0;row<m_completionList->count();++row)if(m_completionList->item(row)->flags()&Qt::ItemIsEnabled){m_completionList->setCurrentRow(row);break;}
}
void Composer::positionCompletion() {
    if(!m_completionPopup||m_completionStart<0)return;
    const auto caret=mapToGlobal(cursorRect().bottomLeft());auto *screen=QGuiApplication::screenAt(caret);
    if(!screen)screen=QGuiApplication::primaryScreen();
    if(!screen)return;
    const auto available=screen->availableGeometry();
    const int width=qMin(qMax(260,qMin(460,this->width())),qMax(1,available.width()-16));
    int height=8;for(int i=0;i<qMin(7,m_completionList->count());++i)height+=m_completionList->sizeHintForRow(i);
    height=qMin(qMax(42,height),qMax(42,available.height()-16));
    const int x=qBound(available.left()+8,caret.x(),qMax(available.left()+8,available.right()-width-7));
    int y=caret.y()+4;if(y+height>available.bottom()-8)y=mapToGlobal(cursorRect().topLeft()).y()-height-4;
    y=qBound(available.top()+8,y,qMax(available.top()+8,available.bottom()-height-7));
    m_completionPopup->setGeometry(x,y,width,height);
}
void Composer::activateCompletion() {
    if(!completionIsCurrent()||m_completionSuppressed)return;
    const auto *row=m_completionList->currentItem();if(!row||!(row->flags()&Qt::ItemIsEnabled))return;
    const auto expansion=row->data(Qt::UserRole+1).toString();
    if(!expansion.isEmpty()){m_expandedCompletionGroups.insert(expansion);renderCompletionItems();positionCompletion();return;}
    const auto selected=row->data(Qt::UserRole).toString();
    for(const auto &value:m_completionItems)if(value.toObject()["id"]==selected){
        const auto item=value.toObject();
        emit completionActivated(m_completionChat,m_completionRequest,item);return;
    }
}
bool Composer::commitCompletion(quint64 requestId,const QString &itemId) {
    if(requestId!=m_completionRequest||!completionIsCurrent()||m_completionSuppressed)return false;
    QString insertion;for(const auto &entry:m_completionItems)if(entry.toObject()["id"]==itemId){insertion=entry.toObject()["insertText"].toString();break;}
    if(insertion.isEmpty())return false;
    auto cursor=textCursor();cursor.setPosition(m_completionStart);cursor.setPosition(m_completionEnd,QTextCursor::KeepAnchor);
    const auto chat=m_completionChat;const auto value=insertion.endsWith(' ')?insertion:insertion+' ';
    auto expected=toPlainText();expected.replace(m_completionStart,m_completionEnd-m_completionStart,value);
    invalidateCompletions();
    const QPointer<Composer> self(this);cursor.insertText(value);
    if(!self)return true;
    if(m_completionChat!=chat||toPlainText()!=expected)return true;
    setTextCursor(cursor);return true;
}
std::optional<QString> Composer::completionDraft(quint64 requestId,const QString &itemId) const {
    if(requestId!=m_completionRequest||!completionIsCurrent()||m_completionSuppressed)return {};
    QString insertion;for(const auto &entry:m_completionItems)if(entry.toObject()["id"]==itemId){insertion=entry.toObject()["insertText"].toString();break;}
    if(insertion.isEmpty())return {};
    auto result=toPlainText();result.replace(m_completionStart,m_completionEnd-m_completionStart,insertion.endsWith(' ')?insertion:insertion+' ');return result;
}
bool Composer::handleCompletionKey(QKeyEvent *event) {
    if(!m_completionPopup||!m_completionPopup->isVisible())return false;
    if(event->key()==Qt::Key_Escape){m_completionPopup->hide();m_completionTimer->stop();m_completionSuppressed=true;event->accept();return true;}
    if(event->key()==Qt::Key_Down||event->key()==Qt::Key_Up){
        const int count=m_completionList->count();int next=m_completionList->currentRow();
        for(int step=0;step<count;++step){next=(next+(event->key()==Qt::Key_Down?1:count-1))%count;if(m_completionList->item(next)->flags()&Qt::ItemIsEnabled){m_completionList->setCurrentRow(next);break;}}
        event->accept();return true;
    }
    if((event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter||event->key()==Qt::Key_Tab)&&!(event->modifiers()&Qt::ShiftModifier)){
        event->accept();activateCompletion();return true;
    }
    return false;
}
void Composer::focusOutEvent(QFocusEvent *event){QPlainTextEdit::focusOutEvent(event);if(m_completionPopup)invalidateCompletions();}
void Composer::hideEvent(QHideEvent *event){QPlainTextEdit::hideEvent(event);invalidateCompletions();}
} // namespace ade
