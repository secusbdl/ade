#include "MiniAppPage.h"
#include "browser/browserwidget.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

namespace ade {
namespace {
QByteArray encoded(const QJsonValue& value){auto bytes=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);return bytes.mid(1,bytes.size()-2);}
constexpr qsizetype MaxMessageBytes=128*1024;
}
MiniAppPage::MiniAppPage(const QUrl& base,const QString& directory,const QString& profile,QWidget* parent):QWidget(parent) {
    setObjectName(QStringLiteral("miniAppPage"));auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);
    if(!m_resources.configure(base,directory,&m_error)){layout->addWidget(new QLabel(m_error,this));return;}
    const auto entry=m_resources.read(QUrl(base.toString()+QStringLiteral("/index.html")));
    if(entry.status!=200){m_error=tr("The app index.html is missing or unavailable.");layout->addWidget(new QLabel(m_error,this));return;}
    m_browser=new BrowserWidget(this);layout->addWidget(m_browser);m_browser->setProfileDirectory(profile);
    m_browser->setPermissionDefaults({{"media","block"},{"geolocation","block"},{"notifications","block"},{"clipboard-read","block"},{"fullscreen","block"},{"downloads","block"},{"midi","block"},{"storage-access","block"},{"local-fonts","block"},{"window-management","block"},{"sensors","block"},{"file-read-write","block"}});
    m_browser->setLocalContentHandler([this](const QUrl& url,const QByteArray& method){const auto r=resource(url,method);return BrowserResourceResponse{r.status,r.body,r.mimeType,r.headers};},[this](const QUrl& url){return url==m_previewUrl;});
    connect(m_browser,&BrowserWidget::webMessageReceived,this,&MiniAppPage::receive);
    connect(m_browser,&BrowserWidget::errorOccurred,this,&MiniAppPage::failed);
    connect(m_browser,&BrowserWidget::downloadRequested,this,[this](const QString& id){m_browser->cancelDownload(id);});
    connect(m_browser,&BrowserWidget::permissionRequested,this,[this](const QString& id){m_browser->resolvePermission(id,false);});
    reloadApp();
}
MiniAppPage::~MiniAppPage(){delete m_browser;m_browser=nullptr;}
QUrl MiniAppPage::appUrl() const{return QUrl(m_resources.baseUrl().toString()+QStringLiteral("/index.html"));}
void MiniAppPage::reloadApp(){
    if(!m_browser)return;
    m_nonce=QUuid::createUuid().toString(QUuid::WithoutBraces);m_previewUrl=QUrl(QStringLiteral("ade-app://preview/")+m_nonce+QStringLiteral("/index.html"));
    m_queue.clear();m_queuedBytes=0;m_ready=false;
    const QPointer<MiniAppPage> self(this);emit readyChanged(false);if(!self)return;
    m_browser->navigate(m_previewUrl);
    const QString nonce=m_nonce;QTimer::singleShot(20000,this,[this,nonce]{if(nonce==m_nonce&&!m_ready)emit failed(tr("The app preview did not become ready within 20 seconds."));});
}
void MiniAppPage::setTheme(const QJsonObject& variables){m_theme=variables;if(m_ready)postMessage(QJsonObject{{"type","__stagewise_theme"},{"variables",variables}});}
bool MiniAppPage::postMessage(const QJsonValue& data,QString* error){
    const auto fail=[&](const QString& text){if(error)*error=text;return false;};
    if(!m_browser)return fail(m_error);
    const qsizetype size=encoded(data).size();
    if(size>MaxMessageBytes)return fail(tr("App message exceeds 128 KiB."));
    if(m_queue.size()>=64||m_queuedBytes+size>1024*1024)return fail(tr("App message queue is full."));
    m_queue.append(data);m_queuedBytes+=size;flush();if(error)error->clear();return true;
}
void MiniAppPage::flush(){
    if(!m_ready||!m_browser)return;
    const QPointer<MiniAppPage> self(this);
    while(!m_queue.isEmpty()){
        const auto message=m_queue.first();
        if(!m_browser->postWebMessage(QJsonObject{{"channel","ade-mini-app"},{"nonce",m_nonce},{"kind","deliver"},{"data",message}})){emit failed(tr("Cannot deliver the app message."));return;}
        if(!self)return;
        m_queuedBytes-=encoded(message).size();m_queue.removeFirst();
    }
}
void MiniAppPage::receive(const QUrl& source,const QJsonValue& value){
    if(source!=m_previewUrl||!value.isObject())return;
    const auto object=value.toObject();if(object["channel"]!="ade-mini-app"||object["nonce"]!=m_nonce)return;
    if(object["kind"]=="ready"){
        if(m_ready)return;
        m_ready=true;const QString nonce=m_nonce;const QPointer<MiniAppPage> self(this);emit readyChanged(true);if(!self||nonce!=m_nonce)return;
        if(!m_theme.isEmpty())postMessage(QJsonObject{{"type","__stagewise_theme"},{"variables",m_theme}});
        if(self)flush();
    }else if(object["kind"]=="message"&&m_ready&&encoded(object["data"]).size()<=MaxMessageBytes)emit messageReceived(object["data"]);
}
AppResourceResponse MiniAppPage::resource(const QUrl& url,const QByteArray& method) const{
    if(url==m_previewUrl){
        if(method!="GET"&&method!="HEAD")return {405,"Read-only preview","text/plain",{}};
        const auto body=previewDocument();const QByteArray origin="app://"+m_resources.baseUrl().host().toUtf8();
        const QByteArray policy="default-src 'none'; frame-src "+origin+"; script-src 'nonce-"+m_nonce.toUtf8()+"'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'";
        return {200,method=="HEAD"?QByteArray():body,"text/html; charset=utf-8",{{"Cache-Control","no-store"},{"Content-Security-Policy",policy},{"X-Content-Type-Options","nosniff"},{"Content-Length",QByteArray::number(body.size())}}};
    }
    auto result=m_resources.read(url,method);
    if(result.status==200){
        result.headers.insert("Content-Security-Policy","default-src 'none'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; font-src 'self' data:; connect-src 'self'; frame-src 'none'; base-uri 'none'; form-action 'none'");
    }
    return result;
}
QByteArray MiniAppPage::previewDocument() const{
    const QByteArray url=encoded(appUrl().toString()),nonce=encoded(m_nonce),origin=encoded(QStringLiteral("app://")+m_resources.baseUrl().host());
    return QByteArray("<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><style>html,body,iframe{margin:0;width:100%;height:100%;border:0;overflow:hidden}iframe{display:block}</style></head><body><iframe id=\"app\" sandbox=\"allow-scripts allow-same-origin\" title=\"App preview\"></iframe><script nonce=\"")+m_nonce.toUtf8()+"\">(()=>{const nonce="+nonce+",origin="+origin+",frame=document.getElementById('app');const send=(kind,data)=>chrome.webview.postMessage({channel:'ade-mini-app',nonce,kind,data});window.addEventListener('message',event=>{if(event.source!==frame.contentWindow||event.origin!==origin)return;send('message',event.data);});chrome.webview.addEventListener('message',event=>{const m=event.data;if(!m||m.channel!=='ade-mini-app'||m.nonce!==nonce||m.kind!=='deliver')return;frame.contentWindow.postMessage(m.data,origin);});frame.addEventListener('load',()=>send('ready'));frame.src="+url+";})();</script></body></html>";
}
}
