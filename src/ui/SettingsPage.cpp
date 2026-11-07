#include "SettingsPage.h"
#include "ProviderModelsDialog.h"
#include "ProviderCatalog.h"
#include "FlowLayout.h"
#include "SettingsServiceMarks.h"
#include <QSvgRenderer>

#include "SessionStore.h"
#include "ModelSelection.h"
#include "UtilityModelSelection.h"
#include <optional>
#include "Icons.h"
#include "ContextPreferences.h"
#include "ProviderRepository.h"
#include "core/ProviderStore.h"
#include <QCryptographicHash>
#include <memory>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMap>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QStyleHints>
#include <QTabWidget>
#include <QTextLayout>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace ade {
namespace {
// QInputDialog's text return cannot distinguish equal display labels belonging
// to different provider instances. Preserve the selected row's typed identity.
std::optional<ade::ModelSelection> chooseModelEntry(QWidget* parent,
        const QList<ade::ModelSelection>& choices,const QString& title) {
    if(choices.isEmpty())return {};
    QPointer<QInputDialog> dialog=new QInputDialog(parent);
    dialog->setObjectName("utilityModelPicker");dialog->setWindowTitle(title);
    dialog->setLabelText("Provider and model");dialog->setComboBoxEditable(false);
    QStringList labels;for(const auto& choice:choices)labels.append(choice.label);
    dialog->setComboBoxItems(labels);
    if(auto* combo=dialog->findChild<QComboBox*>())for(int i=0;i<choices.size();++i){
        combo->setItemData(i,choices[i].toJson());
        combo->setItemData(i,choices[i].providerId+" / "+choices[i].modelId,Qt::ToolTipRole);
    }
    const auto accepted=dialog->exec()==QDialog::Accepted;
    if(!dialog)return {};
    const auto* combo=dialog->findChild<QComboBox*>();
    const int index=accepted&&combo?combo->currentIndex():-1;
    dialog->deleteLater();
    if(index<0||index>=choices.size())return {};
    return choices[index];
}

struct Section { const char *id; const char *label; const char *group; const char *glyph; };
constexpr Section sections[] = {
    {"agent-general","General","Agent","⚙"},
    {"models-providers","Models & Providers","","◉"},
    {"skills-context","Skills & Context files","","▤"},
    {"worktree-setup","Worktrees","","⑂"},
    {"plugins","Plugins","","◇"},
    {"archived-agents","Archived chats","","▣"},
    {"personalization","Personalization","Personal","◌"},
    {"browsing","General","Browsing","◎"},
    {"clear-data","Clear data","","♧"},
    {"history","History","","◷"},
    {"account","Account"," ","♙"},
    {"about","About","","ⓘ"}
};
struct Plugin { const char *id; const char *name; const char *description; const char *origin; const char *credentialName; const char *fieldLabel; const char *helpText; const char *helpUrl; };
constexpr Plugin plugins[] = {
    {"figma","Figma","Teaches the agent how to work with Figma designs and components.","https://api.figma.com","Figma Personal Access Token","Personal Access Token","Create a token in Figma Settings → Security → Personal access tokens.","https://help.figma.com/hc/en-us/articles/8085703771159-Manage-personal-access-tokens"},
    {"github","GitHub","Lets the agent interact with GitHub repositories, issues, pull requests, actions, and more via the REST API.","https://api.github.com","GitHub Personal Access Token","Personal Access Token","Create a token with access to the repositories and actions you intend to use.","https://github.com/settings/tokens"},
    {"posthog","PostHog","Lets the agent query analytics, manage feature flags, inspect events and persons, run HogQL queries, and more via the PostHog REST API.","https://us.posthog.com","PostHog Personal API Key","Personal API Key (phx_...)","Create a personal API key in your PostHog dashboard's user settings.","https://us.posthog.com/settings/user-api-keys"},
    {"supabase","Supabase","Lets the agent interact with Supabase projects via the Management API — run SQL queries, list projects, manage edge functions, secrets, migrations, and more.","https://api.supabase.com","Supabase Access Token","Personal Access Token","Use an account access token for the Management API, not a project anon or service-role key.","https://supabase.com/dashboard/account/tokens"},
    {"vercel","Vercel","Lets the agent manage deployments, inspect logs, and interact with the Vercel API on your behalf.","https://api.vercel.com","Vercel Personal Access Token","Personal Access Token","Create a token at vercel.com/account/tokens.","https://vercel.com/account/tokens"}
};

QByteArray pluginCredentialRevision(const QString& path) {
    QFile file(path);
    if(!file.exists())return QCryptographicHash::hash("missing",QCryptographicHash::Sha256);
    if(!file.open(QIODevice::ReadOnly)||file.size()>4*1024*1024)return {};
    const auto bytes=file.read(4*1024*1024+1);
    if(bytes.size()>4*1024*1024||file.error()!=QFileDevice::NoError)return {};
    return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
}

class TwoLineDescription final:public QLabel {
public:
    explicit TwoLineDescription(const QString& value):QLabel(value){
        setObjectName("settingDescription");setTextFormat(Qt::PlainText);
        setStyleSheet("font-size:12px;");setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
        setAttribute(Qt::WA_TransparentForMouseEvents);setFixedHeight(fontMetrics().lineSpacing()*2);
    }
    QSize minimumSizeHint()const override{return {0,fontMetrics().lineSpacing()*2};}
    QSize sizeHint()const override{return {200,fontMetrics().lineSpacing()*2};}
protected:
    void changeEvent(QEvent* event)override{QLabel::changeEvent(event);if(event->type()==QEvent::FontChange){setFixedHeight(fontMetrics().lineSpacing()*2);updateGeometry();}}
    void paintEvent(QPaintEvent*)override{
        QPainter painter(this);painter.setPen(palette().color(QPalette::WindowText));
        QTextLayout lines(text(),font());QTextOption option;option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);lines.setTextOption(option);lines.beginLayout();
        auto first=lines.createLine();if(!first.isValid()){lines.endLayout();return;}first.setLineWidth(width());lines.endLayout();first.draw(&painter,{0,0});
        if(first.textLength()<text().size()){
            const auto rest=fontMetrics().elidedText(text().mid(first.textLength()).trimmed(),Qt::ElideRight,width());
            painter.drawText(QPoint(0,fontMetrics().lineSpacing()+fontMetrics().ascent()),rest);
        }
    }
};

class ElidedLine final:public QLabel {
public:
    explicit ElidedLine(const QString& value,const QString& name):QLabel(value){
        setObjectName(name);setTextFormat(Qt::PlainText);setToolTip(value);
        setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    QSize minimumSizeHint()const override{return {0,fontMetrics().height()};}
protected:
    void paintEvent(QPaintEvent*)override{
        QPainter painter(this);painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(rect(),Qt::AlignLeft|Qt::AlignVCenter,fontMetrics().elidedText(text(),Qt::ElideRight,width()));
    }
};

QLabel *text(const QString &value, const char *objectName = "settingDescription") {
    auto *label = new QLabel(value);
    label->setObjectName(QString::fromLatin1(objectName));
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}
void heading(QVBoxLayout *layout, const QString &title, const QString &description = {}) {
    layout->addSpacing(18);
    layout->addWidget(text(title, "settingHeading"));
    if (!description.isEmpty()) layout->addWidget(text(description));
}
QPushButton *action(QVBoxLayout *layout, const QString &title, const QString &name,
                    Qt::Alignment alignment = Qt::AlignLeft) {
    auto *button = new QPushButton(title);
    button->setObjectName(name);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumHeight(32);
    layout->addWidget(button, 0, alignment);
    return button;
}
QWidget *card(QVBoxLayout *layout, const QString &title, const QString &description,
              QHBoxLayout **row = nullptr) {
    auto *widget = new QFrame;
    widget->setObjectName("settingCard");
    auto *horizontal = new QHBoxLayout(widget);
    horizontal->setContentsMargins(16, 13, 16, 13);
    auto *labels = new QVBoxLayout;
    labels->setSpacing(3);
    labels->addWidget(text(title, "settingCardTitle"));
    if (!description.isEmpty()) labels->addWidget(text(description));
    horizontal->addLayout(labels, 1);
    layout->addWidget(widget);
    if (row) *row = horizontal;
    return widget;
}
// Source cards identify the configured provider, rather than its selected model.
// Legacy instances may be recognized only by a unique fixed endpoint/kind
// contract. A configurable endpoint or a shared API/plan URL cannot prove which
// setup template was selected. Display inference never changes stored provenance.
const ProviderTemplate* providerPresentationTemplate(const ProviderConfig& provider) {
    if(!provider.templateId.isEmpty()) {
        const auto* entry=ProviderCatalog::find(provider.templateId);
        return entry&&ProviderCatalog::matches(provider,*entry)?entry:nullptr;
    }
    const ProviderTemplate* match=nullptr;
    for(const auto& entry:ProviderCatalog::all()) {
        if(entry.id=="custom"||entry.configurableEndpoint||provider.kind!=entry.kind||
           provider.protocol!=entry.protocol||QUrl(provider.baseUrl)!=QUrl(entry.baseUrl))continue;
        if(match)return nullptr;
        match=&entry;
    }
    return match;
}

QString providerDescription(const ProviderConfig& provider) {
    if(const auto* entry=providerPresentationTemplate(provider))return entry->description;
    return provider.isExternalHarness()?QStringLiteral("External coding agent using its own configured account.")
        :QStringLiteral("Custom model endpoint using %1.").arg(provider.protocol);
}

QString providerMark(const ProviderConfig& provider) {
    const QMap<QString,QString> marks={{"deepseek-api","deepseek"},{"claude-code","anthropic"},{"anthropic-api","anthropic"},{"codex","openai"},{"openai-api","openai"},{"alibaba-api","alibaba"},{"qwen-plan","alibaba"},{"qwen-token-plan","alibaba"},{"minimax-api","minimax"},{"minimax-plan","minimax"},{"moonshotai-api","moonshotai"},{"kimi-plan","moonshotai"},{"mistral-api","mistral"},{"z-ai-api","z-ai"},{"glm-coding-plan","z-ai"},{"ollama","ollama"}};
    if(const auto* entry=providerPresentationTemplate(provider);entry&&marks.contains(entry->id))return marks.value(entry->id);
    return provider.isExternalHarness()?QStringLiteral("terminal"):QStringLiteral("brain");
}

QPixmap settingsPixmap(const QString& name,const QColor& foreground,int size) {
    auto data=settingsServiceSvg(name);
    if(!data.isEmpty()) {
        data.replace("currentColor",foreground.name().toUtf8());
        QSvgRenderer renderer(data);
        QPixmap result(size,size);result.fill(Qt::transparent);QPainter painter(&result);renderer.render(&painter);
        return result;
    }
    if(name=="supabase"||name=="posthog") {
        // Original category artwork, not copies of the service trademarks.
        QPixmap result(size,size);result.fill(Qt::transparent);QPainter painter(&result);painter.setRenderHint(QPainter::Antialiasing);painter.scale(size/24.0,size/24.0);painter.setPen(QPen(foreground,1.6,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        if(name=="posthog") {
            painter.drawLine(4,20,21,20);painter.drawLine(4,20,4,4);
            painter.drawRoundedRect(QRectF(7,12,3,8),.7,.7);painter.drawRoundedRect(QRectF(12,8,3,12),.7,.7);painter.drawRoundedRect(QRectF(17,4,3,16),.7,.7);
        } else {
            painter.drawEllipse(QRectF(4,3,16,6));painter.drawLine(4,6,4,18);painter.drawLine(20,6,20,18);painter.drawArc(QRectF(4,9,16,6),180*16,180*16);painter.drawArc(QRectF(4,15,16,6),180*16,180*16);
        }
        return result;
    }
    return icon(name,foreground).pixmap(size,size);
}

// Selected MIT service assets and original ADE category marks; no Nucleo.
QLabel* settingsMark(const QString& glyph,int size) {
    auto* mark=new QLabel;
    mark->setObjectName("settingsCategoryMark");
    mark->setProperty("settingsGlyph",glyph);
    mark->setProperty("settingsMarkSize",size);
    mark->setFixedSize(size,size);
    mark->setAlignment(Qt::AlignCenter);
    mark->setAttribute(Qt::WA_TransparentForMouseEvents);
    return mark;
}

QVBoxLayout* sectionHeader(QVBoxLayout* parent,const QString& title,const QString& description={}) {
    auto* labels=new QVBoxLayout;
    labels->setSpacing(2);
    labels->addWidget(text(title,"settingHeading"));
    if(!description.isEmpty())labels->addWidget(text(description));
    parent->addLayout(labels);
    return labels;
}

QWidget* actionRow(QVBoxLayout* layout,const QList<QPushButton*>& buttons,Qt::Alignment alignment={}) {
    auto* row=new QWidget;
    row->setObjectName("settingsActionRow");
    auto* flow=new FlowLayout(row);
    for(auto* button:buttons)flow->addWidget(button);
    layout->addWidget(row,0,alignment);
    return row;
}

QJsonValue getPath(const QJsonObject &object, const QStringList &parts, const QJsonValue &fallback) {
    QJsonValue current(object);
    for (const auto &part : parts) {
        if (!current.isObject() || !current.toObject().contains(part)) return fallback;
        current = current.toObject().value(part);
    }
    return current;
}
QJsonObject putPath(QJsonObject object, const QStringList &parts, const QJsonValue &value) {
    if (parts.size() == 1) object[parts.front()] = value;
    else object[parts.front()] = putPath(object[parts.front()].toObject(), parts.mid(1), value);
    return object;
}
bool validWebUrl(const QString &value, bool allowEmpty = false) {
    if (value.isEmpty()) return allowEmpty;
    const QUrl url(value);
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty()
        && (url.scheme() == "http" || url.scheme() == "https");
}
QJsonArray defaultEngines() {
    return {QJsonObject{{"id","google"},{"name","Google"},{"keyword","google.com"},{"url","https://www.google.com/search?q=%s"}},
            QJsonObject{{"id","bing"},{"name","Bing"},{"keyword","bing.com"},{"url","https://www.bing.com/search?q=%s"}},
            QJsonObject{{"id","duckduckgo"},{"name","DuckDuckGo"},{"keyword","duckduckgo.com"},{"url","https://duckduckgo.com/?q=%s"}}};
}

bool matchingKnownTypes(const QJsonObject &object, const QJsonObject &defaults) {
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        if (!object.contains(it.key())) continue;
        const auto value = object.value(it.key());
        if (value.type() != it.value().type()) return false;
        if (value.isObject() && !matchingKnownTypes(value.toObject(), it.value().toObject())) return false;
    }
    return true;
}

bool validPreferences(const QJsonObject &preferences) {
    const auto defaults = SettingsPage::defaultPreferences();
    if (!matchingKnownTypes(preferences, defaults)) return false;
    const auto pluginSettings=preferences["plugins"].toObject();
    for(auto entry=pluginSettings.begin();entry!=pluginSettings.end();++entry){
        if(!entry.value().isObject())return false;
        const auto options=entry.value().toObject();
        if(options.contains("enabled")&&!options["enabled"].isBool())return false;
        if(entry.key()=="posthog"&&options.contains("region")&&(!options["region"].isString()||!QStringList{"us","eu"}.contains(options["region"].toString())))return false;
    }
    const auto value = [&](const QString &path) {
        return getPath(preferences, path.split('.'), getPath(defaults, path.split('.'), {}));
    };
    if(!QStringList{"age","workspace"}.contains(value("sidebar.agentListGroupingMode").toString()))return false;
    for(const auto& path:{"sidebar.pinnedAgentIds","sidebar.workspaceGroupOrder","sidebar.collapsedWorkspaceGroupKeys"}){
        const auto array=value(path).toArray();if(array.size()>10000)return false;QSet<QString> unique;
        for(const auto& entry:array){if(!entry.isString()||entry.toString().isEmpty()||entry.toString().size()>4096||unique.contains(entry.toString())||entry.toString().contains(QChar::Null))return false;unique.insert(entry.toString());}
    }
    const auto zoom = value("personalization.uiZoom");
    if (zoom.toDouble() != zoom.toInt() || zoom.toInt() < 70 || zoom.toInt() > 130) return false;
    for (const auto &path : {"browsing.startupUrl", "browsing.newTabUrl"})
        if (!validWebUrl(value(path).toString(), true)) return false;
    QSet<QString> engineIds, engineKeywords;
    for (const auto &entry : value("browsing.searchEngines").toArray()) {
        if (!entry.isObject()) return false;
        const auto engine = entry.toObject();
        for (const auto &field : {"id", "name", "keyword", "url"})
            if (!engine.value(field).isString() || engine.value(field).toString().trimmed().isEmpty()) return false;
        const auto id = engine["id"].toString();
        const auto keyword = engine["keyword"].toString().toCaseFolded();
        const auto url = engine["url"].toString();
        if (engineIds.contains(id) || engineKeywords.contains(keyword) || !url.contains("%s")
            || !validWebUrl(QString(url).replace("%s", "query"))) return false;
        engineIds.insert(id);
        engineKeywords.insert(keyword);
    }
    if (!engineIds.contains(value("browsing.defaultEngineId").toString())) return false;
    for (const auto &path : {"skills", "contextFiles"}) {
        for (const auto &entry : value(path).toArray()) {
            const auto item = entry.toObject();
            if (!entry.isObject() || !item["path"].isString() || item["path"].toString().isEmpty()
                || (item.contains("enabled") && !item["enabled"].isBool())) return false;
        }
    }
    for(const auto& path:{"skillOptions.enabledGlobalDirectories","skillOptions.disabledSkills","skillOptions.disabledGlobalSkills"})
        for(const auto& entry:value(path).toArray())if(!entry.isString()||entry.toString().isEmpty())return false;
    const auto workspaceSettings=value("skillOptions.workspaceSettings").toObject();
    for(auto entry=workspaceSettings.begin();entry!=workspaceSettings.end();++entry){
        if(!QDir::isAbsolutePath(entry.key())||!entry.value().isObject())return false;
        const auto settings=entry.value().toObject();
        if(settings.contains("respectAgentsMd")&&!settings["respectAgentsMd"].isBool())return false;
        if(settings.contains("disabledSkills")&&!settings["disabledSkills"].isArray())return false;
        for(const auto& skill:settings["disabledSkills"].toArray())if(!skill.isString()||skill.toString().isEmpty())return false;
    }
    const auto validSetting=[](const QJsonValue& setting){return setting.isString()&&(setting=="ask"||setting=="allow"||setting=="block");};
    const QSet<QString> permissionNames={"camera","location","notifications","clipboard","fullscreen","downloads","media","geolocation","clipboard-read"};
    const auto permissions=value("browsing.permissions").toObject();
    for(auto it=permissions.begin();it!=permissions.end();++it)if(!permissionNames.contains(it.key())||!validSetting(it.value()))return false;
    const auto exceptions=value("browsing.permissionExceptions").toObject();
    for(auto type=exceptions.begin();type!=exceptions.end();++type){
        if(!permissionNames.contains(type.key())||!type.value().isObject())return false;
        const auto sites=type.value().toObject();
        for(auto site=sites.begin();site!=sites.end();++site){
            const QUrl origin(site.key(),QUrl::StrictMode);
            if(!validWebUrl(site.key())||!origin.path().isEmpty()||origin.hasQuery()||origin.hasFragment())return false;
            const QJsonValue setting=site.value().isObject()?site.value().toObject().value("setting"):QJsonValue(site.value());
            if(!validSetting(setting)&&!(setting.isDouble()&&setting.toDouble()==setting.toInt()&&setting.toInt()>=0&&setting.toInt()<=2))return false;
        }
    }
    for (const auto &path : {"worktrees.setupScript", "worktrees.directory"}) {
        const auto field = value(path);
        if (!field.isUndefined() && !field.isNull() && !field.isString()) return false;
    }
    const auto validModel=[](const QJsonValue& entry){
        if(!entry.isObject())return false;
        const auto item=entry.toObject();
        for(const auto& key:{"providerInstanceId","modelId"})if(!item[key].isString()||item[key].toString().trimmed().isEmpty())return false;
        if(item.contains("overrides")&&!item["overrides"].isObject())return false;
        const auto options=item["overrides"].toObject();
        if(options.contains("reasoningEffort")&&(!options["reasoningEffort"].isString()||!QStringList{"","minimal","low","medium","high","xhigh","max","none"}.contains(options["reasoningEffort"].toString())))return false;
        if(options.contains("thinkingMode")&&(!options["thinkingMode"].isString()||!QStringList{"","enabled","disabled"}.contains(options["thinkingMode"].toString())))return false;
        for(const auto& key:{"thinkingBudget","maxOutputTokens"})if(options.contains(key)&&(!options[key].isDouble()||options[key].toDouble()!=options[key].toInt()||options[key].toInt()<0||options[key].toInt()>200000))return false;
        return true;
    };
    for (const auto &path : {"utilityModels.titleGeneration", "utilityModels.contextCompression", "modelPresets"}) {
        QSet<QString> presetIds;
        for (const auto &entry : value(path).toArray()) {
            const auto item = entry.toObject();
            if (!validModel(entry)) return false;
            if (QString(path) == "modelPresets" && (!item["name"].isString() || !item["id"].isString())) return false;
            if(QString(path)=="modelPresets"){
                const auto id=item["id"].toString();if(id.isEmpty()||presetIds.contains(id))return false;presetIds.insert(id);
                for(const auto& key:{"models","titleGeneration","contextCompression"}){
                    if(!item.contains(key))continue;
                    if(!item[key].isArray())return false;
                    if(QString(key)=="models"&&item[key].toArray().isEmpty())return false;
                    for(const auto& model:item[key].toArray())if(!validModel(model))return false;
                }
            }
        }
    }
    return true;
}
}

QJsonObject SettingsPage::defaultPreferences() {
    return {{"sidebar",QJsonObject{{"pinnedAgentIds",QJsonArray{}},{"agentListGroupingMode","age"},{"workspaceGroupOrder",QJsonArray{}},{"collapsedWorkspaceGroupKeys",QJsonArray{}}}},
            {"agent", QJsonObject{{"keepAwake",true},{"notificationLoudness","normal"},{"notificationSound","default"}}},
            {"personalization", QJsonObject{{"appearance","system"},{"theme","blue"},{"uiZoom",100},{"language","en"}}},
            {"browsing", QJsonObject{{"searchEngines",defaultEngines()},{"defaultEngineId","google"},
                                    {"newTabUrl",""},{"startupUrl",""},{"permissions",QJsonObject{}},{"permissionExceptions",QJsonObject{}}}},
            {"skills",QJsonArray{}},{"contextFiles",QJsonArray{}},{"skillOptions",QJsonObject{{"respectAgentsMd",true},{"enabledGlobalDirectories",QJsonArray{}},{"disabledSkills",QJsonArray{}},{"disabledGlobalSkills",QJsonArray{}},{"workspaceSettings",QJsonObject{}}}},{"worktrees",QJsonObject{}},
            {"plugins",QJsonObject{}},{"utilityModels",QJsonObject{{"titleGeneration",QJsonArray{}},{"contextCompression",QJsonArray{}}}},
            {"modelPresets",QJsonArray{}}};
}

SettingsPage::SettingsPage(QString dataDir, QWidget *parent)
    : QWidget(parent), m_dataDir(std::move(dataDir)), m_preferences(defaultPreferences()) {
    setObjectName("adeSettings");
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0,0,0,0);
    root->setSpacing(0);
    m_navigation = new QButtonGroup(this);
    m_navigation->setExclusive(true);
    buildNavigation();
    auto *right = new QWidget;
    right->setObjectName("settingsSurface");
    auto *column = new QVBoxLayout(right);
    column->setContentsMargins(0,0,0,0);
    column->setSpacing(0);
    m_scroll = new QScrollArea;
    m_scroll->setObjectName("settingsScroll");
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    column->addWidget(m_scroll,1);
    m_status = text({});
    m_status->setObjectName("settingsStatus");
    m_status->setContentsMargins(20,6,20,8);
    column->addWidget(m_status);
    root->addWidget(right,1);
    setStyleSheet(QStringLiteral(
        "QWidget#adeSettings{background:#eff3f8;color:#232629;}"
        "QLabel{color:#232629;}"
        "QWidget#adeSettings QLabel[settingsMuted=true]{color:#777c81;}"
        "QWidget#settingsNavigation,QScrollArea#settingsNavigationScroll,QWidget#settingsNavigationBody{background:#eff3f8;border:0;}"
        "QWidget#settingsSurface,QScrollArea#settingsScroll,QWidget#settingsScrollBody,QWidget#settingsContent{background:#fdfdfd;}"
        "QPushButton[settingsNav=true]{background:transparent;border:0;text-align:left;padding:6px 8px;border-radius:8px;color:#62676c;font-size:14px;}"
        "QPushButton[settingsNav=true]:checked{background:#e1e6eb;color:#22272c;}"
        "QPushButton[settingsNav=true]:hover{background:#e7ebef;}"
        "QLabel#settingsNavGroup{color:#7b8186;font-size:12px;padding:0 6px 4px 6px;}"
        "QLabel#settingsTitle{min-height:28px;font-size:20px;font-weight:600;color:#202225;}"
        "QLabel#settingHeading{min-height:28px;font-size:18px;font-weight:500;color:#282b2d;}"
        "QLabel#settingCardTitle{font-size:14px;font-weight:500;color:#232629;}"
        "QLabel#settingDescription{color:#777c81;font-size:14px;min-height:20px;}"
        "QLabel#providerDescription,QLabel#pluginUsageStatus{color:#777c81;font-size:12px;}"
        "QPushButton[providerSettingsCard=true]{padding:0;background:#f3f4f4;border:1px solid #e7e9eb;border-radius:8px;text-align:left;}"
        "QPushButton[providerSettingsCard=true]:hover,QPushButton[providerSettingsCard=true]:focus{background:#e9edf1;border-color:#aab0bb;}"
        "QFrame#pluginCredentialCard{background:transparent;border:1px solid #e7e9eb;border-radius:8px;}"
        "QWidget#settingsContent QPushButton#allPlugins{padding:0;background:transparent;border:0;}"
        "QFrame#settingCard{background:#f3f4f4;border:1px solid #e7e9eb;border-radius:8px;}"
        "QFrame#settingsEmptyState{background:transparent;border:1px dashed #e7e9eb;border-radius:6px;}"
        "QFrame#archivedCard{background:#fdfdfd;border:1px solid #e7e9eb;border-radius:12px;}"
        "QWidget#settingsContent QListWidget#archivedChats{background:transparent;border:0;border-radius:0;padding:0;}"
        "QListWidget#archivedChats::item{border-bottom:1px solid #e7e9eb;}"
        "QListWidget#archivedChats::item:selected,QListWidget#archivedChats::item:hover{background:#e7ebef;}"
        "QLabel#archivedTitle{font-size:14px;font-weight:500;color:#232629;}"
        "QLabel#archivedMeta{font-size:12px;color:#777c81;}"
        "QLabel#archivedEmpty{font-size:14px;color:#777c81;}"
        "QWidget#settingsContent QPushButton[archiveTrash=true]{background:transparent;border:0;padding:0;}"
        "QWidget#settingsContent QPushButton[archiveRestore=true]{padding:3px 10px;font-size:12px;}"
        "QWidget#settingsContent QPushButton{padding:6px 13px;background:#f2f3f3;border:1px solid #e4e7e9;border-radius:6px;color:#232629;}"
        "QWidget#settingsContent QPushButton#pluginCard{padding:0;background:#f3f4f4;border:1px solid #e7e9eb;border-radius:8px;}"
        "QWidget#settingsContent QPushButton:hover{background:#e9edf1;}"
        "QWidget#settingsContent QPushButton:disabled{color:#92969a;}"
        "QWidget#settingsContent QPushButton[compactAdd=true]{padding:2px;background:transparent;border:1px dashed #e7e9eb;font-size:12px;}"
        "QWidget#settingsContent QLineEdit,QWidget#settingsContent QPlainTextEdit,QWidget#settingsContent QListWidget{background:#fff;border:1px solid #e1e4e7;border-radius:6px;padding:6px;color:#232629;}"
        "QWidget#settingsContent QComboBox,QWidget#settingsContent QSpinBox{padding:5px;border:1px solid #e1e4e7;border-radius:5px;background:#f7f7f7;color:#232629;}"
        "QWidget#settingsContent QCheckBox,QWidget#settingsContent QRadioButton{spacing:8px;min-height:24px;color:#232629;}"));
    m_baseStyleSheet = styleSheet();
    connect(QGuiApplication::styleHints(),&QStyleHints::colorSchemeChanged,this,[this]{
        if(preference("personalization.appearance").toString()=="system")applyAppearance();
    });
    reload();
}

void SettingsPage::applyAppearance() {
    const auto appearance=preference("personalization.appearance").toString("system");
    const bool dark=appearance=="dark" || (appearance=="system" && QGuiApplication::styleHints()->colorScheme()==Qt::ColorScheme::Dark);
    QString style=m_baseStyleSheet;
    if(dark){
        const QList<QPair<QString,QString>> colors={
            {"#eff3f8","#1d222c"},{"#232629","#e8e8ed"},{"#fdfdfd","#21242b"},
            {"#62676c","#c4c8d0"},{"#e1e6eb","#354052"},{"#22272c","#e8e8ed"},
            {"#e7ebef","#303846"},{"#7b8186","#aab0bb"},{"#202225","#eceef4"},
            {"#282b2d","#eceef4"},{"#777c81","#b7bdc8"},{"#f3f4f4","#2b303a"},
            {"#e7e9eb","#3d424b"},{"#f2f3f3","#303846"},{"#e4e7e9","#424955"},
            {"#92969a","#747d89"},{"#e9edf1","#394354"},{"#e1e4e7","#424955"},{"#f7f7f7","#2b303a"},{"#fff","#252a33"}
        };
        for(const auto &pair:colors)style.replace(pair.first,pair.second,Qt::CaseInsensitive);
    }
    const double scale=preference("personalization.uiZoom").toInt(100)/100.0;
    if(scale!=1.0){
        const QRegularExpression expression("([0-9]+)px");
        auto matches=expression.globalMatch(style);
        QList<QRegularExpressionMatch> positions;
        while(matches.hasNext())positions.append(matches.next());
        for(auto it=positions.crbegin();it!=positions.crend();++it)
            style.replace(it->capturedStart(),it->capturedLength(),QString::number(qMax(1,qRound(it->captured(1).toInt()*scale)))+"px");
    }
    setStyleSheet(style);
    updateResponsiveLayout();
}

void SettingsPage::buildNavigation() {
    auto *navigation = new QWidget;
    navigation->setObjectName("settingsNavigation");
    navigation->setFixedWidth(250);
    auto *layout = new QVBoxLayout(navigation);
    layout->setContentsMargins(8,4,8,10);
    layout->setSpacing(0);
    auto *back = new QPushButton("← Back");
    back->setObjectName("settingsBack");
    back->setProperty("settingsNav",true);
    layout->addWidget(back);
    connect(back,&QPushButton::clicked,this,&SettingsPage::backRequested);
    layout->addSpacing(20);
    auto* scroll=new QScrollArea;
    scroll->setObjectName("settingsNavigationScroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* navBody=new QWidget;
    navBody->setObjectName("settingsNavigationBody");
    auto* navLayout=new QVBoxLayout(navBody);
    navLayout->setContentsMargins(2,0,6,12);
    navLayout->setSpacing(1);
    scroll->setWidget(navBody);
    layout->addWidget(scroll,1);
    bool firstGroup=true;
    for (const auto &section : sections) {
        if (*section.group) {
            if(!firstGroup)navLayout->addSpacing(16);
            firstGroup=false;
            if (!QString::fromUtf8(section.group).trimmed().isEmpty())
            {
                auto *group = text(QString::fromUtf8(section.group),"settingsNavGroup");
                navLayout->addWidget(group);
            }
        }
        const QMap<QString,QString> navIcons={{"agent-general","settings"},{"models-providers","brain"},{"skills-context","document"},{"worktree-setup","branch"},{"plugins","plugin"},{"archived-agents","archive"},{"personalization","palette"},{"browsing","globe"},{"clear-data","trash"},{"history","clock"},{"account","user"},{"about","info"}};
        auto *button = new QPushButton(icon(navIcons.value(section.id),QColor("#70757b")),QString::fromUtf8(section.label).replace("&","&&"));button->setIconSize(QSize(18,18));
        button->setProperty("settingsNav",true);
        button->setProperty("settingsGlyph",navIcons.value(section.id));
        button->setProperty("sectionId",QString::fromLatin1(section.id));
        button->setObjectName("settingsNav_"+QString::fromLatin1(section.id));
        button->setAccessibleName(QString::fromUtf8(section.label));
        button->setCheckable(true);
        button->setMinimumHeight(32);
        button->setToolTip(QString::fromUtf8(section.label));
        button->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
        m_navigation->addButton(button);
        navLayout->addWidget(button);
        connect(button,&QPushButton::clicked,this,[this,id=QString::fromLatin1(section.id)]{selectSection(id);});
    }
    navLayout->addStretch();
    static_cast<QHBoxLayout *>(this->layout())->addWidget(navigation);
}

QJsonValue SettingsPage::preference(const QString &path, const QJsonValue &fallback) const {
    const auto parts = path.split('.');
    return getPath(m_preferences,parts,getPath(defaultPreferences(),parts,fallback));
}

void SettingsPage::reload() {
    m_preferences = defaultPreferences();
    m_storageValid = true;
    m_storageError.clear();
    QFile file(m_dataDir+"/preferences.json");
    m_preferencesFileExisted = file.exists();
    m_persistedPreferences.clear();
    if (m_preferencesFileExisted) {
        if (!file.open(QIODevice::ReadOnly) || file.size() > 2*1024*1024) {
            m_storageValid = false;
            m_storageError = "Preferences could not be read. The saved file has been preserved.";
        } else {
            QJsonParseError error;
            m_persistedPreferences = file.readAll();
            const auto document = QJsonDocument::fromJson(m_persistedPreferences,&error);
            if (error.error != QJsonParseError::NoError || !document.isObject()
                || document.object()["schemaVersion"].toInt() != 1 || !document.object()["preferences"].isObject()
                || !validPreferences(document.object()["preferences"].toObject())) {
                m_storageValid = false;
                m_storageError = "Preferences have an unsupported or invalid format. The saved file has been preserved.";
            } else m_preferences = document.object()["preferences"].toObject();
        }
    }
    applyAppearance();
    const QPointer<SettingsPage> alive(this);
    buildContent();
    if(!alive)return;
    if (!m_storageValid) showStatus(m_storageError,true);
}

bool SettingsPage::savePreferences(const QJsonObject &preferences) {
    if (!m_storageValid) { showStatus(m_storageError,true); return false; }
    if (!validPreferences(preferences)) { showStatus("These settings contain an invalid value.",true); return false; }
    const auto bytes = QJsonDocument(QJsonObject{{"schemaVersion",1},{"preferences",preferences}}).toJson();
    if (bytes.size() > 2*1024*1024) { showStatus("Preferences exceed the 2 MiB size limit.",true); return false; }
    QFile previous(m_dataDir+"/preferences.json");
    if (previous.exists() != m_preferencesFileExisted
        || (previous.exists() && (!previous.open(QIODevice::ReadOnly) || previous.size() > 2*1024*1024
                                  || previous.readAll() != m_persistedPreferences))) {
        m_storageValid = false;
        m_storageError = "Preferences changed outside this page. Reopen settings before saving; the file has been preserved.";
        showStatus(m_storageError,true);
        return false;
    }
    previous.close();
    if (!QDir().mkpath(m_dataDir)) { showStatus("Cannot create the settings directory.",true); return false; }
    QSaveFile file(m_dataDir+"/preferences.json");
    if (!file.open(QIODevice::WriteOnly)) { showStatus("Cannot open settings for saving.",true); return false; }
    if (file.write(bytes) != bytes.size() || !file.commit()) { showStatus("Could not save settings. Previous settings are preserved.",true); return false; }
    m_persistedPreferences = bytes;
    m_preferencesFileExisted = true;
    m_preferences = preferences;
    applyAppearance();
    showStatus("Saved");
    emit preferencesChanged();
    return true;
}

bool SettingsPage::setPreference(const QString &path, const QJsonValue &value) {
    const auto parts = path.split('.');
    if (path.isEmpty() || parts.contains(QString()) || value.isUndefined()) return false;
    if ((path == "browsing.newTabUrl" || path == "browsing.startupUrl") && !validWebUrl(value.toString(),true)) {
        showStatus("Enter a valid HTTP or HTTPS URL without embedded credentials.",true); return false;
    }
    if (path == "personalization.uiZoom" && (!value.isDouble() || value.toInt()<70 || value.toInt()>130)) return false;
    return savePreferences(putPath(m_preferences,parts,value));
}

void SettingsPage::showStatus(const QString &value, bool error) {
    m_status->setText(value);
    m_status->setStyleSheet(error?"color:#b34232;":"color:#7b8186;");
}

void SettingsPage::selectSection(const QString &section) {
    if (std::none_of(std::begin(sections),std::end(sections),[&](const Section &s){return section==s.id;})) return;
    m_section = section;
    m_plugin.clear();
    buildContent();
}

void SettingsPage::buildContent() {
    const auto generation=++m_pluginViewGeneration;
    const QPointer<SettingsPage> alive(this);
    invalidatePluginConnectionCheck();
    if(!alive||generation!=m_pluginViewGeneration)return;
    for (auto *button : m_navigation->buttons()) button->setChecked(button->property("sectionId").toString()==m_section);
    if (auto *old = m_scroll->takeWidget()) {
        // Navigation can originate inside this subtree. Keep its sender alive
        // until the signal unwinds, and prevent stale controls from mutating data.
        for (auto *child : old->findChildren<QObject *>()) disconnect(child,nullptr,this,nullptr);
        old->hide();
        old->setEnabled(false);
        old->deleteLater();
    }
    auto *body = new QWidget;
    body->setObjectName("settingsScrollBody");
    auto *row = new QHBoxLayout(body);
    row->setContentsMargins(24,96,24,96);
    row->addStretch();
    auto *content = new QWidget;
    content->setObjectName("settingsContent");
    content->setMaximumWidth(m_section=="archived-agents"?896:768);
    content->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(10);
    if(m_section=="models-providers")layout->setSpacing(0);
    if(m_section!="plugins"||m_plugin.isEmpty())
        for (const auto &s : sections) if (m_section==s.id) layout->addWidget(text(QString::fromUtf8(s.label),"settingsTitle"));
    if(m_section=="models-providers")layout->addSpacing(32);
    else if(m_section!="plugins"&&m_section!="archived-agents")layout->addSpacing(18);
    if (m_section=="agent-general") buildGeneral(layout);
    else if(m_section=="models-providers") buildProviders(layout);
    else if(m_section=="skills-context") buildSkills(layout);
    else if(m_section=="worktree-setup") buildWorktrees(layout);
    else if(m_section=="plugins") buildPlugins(layout);
    else if(m_section=="archived-agents") buildArchived(layout);
    else if(m_section=="personalization") buildPersonalization(layout);
    else if(m_section=="browsing") buildBrowsing(layout);
    else if(m_section=="clear-data") buildClearData(layout);
    else if(m_section=="history") buildHistory(layout);
    else if(m_section=="account") buildAccount(layout);
    else if(m_section=="about") buildAbout(layout);
    layout->addStretch();
    row->addWidget(content,100);
    row->addStretch();
    m_scroll->setWidget(body);
    showStatus(m_storageError,!m_storageValid);
    updateResponsiveLayout();
    if(!alive||generation!=m_pluginViewGeneration)return;
    m_scroll->verticalScrollBar()->setValue(0);
}

void SettingsPage::buildGeneral(QVBoxLayout *layout) {
    heading(layout,"Keep app awake while agents work","Prevent app suspension while agents run active work. Waiting for questions or approval is idle.");
    auto *awake = new QCheckBox("Keep awake during active work");
    awake->setObjectName("agentKeepAwake");
    awake->setChecked(preference("agent.keepAwake").toBool());
    layout->addWidget(awake);
    connect(awake,&QCheckBox::toggled,this,[this](bool checked){setPreference("agent.keepAwake",checked);});
    heading(layout,"Notification sounds","Choose your notification preferences for completed work, questions and errors.");
    auto *form = new QFormLayout;
    auto *loudness = new QComboBox;
    loudness->setObjectName("notificationLoudness");
    loudness->addItem("Off","off");loudness->addItem("Normal","normal");loudness->addItem("Loud","loud");
    loudness->setCurrentIndex(qMax(0,loudness->findData(preference("agent.notificationLoudness").toString())));
    form->addRow("Loudness",loudness);
    connect(loudness,&QComboBox::currentIndexChanged,this,[this,loudness]{setPreference("agent.notificationLoudness",loudness->currentData().toString());});
    layout->addLayout(form);
    layout->addWidget(text("These preferences are stored locally. Sound packs and per-event playback are still being connected."));
}

void SettingsPage::buildProviders(QVBoxLayout *layout) {
    auto* providerSection=new QVBoxLayout;
    providerSection->setSpacing(24);
    layout->addLayout(providerSection);
    sectionHeader(providerSection,"Providers","Configure how the agent connects to LLM providers. Add API keys, connect coding plans, or set up custom endpoints.");
    auto* cards=new QVBoxLayout;
    cards->setSpacing(12);
    providerSection->addLayout(cards);
    QString error;
    auto providers=ProviderStore::load(m_dataDir+"/providers.json",&error);
    for(auto& provider:providers)provider.apiKey.clear();
    if(!error.isEmpty())cards->addWidget(text(error));
    for(const auto& provider:providers) {
        auto* entry=new QPushButton;
        entry->setObjectName("providerSettingsCard_"+provider.id);
        entry->setProperty("providerSettingsCard",true);
        entry->setAccessibleName("Configure "+provider.name);
        entry->setCursor(Qt::PointingHandCursor);
        entry->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
        auto* row=new QHBoxLayout(entry);
        row->setContentsMargins(12,12,12,12);row->setSpacing(12);
        row->addWidget(settingsMark(providerMark(provider),36));
        auto* labels=new QVBoxLayout;labels->setSpacing(2);
        labels->addWidget(new ElidedLine(provider.name,"settingCardTitle"));
        labels->addWidget(new ElidedLine(providerDescription(provider),"providerDescription"));
        row->addLayout(labels,1);
        auto* models=new QPushButton("Models");
        models->setObjectName("manageProviderModels_"+provider.id);
        models->setAccessibleName("Manage models for "+provider.name);row->addWidget(models);
        connect(models,&QPushButton::clicked,this,[this,id=provider.id]{
            auto* dialog=new ProviderModelsDialog(m_dataDir,id,preference("modelCatalogs").toObject().value(id).toObject(),this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            connect(dialog,&ProviderModelsDialog::modelsChanged,this,[this]{emit providerVisibilityChanged();});
            dialog->open();
        });
        row->addWidget(settingsMark("forward",14));
        cards->addWidget(entry);
        connect(entry,&QPushButton::clicked,this,[this,id=provider.id]{emit providerEditRequested(id);});
    }
    if(providers.isEmpty())card(cards,"No providers configured","Connect your own provider to start an agent session.");
    auto* add=new QPushButton("+ Add Provider");add->setObjectName("settingsAddProvider");
    auto* import=new QPushButton("Import API key file…");import->setObjectName("settingsImportProviderKeys");
    actionRow(cards,{import,add},Qt::AlignRight);
    connect(add,&QPushButton::clicked,this,&SettingsPage::addProviderRequested);
    connect(import,&QPushButton::clicked,this,&SettingsPage::providerKeyImportRequested);
    layout->addSpacing(32);
    auto* utilities=new QVBoxLayout;utilities->setSpacing(16);layout->addLayout(utilities);
    sectionHeader(utilities,"Default utility models","These models are used for background tasks when no preset-specific configuration is set. The agent tries each model in order until one succeeds.");
    auto* lists=new QVBoxLayout;lists->setSpacing(24);utilities->addLayout(lists);
    for(const auto& pair:QList<QPair<QString,QString>>{{"utilityModels.titleGeneration","Title generation"},{"utilityModels.contextCompression","Context compression"}}){
        auto* list=new QVBoxLayout;list->setSpacing(8);lists->addLayout(list);buildUtilityModels(list,pair.first,pair.second);
    }
    layout->addSpacing(32);auto* presetHeading=new QHBoxLayout;auto* presetLabels=new QVBoxLayout;presetLabels->setSpacing(2);
    presetLabels->addWidget(text("Presets","settingHeading"));presetLabels->addWidget(text("Named model configurations for one-click switching. Shown at the top of the model selector."));presetHeading->addLayout(presetLabels,1);
    auto* addPreset=new QPushButton("+ Add preset");addPreset->setObjectName("addModelPreset");presetHeading->addWidget(addPreset,0,Qt::AlignTop);layout->addLayout(presetHeading);layout->addSpacing(16);
    connect(addPreset,&QPushButton::clicked,this,[this]{editModelPreset();});
    const auto presets = preference("modelPresets").toArray();
    for (int i=0;i<presets.size();++i) {
        const auto preset = presets[i].toObject();
        QHBoxLayout *row;
        const auto models=preset["models"].toArray();const auto primary=models.isEmpty()?preset:models.first().toObject();
        card(layout,preset["name"].toString(),primary["modelId"].toString()+(models.size()>1?QString(" · %1 fallback models").arg(models.size()-1):QString()),&row);
        auto *edit = new QPushButton("Edit");edit->setObjectName("editModelPreset_"+preset["id"].toString());row->addWidget(edit);
        connect(edit,&QPushButton::clicked,this,[this,preset]{editModelPreset(preset["id"].toString());});
        auto *remove = new QPushButton("Remove");row->addWidget(remove);
        connect(remove,&QPushButton::clicked,this,[this,i]{auto records=preference("modelPresets").toArray();records.removeAt(i);if(setPreference("modelPresets",records))buildContent();});
    }
    if (presets.isEmpty()){
        auto* empty=new QFrame;empty->setObjectName("settingsEmptyState");empty->setMinimumHeight(88);auto* column=new QVBoxLayout(empty);column->setSpacing(4);column->addStretch();
        auto* title=text("No presets configured.");title->setAlignment(Qt::AlignCenter);column->addWidget(title);
        auto* help=text("Create a preset to quickly switch between model configurations.");help->setStyleSheet("font-size:12px;");help->setAlignment(Qt::AlignCenter);column->addWidget(help);column->addStretch();layout->addWidget(empty);
    }
}

void SettingsPage::editModelPreset(const QString &id) {
    QString error;const auto providers=ProviderStore::load(m_dataDir+"/providers.json",&error);
    if(!error.isEmpty()){showStatus(error,true);return;}
    auto choices=availableModelSelections(providers,preference("modelCatalogs").toObject(),{});
    if(choices.isEmpty()&&id.isEmpty()){emit providersRequested();return;}
    auto records=preference("modelPresets").toArray();int existing=-1;QJsonObject preset;
    for(int i=0;i<records.size();++i)if(records[i].toObject()["id"].toString()==id){existing=i;preset=records[i].toObject();break;}
    const auto existingModels=preset["models"].toArray();const auto primary=existingModels.isEmpty()?preset:existingModels.first().toObject();
    QDialog dialog(this);dialog.setObjectName("modelPresetDialog");dialog.setWindowTitle(id.isEmpty()?"Add model preset":"Edit model preset");dialog.resize(620,570);
    auto* form=new QFormLayout(&dialog);auto* name=new QLineEdit(preset["name"].toString());name->setObjectName("presetName");
    auto* model=new QComboBox;model->setObjectName("presetModel");int selected=-1;
    for(const auto& choice:choices){model->addItem(choice.label,choice.toJson());if(choice.providerId==primary["providerInstanceId"].toString()&&choice.modelId==primary["modelId"].toString())selected=model->count()-1;}
    if(existing>=0&&selected<0){ModelSelection saved{primary["providerInstanceId"].toString(),primary["modelId"].toString(),{},primary["modelId"].toString(),{}};ProviderConfig resolved;const bool available=saved.resolve(providers,&resolved);model->addItem((available?resolved.name+(!isModelEnabled(resolved,saved.modelId)?" (disabled)":""):saved.providerId+" (unavailable)")+" / "+saved.modelId,saved.toJson());selected=model->count()-1;}
    model->setCurrentIndex(selected>=0?selected:0);model->setEditable(true);model->setInsertPolicy(QComboBox::NoInsert);model->completer()->setCompletionMode(QCompleter::PopupCompletion);model->completer()->setFilterMode(Qt::MatchContains);
    auto* reasoning=new QComboBox;reasoning->setObjectName("presetReasoning");reasoning->addItem("Use provider setting",QString());for(const auto& effort:QStringList{"minimal","low","medium","high","xhigh","max","none"})reasoning->addItem(effort,effort);
    auto* thinking=new QComboBox;thinking->addItem("Use provider setting",QString());thinking->addItem("Enabled","enabled");thinking->addItem("Disabled","disabled");
    auto* budget=new QSpinBox;budget->setRange(0,200000);budget->setSpecialValueText("Use provider setting");
    auto* limit=new QSpinBox;limit->setRange(0,200000);limit->setSpecialValueText("Use provider setting");
    const auto options=primary["overrides"].toObject();reasoning->setCurrentIndex(qMax(0,reasoning->findData(options["reasoningEffort"].toString())));thinking->setCurrentIndex(qMax(0,thinking->findData(options["thinkingMode"].toString())));budget->setValue(options["thinkingBudget"].toInt());limit->setValue(options["maxOutputTokens"].toInt());
    form->addRow("Name",name);form->addRow("Provider and model",model);form->addRow("Reasoning effort",reasoning);form->addRow("Thinking",thinking);form->addRow("Thinking budget",budget);form->addRow("Output token limit",limit);
    auto* chains=new QTabWidget;form->addRow(chains);
    auto addChain=[&](const QString& title,const QJsonArray& entries,const QString& description,bool utility){
        const auto chainChoices=utility?availableUtilityModelSelections(providers,preference("modelCatalogs").toObject()):choices;
        auto* pane=new QWidget;auto* column=new QVBoxLayout(pane);auto* hint=new QLabel(description);hint->setWordWrap(true);column->addWidget(hint);
        auto* list=new QListWidget;list->setObjectName(title=="Title generation"?"presetTitleModels":title=="Context compression"?"presetCompressionModels":"presetFallbackModels");list->setMinimumHeight(110);column->addWidget(list);
        auto labelFor=[choices,utility,providers,catalogs=preference("modelCatalogs").toObject()](const QJsonObject& entry){if(utility)return utilityModelEntryLabel(entry,providers,catalogs);for(const auto& candidate:choices)if(candidate.providerId==entry["providerInstanceId"].toString()&&candidate.modelId==entry["modelId"].toString())return candidate.label;return entry["providerInstanceId"].toString()+" / "+entry["modelId"].toString();};
        for(const auto& value:entries){auto* item=new QListWidgetItem(labelFor(value.toObject()),list);item->setData(Qt::UserRole,value.toObject());}
        auto* controls=new QHBoxLayout;column->addLayout(controls);auto* add=new QPushButton("Add model");auto* up=new QPushButton("Move up");auto* down=new QPushButton("Move down");auto* remove=new QPushButton("Remove");for(auto* button:{add,up,down,remove})controls->addWidget(button);
        add->setEnabled(!chainChoices.isEmpty());
        if(utility&&chainChoices.isEmpty())hint->setText(description+" No eligible API models are configured. Coding agents cannot perform utility requests.");
        connect(add,&QPushButton::clicked,pane,[list,chainChoices,labelFor,pane]{
            const QPointer<QWidget> owner(pane);const QPointer<QListWidget> safeList(list);
            const auto chosen=chooseModelEntry(pane,chainChoices,"Add model");
            if(!owner||!safeList||!chosen)return;
            const auto entry=chosen->toJson();
            for(int i=0;i<list->count();++i)if(ModelSelection::fromJson(list->item(i)->data(Qt::UserRole).toJsonObject()).key()==chosen->key())return;
            auto* item=new QListWidgetItem(labelFor(entry),list);item->setData(Qt::UserRole,entry);list->setCurrentItem(item);
        });
        connect(up,&QPushButton::clicked,pane,[list]{const int row=list->currentRow();if(row<=0)return;auto* item=list->takeItem(row);list->insertItem(row-1,item);list->setCurrentRow(row-1);});
        connect(down,&QPushButton::clicked,pane,[list]{const int row=list->currentRow();if(row<0||row>=list->count()-1)return;auto* item=list->takeItem(row);list->insertItem(row+1,item);list->setCurrentRow(row+1);});
        connect(remove,&QPushButton::clicked,pane,[list]{delete list->takeItem(list->currentRow());});
        chains->addTab(pane,title);return list;
    };
    QJsonArray fallbackEntries;for(int i=1;i<existingModels.size();++i)fallbackEntries.append(existingModels[i]);
    auto* fallbacks=addChain("Fallback models",fallbackEntries,"Try these models in order if the main model is overloaded.",false);
    auto* titleModels=addChain("Title generation",preset["titleGeneration"].toArray(),"An empty list uses this preset's eligible chat models, or global utility models for a coding-agent-only preset.",true);
    auto* compressionModels=addChain("Context compression",preset["contextCompression"].toArray(),"An empty list uses this preset's eligible chat models, or global utility models for a coding-agent-only preset.",true);
    auto* feedback=new QLabel;feedback->setWordWrap(true);form->addRow(feedback);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);form->addRow(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{if(name->text().trimmed().isEmpty()||model->currentText()!=model->itemText(model->currentIndex())){feedback->setText("Enter a name and select a model from the list.");return;}dialog.accept();});
    if(dialog.exec()!=QDialog::Accepted)return;
    const auto choice=ModelSelection::fromJson(model->currentData().toJsonObject());QJsonObject overrides;
    if(reasoning->currentIndex()>0)overrides["reasoningEffort"]=reasoning->currentData().toString();
    if(thinking->currentIndex()>0)overrides["thinkingMode"]=thinking->currentData().toString();
    if(budget->value()>0)overrides["thinkingBudget"]=budget->value();
    if(limit->value()>0)overrides["maxOutputTokens"]=limit->value();
    preset["id"]=id.isEmpty()?QUuid::createUuid().toString(QUuid::WithoutBraces):id;preset["name"]=name->text().trimmed();preset["providerInstanceId"]=choice.providerId;preset["modelId"]=choice.modelId;preset["overrides"]=overrides;
    auto collect=[](QListWidget* list){QJsonArray values;for(int i=0;i<list->count();++i)values.append(list->item(i)->data(Qt::UserRole).toJsonObject());return values;};
    auto mainEntry=choice.toJson();mainEntry["overrides"]=overrides;QJsonArray modelChain{mainEntry};for(const auto& value:collect(fallbacks))modelChain.append(value);
    preset["models"]=modelChain;preset["titleGeneration"]=collect(titleModels);preset["contextCompression"]=collect(compressionModels);
    if(existing>=0)records.replace(existing,preset);else records.append(preset);
    if(setPreference("modelPresets",records))buildContent();
}

void SettingsPage::buildUtilityModels(QVBoxLayout *layout,const QString &path,const QString &title) {
    auto* captions=new QVBoxLayout;captions->setSpacing(2);captions->addWidget(text(title,"settingCardTitle"));
    auto* description=text(path.endsWith("titleGeneration")?"Models used to generate conversation titles.":"Models used to compress conversation history.");description->setStyleSheet("font-size:12px;");captions->addWidget(description);layout->addLayout(captions);
    const auto entries=preference(path).toArray();
    QString providerError;const auto providers=ProviderStore::load(m_dataDir+"/providers.json",&providerError);
    const auto catalogs=preference("modelCatalogs").toObject();
    if(entries.isEmpty()){
        auto* empty=new QFrame;empty->setObjectName("settingsEmptyState");empty->setFixedHeight(32);auto* column=new QVBoxLayout(empty);column->setContentsMargins(4,4,4,4);
        auto* label=text("No model configured; uses an eligible chat model.");label->setStyleSheet("font-size:12px;");label->setAlignment(Qt::AlignCenter);column->addWidget(label);layout->addWidget(empty);
    }
    for(int i=0;i<entries.size();++i){
        const auto entry=entries[i].toObject();QHBoxLayout *row;
        card(layout,entry["modelId"].toString(),providerError.isEmpty()?utilityModelEntryLabel(entry,providers,catalogs):entry["providerInstanceId"].toString()+" (provider configuration unavailable)",&row);
        auto *up=new QPushButton("↑");up->setEnabled(i>0);up->setAccessibleName("Move model up");row->addWidget(up);
        auto *remove=new QPushButton("×");remove->setAccessibleName("Remove utility model");row->addWidget(remove);
        connect(up,&QPushButton::clicked,this,[this,path,i]{auto records=preference(path).toArray();const auto value=records.takeAt(i);records.insert(i-1,value);if(setPreference(path,records))buildContent();});
        connect(remove,&QPushButton::clicked,this,[this,path,i]{auto records=preference(path).toArray();records.removeAt(i);if(setPreference(path,records))buildContent();});
    }
    auto* add=action(layout,"+ Add",path+"Add",{});add->setProperty("compactAdd",true);add->setMinimumHeight(0);add->setFixedHeight(26);
    connect(add,&QPushButton::clicked,this,[this,path]{
        QString error;const auto providers=ProviderStore::load(m_dataDir+"/providers.json",&error);
        if(!error.isEmpty()){showStatus(error,true);return;}
        if(providers.isEmpty()){emit providersRequested();return;}
        const auto choices=availableUtilityModelSelections(providers,preference("modelCatalogs").toObject());
        if(choices.isEmpty()){showStatus("No eligible API models are configured. Coding agents cannot perform utility requests.",true);return;}
        const QPointer<SettingsPage> self(this);
        const auto selected=chooseModelEntry(this,choices,"Utility model");
        if(!self||!selected)return;
        const auto selection=*selected;auto records=preference(path).toArray();
        records.append(QJsonObject{{"providerInstanceId",selection.providerId},{"modelId",selection.modelId}});
        if(setPreference(path,records))buildContent();
    });
}

void SettingsPage::buildSkills(QVBoxLayout *layout) {
    const auto workspaceKey=contextWorkspaceKey(m_contextWorkspace);
    // Dot-containing workspace paths must remain JSON object keys, not dotted
    // setPreference paths. Both changes are committed in one atomic save.
    const auto scopedOptions=[this,workspaceKey](QJsonObject options,const QString& field,const QJsonValue& value){
        auto settings=workspaceContextSettingsFor(m_preferences,m_contextWorkspace);
        settings[field]=value;
        auto all=options["workspaceSettings"].toObject();all[workspaceKey]=settings;options["workspaceSettings"]=all;
        options.remove("respectAgentsMd");options.remove("disabledSkills");
        return options;
    };
    heading(layout,"Workspace instructions",workspaceKey.isEmpty()?"Connect a workspace to configure its AGENTS.md instructions.":"These instruction and workspace-skill choices apply only to the connected workspace.");
    auto* respect=new QCheckBox("Respect workspace AGENTS.md");respect->setObjectName("respectWorkspaceAgents");respect->setChecked(workspaceContextSettingsFor(m_preferences,m_contextWorkspace)["respectAgentsMd"].toBool(true));respect->setEnabled(!workspaceKey.isEmpty());layout->addWidget(respect);
    connect(respect,&QCheckBox::toggled,this,[this,respect,scopedOptions](bool checked){
        if(!setPreference("skillOptions",scopedOptions(preference("skillOptions").toObject(),"respectAgentsMd",checked))){QSignalBlocker blocked(respect);respect->setChecked(!checked);}
    });
    heading(layout,"Global skill directories","Skills from ~/.stagewise/skills and ~/.agents/skills are discovered automatically. You can include these additional directories.");
    for(const auto& entry:QList<QPair<QString,QString>>{{"globalskills-codex","~/.codex/skills"},{"globalskills-claude","~/.claude/skills"}}){
        auto* enabled=new QCheckBox(entry.second);enabled->setChecked(preference("skillOptions.enabledGlobalDirectories").toArray().contains(entry.first));layout->addWidget(enabled);
        connect(enabled,&QCheckBox::toggled,this,[this,enabled,key=entry.first](bool checked){auto values=preference("skillOptions.enabledGlobalDirectories").toArray();for(int i=values.size()-1;i>=0;--i)if(values[i].toString()==key)values.removeAt(i);if(checked)values.append(key);if(setPreference("skillOptions.enabledGlobalDirectories",values))buildContent();else{QSignalBlocker blocked(enabled);enabled->setChecked(!checked);}});
    }
    const auto addPaths=[this,layout](const QString &path,const QString &title,bool directory){
        heading(layout,title,directory?"Choose local skill directories for your agent context.":"Choose local instruction and context files.");
        const auto paths=preference(path).toArray();
        for(int i=0;i<paths.size();++i){const auto entry=paths[i].toObject();const auto file=entry["path"].toString();QHBoxLayout *row;
            card(layout,QFileInfo(file).fileName(),file,&row);
            auto *enabled=new QCheckBox("Enabled");enabled->setChecked(entry["enabled"].toBool(true));row->addWidget(enabled);
            auto *open=new QPushButton("Open");row->addWidget(open);auto *remove=new QPushButton("Remove");row->addWidget(remove);
            connect(enabled,&QCheckBox::toggled,this,[this,path,i,enabled](bool checked){auto entries=preference(path).toArray();auto entry=entries[i].toObject();entry["enabled"]=checked;entries[i]=entry;if(setPreference(path,entries))buildContent();else{QSignalBlocker blocked(enabled);enabled->setChecked(!checked);}});
            connect(open,&QPushButton::clicked,this,[file]{QDesktopServices::openUrl(QUrl::fromLocalFile(file));});
            connect(remove,&QPushButton::clicked,this,[this,path,i]{auto entries=preference(path).toArray();entries.removeAt(i);if(setPreference(path,entries))buildContent();});
        }
        connect(action(layout,"+ Add "+(directory?QString("directory"):QString("context file")),"add_"+path),&QPushButton::clicked,this,[this,path,directory]{
            const auto file=directory?QFileDialog::getExistingDirectory(this,"Add skill directory"):QFileDialog::getOpenFileName(this,"Add context file",{},"Context files (*.md *.txt);;All files (*)");
            if(file.isEmpty())return;
            auto entries=preference(path).toArray();
            for(const auto &entry:entries)if(entry.toObject()["path"].toString()==file)return;
            entries.append(QJsonObject{{"path",file},{"enabled",true}});if(setPreference(path,entries))buildContent();
        });
    };
    addPaths("skills","Skills",true);addPaths("contextFiles","Context files",false);
    heading(layout,"Discovered skills","Enabled skills are available to the agent in this workspace. Use /skill-name to invoke a user-invocable skill.");
    auto options=contextOptionsFor(m_preferences,m_contextWorkspace,QDir::homePath());
    const auto disabled=options.disabledSkills;const auto disabledGlobal=options.disabledGlobalSkills;
    options.disabledSkills.clear();options.disabledGlobalSkills.clear();
    ContextLoader loader;const auto snapshot=loader.load(options);
    auto* search=new QLineEdit;search->setObjectName("skillSearch");search->setPlaceholderText("Search skills");layout->addWidget(search);
    auto* list=new QListWidget;list->setObjectName("discoveredSkills");list->setMinimumHeight(220);layout->addWidget(list);
    for(const auto& skill:snapshot.skills){
        auto* item=new QListWidgetItem(skill.name+" · "+skill.source+"\n"+skill.description,list);
        item->setData(Qt::UserRole,skill.name);item->setData(Qt::UserRole+1,skill.source);
        item->setToolTip(skill.contentPath);item->setFlags(item->flags()|Qt::ItemIsUserCheckable);
        item->setCheckState(disabled.contains(skill.name)||(skill.source=="global"&&disabledGlobal.contains(skill.name))?Qt::Unchecked:Qt::Checked);
    }
    connect(search,&QLineEdit::textChanged,list,[list](const QString& query){for(int i=0;i<list->count();++i)list->item(i)->setHidden(!list->item(i)->text().contains(query,Qt::CaseInsensitive));});
    connect(list,&QListWidget::itemChanged,this,[this,list,workspaceKey,scopedOptions](QListWidgetItem* item){
        auto options=preference("skillOptions").toObject();const auto name=item->data(Qt::UserRole).toString();
        const bool checked=item->checkState()==Qt::Checked;
        if(item->data(Qt::UserRole+1).toString()=="global"){
            auto global=options["disabledGlobalSkills"].toArray();for(int i=global.size()-1;i>=0;--i)if(global[i].toString()==name)global.removeAt(i);
            if(!checked)global.append(name);
            options["disabledGlobalSkills"]=global;
            // Explicitly re-enabling a displayed skill must also lift any
            // workspace suppression that made this checkbox unchecked.
            if(checked){auto disabled=workspaceContextSettingsFor(m_preferences,m_contextWorkspace)["disabledSkills"].toArray();
                for(int i=disabled.size()-1;i>=0;--i)if(disabled[i].toString()==name)disabled.removeAt(i);
                if(!workspaceKey.isEmpty())options=scopedOptions(options,"disabledSkills",disabled);else options["disabledSkills"]=disabled;
            }
        }else{
            auto disabled=workspaceContextSettingsFor(m_preferences,m_contextWorkspace)["disabledSkills"].toArray();
            for(int i=disabled.size()-1;i>=0;--i)if(disabled[i].toString()==name)disabled.removeAt(i);
            if(!checked)disabled.append(name);
            options=scopedOptions(options,"disabledSkills",disabled);
        }
        if(!setPreference("skillOptions",options)){QSignalBlocker blocked(list);item->setCheckState(checked?Qt::Unchecked:Qt::Checked);}
    });
    if(snapshot.skills.isEmpty())layout->addWidget(text("No skills found in the enabled directories."));
    for(const auto& warning:snapshot.warnings)layout->addWidget(text(warning));
}

void SettingsPage::setWorktreePageFactory(std::function<QWidget*(QWidget*)> factory){
    m_worktreePageFactory=std::move(factory);if(m_section=="worktree-setup")buildContent();
}
void SettingsPage::buildWorktrees(QVBoxLayout *layout) {
    if(m_worktreePageFactory)layout->addWidget(m_worktreePageFactory(layout->parentWidget()));
    else layout->addWidget(text("Connect a workspace to configure repository setup scripts and manage worktrees."));
    const auto legacy=preference("worktrees.setupScript").toString();
    if(!legacy.isEmpty()){
        heading(layout,"Previous setup draft","This saved draft has not been assigned to a repository. Copy the commands you want into that repository's setup script.");
        auto* draft=new QPlainTextEdit(legacy);draft->setObjectName("previousWorktreeSetupDraft");draft->setReadOnly(true);draft->setMaximumHeight(140);layout->addWidget(draft);
    }
}

void SettingsPage::updateResponsiveLayout() {
    const QPointer<SettingsPage> self(this);
    const auto generation=m_pluginViewGeneration;
    const auto preferences=m_preferences;
    const auto requestedSize=size();
    const auto current=[self,generation,preferences,requestedSize]{
        return self&&self->m_pluginViewGeneration==generation&&self->m_preferences==preferences&&self->size()==requestedSize;
    };
    const qreal scale=preference("personalization.uiZoom").toInt(100)/100.0;
    const bool compact=width()<qRound(700*scale);
    const QColor foreground=preference("personalization.appearance").toString()=="dark"
        ||(preference("personalization.appearance").toString()=="system"&&QGuiApplication::styleHints()->colorScheme()==Qt::ColorScheme::Dark)
        ?QColor("#c4c8d0"):QColor("#62676c");
    if(auto* nav=findChild<QWidget*>("settingsNavigation")) {
        nav->setFixedWidth(qRound((compact?64:250)*scale));
        if(!current())return;
        if(auto* body=nav->findChild<QWidget*>("settingsNavigationBody"))body->layout()->setContentsMargins(compact?0:2,0,compact?0:6,12);
        for(auto* label:nav->findChildren<QLabel*>("settingsNavGroup")){label->setVisible(!compact);if(!current())return;}
        for(auto* button:m_navigation->buttons()) {
            button->setText(compact?QString():button->accessibleName().replace("&","&&"));
            if(!current())return;
            button->setIcon(icon(button->property("settingsGlyph").toString(),foreground));
            if(!current())return;
            button->setIconSize(QSize(qRound(18*scale),qRound(18*scale)));
            button->setFixedHeight(qRound(32*scale));
            if(!current())return;
        }
        if(auto* back=nav->findChild<QPushButton*>("settingsBack")) {
            back->setText(compact?QStringLiteral("←"):QStringLiteral("← Back"));
            if(!current())return;
            back->setAccessibleName("Back");back->setToolTip("Back");
        }
    }
    if(!m_scroll||!m_scroll->widget())return;
    auto* body=m_scroll->widget();
    if(auto* row=qobject_cast<QHBoxLayout*>(body->layout()))
        row->setContentsMargins(qRound((compact?16:24)*scale),qRound((compact?32:96)*scale),qRound((compact?16:24)*scale),qRound(96*scale));
    if(auto* content=body->findChild<QWidget*>("settingsContent"))content->setMaximumWidth(qRound((m_section=="archived-agents"?896:768)*scale));
    if(!current())return;
    for(auto* mark:body->findChildren<QLabel*>("settingsCategoryMark")) {
        const int size=qRound(mark->property("settingsMarkSize").toInt()*scale);
        mark->setFixedSize(size,size);
        if(!current())return;
        const int glyph=qRound(qMin(24,mark->property("settingsMarkSize").toInt())*scale);
        mark->setPixmap(settingsPixmap(mark->property("settingsGlyph").toString(),foreground,glyph));
        if(!current())return;
    }
    for(auto* card:body->findChildren<QPushButton*>())if(card->property("providerSettingsCard").toBool()) {
        card->setMinimumHeight(qMax(qRound(64*scale),card->layout()->minimumSize().height()));
        if(!current())return;
    }
    if(auto* overview=body->findChild<QWidget*>("pluginOverview")) {
        auto* grid=qobject_cast<QGridLayout*>(overview->layout());
        auto* mark=overview->findChild<QLabel*>("settingsCategoryMark");
        auto* summary=overview->findChild<QWidget*>("pluginOverviewSummary");
        auto* enabled=overview->findChild<QCheckBox*>();
        if(grid&&mark&&summary&&enabled) {
            const bool stacked=body->findChild<QWidget*>("settingsContent")->width()<qRound(440*scale);
            grid->addWidget(mark,0,0,1,1,Qt::AlignTop);
            if(!current())return;
            grid->addWidget(summary,stacked?1:0,stacked?0:1,1,stacked?2:1);
            if(!current())return;
            grid->addWidget(enabled,0,stacked?1:2,1,1,Qt::AlignRight|Qt::AlignTop);
            if(!current())return;
            grid->setColumnStretch(1,1);
        }
    }
}

void SettingsPage::resizeEvent(QResizeEvent* event){
    const QPointer<SettingsPage> self(this);
    QWidget::resizeEvent(event);
    if(!self)return;
    updateResponsiveLayout();
    if(!self)return;
    const double scale=preference("personalization.uiZoom").toInt(100)/100.0;
    if(m_scroll&&m_section=="plugins"&&m_plugin.isEmpty()&&m_pluginColumns!=(width()>=qRound(1024*scale)?2:1))buildContent();
}

void SettingsPage::buildPlugins(QVBoxLayout *layout) {
    if(m_plugin.isEmpty()){
        QString credentialError;const auto saved=ProviderStore::load(m_dataDir+"/plugin-credentials.json",&credentialError);
        layout->addWidget(text("Enable or disable plugins to extend the agent's capabilities with additional skills."));
        if(!credentialError.isEmpty())layout->addWidget(text("Saved plugin credentials could not be read. Open a plugin to review the storage error."));
        m_pluginColumns=width()>=qRound(1024*preference("personalization.uiZoom").toInt(100)/100.0)?2:1;
        for(const bool enabled:{true,false}){
            QList<const Plugin*> entries;for(const auto& plugin:plugins)if(preference("plugins."+QString(plugin.id)+".enabled",true).toBool()==enabled)entries.append(&plugin);
            if(entries.isEmpty())continue;
            layout->addSpacing(22);layout->addWidget(text(enabled?"Enabled":"Disabled"));layout->addSpacing(12);
            auto* grid=new QGridLayout;grid->setSpacing(12);for(int col=0;col<m_pluginColumns;++col)grid->setColumnStretch(col,1);layout->addLayout(grid);
            int index=0;for(const auto* plugin:entries){
                auto* details=new QPushButton;details->setObjectName("pluginCard");details->setAccessibleName("Configure "+QString(plugin->name));details->setCursor(Qt::PointingHandCursor);details->setMinimumHeight(80);details->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
                auto* row=new QHBoxLayout(details);row->setContentsMargins(14,10,12,10);row->setSpacing(16);
                row->addWidget(settingsMark(plugin->id,28));
                auto* labels=new QVBoxLayout;labels->setSpacing(2);auto* titleRow=new QHBoxLayout;titleRow->setSpacing(7);
                auto* title=text(plugin->name,"settingCardTitle");title->setAttribute(Qt::WA_TransparentForMouseEvents);titleRow->addWidget(title);
                const bool configured=std::any_of(saved.begin(),saved.end(),[&](const ProviderConfig& value){return value.id==plugin->id&&value.protocol=="credential"&&!value.apiKey.isEmpty();});
                const QString savedState=!credentialError.isEmpty()?"Storage error":configured?"Token saved":"Needs token";
                auto* meta=new ElidedLine("1 skill · "+savedState,"pluginCardStatus_"+QString(plugin->id));meta->setProperty("settingsMuted",true);meta->setStyleSheet("font-size:12px;");meta->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);meta->setAttribute(Qt::WA_TransparentForMouseEvents);titleRow->addWidget(meta,1);labels->addLayout(titleRow);
                auto* description=new TwoLineDescription(plugin->description);labels->addWidget(description);row->addLayout(labels,1);
                auto* arrow=new QLabel;arrow->setPixmap(icon("forward",QColor("#777c81")).pixmap(14,14));arrow->setAttribute(Qt::WA_TransparentForMouseEvents);row->addWidget(arrow,0,Qt::AlignTop);
                details->setToolTip(plugin->description);grid->addWidget(details,index/m_pluginColumns,index%m_pluginColumns);++index;
                connect(details,&QPushButton::clicked,this,[this,id=QString(plugin->id)]{m_plugin=id;buildContent();});
            }
        }
        layout->addWidget(text("Enabled plugin skills use your saved credentials for service requests."));
        return;
    }
    const Plugin *selected=nullptr;for(const auto &plugin:plugins)if(m_plugin==plugin.id)selected=&plugin;if(!selected)return;
    auto* header=new QHBoxLayout;header->setSpacing(12);
    auto* back=new QPushButton("←");back->setObjectName("allPlugins");back->setAccessibleName("All plugins");back->setToolTip("All plugins");back->setFixedSize(32,32);
    header->addWidget(back);header->addWidget(text(selected->name,"settingsTitle"),1);layout->addLayout(header);
    connect(back,&QPushButton::clicked,this,[this]{m_plugin.clear();buildContent();});
    layout->addSpacing(16);
    auto* overview=new QWidget;overview->setObjectName("pluginOverview");
    auto* grid=new QGridLayout(overview);grid->setContentsMargins(0,0,0,0);grid->setSpacing(16);grid->setColumnStretch(1,1);
    grid->addWidget(settingsMark(selected->id,40),0,0,Qt::AlignTop);
    auto* summaryWidget=new QWidget;summaryWidget->setObjectName("pluginOverviewSummary");
    auto* summary=new QVBoxLayout(summaryWidget);summary->setContentsMargins(0,0,0,0);summary->setSpacing(4);summary->addWidget(text(selected->description));
    auto* usage=text({});usage->setObjectName("pluginUsageStatus");usage->setProperty("settingsMuted",true);summary->addWidget(usage);grid->addWidget(summaryWidget,0,1);
    auto* enabled=new QCheckBox("Enabled");enabled->setObjectName("pluginEnabled_"+m_plugin);enabled->setChecked(preference("plugins."+m_plugin+".enabled",true).toBool());grid->addWidget(enabled,0,2,Qt::AlignTop);
    layout->addWidget(overview);layout->addSpacing(22);
    auto* credentialCard=new QFrame;credentialCard->setObjectName("pluginCredentialCard");layout->addWidget(credentialCard);
    layout=new QVBoxLayout(credentialCard);layout->setContentsMargins(16,16,16,16);layout->setSpacing(10);
    auto* credentialTitle=text(QString::fromUtf8(selected->credentialName),"settingCardTitle");layout->addWidget(credentialTitle);
    auto* encrypted=text("Stored with Windows account encryption. Saved tokens are never displayed again.");encrypted->setStyleSheet("font-size:12px;");layout->addWidget(encrypted);
    const QString credentialPath=m_dataDir+"/plugin-credentials.json";
    auto repository=std::make_shared<ProviderRepository>(credentialPath);
    QString error;auto records=std::make_shared<QList<ProviderConfig>>(repository->load(&error));
    auto configured=std::make_shared<bool>(std::any_of(records->begin(),records->end(),[this](const ProviderConfig &p){return p.id==m_plugin&&p.protocol=="credential"&&!p.apiKey.isEmpty();}));
    if(!error.isEmpty())layout->addWidget(text("Plugin credentials: "+error));
    auto *token=new QLineEdit;token->setObjectName("pluginCredential");token->setAccessibleName(selected->fieldLabel);token->setEchoMode(QLineEdit::Password);token->setMaxLength(65537);token->setPlaceholderText(*configured?"Token saved — enter a replacement":QString("Enter %1…").arg(QString(selected->fieldLabel).toLower()));layout->addWidget(token);
    layout->addWidget(text(QString::fromUtf8(selected->helpText)));
    const auto plugin=*selected;
    connect(action(layout,"Create or manage token","pluginTokenHelp"),&QPushButton::clicked,this,[this,plugin]{emit openUrlRequested(QUrl(plugin.helpUrl));});
    auto* status=text(*configured?"Token saved — connection has not been checked.":"No token saved.");status->setObjectName("pluginConnectionStatus");status->setProperty("settingsMuted",true);layout->addWidget(status);m_pluginCheckStatus=status;
    auto* save=new QPushButton("Save token");save->setObjectName("savePluginCredential");
    auto* check=new QPushButton("Check saved token");check->setObjectName("checkPluginConnection");m_pluginCheckButton=check;
    auto* remove=new QPushButton("Remove token");remove->setObjectName("removePluginCredential");
    actionRow(layout,{save,check,remove});
    const auto generation=m_pluginViewGeneration;
    const auto refresh=[this,token,save,check,remove,usage,configured,plugin,error]{
        const bool draft=!token->text().isEmpty();save->setEnabled(error.isEmpty()&&!token->text().trimmed().isEmpty());remove->setEnabled(error.isEmpty()&&*configured);
        check->setEnabled(error.isEmpty()&&*configured&&!draft&&!m_pluginCheckRequest);
        usage->setText(preference("plugins."+QString(plugin.id)+".enabled",true).toBool()?(*configured?"Enabled · saved token is available to this plugin's skills.":"Enabled · add a token to authorize service requests."):"Disabled · saved tokens are retained but are not available to agent skills.");
    };
    refresh();
    connect(enabled,&QCheckBox::toggled,this,[this,enabled,id=m_plugin,generation,refresh](bool checked){const QPointer<SettingsPage> self(this);const QPointer<QCheckBox> control(enabled);const bool saved=setPreference("plugins."+id+".enabled",checked);if(!self||!control||generation!=m_pluginViewGeneration)return;if(!saved){QSignalBlocker blocked(control);control->setChecked(!checked);}refresh();});
    connect(token,&QLineEdit::textChanged,this,[this,status,token,check,configured,generation,refresh]{const QPointer<SettingsPage> self(this);invalidatePluginConnectionCheck();if(!self||generation!=m_pluginViewGeneration)return;m_pluginCheckStatus=status;m_pluginCheckButton=check;status->setText(token->text().isEmpty()?(*configured?"Saved token has not been checked.":"No token saved."):"Unsaved changes · save the token before checking it.");refresh();});
    auto saveToken=[this,token,status,check,plugin,repository,records,configured,generation,refresh]{
        const QString value=token->text().trimmed();if(value.isEmpty())return;
        if(value.size()>65536||value.contains(QRegularExpression("[\\s\\x00-\\x1f\\x7f]"))){showStatus("Enter a token without spaces or control characters (maximum 65,536 characters).",true);return;}
        auto next=*records;next.erase(std::remove_if(next.begin(),next.end(),[&](const ProviderConfig& p){return p.id==plugin.id;}),next.end());
        ProviderConfig credential;credential.id=plugin.id;credential.name=plugin.name;credential.baseUrl=plugin.origin;credential.protocol="credential";credential.apiKey=value;next.append(credential);
        const QPointer<SettingsPage> self(this);invalidatePluginConnectionCheck();if(!self||generation!=m_pluginViewGeneration)return;
        QString failure;if(!repository->save(next,&failure)){showStatus("Plugin credentials: "+failure,true);return;}
        *records=next;*configured=true;{QSignalBlocker blocker(token);token->clear();}token->setPlaceholderText("Token saved — enter a replacement");status->setText("Token saved — connection has not been checked.");m_pluginCheckStatus=status;m_pluginCheckButton=check;refresh();showStatus("Token saved for this Windows account. Check saved token to verify access.");emit pluginCredentialsChanged();
    };
    connect(save,&QPushButton::clicked,this,saveToken);connect(token,&QLineEdit::returnPressed,this,saveToken);
    connect(remove,&QPushButton::clicked,this,[this,token,status,check,plugin,repository,records,configured,generation,refresh]{
        auto next=*records;next.erase(std::remove_if(next.begin(),next.end(),[&](const ProviderConfig& p){return p.id==plugin.id;}),next.end());
        const QPointer<SettingsPage> self(this);invalidatePluginConnectionCheck();if(!self||generation!=m_pluginViewGeneration)return;
        QString failure;if(!repository->save(next,&failure)){showStatus("Plugin credentials: "+failure,true);return;}
        *records=next;*configured=false;{QSignalBlocker blocker(token);token->clear();}token->setPlaceholderText(QString("Enter %1…").arg(QString(plugin.fieldLabel).toLower()));status->setText("Token removed. This plugin can no longer make authenticated requests.");m_pluginCheckStatus=status;m_pluginCheckButton=check;refresh();showStatus("Saved plugin token removed.");emit pluginCredentialsChanged();
    });
    if(m_plugin=="posthog"){
        heading(layout,"PostHog region","Choose the region used for this read-only connection check.");auto* region=new QComboBox;region->setObjectName("pluginPosthogRegion");region->addItem("US Cloud","us");region->addItem("EU Cloud","eu");region->setCurrentIndex(qMax(0,region->findData(preference("plugins.posthog.region","us").toString())));layout->addWidget(region);
        connect(region,&QComboBox::currentIndexChanged,this,[this,region,status,check,generation,refresh]{const auto previous=preference("plugins.posthog.region","us").toString();const QPointer<SettingsPage> self(this);const QPointer<QComboBox> control(region);const bool saved=setPreference("plugins.posthog.region",region->currentData().toString());if(!self||!control||generation!=m_pluginViewGeneration)return;if(!saved){QSignalBlocker blocker(control);control->setCurrentIndex(control->findData(previous));return;}invalidatePluginConnectionCheck();if(!self||generation!=m_pluginViewGeneration)return;m_pluginCheckStatus=status;m_pluginCheckButton=check;status->setText("Region changed — check the saved token again.");refresh();});
    }
    connect(check,&QPushButton::clicked,this,[this,plugin,credentialPath,status,check,generation]{
        const auto revision=pluginCredentialRevision(credentialPath);if(revision.isEmpty()){showStatus("Cannot read saved plugin credentials for a connection check.",true);return;}
        const QPointer<SettingsPage> self(this);invalidatePluginConnectionCheck();if(!self||generation!=m_pluginViewGeneration)return;
        m_pluginCheckRequest=++m_nextPluginCheck;m_pluginCheckId=plugin.id;m_pluginCheckRegion=preference("plugins.posthog.region","us").toString();m_pluginCheckRevision=revision;m_pluginCheckStatus=status;m_pluginCheckButton=check;check->setEnabled(false);status->setText("Checking saved token…");emit pluginConnectionCheckRequested(m_pluginCheckRequest,plugin.id);
    });
}

void SettingsPage::invalidatePluginConnectionCheck(){
    const auto request=m_pluginCheckRequest;m_pluginCheckRequest=0;m_pluginCheckId.clear();m_pluginCheckRevision.clear();m_pluginCheckStatus.clear();m_pluginCheckButton.clear();
    if(request)emit pluginConnectionCheckCancelled(request);
}

void SettingsPage::completePluginConnectionCheck(quint64 requestId,const QString& pluginId,bool success,const QString& safeMessage){
    if(!requestId||requestId!=m_pluginCheckRequest||pluginId!=m_pluginCheckId||m_section!="plugins"||m_plugin!=pluginId)return;
    m_pluginCheckRequest=0;
    const bool current=pluginCredentialRevision(m_dataDir+"/plugin-credentials.json")==m_pluginCheckRevision&&m_pluginCheckRegion==preference("plugins.posthog.region","us").toString();
    if(m_pluginCheckStatus)m_pluginCheckStatus->setText(current?(success?"Connection verified. ":"Connection check failed. ")+safeMessage.left(2048):"Saved credentials or region changed. Reopen this plugin before checking again.");
    if(m_pluginCheckButton)m_pluginCheckButton->setEnabled(current);
}

void SettingsPage::buildArchived(QVBoxLayout *layout) {
    layout->setSpacing(4);
    layout->addWidget(text("Archived agents are stopped and hidden from your chat list."));
    layout->addSpacing(20);
    auto *search=new QLineEdit;search->setObjectName("archivedSearch");search->setPlaceholderText("Search archived chats");search->setAccessibleName("Search archived chats");search->addAction(icon("search",QColor("#777c81")),QLineEdit::LeadingPosition);search->setText(m_archivedQuery);search->setMinimumHeight(32);layout->addWidget(search);
    layout->addSpacing(20);
    auto* frame=new QFrame;frame->setObjectName("archivedCard");auto* rows=new QVBoxLayout(frame);rows->setContentsMargins(1,1,1,1);rows->setSpacing(0);layout->addWidget(frame);
    auto *list=new QListWidget;list->setObjectName("archivedChats");list->setSelectionMode(QAbstractItemView::NoSelection);list->setFrameShape(QFrame::NoFrame);list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);rows->addWidget(list);
    QString error;auto conversations=SessionStore(m_dataDir).load(&error);
    std::stable_sort(conversations.begin(),conversations.end(),[](const Conversation& left,const Conversation& right){
        const auto a=left.archivedAt?left.archivedAt:left.updated,b=right.archivedAt?right.archivedAt:right.updated;
        return a==b?left.updated>right.updated:a>b;
    });
    auto* footer=new QWidget;auto* footerLayout=new QHBoxLayout(footer);footerLayout->setContentsMargins(12,12,12,12);footerLayout->addStretch();
    auto* more=new QPushButton("Show more");more->setObjectName("loadMoreArchivedChats");more->setMinimumHeight(32);footerLayout->addWidget(more);footerLayout->addStretch();rows->addWidget(footer);
    auto *empty=text({});empty->setObjectName("archivedEmpty");empty->setAlignment(Qt::AlignCenter);empty->setMinimumHeight(160);rows->addWidget(empty);
    const auto populate=[this,list,more,footer,empty,conversations,error]{
        const QSignalBlocker blocker(list);list->clear();int matches=0;int selected=-1;
        for(const auto &conversation:conversations){
            if(!conversation.archived||(!m_archivedQuery.trimmed().isEmpty()&&!conversation.title.contains(m_archivedQuery.trimmed(),Qt::CaseInsensitive)))continue;
            if(++matches>m_archivedLimit)continue;
            const auto title=conversation.title.isEmpty()?QString("Untitled chat"):conversation.title;
            auto *item=new QListWidgetItem(list);item->setData(Qt::UserRole,conversation.id);item->setData(Qt::AccessibleTextRole,title);item->setSizeHint(QSize(0,64));
            auto* row=new QWidget;auto* horizontal=new QHBoxLayout(row);horizontal->setContentsMargins(16,10,16,10);horizontal->setSpacing(16);
            auto* labels=new QVBoxLayout;labels->setSpacing(2);labels->addWidget(new ElidedLine(title,"archivedTitle"));
            auto* meta=new QHBoxLayout;meta->setSpacing(6);const auto date=QDateTime::fromMSecsSinceEpoch(conversation.archivedAt?conversation.archivedAt:conversation.updated);
            auto* dateLabel=text(QLocale().toString(date.date(),"MMM d, yyyy")+", "+QLocale().toString(date.time(),QLocale::ShortFormat),"archivedMeta");dateLabel->setWordWrap(false);meta->addWidget(dateLabel);
            if(!conversation.workspace.isEmpty()){
                meta->addWidget(text("·","archivedMeta"));auto* folder=new QLabel;folder->setPixmap(icon("folder",QColor("#777c81")).pixmap(12,12));meta->addWidget(folder);
                const auto name=QFileInfo(QDir::cleanPath(conversation.workspace)).fileName();auto* workspace=new ElidedLine(name.isEmpty()?conversation.workspace:name,"archivedMeta");workspace->setToolTip(conversation.workspace);meta->addWidget(workspace,1);
            }else meta->addStretch();
            labels->addLayout(meta);horizontal->addLayout(labels,1);
            auto* remove=new QPushButton(icon("trash",QColor("#777c81")),QString());remove->setObjectName("deleteArchivedChat_"+conversation.id);remove->setProperty("conversationId",conversation.id);remove->setProperty("archiveTrash",true);remove->setAccessibleName("Permanently delete "+title);remove->setToolTip("Permanently delete chat");remove->setFixedSize(24,24);remove->setIconSize(QSize(14,14));horizontal->addWidget(remove);
            auto* restore=new QPushButton("Unarchive");restore->setObjectName("restoreArchivedChat_"+conversation.id);restore->setProperty("conversationId",conversation.id);restore->setProperty("archiveRestore",true);restore->setAccessibleName("Unarchive "+title);restore->setFixedHeight(26);horizontal->addWidget(restore);
            connect(restore,&QPushButton::clicked,this,[this,id=conversation.id]{emit restoreConversationRequested(id);});
            connect(remove,&QPushButton::clicked,this,[this,id=conversation.id]{emit deleteConversationRequested(id);});
            list->setItemWidget(item,row);
            if(conversation.id==m_archivedSelection)selected=list->count()-1;
        }
        footer->setVisible(matches>m_archivedLimit);more->setVisible(matches>m_archivedLimit);list->setVisible(matches>0);list->setFixedHeight(list->count()*64);empty->setVisible(matches==0);
        empty->setText(!error.isEmpty()?QString("Couldn't load archived chats. Please try again."):m_archivedQuery.trimmed().isEmpty()?QString("Chats you archive will appear here."):QString("No matching archived chats."));
        if(list->count()){list->setCurrentRow(selected<0?0:selected);m_archivedSelection=list->currentItem()->data(Qt::UserRole).toString();}
    };
    auto* debounce=new QTimer(list);debounce->setSingleShot(true);debounce->setInterval(250);
    connect(search,&QLineEdit::textChanged,list,[this,debounce](const QString& query){m_archivedQuery=query;m_archivedLimit=25;debounce->start();});
    connect(debounce,&QTimer::timeout,list,populate);
    connect(more,&QPushButton::clicked,list,[this,populate]{m_archivedLimit+=25;populate();});
    connect(list,&QListWidget::currentItemChanged,this,[this](QListWidgetItem* item){if(item)m_archivedSelection=item->data(Qt::UserRole).toString();});
    populate();
}
void SettingsPage::reportConversationError(const QString& error){showStatus(error,true);}

void SettingsPage::buildPersonalization(QVBoxLayout *layout) {
    heading(layout,"UI size","Adjust the size of the application interface.");
    auto *zoom=new QSlider(Qt::Horizontal);zoom->setObjectName("settingsUiZoom");zoom->setRange(70,130);zoom->setValue(preference("personalization.uiZoom").toInt(100));zoom->setMaximumWidth(300);
    auto *value=text(QString::number(zoom->value())+"%");layout->addWidget(zoom);layout->addWidget(value);
    connect(zoom,&QSlider::valueChanged,value,[value](int n){value->setText(QString::number(n)+"%");});
    connect(zoom,&QSlider::valueChanged,this,[this,zoom](int value){
        if(!zoom->isSliderDown())setPreference("personalization.uiZoom",value);
    });
    connect(zoom,&QSlider::sliderReleased,this,[this,zoom]{setPreference("personalization.uiZoom",zoom->value());});
    heading(layout,"Appearance");
    auto *appearance=new QComboBox;appearance->setObjectName("settingsAppearance");appearance->addItem("System","system");appearance->addItem("Light","light");appearance->addItem("Dark","dark");appearance->setCurrentIndex(qMax(0,appearance->findData(preference("personalization.appearance").toString())));layout->addWidget(appearance);
    connect(appearance,&QComboBox::currentIndexChanged,this,[this,appearance]{setPreference("personalization.appearance",appearance->currentData().toString());});
    heading(layout,"Color scheme");auto *theme=new QComboBox;theme->setObjectName("settingsTheme");for(const auto &name:{"Blue","Violet","Green","Amber","Neutral"})theme->addItem(name,QString(name).toLower());theme->setCurrentIndex(qMax(0,theme->findData(preference("personalization.theme").toString())));layout->addWidget(theme);
    connect(theme,&QComboBox::currentIndexChanged,this,[this,theme]{setPreference("personalization.theme",theme->currentData().toString());});
    heading(layout,"Language");auto *language=new QComboBox;language->setObjectName("settingsLanguage");language->addItem("English","en");language->setCurrentIndex(0);layout->addWidget(language);
    layout->addWidget(text("Additional interface translations are not available in this build."));
}

bool SettingsPage::addSearchEngine(const QString &name,const QString &keyword,const QString &url) {
    const auto title=name.trimmed(),word=keyword.trimmed(),address=url.trimmed();
    if(title.isEmpty()||word.isEmpty()||!address.contains("%s")||!validWebUrl(QString(address).replace("%s","query"))){showStatus("Enter a name, keyword and HTTP(S) URL containing %s for the search query.",true);return false;}
    auto engines=preference("browsing.searchEngines").toArray();
    for(const auto &engine:engines)if(engine.toObject()["keyword"].toString().compare(word,Qt::CaseInsensitive)==0){showStatus("That search keyword already exists.",true);return false;}
    engines.append(QJsonObject{{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"name",title},{"keyword",word},{"url",address}});
    return setPreference("browsing.searchEngines",engines);
}

void SettingsPage::buildBrowsing(QVBoxLayout *layout) {
    heading(layout,"General");heading(layout,"Default Search Engine");
    auto *group=new QButtonGroup(layout->parentWidget());
    const auto engines=preference("browsing.searchEngines").toArray();
    for(int i=0;i<engines.size();++i){const auto engine=engines[i].toObject();auto *row=new QHBoxLayout;
        auto *radio=new QRadioButton(engine["name"].toString()+"\n"+engine["keyword"].toString());radio->setObjectName("searchEngine_"+engine["id"].toString());radio->setChecked(preference("browsing.defaultEngineId").toString()==engine["id"].toString());group->addButton(radio);row->addWidget(radio,1);
        connect(radio,&QRadioButton::clicked,this,[this,id=engine["id"].toString()]{setPreference("browsing.defaultEngineId",id);});
        if(i>=3){auto *remove=new QPushButton("Remove");row->addWidget(remove);connect(remove,&QPushButton::clicked,this,[this,id=engine["id"].toString()]{auto records=preference("browsing.searchEngines").toArray();for(int n=records.size()-1;n>=0;--n)if(records[n].toObject()["id"].toString()==id)records.removeAt(n);auto prefs=putPath(m_preferences,{"browsing","searchEngines"},records);if(preference("browsing.defaultEngineId").toString()==id)prefs=putPath(prefs,{"browsing","defaultEngineId"},"google");if(savePreferences(prefs))buildContent();});}
        layout->addLayout(row);
    }
    connect(action(layout,"+ Add Search Engine","addSearchEngine"),&QPushButton::clicked,this,[this]{
        QDialog dialog(this);dialog.setWindowTitle("Add search engine");auto *form=new QFormLayout(&dialog);auto *name=new QLineEdit;auto *keyword=new QLineEdit;auto *url=new QLineEdit;url->setPlaceholderText("https://example.com/search?q=%s");form->addRow("Name",name);form->addRow("Keyword",keyword);form->addRow("URL",url);
        auto *error=text({});form->addRow(error);auto *buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);form->addRow(buttons);
        connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);connect(buttons,&QDialogButtonBox::accepted,&dialog,[&,this]{if(addSearchEngine(name->text(),keyword->text(),url->text()))dialog.accept();else error->setText(m_status->text());});
        if(dialog.exec()==QDialog::Accepted)buildContent();
    });
    const auto pageChoice=[this,layout](const QString &key,const QString &title,const QString &description){heading(layout,title,description);auto *buttons=new QButtonGroup(layout->parentWidget());auto *home=new QRadioButton("ADE Home");auto *custom=new QRadioButton("Custom URL");buttons->addButton(home);buttons->addButton(custom);layout->addWidget(home);layout->addWidget(custom);auto *url=new QLineEdit(preference(key).toString());url->setObjectName(key);url->setPlaceholderText("https://example.com");layout->addWidget(url);home->setChecked(url->text().isEmpty());custom->setChecked(!url->text().isEmpty());url->setEnabled(custom->isChecked());
        connect(home,&QRadioButton::clicked,this,[this,key,url]{url->setEnabled(false);setPreference(key,QString());});
        connect(custom,&QRadioButton::clicked,this,[url]{url->setEnabled(true);url->setFocus();});
        connect(url,&QLineEdit::editingFinished,this,[this,key,url,custom]{if(custom->isChecked())setPreference(key,url->text().trimmed());});};
    pageChoice("browsing.newTabUrl","New Tab Page","Choose what page opens when you create a new tab.");pageChoice("browsing.startupUrl","On Browser Start","Choose what page opens when ADE starts.");
    heading(layout,"Privacy","Manage website permission defaults.");heading(layout,"Permission Defaults","Choose how websites can access these features. Site exceptions take precedence.");
    auto *form=new QFormLayout;layout->addLayout(form);
    const QList<QPair<QString,QString>> permissions={{"camera","Camera & Microphone"},{"location","Location"},{"notifications","Notifications"},{"clipboard","Clipboard"},{"fullscreen","Fullscreen"},{"downloads","Automatic Downloads"}};
    for(const auto &pair:permissions){auto *choice=new QComboBox;choice->setObjectName("permission_"+pair.first);choice->addItem("Ask","ask");choice->addItem("Block","block");choice->addItem("Allow","allow");choice->setCurrentIndex(qMax(0,choice->findData(preference("browsing.permissions."+pair.first,pair.first=="fullscreen"?"allow":"ask").toString())));form->addRow(pair.second,choice);connect(choice,&QComboBox::currentIndexChanged,this,[this,key=pair.first,choice]{setPreference("browsing.permissions."+key,choice->currentData().toString());});}
    heading(layout,"Site exceptions","Rules apply to one origin, including its port.");
    const auto exceptions=preference("browsing.permissionExceptions").toObject();
    for(auto type=exceptions.begin();type!=exceptions.end();++type){const auto sites=type.value().toObject();for(auto site=sites.begin();site!=sites.end();++site){
        const QJsonValue setting=site.value().isObject()?site.value().toObject().value("setting"):QJsonValue(site.value());
        const QString label=setting.isString()?setting.toString():QStringList{"ask","allow","block"}.value(setting.toInt());
        QHBoxLayout* row;card(layout,site.key(),type.key()+" · "+label,&row);auto* remove=new QPushButton("Remove");row->addWidget(remove);
        connect(remove,&QPushButton::clicked,this,[this,type=type.key(),origin=site.key()]{if(setSitePermission(type,QUrl(origin),"default"))buildContent();});
    }}
    connect(action(layout,"+ Add site exception","addSiteException"),&QPushButton::clicked,this,[this,permissions]{
        QDialog dialog(this);dialog.setWindowTitle("Site permission");auto* form=new QFormLayout(&dialog);auto* origin=new QLineEdit;origin->setPlaceholderText("https://example.com");
        auto* type=new QComboBox;for(const auto& pair:permissions)type->addItem(pair.second,pair.first);auto* setting=new QComboBox;setting->addItem("Ask","ask");setting->addItem("Allow","allow");setting->addItem("Block","block");
        form->addRow("Site origin",origin);form->addRow("Permission",type);form->addRow("Setting",setting);auto* error=text({});form->addRow(error);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);form->addRow(buttons);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
        connect(buttons,&QDialogButtonBox::accepted,&dialog,[&,this]{if(setSitePermission(type->currentData().toString(),QUrl(origin->text().trimmed(),QUrl::StrictMode),setting->currentData().toString()))dialog.accept();else error->setText(m_status->text());});
        if(dialog.exec()==QDialog::Accepted)buildContent();
    });
}

bool SettingsPage::setSitePermission(const QString& permission,const QUrl& supplied,const QString& setting){
    QUrl origin=supplied;
    if(!validWebUrl(origin.toString())||(!origin.path().isEmpty()&&origin.path()!="/")||origin.hasQuery()||origin.hasFragment()){
        showStatus("Enter an HTTP or HTTPS origin without a path, credentials, query or fragment.",true);return false;
    }
    origin.setPath({});if((origin.scheme()=="https"&&origin.port()==443)||(origin.scheme()=="http"&&origin.port()==80))origin.setPort(-1);
    auto exceptions=preference("browsing.permissionExceptions").toObject();auto sites=exceptions[permission].toObject();
    if(setting=="default")sites.remove(origin.toString());else sites[origin.toString()]=setting;
    if(sites.isEmpty())exceptions.remove(permission);else exceptions[permission]=sites;
    return setPreference("browsing.permissionExceptions",exceptions);
}

void SettingsPage::buildClearData(QVBoxLayout *layout) {
    heading(layout,"Clear browsing data","Remove selected data for all time. Downloaded files and conversations are preserved.");
    QList<QPair<QString,QCheckBox*>> choices;
    for(const auto& entry:QList<QPair<QString,QString>>{{"history","Browsing history"},{"downloads","Download history"},{"cookies","Cookies"},{"cache","Cached images and files"},{"storage","Site storage"},{"permissionExceptions","Site permission exceptions"},{"pluginCredentials","Saved plugin credentials"}}){
        auto* option=new QCheckBox(entry.second);option->setObjectName("clear_"+entry.first);layout->addWidget(option);choices.append({entry.first,option});
    }
    auto *button=action(layout,"Clear selected data","clearSelectedData");
    button->setEnabled(!m_clearingData);
    connect(button,&QPushButton::clicked,this,[this,choices,button]{
        QJsonObject options;for(const auto& choice:choices)if(choice.second->isChecked())options[choice.first]=true;
        if(options.isEmpty()){showStatus("Choose data to clear.");return;}
        if(QMessageBox::question(this,"Clear selected data","Remove the selected ADE data? This cannot be undone.")!=QMessageBox::Yes)return;
        m_clearingData=true;button->setEnabled(false);showStatus("Clearing selected data…");emit clearBrowsingDataRequested(options);
    });
}

void SettingsPage::completeBrowsingDataClear(bool success,const QString& message){
    m_clearingData=false;if(auto* button=findChild<QPushButton*>("clearSelectedData"))button->setEnabled(true);
    showStatus(message.isEmpty()?(success?"Selected data cleared.":"Data could not be cleared."):message,!success);
}

void SettingsPage::buildHistory(QVBoxLayout *layout) {
    auto *search=new QLineEdit;search->setObjectName("historySearch");search->setPlaceholderText("Search browsing history");layout->addWidget(search);
    auto *list=new QListWidget;list->setObjectName("browsingHistory");list->setMinimumHeight(280);layout->addWidget(list);
    QFile file(m_dataDir+"/browsing-history.json");
    if(file.open(QIODevice::ReadOnly)&&file.size()<4*1024*1024){const auto document=QJsonDocument::fromJson(file.readAll());const auto entries=document.isArray()?document.array():document.object()["entries"].toArray();for(const auto &value:entries){const auto entry=value.toObject();const auto url=entry["url"].toString();if(!validWebUrl(url))continue;auto *item=new QListWidgetItem(entry["title"].toString(url)+"\n"+url,list);item->setData(Qt::UserRole,url);}}
    connect(search,&QLineEdit::textChanged,list,[list](const QString &query){for(int i=0;i<list->count();++i)list->item(i)->setHidden(!list->item(i)->text().contains(query,Qt::CaseInsensitive));});
    connect(list,&QListWidget::itemDoubleClicked,this,[this](QListWidgetItem *item){emit openUrlRequested(QUrl(item->data(Qt::UserRole).toString()));});
    if(list->count()==0)layout->addWidget(text("No recorded browsing history."));
}

void SettingsPage::buildAccount(QVBoxLayout *layout) {
    heading(layout,"Use your own models","Connect API keys and compatible endpoints to run agents with your own providers.");
    connect(action(layout,"Configure Models & Providers","accountProviders"),&QPushButton::clicked,this,[this]{selectSection("models-providers");});
    heading(layout,"Coding subscriptions","Use installed Codex, Claude Code, or OpenCode agents through Models & Providers. Sign in using the selected agent’s own supported login flow.");
    card(layout,"Google · GitHub · Email","ADE has no account authentication service configured. Your existing Stagewise account and chats are unchanged.");
}

void SettingsPage::buildAbout(QVBoxLayout *layout) {
    heading(layout,"ADE","Native C++ and Qt desktop application · development build 0.1.0");
    layout->addWidget(text("Stagewise source baseline: 1.31.1\nCommit: 17a0123a0e896885465df36169457199d5225eb5"));
    layout->addWidget(text("This build is an incomplete native port. Feature and verification gaps are tracked in the project audit."));
    heading(layout,"Open source","Derived from Stagewise. AGPL-3.0 terms and third-party notices apply.");
    connect(action(layout,"View upstream source","aboutSource"),&QPushButton::clicked,this,[]{QDesktopServices::openUrl(QUrl("https://github.com/stagewise-io/stagewise"));});
    connect(action(layout,"View license","aboutLicense"),&QPushButton::clicked,this,[]{QDesktopServices::openUrl(QUrl("https://github.com/stagewise-io/stagewise/blob/17a0123a0e896885465df36169457199d5225eb5/LICENSE"));});
    heading(layout,"App Data",m_dataDir);
    connect(action(layout,"Open data folder","openAppData"),&QPushButton::clicked,this,[this]{QDir().mkpath(m_dataDir);QDesktopServices::openUrl(QUrl::fromLocalFile(m_dataDir));});
    heading(layout,"Updates","Automatic updates and signed release distribution are not configured in this build.");
}

} // namespace ade
