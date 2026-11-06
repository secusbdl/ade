#include "SandboxHost.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMimeDatabase>
#include <QMetaMethod>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace ade {
namespace {
constexpr qsizetype MaxFrameBytes = 32 * 1024 * 1024;
QString json(const QJsonObject& object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); }
QString failure(const QString& reason) { return json({{"message", "Sandbox JavaScript failed"}, {"error", reason}}); }
bool linkedAncestor(const QString& directory) {
    QString cursor=QDir(directory).absolutePath();
    while(true){const QFileInfo info(cursor);if(info.isSymLink()||info.isJunction())return true;const QString parent=info.absolutePath();if(parent==cursor)return false;cursor=parent;}
}
}

SandboxHost::SandboxHost(QObject* parent) : QObject(parent) {
    m_executable = QCoreApplication::applicationDirPath() + QStringLiteral("/ADE-sandbox-worker.exe");
    m_artifactDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/sandbox-attachments");
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
    m_process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    connect(&m_process, &QProcess::started, this, [this] {
        const auto messages = std::exchange(m_outgoing, {});
        const QPointer<SandboxHost> self(this);
        for (const auto& message : messages) { send(message); if (!self) return; }
    });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &SandboxHost::readOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        const QString text = QString::fromUtf8(m_process.readAllStandardError()).left(16384);
        if (!text.isEmpty()) emit consoleMessage(QString(), QStringLiteral("worker"), text);
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!m_destroying && error == QProcess::FailedToStart) failAll(QStringLiteral("Cannot start native sandbox worker: ") + m_process.errorString());
    });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus) {
        const QPointer<SandboxHost> self(this);
        readOutput();
        if (!self) return;
        m_buffer.clear();
        m_outgoing.clear();
        if (!m_destroying) failAll(QStringLiteral("Native sandbox worker exited (%1); its persistent contexts were reset.").arg(code));
    });
}

SandboxHost::~SandboxHost() {
    m_destroying = true;
    disconnect(&m_process, nullptr, this, nullptr);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.closeWriteChannel();
        if (!m_process.waitForFinished(250)) { m_process.kill(); m_process.waitForFinished(1000); }
    }
}

void SandboxHost::setAgentId(const QString& id) { if (!id.isEmpty() && id.size() <= 256) m_agentId = id; }
void SandboxHost::setWorkspace(const QString& path) { m_workspace = path.isEmpty() ? QString() : QDir(path).absolutePath(); }
void SandboxHost::setAppsDirectory(const QString& path) {m_appsDirectory=path.isEmpty()?QString():QDir(path).absolutePath();}
void SandboxHost::setArtifactDirectory(const QString& path) {m_artifactDirectory=path.isEmpty()?QString():QDir(path).absolutePath();}
void SandboxHost::setWorkerExecutable(const QString& path) { if (m_process.state() == QProcess::NotRunning) m_executable = QFileInfo(path).absoluteFilePath(); }
void SandboxHost::setExecutionLimits(int synchronousMs, int inactivityMs, int hardMs, quint64 memoryBytes) {
    m_synchronousMs = qBound(50, synchronousMs, 30000);
    m_inactivityMs = qBound(50, inactivityMs, 45000);
    m_hardMs = qBound(100, hardMs, 180000);
    m_memoryBytes = qBound<quint64>(8ull * 1024 * 1024, memoryBytes, 512ull * 1024 * 1024);
}
bool SandboxHost::isRunning() const { return m_process.state() != QProcess::NotRunning; }
bool SandboxHost::handles(const QString& name) const { return name == QStringLiteral("executeSandboxJs"); }

QJsonArray SandboxHost::definitions() const {
    const QJsonObject properties{
        {"explanation", QJsonObject{{"type", "string"}, {"description", "Concise human-readable description, at most five words."}}},
        {"script", QJsonObject{{"type", "string"}, {"description", "JavaScript body of an async function. Use return for a final value."}}}
    };
    const QJsonObject parameters{{"type", "object"}, {"properties", properties}, {"required", QJsonArray{"explanation", "script"}}, {"additionalProperties", false}};
    const QString description = QStringLiteral(
        "Execute JavaScript in a persistent per-agent native QuickJS worker, outside browser pages. Supports async/await, persistent globalThis, timers, API.output, API.sendCDP, API.createAttachment, fsPromises.readFile/writeFile (UTF-8 strings or exact binary Buffer bytes), mkdir and readdir, basic Buffer, buffered HTTP(S) fetch with Headers/Request/Response and AbortController, and API.getCredential for host-authorized opaque credential handles. API.openApp(appId,{pluginId?,title?,target:'tab',setActive?}) opens a host-authorized app and returns {tabId}; API.closeApp() closes the current app; API.sendMessage(appId,data,{pluginId?}) posts JSON to it. API.onMessage(appId,callback,{pluginId?}) and API.onCDPEvent(tabId,event,callback) return unsubscribe functions. These listeners persist across executions and can update globalThis and send app messages while idle. Listener queues are bounded; idle callbacks have a 1-second synchronous limit and idle broker requests a 45-second deadline. Await API calls made by the script body: its outstanding broker calls are canceled when it returns, while independent listener subscriptions and idle requests persist. Fetch supports text or binary bodies up to 1 MiB and buffered responses up to 2 MiB. Page DOM is available through API.sendCDP Runtime.evaluate. Returns API.output messages followed by the return value; API.output requires an active execution. Console is diagnostic. Node builtins beyond the documented subset, importModule, streaming, Blob, FormData, URL helpers, browser cookies and full Node/Web API parity are not implemented. Cancellation/time or memory failure resets this agent's context, listeners, apps and credential handles. The host-owned apps/ mount is available without a workspace for apps/<id>/index.html and local assets. Binary file transfers are bounded to 4 MiB; UTF-8 text reads to 64 Ki characters. The read-only att/ mount provides this chat's original imported or API-generated attachment blobs without a workspace; fs.readFile and fs.readdir can read/list it, while fs writes and mkdir are denied. History snapshots are separate and are not exposed as attachment files. Other filesystem APIs, streams and additional mounts are not implemented. No unrestricted OS/process APIs.");
    return {QJsonObject{{"type", "function"}, {"function", QJsonObject{{"name", "executeSandboxJs"}, {"description", description}, {"parameters", parameters}}}}};
}

void SandboxHost::ensureProcess() {
    if (m_process.state() != QProcess::NotRunning) return;
    m_buffer.clear();
    // Do not give the worker the parent's provider-key/proxy/token environment.
    QProcessEnvironment environment;
    const auto inherited = QProcessEnvironment::systemEnvironment();
    for (const QString& name : {QStringLiteral("SystemRoot"), QStringLiteral("WINDIR"), QStringLiteral("TEMP"), QStringLiteral("TMP")})
        if (inherited.contains(name)) environment.insert(name, inherited.value(name));
    environment.insert(QStringLiteral("PATH"), inherited.value(QStringLiteral("SystemRoot"), QStringLiteral("C:/Windows")) + QStringLiteral("/System32"));
    m_process.setProcessEnvironment(environment);
    m_process.setWorkingDirectory(QDir::tempPath());
    m_process.start(m_executable, {});
}

void SandboxHost::send(const QJsonObject& message) {
    if (m_process.state() != QProcess::Running) { m_outgoing.append(message); return; }
    const QByteArray frame = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (frame.size() > MaxFrameBytes) { failAll(QStringLiteral("Native sandbox IPC frame exceeds its limit.")); return; }
    m_process.write(frame);
}

void SandboxHost::execute(const QString& id, const QString& name, const QJsonObject& arguments) {
    if (m_pending.contains(id)) { cancel(id); return; }
    if (id.isEmpty() || id.size() > 512 || !handles(name) || !arguments.value("script").isString()
        || !arguments.value("explanation").isString() || arguments.value("script").toString().size() > 256 * 1024) {
        emit completed(id, failure(QStringLiteral("Invalid executeSandboxJs request or script exceeds 256 KiB.")), true); return;
    }
    for (const auto& pending : std::as_const(m_pending)) if (pending.agentId == m_agentId) {
        emit completed(id, failure(QStringLiteral("This agent already has a running sandbox call.")), true); return;
    }
    Pending pending;
    pending.wireId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    pending.agentId = m_agentId;
    if(!m_contextIds.contains(m_agentId))m_contextIds.insert(m_agentId,QUuid::createUuid().toString(QUuid::WithoutBraces));
    pending.contextId=m_contextIds.value(m_agentId);
    pending.watchdog = new QTimer(this);
    pending.watchdog->setSingleShot(true);
    const QString wireId = pending.wireId;
    connect(pending.watchdog, &QTimer::timeout, this, [this, id, wireId] {
        if (!m_pending.contains(id) || m_pending.value(id).wireId != wireId) return;
        m_process.kill();
        failAll(QStringLiteral("Native sandbox worker exceeded its watchdog deadline; worker state was reset."));
    });
    pending.watchdog->start(m_hardMs + 3000);
    m_pending.insert(id, pending);
    m_wireToId.insert(wireId, id);
    m_contexts.insert(pending.agentId);
    // A failed QProcess::start can emit errorOccurred synchronously. Its
    // completion observer may destroy this host before ensureProcess returns.
    const QPointer<SandboxHost> self(this);
    ensureProcess();
    if(!self)return;
    const auto current=m_pending.constFind(id);
    if(current==m_pending.cend()||current->wireId!=wireId||current->cancelRequested
        ||m_contextIds.value(pending.agentId)!=pending.contextId)return;
    send({{"type", "execute"}, {"id", wireId}, {"agentId", pending.agentId}, {"contextId",pending.contextId}, {"script", arguments.value("script")},
        {"limits", QJsonObject{{"synchronousMs", m_synchronousMs}, {"inactivityMs", m_inactivityMs}, {"hardMs", m_hardMs}, {"memoryBytes", static_cast<qint64>(m_memoryBytes)}}}});
}

void SandboxHost::readOutput() {
    m_buffer += m_process.readAllStandardOutput();
    while (true) {
        const qsizetype newline = m_buffer.indexOf('\n');
        if (newline < 0) break;
        const QByteArray frame = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        if (frame.size() > MaxFrameBytes) { m_process.kill(); failAll(QStringLiteral("Sandbox worker sent an oversized message.")); return; }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(frame, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            m_process.kill(); failAll(QStringLiteral("Sandbox worker sent invalid IPC JSON.")); return;
        }
        const QPointer<SandboxHost> self(this);
        receive(document.object());
        if (!self) return;
    }
    if (m_buffer.size() > MaxFrameBytes) { m_process.kill(); failAll(QStringLiteral("Sandbox worker message exceeded the framing limit.")); }
}

void SandboxHost::receive(const QJsonObject& message) {
    const QString type = message.value("type").toString();
    if (type == QStringLiteral("ready")) return;
    if (type == QStringLiteral("contextReset")) {
        const QString agent = message.value("agentId").toString();
        invalidateContext(agent,message.value("contextId").toString());
        return;
    }
    if (type == QStringLiteral("console")) {
        emit consoleMessage(message.value("agentId").toString(), message.value("level").toString(), message.value("text").toString()); return;
    }
    const QString wireId = message.value("id").toString();
    const QString id = m_wireToId.value(wireId);
    const QString agent=id.isEmpty()?message.value("agentId").toString():m_pending.value(id).agentId;
    const QString context=id.isEmpty()?message.value("contextId").toString():m_pending.value(id).contextId;
    const bool live=!context.isEmpty()&&m_contextIds.value(agent)==context;
    if (type == QStringLiteral("result")) {
        if(id.isEmpty())return;
        const QPointer<SandboxHost> self(this);
        if (message.value("contextReset").toBool())invalidateContext(agent,context);
        if (!self) return;
        settle(id, message.value("output").toString(), message.value("error").toBool()); return;
    }
    if (type == QStringLiteral("output")) { if(!id.isEmpty())emit output(id, message.value("text").toString()); return; }
    if(!live)return;
    if(type=="subscribe"){
        const QString listener=message["listenerId"].toString(),kind=message["kind"].toString();
        if(listener.isEmpty()||listener.size()>256||m_subscriptions.contains(listener)||m_subscriptions.size()>=128||!QStringList{"cdp","appMessage"}.contains(kind)){
            send({{"type","subscriptionEnded"},{"listenerId",listener},{"error","Invalid or excessive event subscriptions."}});return;
        }
        m_subscriptions.insert(listener,Subscription{agent,context,{},0,false});
        if(!isSignalConnected(QMetaMethod::fromSignal(&SandboxHost::eventSubscriptionRequested))){endEventSubscription(listener,QStringLiteral("No event broker is configured."));return;}
        emit eventSubscriptionRequested(listener,agent,kind,message["arguments"].toObject());return;
    }
    if(type=="unsubscribe"){
        const QString listener=message["listenerId"].toString();if(m_subscriptions.value(listener).contextId!=context)return;
        if(m_subscriptions.remove(listener))emit eventUnsubscriptionRequested(listener);
        return;
    }
    if(type=="eventDone"){
        const QString listener=message["listenerId"].toString();auto it=m_subscriptions.find(listener);if(it==m_subscriptions.end()||it->contextId!=context)return;
        it->inFlight=false;flushEvent(listener);return;
    }
    if (type == QStringLiteral("cancelRpc")) {
        const QString requestId = message.value("requestId").toString();
        const auto rpc = m_rpc.constFind(requestId);
        if (rpc == m_rpc.cend() || rpc->contextId!=context || rpc->kind != QStringLiteral("network")) return;
        completeRpc(requestId, QJsonValue(), QStringLiteral("Network request aborted."));
        emit brokerCancellationRequested(requestId);
        return;
    }
    if (type != QStringLiteral("rpc") || (!id.isEmpty()&&m_pending.value(id).cancelRequested)) return;
    const QString requestId = message.value("requestId").toString();
    const QString kind = message.value("kind").toString();
    if (requestId.isEmpty() || m_rpc.contains(requestId) || m_rpc.size() >= 256) {
        send({{"type", "rpcResult"}, {"requestId", requestId}, {"error", "Invalid or excessive sandbox RPC requests."}}); return;
    }
    Rpc record{wireId,kind,agent,context,{}};
    if(id.isEmpty()){
        record.deadline=new QTimer(this);record.deadline->setSingleShot(true);record.deadline->start(45000);
        connect(record.deadline,&QTimer::timeout,this,[this,requestId]{if(!m_rpc.contains(requestId))return;completeRpc(requestId,QJsonValue(),QStringLiteral("Background sandbox request exceeded 45 seconds."));emit brokerCancellationRequested(requestId);});
    }
    m_rpc.insert(requestId,record);
    // An idle realm reset can be reported while a new execution is already
    // queued. Seeing a new RPC establishes that agent's current live realm.
    m_contexts.insert(agent);
    const auto arguments = message.value("arguments").toObject();
    if (kind == QStringLiteral("browser")) {
        emit browserRequest(requestId, arguments.value("tabId").toString(), arguments.value("method").toString(), arguments.value("params").toObject());
    }else if(QStringList{"readFile","writeFile","readBytes","writeBytes","mkdir","readDirectory"}.contains(kind)){
        QString error;const QString path=workspacePath(arguments["path"].toString(),&error);
        if(!error.isEmpty()){completeRpc(requestId,QJsonValue(),error);return;}
        if(path=="att"||path.startsWith("att/")){
            if(kind!="readFile"&&kind!="readBytes"&&kind!="readDirectory"){completeRpc(requestId,QJsonValue(),"The att/ mount allows read and list operations only.");return;}
            if(!isSignalConnected(QMetaMethod::fromSignal(&SandboxHost::attachmentFileRequest))){completeRpc(requestId,QJsonValue(),"No attachment file broker is configured.");return;}
            emit attachmentFileRequest(requestId,agent,kind,{{"path",path},{"recursive",arguments["recursive"]}});return;
        }
        if(kind=="readFile")emit fileRequest(requestId,"read",{{"path",path},{"raw",true},{"encoding","utf8"}});
        else if(kind=="writeFile")emit fileRequest(requestId,"write",{{"path",path},{"raw",true},{"content",arguments["content"]}});
        else if(kind=="writeBytes")emit fileRequest(requestId,"writeBytes",{{"path",path},{"raw",true},{"base64",arguments["base64"]}});
        else if(kind=="mkdir")emit fileRequest(requestId,"mkdir",{{"path",path},{"raw",true},{"recursive",arguments["recursive"]}});
        else emit fileRequest(requestId,kind,{{"path",path},{"recursive",arguments["recursive"]}});
    } else if (kind == QStringLiteral("network")) {
        if (!isSignalConnected(QMetaMethod::fromSignal(&SandboxHost::networkRequest))) {
            completeRpc(requestId, QJsonValue(), QStringLiteral("No network broker is configured.")); return;
        }
        emit networkRequest(requestId, agent, arguments);
    } else if (kind == QStringLiteral("credential")) {
        const QString typeId = arguments.value("typeId").toString();
        if (typeId.isEmpty() || typeId.size() > 128) {
            completeRpc(requestId, QJsonValue(), QStringLiteral("Invalid credential type ID.")); return;
        }
        if (!isSignalConnected(QMetaMethod::fromSignal(&SandboxHost::credentialRequest))) {
            completeRpc(requestId, QJsonValue(), QStringLiteral("No credential broker is configured.")); return;
        }
        emit credentialRequest(requestId, agent, typeId);
    } else if(kind=="appOpen"||kind=="appClose"||kind=="appSend"){
        if(!isSignalConnected(QMetaMethod::fromSignal(&SandboxHost::appRequest))){completeRpc(requestId,QJsonValue(),QStringLiteral("No app broker is configured."));return;}
        emit appRequest(requestId,agent,kind=="appOpen"?QStringLiteral("open"):kind=="appClose"?QStringLiteral("close"):QStringLiteral("send"),arguments);
    } else if (kind == QStringLiteral("attachment")) createAttachment(requestId, arguments);
    else completeRpc(requestId, QJsonValue(), QStringLiteral("Unsupported native sandbox RPC."));
}

QString SandboxHost::workspacePath(const QString& input,QString* error) const {
    QString path=QDir::fromNativeSeparators(input);
    if(path=="att"||path.startsWith("att/"))return path;
    if(path=="apps"||path.startsWith("apps/")){
        if(m_appsDirectory.isEmpty()){*error="The host-owned apps/ mount is unavailable.";return {};}
    }else{
        if(m_workspace.isEmpty()){*error="No workspace is mounted. Use apps/ for this chat's mini-app files.";return {};}
        if(QRegularExpression(QStringLiteral("^(att|plugins|logs|shells|memory|w[0-9a-f]+|ws[0-9]+)(/|$)")).match(path).hasMatch()
            &&path!="w1"&&path!="ws1"&&!path.startsWith("w1/")&&!path.startsWith("ws1/")){
            *error="This native sandbox exposes only ws1/w1, apps/ and read-only att/ mounts.";return {};
        }
    }
    if(path.isEmpty()){*error="File path is empty.";return {};}
    return path;
}

void SandboxHost::completeRpc(const QString& requestId, const QJsonValue& value, const QString& error) {
    if (!m_rpc.contains(requestId)) return;
    const auto rpc = m_rpc.take(requestId);
    if(rpc.deadline){rpc.deadline->stop();rpc.deadline->deleteLater();}
    if ((!rpc.wireId.isEmpty()&&!m_wireToId.contains(rpc.wireId))||m_contextIds.value(rpc.agentId)!=rpc.contextId) return;
    send({{"type", "rpcResult"}, {"requestId", requestId}, {"value", value}, {"error", error}});
}
void SandboxHost::completeBrowserRequest(const QString& requestId, const QString& result, bool error) {
    if (!m_rpc.contains(requestId) || m_rpc.value(requestId).kind != QStringLiteral("browser")) return;
    if (error) { completeRpc(requestId, QJsonValue(), result); return; }
    QJsonParseError parseError;
    const auto wrapped = QJsonDocument::fromJson((QByteArray("[") + result.toUtf8() + ']'), &parseError);
    if (parseError.error != QJsonParseError::NoError || wrapped.array().size() != 1) {
        completeRpc(requestId, QJsonValue(), QStringLiteral("Browser broker returned invalid JSON.")); return;
    }
    completeRpc(requestId, wrapped.array().first(), QString());
}
void SandboxHost::completeFileRequest(const QString& requestId,const QString& result,bool error) {
    if(!m_rpc.contains(requestId))return;
    const auto kind=m_rpc.value(requestId).kind;
    if(!QStringList{"readFile","writeFile","readBytes","writeBytes","mkdir","readDirectory"}.contains(kind))return;
    if(error){completeRpc(requestId,QJsonValue(),result);return;}
    if(kind=="readDirectory"){
        QJsonParseError parse;const auto value=QJsonDocument::fromJson(result.toUtf8(),&parse);
        if(parse.error!=QJsonParseError::NoError||!value.isArray()){completeRpc(requestId,QJsonValue(),"Invalid directory broker response.");return;}
        completeRpc(requestId,value.array(),{});
    }else completeRpc(requestId,result,{});
}

void SandboxHost::completeNetworkRequest(const QString& requestId, const QJsonObject& response, const QString& error) {
    if (!m_rpc.contains(requestId) || m_rpc.value(requestId).kind != QStringLiteral("network")) return;
    completeRpc(requestId, response, error);
}
void SandboxHost::completeCredentialRequest(const QString& requestId, const QJsonValue& value, const QString& error) {
    if (!m_rpc.contains(requestId) || m_rpc.value(requestId).kind != QStringLiteral("credential")) return;
    completeRpc(requestId, value, error);
}
void SandboxHost::completeAppRequest(const QString& requestId,const QJsonValue& value,const QString& error){
    if(!m_rpc.contains(requestId)||!QStringList{"appOpen","appClose","appSend"}.contains(m_rpc.value(requestId).kind))return;
    completeRpc(requestId,value,error);
}
void SandboxHost::deliverEvent(const QString& id,const QJsonValue& value){
    auto it=m_subscriptions.find(id);if(it==m_subscriptions.end()||m_contextIds.value(it->agentId)!=it->contextId)return;
    const qsizetype bytes=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).size();
    if(bytes>256*1024||it->queue.size()>=64||it->bytes+bytes>1024*1024){endEventSubscription(id,QStringLiteral("Event subscription exceeded its queue or message limit."));return;}
    it->queue.append(value);it->bytes+=bytes;flushEvent(id);
}
void SandboxHost::flushEvent(const QString& id){
    auto it=m_subscriptions.find(id);if(it==m_subscriptions.end()||it->inFlight||it->queue.isEmpty())return;
    const auto value=it->queue.takeFirst();it->bytes-=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).size();it->inFlight=true;
    send({{"type","event"},{"listenerId",id},{"agentId",it->agentId},{"contextId",it->contextId},{"value",value}});
}
void SandboxHost::endEventSubscription(const QString& id,const QString& error){
    if(!m_subscriptions.contains(id))return;
    m_subscriptions.remove(id);
    const QPointer<SandboxHost> self(this);send({{"type","subscriptionEnded"},{"listenerId",id},{"error",error}});if(!self)return;
    emit eventUnsubscriptionRequested(id);
}
void SandboxHost::invalidateContext(const QString& agent,const QString& context){
    if(context.isEmpty()||m_contextIds.value(agent)!=context)return;
    m_contextIds.remove(agent);m_contexts.remove(agent);const QPointer<SandboxHost> self(this);
    const auto listeners=m_subscriptions.keys();for(const auto& id:listeners)if(m_subscriptions.value(id).contextId==context){m_subscriptions.remove(id);emit eventUnsubscriptionRequested(id);if(!self)return;}
    const auto requests=m_rpc.keys();for(const auto& id:requests)if(m_rpc.value(id).contextId==context){const auto rpc=m_rpc.take(id);if(rpc.deadline){rpc.deadline->stop();rpc.deadline->deleteLater();}emit brokerCancellationRequested(id);if(!self)return;}
    emit contextReset(agent);
}

void SandboxHost::createAttachment(const QString& requestId, const QJsonObject& arguments) {
    if(m_artifactDirectory.isEmpty()){completeRpc(requestId,QJsonValue(),"Attachment storage is unavailable.");return;}
    const QString original = arguments.value("name").toString();
    const QByteArray encoded = arguments.value("base64").toString().toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() > 16 * 1024 * 1024 || original.isEmpty()) {
        completeRpc(requestId, QJsonValue(), QStringLiteral("Invalid attachment or attachment exceeds 16 MiB.")); return;
    }
    QString name = QFileInfo(original).fileName().left(100);
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    if (name.isEmpty()) name = QStringLiteral("attachment.bin");
    name = QUuid::createUuid().toString(QUuid::WithoutBraces) + QLatin1Char('-') + name;
    if (linkedAncestor(m_artifactDirectory)) { completeRpc(requestId,QJsonValue(),QStringLiteral("Attachment storage cannot use links or junctions.")); return; }
    if (!QDir().mkpath(m_artifactDirectory)) { completeRpc(requestId, QJsonValue(), QStringLiteral("Cannot create attachment directory.")); return; }
    if (linkedAncestor(m_artifactDirectory)) { completeRpc(requestId,QJsonValue(),QStringLiteral("Attachment storage cannot use links or junctions.")); return; }
    const QString canonical=QFileInfo(m_artifactDirectory).canonicalFilePath();
    if(canonical.isEmpty()){completeRpc(requestId,QJsonValue(),QStringLiteral("Cannot resolve attachment storage."));return;}
    const QString path = QDir(canonical).filePath(name);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(decoded.decoded) != decoded.decoded.size() || !file.commit()) {
        completeRpc(requestId, QJsonValue(), QStringLiteral("Cannot save attachment: ") + file.errorString()); return;
    }
    // Upstream attributes an attachment to the agent's current tool, even if
    // a persistent event callback created it. A truly idle write still succeeds
    // but must not become metadata for a later, unrelated tool or another agent.
    const auto rpc=m_rpc.value(requestId);
    QString id;
    for(auto it=m_pending.cbegin();it!=m_pending.cend();++it) {
        if(it->agentId==rpc.agentId&&it->contextId==rpc.contextId&&!it->cancelRequested) {id=it.key();break;}
    }
    // Stagewise returns the canonical file name; consumers prepend att/ when
    // addressing the attachment mount. The signal carries its actual path.
    const QPointer<SandboxHost> self(this);
    emit attachmentCreated(id, path, QMimeDatabase().mimeTypeForFile(path).name());
    if(self)completeRpc(requestId, name, QString());
}

void SandboxHost::settle(const QString& id, const QString& value, bool error) {
    if (!m_pending.contains(id)) return;
    const auto pending = m_pending.take(id);
    m_wireToId.remove(pending.wireId);
    if (pending.watchdog) { pending.watchdog->stop(); pending.watchdog->deleteLater(); }
    const auto requestIds = m_rpc.keys();
    const QPointer<SandboxHost> self(this);
    for (const auto& requestId : requestIds) if (m_rpc.value(requestId).wireId == pending.wireId) {
        m_rpc.remove(requestId);
        emit brokerCancellationRequested(requestId);
        if (!self) return;
    }
    emit completed(id, pending.cancelRequested ? failure(QStringLiteral("Sandbox execution canceled; this agent's context was reset.")) : value,
        pending.cancelRequested || error);
}
void SandboxHost::failAll(const QString& reason) {
    m_outgoing.clear();
    const auto ids = m_pending.keys();
    const QPointer<SandboxHost> self(this);
    const auto contexts = std::exchange(m_contexts, {});
    for (const auto& agent : contexts) { invalidateContext(agent,m_contextIds.value(agent)); if (!self) return; }
    for (const auto& id : ids) { settle(id, failure(reason), true); if (!self) return; }
}
void SandboxHost::cancel(const QString& id) {
    auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    it->cancelRequested = true;
    const QString wireId = it->wireId;
    const QString agent=it->agentId,context=it->contextId;
    if (it->watchdog) it->watchdog->start(1000);
    const QPointer<SandboxHost> self(this);
    send({{"type", "cancel"}, {"id", wireId}});
    if (!self) return;
    // Stop host I/O synchronously; do not wait for the worker to observe its
    // interrupt and acknowledge the realm reset. Other executions keep running.
    invalidateContext(agent,context);
}
void SandboxHost::cancelAll() {
    const auto ids = m_pending.keys();
    const QPointer<SandboxHost> self(this);
    for (const auto& id : ids) {cancel(id);if(!self)return;}
}
void SandboxHost::resetAgent(const QString& agentId) {
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it)
        if (it->agentId == agentId) { cancel(it.key()); return; }
    const QString context=m_contextIds.value(agentId);
    const QPointer<SandboxHost> self(this);
    if (isRunning()) send({{"type", "reset"}, {"agentId", agentId}});
    if(self)invalidateContext(agentId,context);
}
} // namespace ade
