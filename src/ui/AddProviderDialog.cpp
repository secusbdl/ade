#include "AddProviderDialog.h"
#include "ProviderCatalog.h"
#include <QEvent>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
namespace ade {
namespace {
class ProviderCard final : public QPushButton {
public:
    ProviderCard(const ProviderTemplate& entry,int count,QWidget* parent)
        :QPushButton(parent),m_name(entry.name),m_count(count) {
        setObjectName("providerChoice_"+entry.id);setProperty("templateId",entry.id);
        setAccessibleName(entry.name);setAccessibleDescription(entry.description);
        setToolTip(entry.description);setFocusPolicy(Qt::StrongFocus);
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);setFixedHeight(38);
        setMinimumWidth(0);
    }
    QSize sizeHint()const override{return {210,38};}
protected:
    void paintEvent(QPaintEvent* event)override {
        QPushButton::paintEvent(event);QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
        const auto color=palette().color(QPalette::ButtonText);painter.setPen(color);
        QFont iconFont=font();iconFont.setBold(true);iconFont.setPixelSize(12);painter.setFont(iconFont);
        const auto initial=m_name.startsWith("Add ")?QStringLiteral("+"):m_name.left(1);
        painter.drawText(QRect(9,0,18,height()),Qt::AlignCenter,initial);
        painter.setFont(font());QString badge=m_count?QString::number(m_count)+" configured":QString();
        const int badgeWidth=badge.isEmpty()?0:painter.fontMetrics().horizontalAdvance(badge)+10;
        const QRect textRect(34,0,qMax(0,width()-44-badgeWidth),height());
        painter.drawText(textRect,Qt::AlignVCenter|Qt::AlignLeft,painter.fontMetrics().elidedText(m_name,Qt::ElideRight,textRect.width()));
        if(!badge.isEmpty()) { auto muted=color;muted.setAlpha(160);painter.setPen(muted);painter.drawText(QRect(width()-badgeWidth-8,0,badgeWidth,height()),Qt::AlignVCenter|Qt::AlignRight,badge); }
    }
private:QString m_name;int m_count;
};
}
struct AddProviderDialog::Section {
    QWidget* container=nullptr;QGridLayout* grid=nullptr;QList<ProviderCard*> cards;
};
AddProviderDialog::AddProviderDialog(const QList<ProviderConfig>& providers,QWidget* parent):QDialog(parent) {
    // Dialogs are windows: explicitly follow the owning surface's palette.
    if(parent){setPalette(parent->palette());parent->installEventFilter(this);}
    setObjectName("addProviderDialog");setWindowTitle("Add Provider");setMinimumSize(300,300);resize(512,455);
    setWindowModality(Qt::WindowModal);
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(22,20,22,18);layout->setSpacing(12);
    auto* title=new QLabel("Add Provider",this);title->setObjectName("addProviderTitle");layout->addWidget(title);
    auto* description=new QLabel("Connect a coding agent or model source.",this);description->setObjectName("addProviderDescription");description->setWordWrap(true);layout->addWidget(description);
    m_search=new QLineEdit(this);m_search->setObjectName("providerSearch");m_search->setPlaceholderText("Search providers…");m_search->setClearButtonEnabled(true);m_search->setMaxLength(256);layout->addWidget(m_search);
    m_scroll=new QScrollArea(this);m_scroll->setObjectName("providerChoicesScroll");m_scroll->setWidgetResizable(true);m_scroll->setFrameShape(QFrame::NoFrame);m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);m_scroll->setMinimumHeight(130);layout->addWidget(m_scroll,1);
    m_content=new QWidget;m_content->setObjectName("providerChoicesContent");m_content->setMinimumWidth(0);m_sections=new QVBoxLayout(m_content);m_sections->setContentsMargins(1,0,8,4);m_sections->setSpacing(16);
    m_empty=new QLabel(m_content);m_empty->setObjectName("providerSearchEmpty");m_empty->setTextFormat(Qt::PlainText);m_empty->setWordWrap(true);m_empty->setAlignment(Qt::AlignCenter);m_sections->addWidget(m_empty);
    for(const auto& group:ProviderCatalog::groups()) {
        if(group=="Subscriptions & Plans") {
            m_modelHeading=new QWidget(m_content);m_modelHeading->setObjectName("providerModelHeading");auto* heading=new QVBoxLayout(m_modelHeading);heading->setContentsMargins(0,6,0,0);heading->setSpacing(5);
            auto* label=new QLabel("ADE Agent",m_modelHeading);label->setProperty("providerSectionTitle",true);heading->addWidget(label);
            auto* help=new QLabel("Choose what model powers the ADE agent.",m_modelHeading);help->setProperty("providerSectionDescription",true);help->setWordWrap(true);heading->addWidget(help);m_sections->addWidget(m_modelHeading);
        }
        auto* section=new Section;section->container=new QWidget(m_content);section->container->setObjectName("providerSection_"+group);
        auto* column=new QVBoxLayout(section->container);column->setContentsMargins(0,0,0,0);column->setSpacing(7);
        auto* label=new QLabel(group,section->container);label->setProperty("providerSectionTitle",true);column->addWidget(label);
        if(group=="Coding Agents") {auto* help=new QLabel("Use an external coding agent and its native harness.",section->container);help->setProperty("providerSectionDescription",true);help->setWordWrap(true);column->addWidget(help);}
        section->grid=new QGridLayout;section->grid->setContentsMargins(0,0,0,0);section->grid->setSpacing(7);column->addLayout(section->grid);
        for(const auto& entry:ProviderCatalog::all())if(entry.group==group) {
            int count=0;for(const auto& p:providers)if(ProviderCatalog::matches(p,entry))++count;
            auto* card=new ProviderCard(entry,count,section->container);section->cards.append(card);
            connect(card,&QPushButton::clicked,this,[this,id=entry.id]{m_selected=id;accept();});
        }
        m_groups.append(section);m_sections->addWidget(section->container);
    }
    m_sections->addStretch();m_scroll->setWidget(m_content);m_scroll->viewport()->installEventFilter(this);
    connect(this,&QObject::destroyed,[groups=m_groups]{qDeleteAll(groups);});
    connect(m_search,&QLineEdit::textChanged,this,[this]{const QPointer<AddProviderDialog> self(this);rebuildLayout();if(self)m_scroll->verticalScrollBar()->setValue(0);});
    auto* cancel=new QPushButton("Cancel",this);cancel->setObjectName("cancelAddProvider");connect(cancel,&QPushButton::clicked,this,&QDialog::reject);layout->addWidget(cancel,0,Qt::AlignRight);
    updateColors();rebuildLayout();m_search->setFocus();
}
QStringList AddProviderDialog::visibleTemplateIds()const {
    QStringList result;for(const auto* section:m_groups)for(auto* card:section->cards)if(!card->isHidden())result.append(card->property("templateId").toString());return result;
}
void AddProviderDialog::rebuildLayout() {
    const auto query=m_search->text().trimmed();const bool narrow=m_scroll->viewport()->width()<420;int total=0,models=0;
    for(auto* section:m_groups) {
        while(auto* item=section->grid->takeAt(0))delete item;
        int count=0;
        for(auto* card:section->cards) {
            const auto* entry=ProviderCatalog::find(card->property("templateId").toString());
            const bool visible=entry&&(query.isEmpty()||(entry->name+" "+entry->description+" "+entry->group).contains(query,Qt::CaseInsensitive));
            card->setVisible(visible);if(!visible)continue;
            const int columns=narrow||entry->id=="custom"?1:2;section->grid->addWidget(card,count/columns,count%columns,1,!narrow&&entry->id=="custom"?2:1);++count;
            if(entry->kind=="api")++models;
        }
        section->grid->setColumnStretch(0,1);section->grid->setColumnStretch(1,narrow?0:1);
        section->container->setVisible(count>0);total+=count;
    }
    m_empty->setText(QString("No providers match “%1”.").arg(query));m_empty->setVisible(!total);
    m_modelHeading->setVisible(models>0);
}
bool AddProviderDialog::eventFilter(QObject* object,QEvent* event) {
    if(object==parentWidget()&&event->type()==QEvent::PaletteChange){setPalette(parentWidget()->palette());updateColors();}
    if(object==m_scroll->viewport()&&event->type()==QEvent::Resize&&!m_layoutPending) {
        m_layoutPending=true;QTimer::singleShot(0,this,[this]{m_layoutPending=false;rebuildLayout();});
    }
    return QDialog::eventFilter(object,event);
}
void AddProviderDialog::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);if(event->type()==QEvent::PaletteChange&&!m_coloring)updateColors();
}
void AddProviderDialog::updateColors() {
    if(m_coloring)return;
    m_coloring=true;
    auto base=palette().color(QPalette::Base);if(base.alpha()!=255&&parentWidget())base=parentWidget()->palette().color(QPalette::Base);
    const bool dark=base.lightness()<128;
    const auto background=dark?QStringLiteral("#21242b"):QStringLiteral("#fdfdfd"),foreground=dark?QStringLiteral("#e8e9ec"):QStringLiteral("#292a2d"),muted=dark?QStringLiteral("#a1a6b0"):QStringLiteral("#70757d"),border=dark?QStringLiteral("#3b4049"):QStringLiteral("#e4e6e8"),hover=dark?QStringLiteral("#30343d"):QStringLiteral("#f1f3f5");
    const QString style=QStringLiteral("QDialog#addProviderDialog, QWidget#providerChoicesContent {background:%1;color:%2;} QDialog#addProviderDialog QLabel {color:%2;background:transparent;} QLabel#addProviderTitle {font-size:18px;font-weight:600;} QLabel#addProviderDescription,QLabel[providerSectionDescription=\"true\"],QLabel#providerSearchEmpty {color:%3;font-size:12px;} QLabel[providerSectionTitle=\"true\"] {font-weight:600;font-size:12px;} QDialog#addProviderDialog QPushButton {background:%1;color:%2;border:1px solid %4;border-radius:7px;} QDialog#addProviderDialog QPushButton:hover,QDialog#addProviderDialog QPushButton:focus {background:%5;border-color:%3;} QLineEdit#providerSearch {background:%1;color:%2;border:1px solid %4;padding:7px;border-radius:5px;} QScrollArea#providerChoicesScroll {background:transparent;border:0;}").arg(background,foreground,muted,border,hover);
    if(styleSheet()!=style)setStyleSheet(style);
    m_coloring=false;
}
}
