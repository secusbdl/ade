#include "QuestionToolPart.h"
#include "MarkdownParts.h"
#include <QEvent>
#include <QFrame>
#include <QFontInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QSet>
#include <QVBoxLayout>
namespace ade {
namespace {
constexpr int MaximumJson=2*1024*1024,MaximumFields=50,MaximumDisplay=65536;
QString cut(const QString& text,int maximum,bool* truncated){int n=qMin(text.size(),qMax(0,maximum));if(n<text.size()&&n>0&&text[n-1].isHighSurrogate())--n;if(n<text.size())*truncated=true;return text.left(n);}
QJsonObject object(const QJsonValue& value,bool* valid=nullptr){
 if(valid)*valid=false;
 if(value.isObject()){if(valid)*valid=true;return value.toObject();}
 if(!value.isString()||value.toString().size()>MaximumJson)return {};
 const auto bytes=value.toString().toUtf8();
 if(bytes.size()>MaximumJson)return {};
 const auto document=QJsonDocument::fromJson(bytes);
 if(valid)*valid=document.isObject();
 return document.isObject()?document.object():QJsonObject{};
}
QString jsString(const QJsonValue& value,bool* truncated,int depth=0){
 if(value.isString())return cut(value.toString(),MaximumDisplay,truncated);
 if(value.isBool())return value.toBool()?QStringLiteral("true"):QStringLiteral("false");
 if(value.isDouble())return QString::number(value.toDouble(),'g',16);
 if(value.isNull()||value.isUndefined())return {};
 if(value.isArray()){
  if(depth>8){*truncated=true;return {};}
  QString result;int count=0;
  for(const auto& item:value.toArray()){
   if(count>=100||result.size()>=MaximumDisplay){*truncated=true;break;}
   if(count++)result+=',';
   result+=cut(jsString(item,truncated,depth+1),MaximumDisplay-result.size(),truncated);
  }
  return result;
 }
 return QStringLiteral("[object Object]");
}
QString answer(const QJsonObject& field,const QJsonValue& value,bool* truncated){
 if(value.isNull()||value.isUndefined()||value==QString())return QStringLiteral("(empty)");
 if(field.value("type")=="input"&&field.value("inputType")=="password")return QString::fromUtf8("••••••");
 if(value.isString()&&value.toString().startsWith("__other__:")){
  const auto other=value.toString().mid(10);
  return other.isEmpty()?QStringLiteral("(empty)"):cut(other,MaximumDisplay,truncated);
 }
 const auto label=[&](const QJsonValue& selected){
  int visited=0;
  for(const auto& item:field.value("options").toArray()){
   if(visited++>=100){*truncated=true;break;}
   const auto option=item.toObject();
   if(option.value("value")==selected&&option.value("label").isString())return cut(option.value("label").toString(),MaximumDisplay,truncated);
  }
  return jsString(selected,truncated);
 };
 if(field.value("type")=="radio-group"&&value.isString())return label(value);
 if(field.value("type")=="checkbox-group"&&value.isArray()){
  if(value.toArray().isEmpty())return QStringLiteral("(none)");
  QString result;int count=0;
  for(const auto& item:value.toArray()){
   if(count>=100||result.size()>=MaximumDisplay){*truncated=true;break;}
   if(count++)result+=", ";
   result+=cut(label(item),MaximumDisplay-result.size(),truncated);
  }
  return result;
 }
 if(value.isBool())return value.toBool()?QStringLiteral("Yes"):QStringLiteral("No");
 return jsString(value,truncated);
}
class SummaryLabel final:public QLabel {
public:
 explicit SummaryLabel(QWidget* parent):QLabel(parent){setMinimumWidth(0);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);setFixedHeight(24);auto f=font();f.setPixelSize(12);f.setWeight(QFont::Medium);setFont(f);}
protected:
 void paintEvent(QPaintEvent*)override{QPainter p(this);p.setRenderHint(QPainter::Antialiasing);const double scale=height()/24.0;auto foreground=palette().color(QPalette::Text);foreground.setAlphaF(0.7);p.setPen(QPen(foreground,1.1));p.save();p.scale(scale,scale);p.drawRoundedRect(QRectF(1,6,10,8),2,2);p.drawLine(QPointF(3,14),QPointF(3,17));p.drawLine(QPointF(3,17),QPointF(6,14));p.restore();p.setFont(font());const int inset=qRound(16*scale);p.drawText(QRect(inset,0,qMax(0,width()-inset),height()),Qt::AlignVCenter|Qt::AlignLeft,fontMetrics().elidedText(text(),Qt::ElideRight,qMax(0,width()-inset)));}
};
}
bool isNativeQuestionPart(const QJsonObject& part){return part.value("type")=="tool"&&part.value("name")=="askUserQuestions";}
QuestionToolDisplay questionToolDisplay(const QJsonObject& part){
 QuestionToolDisplay out;
 const auto state=part.value("state").toString();
 if(state=="output-error"){out.summary="Questions cancelled (agent stopped)";return out;}
 if(state=="input-streaming"){out.summary="Writing up some questions...";return out;}
 if(state=="input-available"||state=="running"){out.summary="Waiting for response...";return out;}
 auto input=object(part.value("input"));
 if(input.isEmpty())input=object(part.value("inputText"));
 out.summary=cut(input.value("title").toString("User questions"),8192,&out.truncated);
 bool hasOutput=false;const auto output=object(part.value("output"),&hasOutput);
 if(!hasOutput){if(state=="canceled")out.summary="Questions cancelled (agent stopped)";return out;}
 if(!output.value("notice").toString().isEmpty())out.summary="User sent answers and message";
 else if(output.value("cancelled").toBool())out.summary=output.value("cancelReason")=="agent_stopped"?"Questions cancelled (agent stopped)":"User dismissed questions";
 else out.summary=input.value("title").toString("Questions answered");
 out.summary=cut(out.summary,8192,&out.truncated);
 QHash<QString,QJsonObject> fields;int visited=0,steps=0;
 for(const auto& step:input.value("steps").toArray()){
  if(steps++>=5){out.truncated=true;break;}
  for(const auto& entry:step.toObject().value("fields").toArray()){
   if(visited++>=MaximumFields){out.truncated=true;break;}
   const auto field=entry.toObject();fields.insert(field.value("questionId").toString(),field);
  }
  if(visited>MaximumFields)break;
 }
 int remaining=MaximumDisplay-out.summary.size();const auto values=output.value("answers").toObject();
 for(auto it=values.begin();it!=values.end();++it){
  if(out.answers.size()>=MaximumFields||remaining<=0){out.truncated=true;break;}
  const auto field=fields.value(it.key());
  auto label=cut(field.value("label").toString(it.key()),qMin(8192,remaining),&out.truncated);remaining-=label.size();
  auto display=cut(answer(field,it.value(),&out.truncated),remaining,&out.truncated);remaining-=display.size();
  out.answers.append({it.key(),label,display});
 }
 return out;
}
QString questionToolPlainText(const QJsonObject& part){const auto display=questionToolDisplay(part);QStringList text{display.summary};for(const auto& answer:display.answers){text.append(answer.label);text.append(answer.value);}if(display.truncated)text.append("Some question content is not shown.");return text.join('\n');}
QuestionToolPart::QuestionToolPart(QWidget* parent):QWidget(parent){
 setObjectName("questionToolPart");auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(4);
 m_summary=new SummaryLabel(this);m_summary->setObjectName("questionSummary");layout->addWidget(m_summary);
 m_answers=new QFrame(this);m_answers->setObjectName("questionAnswerSummary");m_answerLayout=new QVBoxLayout(m_answers);m_answerLayout->setContentsMargins(10,10,10,10);m_answerLayout->setSpacing(8);layout->addWidget(m_answers);m_answers->hide();
 m_notice=new QLabel("Some question content is not shown.",this);m_notice->setObjectName("questionSummaryTruncated");m_notice->setWordWrap(true);layout->addWidget(m_notice);m_notice->hide();colors();fonts();
}
void QuestionToolPart::updatePart(const QJsonObject& part){
 if(m_initialized&&m_part==part)return;
 m_initialized=true;m_part=part;const auto epoch=++m_epoch;const QPointer<QuestionToolPart> self(this);const auto current=[&]{return self&&m_epoch==epoch;};const auto display=questionToolDisplay(part);
 setProperty("questionSummary",display.summary);if(!current())return;m_summary->setText(display.summary);if(!current())return;
 QSet<QString> keep;int position=0;
 for(const auto& answer:display.answers){keep.insert(answer.id);auto row=m_rows.value(answer.id,{nullptr,nullptr,nullptr});
  if(!row.widget){row.widget=new QWidget(m_answers);auto* layout=new QVBoxLayout(row.widget);layout->setContentsMargins(0,0,0,0);layout->setSpacing(2);row.label=new TranscriptText(row.widget);row.label->setObjectName("questionAnswerLabel");auto small=font();small.setPixelSize(12);small.setWeight(QFont::Medium);row.label->setFont(small);row.value=new TranscriptText(row.widget);row.value->setObjectName("questionAnswerValue");auto normal=font();normal.setPixelSize(14);row.value->setFont(normal);layout->addWidget(row.label);layout->addWidget(row.value);m_rows.insert(answer.id,row);connect(row.label,&QTextBrowser::anchorClicked,this,&QuestionToolPart::linkActivated);row.widget->setProperty("questionId",answer.id);if(!current())return;}
  const auto* item=m_answerLayout->itemAt(position);if(!item||item->widget()!=row.widget){m_answerLayout->removeWidget(row.widget);if(!current())return;m_answerLayout->insertWidget(position,row.widget);if(!current())return;}++position;
  row.label->setSource(answer.label);if(!current())return;row.value->setSource(answer.value,false);if(!current())return;
 }
 const auto previous=m_rows.keys();for(const auto& id:previous)if(!keep.contains(id)){delete m_rows.take(id).widget;if(!current())return;}
 fonts();if(!current())return;
 m_answers->setVisible(!display.answers.isEmpty());if(!current())return;m_notice->setVisible(display.truncated);if(!current())return;updateGeometry();
}
void QuestionToolPart::colors(){if(m_coloring||!m_answers)return;m_coloring=true;const QPointer<QuestionToolPart> self(this);const bool dark=palette().color(QPalette::Window).lightness()<128;m_answers->setStyleSheet(QString("QFrame#questionAnswerSummary{border:1px solid %1;border-radius:6px;background:transparent;}").arg(dark?"#3b4048":"#e4e6e8"));if(self){m_coloring=false;m_summary->update();}}
void QuestionToolPart::fonts(){
 const QPointer<QuestionToolPart> self(this);const auto epoch=m_epoch;const QWidget* owner=parentWidget();while(owner&&owner->objectName()!="conversationView")owner=owner->parentWidget();const int pixels=qMax(1,QFontInfo(owner?owner->font():font()).pixelSize());
 auto small=font();small.setPixelSize(qMax(1,qRound(pixels*12.0/13)));small.setWeight(QFont::Medium);
 auto normal=font();normal.setPixelSize(qMax(1,qRound(pixels*14.0/13)));
 m_summary->setFont(small);if(!self||epoch!=m_epoch)return;m_summary->setFixedHeight(qMax(1,qRound(pixels*24.0/13)));if(!self||epoch!=m_epoch)return;
 const auto rows=m_rows.values();for(const auto& row:rows){row.label->setFont(small);if(!self||epoch!=m_epoch)return;row.value->setFont(normal);if(!self||epoch!=m_epoch)return;}
}
void QuestionToolPart::refreshAppearance(){const QPointer<QuestionToolPart> self(this);colors();if(self)fonts();}
void QuestionToolPart::changeEvent(QEvent* event){const QPointer<QuestionToolPart> self(this);QWidget::changeEvent(event);if(!self)return;if(event->type()==QEvent::PaletteChange)colors();else if(event->type()==QEvent::FontChange&&m_summary)fonts();}
}
