#include <QCoreApplication>
#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QStringDecoder>
#include <QTimer>
#include <quickjs.h>
#include "network_bootstrap.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
constexpr qsizetype MaxOutputChars = 65536;
constexpr qsizetype MaxIpcBytes = 32 * 1024 * 1024;
qint64 nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct CancelFlag { std::atomic<bool> canceled{false}; };
struct CancellationRegistry {
    std::mutex mutex;
    QHash<QString, std::weak_ptr<CancelFlag>> running;
    QSet<QString> preCanceled;
    void add(const QString& id, const std::shared_ptr<CancelFlag>& flag) {
        std::lock_guard lock(mutex);
        running.insert(id, flag);
        if (preCanceled.remove(id)) flag->canceled = true;
    }
    void cancel(const QString& id) {
        std::lock_guard lock(mutex);
        if (auto flag = running.value(id).lock()) flag->canceled = true;
        else if (preCanceled.size() < 1024) preCanceled.insert(id);
    }
    void remove(const QString& id) {
        std::lock_guard lock(mutex);
        running.remove(id);
        preCanceled.remove(id);
    }
};

class Worker;
struct Context;
struct Run {
    QString id;
    JSValue promise = JS_UNDEFINED;
    QStringList outputs;
    qsizetype outputChars = 0;
    qint64 start = nowMs();
    qint64 lastOutput = start;
    std::shared_ptr<CancelFlag> cancel = std::make_shared<CancelFlag>();
};
struct PromiseRpc { JSValue resolve = JS_UNDEFINED; JSValue reject = JS_UNDEFINED; QString runId; QString kind; };
struct EventListener {JSValue callback=JS_UNDEFINED;QString kind;QJsonObject arguments;};
struct ScriptTimer {
    QTimer* timer = nullptr;
    JSValue callback = JS_UNDEFINED;
    QList<JSValue> arguments;
    bool repeat = false;
};
// Track native allocations as well as JS objects: an exhausted engine can no
// longer allocate an Error and may reject with null. The allocator preserves a
// reliable reason and makes the memory ceiling independent of error creation.
struct Allocator { size_t used = 0; size_t limit = 64 * 1024 * 1024; bool denied = false; };
struct alignas(std::max_align_t) Allocation { size_t size; };
void* allocate(void* opaque, size_t size) {
    auto* state = static_cast<Allocator*>(opaque);
    if (state->used > state->limit || size > std::numeric_limits<size_t>::max() - sizeof(Allocation)
        || size + sizeof(Allocation) > state->limit - state->used) { state->denied = true; return nullptr; }
    auto* allocation = static_cast<Allocation*>(std::malloc(size + sizeof(Allocation)));
    if (!allocation) { state->denied = true; return nullptr; }
    allocation->size = size;
    state->used += size + sizeof(Allocation);
    return allocation + 1;
}
void release(void* opaque, void* pointer) {
    if (!pointer) return;
    auto* allocation = static_cast<Allocation*>(pointer) - 1;
    static_cast<Allocator*>(opaque)->used -= allocation->size + sizeof(Allocation);
    std::free(allocation);
}
void* allocateZero(void* opaque, size_t count, size_t size) {
    if (size && count > std::numeric_limits<size_t>::max() / size) { static_cast<Allocator*>(opaque)->denied = true; return nullptr; }
    void* pointer = allocate(opaque, count * size);
    if (pointer) std::memset(pointer, 0, count * size);
    return pointer;
}
void* reallocate(void* opaque, void* pointer, size_t size) {
    if (!pointer) return allocate(opaque, size);
    if (!size) { release(opaque, pointer); return nullptr; }
    auto* state = static_cast<Allocator*>(opaque);
    auto* previous = static_cast<Allocation*>(pointer) - 1;
    const size_t oldSize = previous->size;
    if (state->used > state->limit || size > std::numeric_limits<size_t>::max() - sizeof(Allocation)
        || (size > oldSize && size - oldSize > state->limit - state->used)) { state->denied = true; return nullptr; }
    auto* resized = static_cast<Allocation*>(std::realloc(previous, size + sizeof(Allocation)));
    if (!resized) { state->denied = true; return nullptr; }
    resized->size = size;
    state->used = state->used - oldSize + size;
    return resized + 1;
}
size_t allocationSize(const void* pointer) { return pointer ? (static_cast<const Allocation*>(pointer) - 1)->size : 0; }
const JSMallocFunctions allocatorFunctions{allocateZero, allocate, release, reallocate, allocationSize};
struct Context {
    Worker* worker = nullptr;
    QString agentId;
    QString contextId;
    Allocator allocator;
    JSRuntime* runtime = nullptr;
    JSContext* js = nullptr;
    std::unique_ptr<Run> run;
    QHash<QString, PromiseRpc> promises;
    QHash<QString,EventListener> listeners;
    QHash<int, ScriptTimer> timers;
    int nextTimer = 1;
    int syncMs = 30000;
    int inactivityMs = 45000;
    int hardMs = 180000;
    qint64 callDeadline = 0;
    int interrupted = 0;
    qsizetype consoleChars = 0;
    int consoleMessages = 0;
    bool resetWanted = false;
    bool resetReported = false;
    int eventDepth=0;
    ~Context();
};

QString jsString(JSContext* js, JSValueConst value, bool* ok = nullptr) {
    size_t length = 0;
    const char* data = JS_ToCStringLen(js, &length, value);
    if (ok) *ok = data != nullptr;
    if (!data) return {};
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom | QStringConverter::Flag::Stateless);
    const QString text = decoder(QByteArrayView(data, static_cast<qsizetype>(length)));
    JS_FreeCString(js, data);
    return text;
}
JSValue toJs(JSContext* js, const QJsonValue& value) {
    const auto wrapper = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    const QByteArray data = wrapper.mid(1, wrapper.size() - 2);
    return JS_ParseJSON(js, data.constData(), static_cast<size_t>(data.size()), "native-ipc");
}
QJsonValue fromJs(JSContext* js, JSValueConst value, bool* ok) {
    JSValue encoded = JS_JSONStringify(js, value, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(encoded)) { *ok = false; return {}; }
    if (JS_IsUndefined(encoded)) { JS_FreeValue(js, encoded); *ok = true; return QJsonValue(QJsonValue::Null); }
    const QString text = jsString(js, encoded, ok);
    JS_FreeValue(js, encoded);
    if (!*ok) return {};
    const auto result = QJsonDocument::fromJson(QByteArray("[") + text.toUtf8() + ']');
    *ok = result.isArray() && result.array().size() == 1;
    return *ok ? result.array().first() : QJsonValue();
}
QString outputText(JSContext* js, JSValueConst value, bool* ok, bool emptyUndefined = false) {
    if (JS_IsString(value)) return jsString(js, value, ok);
    JSValue text = JS_JSONStringify(js, value, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(text)) { *ok = false; return {}; }
    if (JS_IsUndefined(text)) { JS_FreeValue(js, text); *ok = true; return emptyUndefined ? QString() : QStringLiteral("undefined"); }
    QString result = jsString(js, text, ok);
    JS_FreeValue(js, text);
    return result;
}
QString exceptionText(JSContext* js, JSValueConst error) {
    bool ok = false;
    QString result = jsString(js, error, &ok);
    if (!ok) {
        JSValue pending = JS_GetException(js);
        JS_FreeValue(js, pending);
        result = QStringLiteral("JavaScript exception or memory limit exceeded");
    }
    JSValue stack = JS_GetPropertyStr(js, error, "stack");
    if (!JS_IsException(stack) && !JS_IsUndefined(stack)) {
        const QString stackText = jsString(js, stack);
        if (!stackText.isEmpty() && !result.contains(stackText)) result += QLatin1Char('\n') + stackText;
    }
    JS_FreeValue(js, stack);
    return result.left(MaxOutputChars);
}

class Worker final : public QObject {
public:
    explicit Worker(std::shared_ptr<CancellationRegistry> registry, QObject* parent = nullptr)
        : QObject(parent), cancellations(std::move(registry)) {
        pumpTimer.setInterval(5);
        connect(&pumpTimer, &QTimer::timeout, this, [this] { pump(); });
        pumpTimer.start();
        send({{"type", "ready"}, {"runtime", "QuickJS-NG 0.18.0"}});
    }
    ~Worker() override { contexts.clear(); }
    void receive(const QJsonObject& message);
    void send(const QJsonObject& message) {
        const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
        if (bytes.size() > MaxIpcBytes) std::_Exit(3);
        if (std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout) != static_cast<size_t>(bytes.size())) std::_Exit(4);
        std::fflush(stdout);
    }
    JSValue rpc(Context* context, const QString& kind, const QJsonObject& arguments);
    void finish(Context* context, const QString& result, bool error, bool reset = false);
    void invokeTimer(Context* context, int id);
    void clearTimer(Context* context, int id);
    void pump();
    void reportException(Context* context, bool reset = false);

    std::shared_ptr<CancellationRegistry> cancellations;
    std::map<QString, std::unique_ptr<Context>> contexts;
    QHash<QString, Context*> rpcOwners;
    QHash<QString,Context*> listenerOwners;
    quint64 rpcSequence = 0;
    QTimer pumpTimer;
};

Context::~Context() {
    for(auto it=listeners.cbegin();it!=listeners.cend();++it){worker->listenerOwners.remove(it.key());JS_FreeValue(js,it->callback);}
    for (auto& timer : timers) {
        timer.timer->stop(); delete timer.timer;
        JS_FreeValue(js, timer.callback);
        for (const auto& argument : timer.arguments) JS_FreeValue(js, argument);
    }
    for (auto it = promises.cbegin(); it != promises.cend(); ++it) {
        worker->rpcOwners.remove(it.key());
        JS_FreeValue(js, it->resolve); JS_FreeValue(js, it->reject);
    }
    if (run) {
        worker->cancellations->remove(run->id);
        JS_FreeValue(js, run->promise);
    }
    if (js) JS_FreeContext(js);
    if (runtime) JS_FreeRuntime(runtime);
}

int interrupt(JSContext*, void* opaque) {
    auto* context = static_cast<Context*>(opaque);
    const qint64 now = nowMs();
    int reason = 0;
    if (context->run && context->run->cancel->canceled.load()) reason = 1;
    else if (context->run && now - context->run->start >= context->hardMs) reason = 2;
    else if (context->run && now - context->run->lastOutput >= context->inactivityMs) reason = 3;
    else if (context->callDeadline && now >= context->callDeadline) reason = 4;
    if (reason) context->interrupted = reason;
    return reason != 0;
}

JSValue nativeRpc(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* context = static_cast<Context*>(JS_GetContextOpaque(js));
    if (argc != 2 || !JS_IsString(argv[0]) || !JS_IsObject(argv[1])) return JS_ThrowTypeError(js, "Invalid native RPC arguments");
    bool ok = false;
    const QJsonValue args = fromJs(js, argv[1], &ok);
    if (!ok) return JS_EXCEPTION;
    if (!args.isObject()) return JS_ThrowTypeError(js, "RPC arguments must be an object");
    return context->worker->rpc(context, jsString(js, argv[0]), args.toObject());
}
JSValue nativeCancelRpc(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    auto* context = static_cast<Context*>(JS_GetContextOpaque(js));
    if (argc != 1 || !JS_IsString(argv[0])) return JS_UNDEFINED;
    const QString id = jsString(js, argv[0]);
    const auto rpc = context->promises.constFind(id);
    if (rpc == context->promises.cend() || rpc->kind != QStringLiteral("network")) return JS_UNDEFINED;
    context->worker->send({{"type", "cancelRpc"}, {"id", rpc->runId}, {"agentId",context->agentId},{"contextId",context->contextId},{"requestId", id}});
    return JS_UNDEFINED;
}
JSValue nativeListen(JSContext* js,JSValueConst,int argc,JSValueConst* argv){
    auto* context=static_cast<Context*>(JS_GetContextOpaque(js));
    if(argc!=3||!JS_IsString(argv[0])||!JS_IsObject(argv[1])||!JS_IsFunction(js,argv[2]))return JS_ThrowTypeError(js,"Invalid event listener arguments");
    const QString kind=jsString(js,argv[0]);if(kind!="cdp"&&kind!="appMessage")return JS_ThrowTypeError(js,"Invalid listener type");
    bool ok=false;const auto value=fromJs(js,argv[1],&ok);if(!ok)return JS_EXCEPTION;
    if(!value.isObject()||QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact).size()>4096)return JS_ThrowTypeError(js,"Invalid listener target");
    const auto args=value.toObject();
    if(kind=="cdp")for(auto it=context->listeners.cbegin();it!=context->listeners.cend();++it)if(it->kind==kind&&it->arguments==args&&JS_IsStrictEqual(js,it->callback,argv[2]))return JS_NewString(js,it.key().toUtf8().constData());
    if(context->listeners.size()>=64)return JS_ThrowRangeError(js,"Sandbox listener limit (64) reached");
    const QString id=QStringLiteral("listener-%1-%2").arg(context->contextId).arg(++context->worker->rpcSequence);
    context->listeners.insert(id,{JS_DupValue(js,argv[2]),kind,args});context->worker->listenerOwners.insert(id,context);
    context->worker->send({{"type","subscribe"},{"agentId",context->agentId},{"contextId",context->contextId},{"listenerId",id},{"kind",kind},{"arguments",args}});
    return JS_NewString(js,id.toUtf8().constData());
}
JSValue nativeUnlisten(JSContext* js,JSValueConst,int argc,JSValueConst* argv){
    auto* context=static_cast<Context*>(JS_GetContextOpaque(js));if(argc!=1||!JS_IsString(argv[0]))return JS_UNDEFINED;
    const QString id=jsString(js,argv[0]);if(!context->listeners.contains(id))return JS_UNDEFINED;
    const auto listener=context->listeners.take(id);JS_FreeValue(js,listener.callback);context->worker->listenerOwners.remove(id);
    context->worker->send({{"type","unsubscribe"},{"agentId",context->agentId},{"contextId",context->contextId},{"listenerId",id}});return JS_UNDEFINED;
}
JSValue nativeOutput(JSContext* js, JSValueConst, int argc, JSValueConst* argv, int magic) {
    auto* context = static_cast<Context*>(JS_GetContextOpaque(js));
    if (magic == 0) argc = qMin(argc, 1); // API.output accepts one value upstream.
    QStringList pieces;
    for (int i = 0; i < argc; ++i) {
        bool ok = false;
        QString value = outputText(js, argv[i], &ok, magic == 0);
        if (!ok) {
            if (magic == 0) return JS_EXCEPTION;
            JSValue error = JS_GetException(js); JS_FreeValue(js, error);
            value = QStringLiteral("[unserializable value]");
        }
        pieces.append(value);
    }
    const QString text = pieces.join(QLatin1Char(' '));
    if (magic != 0) {
        if (context->consoleMessages++ >= 1024 || context->consoleChars >= MaxOutputChars) return JS_UNDEFINED;
        const QString bounded = text.left(MaxOutputChars - context->consoleChars);
        context->consoleChars += bounded.size() + 1;
        static const QStringList levels{"", "log", "info", "warn", "error", "debug", "trace"};
        context->worker->send({{"type", "console"}, {"agentId", context->agentId}, {"level", levels.value(magic, "log")}, {"text", bounded}});
        return JS_UNDEFINED;
    }
    if (!context->run) return JS_ThrowInternalError(js, "API.output requires an active sandbox call");
    if (context->run->outputChars + text.size() + 1 > MaxOutputChars || context->run->outputs.size() >= 1024)
        return JS_ThrowRangeError(js, "Sandbox output exceeds 65536 characters or 1024 messages");
    context->run->outputChars += text.size() + 1;
    context->run->outputs.append(text);
    context->run->lastOutput = nowMs();
    context->worker->send({{"type", "output"}, {"id", context->run->id}, {"text", text}});
    return JS_UNDEFINED;
}

JSValue nativeCreateTimer(JSContext* js, JSValueConst, int argc, JSValueConst* argv, int repeat) {
    auto* context = static_cast<Context*>(JS_GetContextOpaque(js));
    if (argc < 1 || !JS_IsFunction(js, argv[0])) return JS_ThrowTypeError(js, "Timer callback must be a function");
    if (context->timers.size() >= 128) return JS_ThrowRangeError(js, "Sandbox timer limit reached");
    int32_t delay = 0;
    if (argc > 1 && JS_ToInt32(js, &delay, argv[1]) < 0) return JS_EXCEPTION;
    ScriptTimer timer;
    timer.callback = JS_DupValue(js, argv[0]);
    timer.repeat = repeat;
    timer.timer = new QTimer(context->worker);
    timer.timer->setSingleShot(!repeat);
    for (int i = 2; i < argc; ++i) timer.arguments.append(JS_DupValue(js, argv[i]));
    const int id = context->nextTimer++;
    QObject::connect(timer.timer, &QTimer::timeout, context->worker, [context, id] { context->worker->invokeTimer(context, id); });
    context->timers.insert(id, timer);
    timer.timer->start(qBound(repeat ? 1 : 0, delay, 180000));
    return JS_NewInt32(js, id);
}
JSValue nativeClearTimer(JSContext* js, JSValueConst, int argc, JSValueConst* argv) {
    int32_t id = 0;
    if (argc && JS_ToInt32(js, &id, argv[0]) < 0) return JS_EXCEPTION;
    auto* context = static_cast<Context*>(JS_GetContextOpaque(js));
    context->worker->clearTimer(context, id);
    return JS_UNDEFINED;
}

QByteArray typedBytes(JSContext* js, JSValueConst value, bool* ok) {
    size_t offset = 0, length = 0, elementSize = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(js, value, &offset, &length, &elementSize);
    if (JS_IsException(buffer)) { *ok = false; return {}; }
    size_t bufferSize = 0;
    uint8_t* bytes = JS_GetArrayBuffer(js, &bufferSize, buffer);
    *ok = (bytes || bufferSize == 0) && offset <= bufferSize && length <= bufferSize - offset && length <= 16 * 1024 * 1024;
    const QByteArray result = *ok && length ? QByteArray(reinterpret_cast<const char*>(bytes + offset), static_cast<qsizetype>(length)) : QByteArray();
    JS_FreeValue(js, buffer);
    if (!*ok) JS_ThrowRangeError(js, "Invalid byte buffer or buffer exceeds 16 MiB");
    return result;
}
JSValue nativeEncode(JSContext* js, JSValueConst, int argc, JSValueConst* argv, int mode) {
    if (!argc) return JS_ThrowTypeError(js, "A string is required");
    bool ok = false;
    const QString text = jsString(js, argv[0], &ok);
    if (!ok) return JS_EXCEPTION;
    QByteArray bytes;
    if (mode == 1) {
        const auto decoded = QByteArray::fromBase64Encoding(text.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) return JS_ThrowTypeError(js, "Invalid base64 input");
        bytes = decoded.decoded;
    } else if (mode == 2) bytes = text.toLatin1();
    else bytes = text.toUtf8();
    if (bytes.size() > 16 * 1024 * 1024) return JS_ThrowRangeError(js, "Byte buffer exceeds 16 MiB");
    return JS_NewUint8ArrayCopy(js, reinterpret_cast<const uint8_t*>(bytes.constData()), static_cast<size_t>(bytes.size()));
}
JSValue nativeDecode(JSContext* js, JSValueConst, int argc, JSValueConst* argv, int mode) {
    if (!argc) return JS_ThrowTypeError(js, "A byte array is required");
    bool ok = false;
    const QByteArray bytes = typedBytes(js, argv[0], &ok);
    if (!ok) return JS_EXCEPTION;
    QByteArray text;
    if (mode == 1) text = bytes.toBase64();
    else if (mode == 2) {
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom | QStringConverter::Flag::Stateless);
        const QString decoded = decoder.decode(bytes);
        if (decoder.hasError()) return JS_ThrowTypeError(js, "Native file writes support valid UTF-8 bytes only");
        text = decoded.toUtf8();
    } else if (mode == 3) text = QString::fromLatin1(bytes).toUtf8();
    else {
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom | QStringConverter::Flag::Stateless);
        const QString decoded = decoder(bytes);
        text = decoded.toUtf8();
    }
    return JS_NewStringLen(js, text.constData(), static_cast<size_t>(text.size()));
}

const char* bootstrap = R"JS(
(() => {
  const unsupported = name => { throw new Error(name + ' is not implemented by the native sandbox'); };
  const utf8 = encoding => encoding === undefined || encoding === null || encoding === 'utf8' || encoding === 'utf-8';
  class Buffer extends Uint8Array {
    static from(value, encoding='utf8') {
      let data;
      if (typeof value === 'string') {
        if (utf8(encoding)) data = __encodeUtf8(value);
        else if (encoding === 'base64') data = __decodeBase64(value);
        else if (encoding === 'latin1' || encoding === 'binary') data = __encodeLatin1(value);
        else return unsupported('Buffer encoding ' + encoding);
      } else if (value instanceof ArrayBuffer) data = new Uint8Array(value);
      else data = new Uint8Array(value);
      return new Buffer(data);
    }
    static isBuffer(value) { return value instanceof Buffer; }
    static alloc(size, fill=0) { if (!Number.isSafeInteger(size) || size<0 || size>16777216) throw new RangeError('Buffer size limit'); return new Buffer(size).fill(fill); }
    static concat(values) { const n=values.reduce((sum,v)=>sum+v.length,0); const out=Buffer.alloc(n); let p=0; for(const v of values){out.set(v,p);p+=v.length;} return out; }
    toString(encoding='utf8') {
      if (utf8(encoding)) return __decodeUtf8(this);
      if (encoding==='base64') return __encodeBase64(this);
      if (encoding==='latin1' || encoding==='binary') return __decodeLatin1(this);
      return unsupported('Buffer encoding ' + encoding);
    }
    toJSON() { return {type:'Buffer',data:Array.from(this)}; }
  }
  globalThis.Buffer = Buffer;
  globalThis.TextEncoder = class { get encoding(){return 'utf-8';} encode(text=''){return __encodeUtf8(String(text));} };
  globalThis.TextDecoder = class {
    constructor(label='utf-8',opts={}){if(!utf8(label))unsupported('TextDecoder '+label);this.fatal=!!opts.fatal;}
    decode(bytes=new Uint8Array()){return this.fatal?__decodeUtf8Strict(bytes):__decodeUtf8(bytes);}
  };
  globalThis.atob = s => Buffer.from(s,'base64').toString('latin1');
  globalThis.btoa = s => {s=String(s);for(const c of s)if(c.charCodeAt(0)>255)throw new TypeError('Invalid btoa character');return Buffer.from(s,'latin1').toString('base64');};
  globalThis.queueMicrotask = fn => Promise.resolve().then(fn);
  globalThis.setImmediate = (fn,...args) => setTimeout(fn,0,...args);
  globalThis.clearImmediate = clearTimeout;
  globalThis.self = globalThis;
  globalThis.global = globalThis;
  globalThis.process = Object.freeze({env:Object.freeze({NODE_ENV:'production'}),browser:true,version:'',nextTick:(fn,...args)=>queueMicrotask(()=>fn(...args))});
  const promises = {
    async readFile(path,options) {
      if(options && typeof options==='object' && Object.keys(options).some(k=>k!=='encoding'))return unsupported('fs read options other than encoding');
      const encoding=typeof options==='object'&&options!==null?options.encoding:options;
      if(encoding===undefined||encoding===null)return Buffer.from(await __rpc('readBytes',{path:String(path)}),'base64');
      if(!utf8(encoding))return unsupported('fs read encoding '+encoding);
      return await __rpc('readFile',{path:String(path)});
    },
    async writeFile(path,data,options='utf8') {
      if(options && typeof options==='object' && Object.keys(options).some(k=>k!=='encoding'))return unsupported('fs write options other than encoding');
      const encoding=typeof options==='object'&&options!==null?options.encoding:options;
      if(!utf8(encoding))return unsupported('fs write encoding '+encoding);
      if(typeof data==='string')await __rpc('writeFile',{path:String(path),content:data});
      else {
        if(!(data instanceof Uint8Array)&&!(data instanceof ArrayBuffer))throw new TypeError('File data must be a string, Buffer, Uint8Array or ArrayBuffer');
        const bytes=Buffer.from(data);if(bytes.length>4194304)throw new RangeError('Binary file writes are limited to 4 MiB');
        await __rpc('writeBytes',{path:String(path),base64:bytes.toString('base64')});
      }
    },
    async mkdir(path,options={}) {
      if(!options||typeof options!=='object'||Object.keys(options).some(k=>k!=='recursive')||(options.recursive!==undefined&&typeof options.recursive!=='boolean'))return unsupported('fs mkdir options other than recursive');
      const created=await __rpc('mkdir',{path:String(path),recursive:!!options.recursive});
      return created||undefined;
    },
    async readdir(path,options={}) {
      if(!options||typeof options!=='object'||Object.keys(options).some(k=>k!=='recursive')||(options.recursive!==undefined&&typeof options.recursive!=='boolean'))return unsupported('fs readdir options other than recursive');
      return await __rpc('readDirectory',{path:String(path),recursive:!!options.recursive});
    }
  };
  for(const method of ['stat','lstat','rm','unlink','rename','copyFile','appendFile','access']) promises[method]=()=>unsupported('fsPromises.'+method);
  const fs={promises};
  for(const method of ['readFile','writeFile','mkdir','readdir']) fs[method]=function(path,data,options,callback){
    if(method!=='writeFile') {callback=typeof data==='function'?data:options;options=typeof data==='function'?undefined:data;}
    else if(typeof options==='function'){callback=options;options=undefined;}
    if(typeof callback!=='function')throw new TypeError('A callback is required');
    const promise=method==='writeFile'?promises.writeFile(path,data,options):promises[method](path,options);
    promise.then(value=>callback(null,value),error=>callback(error));
  };
  for(const method of ['readFileSync','writeFileSync','readdirSync','statSync','existsSync','mkdirSync','rmSync']) fs[method]=()=>unsupported('fs.'+method);
  globalThis.fsPromises=Object.freeze(promises);
  globalThis.fs=Object.freeze(fs);
  globalThis.require=name=>{
    name=String(name).replace(/^node:/,'');
    if(name==='fs')return fs;
    if(name==='fs/promises')return promises;
    if(name==='buffer')return {Buffer};
    return unsupported('require('+name+')');
  };
  globalThis.importModule=async()=>unsupported('importModule');
  globalThis.API=Object.freeze({
    output:__output,
    sendCDP:(tabId,method,params={})=>__rpc('browser',{tabId:String(tabId),method:String(method),params}),
    createAttachment:(name,data)=>{
      if(typeof name!=='string'||!name||(typeof data!=='string'&&!Buffer.isBuffer(data)))throw new TypeError('createAttachment requires a file name and Buffer or base64 string');
      return __rpc('attachment',{name,base64:typeof data==='string'?data:data.toString('base64')});
    },
    getCredential:async typeId=>{
      if(typeof typeId!=='string'||!typeId||typeId.length>128)throw new TypeError('Invalid credential type ID');
      return __rpc('credential',{typeId});
    },
    openApp:(appId,opts={})=>__rpc('appOpen',{...opts,appId:String(appId)}),
    closeApp:async()=>{await __rpc('appClose',{});},
    sendMessage:async(appId,data,opts={})=>{await __rpc('appSend',{appId:String(appId),pluginId:opts.pluginId,data});},
    onMessage:(appId,callback,opts={})=>{const id=__listen('appMessage',{appId:String(appId),pluginId:opts.pluginId},callback);return ()=>__unlisten(id);},
    onCDPEvent:(tabId,event,callback)=>{const id=__listen('cdp',{tabId:String(tabId),event:String(event)},callback);return ()=>__unlisten(id);}
  });
})();
)JS";

std::unique_ptr<Context> createContext(Worker* worker, const QString& agent,const QString& contextId, const QJsonObject& limits) {
    auto context = std::make_unique<Context>();
    context->worker = worker;
    context->agentId = agent;
    context->contextId=contextId;
    context->allocator.limit = static_cast<size_t>(qBound<qint64>(8ll * 1024 * 1024, limits.value("memoryBytes").toInteger(64ll * 1024 * 1024), 512ll * 1024 * 1024));
    context->runtime = JS_NewRuntime2(&allocatorFunctions, &context->allocator);
    if (!context->runtime) return {};
    JS_SetMemoryLimit(context->runtime, 0); // The tracking allocator enforces the native-byte limit.
    JS_SetMaxStackSize(context->runtime, 1024 * 1024);
    context->js = JS_NewContext(context->runtime);
    if (!context->js) return {};
    JS_SetContextOpaque(context->js, context.get());
    JS_SetInterruptHandler(context->runtime, interrupt, context.get());
    JSValue global = JS_GetGlobalObject(context->js);
    auto function = [&](const char* name, JSCFunction* fn, int argc) { JS_SetPropertyStr(context->js, global, name, JS_NewCFunction(context->js, fn, name, argc)); };
    auto magic = [&](const char* name, JSCFunctionMagic* fn, int argc, int id) { JS_SetPropertyStr(context->js, global, name, JS_NewCFunctionMagic(context->js, fn, name, argc, JS_CFUNC_generic_magic, id)); };
    function("__rpc", nativeRpc, 2);
    function("__cancelRpc", nativeCancelRpc, 1);
    function("__listen",nativeListen,3);
    function("__unlisten",nativeUnlisten,1);
    magic("__output", nativeOutput, 1, 0);
    magic("setTimeout", nativeCreateTimer, 2, 0);
    magic("setInterval", nativeCreateTimer, 2, 1);
    function("clearTimeout", nativeClearTimer, 1);
    function("clearInterval", nativeClearTimer, 1);
    magic("__encodeUtf8", nativeEncode, 1, 0);
    magic("__decodeBase64", nativeEncode, 1, 1);
    magic("__encodeLatin1", nativeEncode, 1, 2);
    magic("__decodeUtf8", nativeDecode, 1, 0);
    magic("__encodeBase64", nativeDecode, 1, 1);
    magic("__decodeUtf8Strict", nativeDecode, 1, 2);
    magic("__decodeLatin1", nativeDecode, 1, 3);
    JSValue console = JS_NewObject(context->js);
    const char* names[] = {"log", "info", "warn", "error", "debug", "trace"};
    for (int i = 0; i < 6; ++i) JS_SetPropertyStr(context->js, console, names[i], JS_NewCFunctionMagic(context->js, nativeOutput, names[i], 1, JS_CFUNC_generic_magic, i + 1));
    JS_SetPropertyStr(context->js, global, "console", console);
    JS_FreeValue(context->js, global);
    JSValue initialized = JS_Eval(context->js, bootstrap, std::strlen(bootstrap), "native-bootstrap.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(initialized)) { JS_FreeValue(context->js, initialized); return {}; }
    JS_FreeValue(context->js, initialized);
    initialized = JS_Eval(context->js, networkBootstrap, std::strlen(networkBootstrap), "native-network.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(initialized)) { JS_FreeValue(context->js, initialized); return {}; }
    JS_FreeValue(context->js, initialized);
    return context;
}

void Worker::receive(const QJsonObject& message) {
    const QString type = message.value("type").toString();
    if (type == QStringLiteral("execute")) {
        const QString id = message.value("id").toString();
        const QString agent = message.value("agentId").toString();
        const QString contextId=message.value("contextId").toString();
        const auto limits = message.value("limits").toObject();
        if (id.isEmpty() || agent.isEmpty() || agent.size() > 256 || contextId.isEmpty()||contextId.size()>128|| message.value("script").toString().size() > 256 * 1024) return;
        if(contexts.contains(agent)&&contexts.at(agent)->contextId!=contextId&&!contexts.at(agent)->run)contexts.erase(agent);
        if (!contexts.contains(agent)) {
            if (contexts.size() >= 32) {
                send({{"type", "result"}, {"id", id}, {"error", true}, {"output", "Sandbox context limit (32) reached."}}); return;
            }
            auto context = createContext(this, agent, contextId,limits);
            if (!context) {
                send({{"type", "result"}, {"id", id}, {"error", true}, {"output", "Cannot initialize native JavaScript context."}}); return;
            }
            contexts.emplace(agent, std::move(context));
        }
        auto* context = contexts.at(agent).get();
        if (context->run) {
            send({{"type", "result"}, {"id", id}, {"error", true}, {"output", "Agent already has an active sandbox call."}}); return;
        }
        context->syncMs = qBound(50, limits.value("synchronousMs").toInt(30000), 30000);
        context->inactivityMs = qBound(50, limits.value("inactivityMs").toInt(45000), 45000);
        context->hardMs = qBound(100, limits.value("hardMs").toInt(180000), 180000);
        context->allocator.limit = static_cast<size_t>(qBound<qint64>(8ll * 1024 * 1024, limits.value("memoryBytes").toInteger(64ll * 1024 * 1024), 512ll * 1024 * 1024));
        context->allocator.denied |= context->allocator.used > context->allocator.limit;
        context->run = std::make_unique<Run>();
        context->interrupted = 0;
        context->consoleChars = 0;
        context->consoleMessages = 0;
        context->run->id = id;
        cancellations->add(id, context->run->cancel);
        if (context->allocator.denied) { pump(); return; }
        context->callDeadline = nowMs() + context->syncMs;
        const QByteArray source = QByteArray("(async () => {\n") + message.value("script").toString().toUtf8() + "\n})()";
        JSValue promise = JS_Eval(context->js, source.constData(), static_cast<size_t>(source.size()), "agent-script.js", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(promise)) reportException(context);
        else context->run->promise = promise;
        context->callDeadline = 0;
        pump();
    } else if (type == QStringLiteral("cancel")) {
        const QString id = message.value("id").toString();
        for (auto& [agent, context] : contexts) if (context->run && context->run->id == id) {
            finish(context.get(), QStringLiteral("Sandbox execution canceled; context reset."), true, true);
            break;
        }
        pump();
    } else if (type == QStringLiteral("reset")) {
        const QString agent = message.value("agentId").toString();
        auto it = contexts.find(agent);
        if (it != contexts.end() && !it->second->run) contexts.erase(it);
    } else if(type=="subscriptionEnded"){
        const QString id=message["listenerId"].toString();Context* context=listenerOwners.take(id);if(!context||!context->listeners.contains(id))return;
        const auto listener=context->listeners.take(id);JS_FreeValue(context->js,listener.callback);
        const QString error=message["error"].toString();if(!error.isEmpty())send({{"type","console"},{"agentId",context->agentId},{"level","error"},{"text",error.left(4096)}});
    } else if(type=="event"){
        const QString id=message["listenerId"].toString();Context* context=listenerOwners.value(id);
        if(!context||context->resetWanted||!context->listeners.contains(id)||context->contextId!=message["contextId"].toString()||context->agentId!=message["agentId"].toString())return;
        JSValue callback=JS_DupValue(context->js,context->listeners.value(id).callback),value=toJs(context->js,message["value"]);
        context->callDeadline=nowMs()+(context->run?context->syncMs:1000);
        ++context->eventDepth;
        JSValue result=JS_IsException(value)?JS_EXCEPTION:JS_Call(context->js,callback,JS_UNDEFINED,1,&value);
        --context->eventDepth;
        if(JS_IsException(result)){
            if(context->interrupted||context->allocator.denied)reportException(context,true);
            else{JSValue error=JS_GetException(context->js);const QString text=exceptionText(context->js,error);JS_FreeValue(context->js,error);send({{"type","console"},{"agentId",context->agentId},{"level","error"},{"text",text.left(4096)}});}
        }
        JS_FreeValue(context->js,result);JS_FreeValue(context->js,value);JS_FreeValue(context->js,callback);context->callDeadline=0;
        send({{"type","eventDone"},{"agentId",context->agentId},{"contextId",context->contextId},{"listenerId",id}});pump();
    } else if (type == QStringLiteral("rpcResult")) {
        const QString requestId = message.value("requestId").toString();
        Context* context = rpcOwners.take(requestId);
        if (!context || !context->promises.contains(requestId) || context->resetWanted) return;
        const auto promise = context->promises.take(requestId);
        const QString error = message.value("error").toString();
        JSValue value = error.isEmpty() ? toJs(context->js, message.value("value")) : JS_NewError(context->js);
        if (!error.isEmpty() && !JS_IsException(value)) {
            const auto bytes = error.toUtf8();
            if (JS_SetPropertyStr(context->js, value, "message", JS_NewStringLen(context->js, bytes.constData(), static_cast<size_t>(bytes.size()))) < 0) {
                JS_FreeValue(context->js, value);
                value = JS_EXCEPTION;
            }
        }
        if (JS_IsException(value)) {
            JS_FreeValue(context->js, promise.resolve); JS_FreeValue(context->js, promise.reject);
            reportException(context); pump(); return;
        }
        JSValue result = JS_Call(context->js, error.isEmpty() ? promise.resolve : promise.reject, JS_UNDEFINED, 1, &value);
        const bool failed = JS_IsException(result);
        JS_FreeValue(context->js, result); JS_FreeValue(context->js, value);
        JS_FreeValue(context->js, promise.resolve); JS_FreeValue(context->js, promise.reject);
        if (failed) reportException(context);
        pump();
    }
}

JSValue Worker::rpc(Context* context, const QString& kind, const QJsonObject& arguments) {
    if(context->resetWanted)return JS_ThrowInternalError(context->js,"Sandbox context is resetting");
    if (context->promises.size() >= 128) return JS_ThrowRangeError(context->js, "Sandbox RPC limit reached");
    if (kind != QStringLiteral("browser") && kind != QStringLiteral("readFile") && kind != QStringLiteral("writeFile") && kind != QStringLiteral("attachment")
        && kind != QStringLiteral("readBytes") && kind != QStringLiteral("writeBytes") && kind != QStringLiteral("mkdir") && kind != QStringLiteral("readDirectory")
        && kind != QStringLiteral("network") && kind != QStringLiteral("credential")
        && kind!=QStringLiteral("appOpen")&&kind!=QStringLiteral("appClose")&&kind!=QStringLiteral("appSend"))
        return JS_ThrowTypeError(context->js, "Unsupported native API");
    if (QJsonDocument(arguments).toJson(QJsonDocument::Compact).size() > 24 * 1024 * 1024)
        return JS_ThrowRangeError(context->js, "Native API arguments exceed limit");
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(context->js, resolving);
    if (JS_IsException(promise)) return promise;
    const QString runId=context->run&&context->eventDepth==0?context->run->id:QString();
    const QString requestId = QStringLiteral("sandbox-%1-%2").arg(context->contextId).arg(++rpcSequence);
    if (kind == QStringLiteral("network")) {
        const auto bytes = requestId.toUtf8();
        if (JS_DefinePropertyValueStr(context->js, promise, "nativeRequestId", JS_NewStringLen(context->js, bytes.constData(), static_cast<size_t>(bytes.size())), 0) < 0) {
            JS_FreeValue(context->js, resolving[0]); JS_FreeValue(context->js, resolving[1]); JS_FreeValue(context->js, promise); return JS_EXCEPTION;
        }
    }
    context->promises.insert(requestId, {resolving[0], resolving[1], runId, kind});
    rpcOwners.insert(requestId, context);
    if (kind == QStringLiteral("attachment")&&context->run) context->run->lastOutput = nowMs();
    send({{"type", "rpc"}, {"id", runId}, {"agentId",context->agentId},{"contextId",context->contextId},{"requestId", requestId}, {"kind", kind}, {"arguments", arguments}});
    return promise;
}

void Worker::clearTimer(Context* context, int id) {
    if (!context->timers.contains(id)) return;
    const auto timer = context->timers.take(id);
    timer.timer->stop(); timer.timer->deleteLater();
    JS_FreeValue(context->js, timer.callback);
    for (const auto& argument : timer.arguments) JS_FreeValue(context->js, argument);
}
void Worker::invokeTimer(Context* context, int id) {
    if (!context->timers.contains(id) || context->resetWanted) return;
    const auto timer = context->timers.value(id);
    JSValue callback = JS_DupValue(context->js, timer.callback);
    QList<JSValue> arguments;
    for (const auto& value : timer.arguments) arguments.append(JS_DupValue(context->js, value));
    if (!timer.repeat) clearTimer(context, id);
    context->callDeadline = nowMs() + (context->run ? context->syncMs : 1000);
    JSValue result = JS_Call(context->js, callback, JS_UNDEFINED, static_cast<int>(arguments.size()), arguments.data());
    JS_FreeValue(context->js, callback);
    for (const auto& value : arguments) JS_FreeValue(context->js, value);
    if (JS_IsException(result)) reportException(context, !context->run);
    JS_FreeValue(context->js, result);
    context->callDeadline = 0;
    pump();
}

void Worker::reportException(Context* context, bool reset) {
    JSValue exception = JS_GetException(context->js);
    QString text = exceptionText(context->js, exception);
    JS_FreeValue(context->js, exception);
    const qint64 now = nowMs();
    if (context->run) {
        if (context->allocator.denied) { text = QStringLiteral("Sandbox memory limit exceeded; context reset."); reset = true; }
        else if (context->run->cancel->canceled) { text = QStringLiteral("Sandbox execution canceled; context reset."); reset = true; }
        else if (now - context->run->start >= context->hardMs) { text = QStringLiteral("Sandbox hard time limit reached; context reset."); reset = true; }
        else if (now - context->run->lastOutput >= context->inactivityMs) { text = QStringLiteral("Sandbox inactivity time limit reached; context reset."); reset = true; }
        else if (context->callDeadline && now >= context->callDeadline) { text = QStringLiteral("Sandbox synchronous time limit reached; context reset."); reset = true; }
        else if (text.contains(QStringLiteral("memory"), Qt::CaseInsensitive)) reset = true;
        finish(context, text, true, reset);
    } else {
        send({{"type", "console"}, {"agentId", context->agentId}, {"level", "error"}, {"text", text}});
        context->resetWanted = reset;
    }
}

void Worker::finish(Context* context, const QString& result, bool error, bool reset) {
    if (!context->run) { context->resetWanted |= reset; return; }
    const QString id = context->run->id;
    QStringList parts = context->run->outputs;
    if (!result.isEmpty() && !error) parts.append(result);
    const QJsonObject output = error
        ? QJsonObject{{"message", "Sandbox JavaScript failed"}, {"error", result}, {"output", parts.join(QLatin1Char('\n'))}}
        : QJsonObject{{"message", "Successfully executed sandbox JavaScript"}, {"result", parts.join(QLatin1Char('\n'))}};
    JS_FreeValue(context->js, context->run->promise);
    cancellations->remove(id);
    context->run.reset();
    context->resetWanted |= reset;
    context->resetReported |= reset;
    const auto requests = context->promises.keys();
    for (const auto& request : requests) {
        if (context->promises.value(request).runId != id) continue;
        const auto promise = context->promises.take(request);
        JS_FreeValue(context->js, promise.resolve); JS_FreeValue(context->js, promise.reject);
        rpcOwners.remove(request);
    }
    send({{"type", "result"}, {"id", id}, {"error", error}, {"contextReset", reset}, {"output", QString::fromUtf8(QJsonDocument(output).toJson(QJsonDocument::Compact))}});
}

void Worker::pump() {
    for (auto& [agent, owner] : contexts) {
        auto* context = owner.get();
        if (context->resetWanted) continue;
        if (context->allocator.denied) {
            finish(context, QStringLiteral("Sandbox memory limit exceeded; context reset."), true, true);
            continue;
        }
        if (context->interrupted) {
            const char* reasons[] = {"", "Sandbox execution canceled; context reset.", "Sandbox hard time limit reached; context reset.",
                "Sandbox inactivity time limit reached; context reset.", "Sandbox synchronous time limit reached; context reset."};
            finish(context, QString::fromLatin1(reasons[context->interrupted]), true, true);
            continue;
        }
        const qint64 now = nowMs();
        if (context->run && (context->run->cancel->canceled || now - context->run->start >= context->hardMs || now - context->run->lastOutput >= context->inactivityMs)) {
            const QString reason = context->run->cancel->canceled ? QStringLiteral("Sandbox execution canceled; context reset.")
                : now - context->run->start >= context->hardMs ? QStringLiteral("Sandbox hard time limit reached; context reset.")
                : QStringLiteral("Sandbox inactivity time limit reached; context reset.");
            finish(context, reason, true, true); continue;
        }
        for (int jobs = 0; jobs < 100 && !context->resetWanted; ++jobs) {
            JSContext* pendingContext = nullptr;
            context->callDeadline = nowMs() + (context->run ? context->syncMs : 1000);
            const int status = JS_ExecutePendingJob(context->runtime, &pendingContext);
            if (status < 0) reportException(context, !context->run);
            context->callDeadline = 0;
            if (status <= 0) break;
        }
        if (!context->run || context->resetWanted || JS_IsUndefined(context->run->promise)) continue;
        if (context->interrupted || context->allocator.denied) continue; // Classify interrupted/exhausted promise jobs on the next pump.
        const auto state = JS_PromiseState(context->js, context->run->promise);
        if (state == JS_PROMISE_PENDING) continue;
        JSValue result = JS_PromiseResult(context->js, context->run->promise);
        if (state == JS_PROMISE_REJECTED) {
            const QString error = exceptionText(context->js, result);
            JS_FreeValue(context->js, result);
            finish(context, error, true, error.contains(QStringLiteral("memory"), Qt::CaseInsensitive));
        } else {
            bool ok = true;
            context->callDeadline = nowMs() + context->syncMs;
            const QString value = JS_IsUndefined(result) || JS_IsNull(result) ? QString() : outputText(context->js, result, &ok, true);
            JS_FreeValue(context->js, result);
            if (!ok) reportException(context);
            else if (value.size() + context->run->outputChars > MaxOutputChars) finish(context, QStringLiteral("Sandbox output exceeds 65536 characters"), true);
            else finish(context, value, false);
            context->callDeadline = 0;
        }
    }
    for (auto it = contexts.begin(); it != contexts.end();) {
        if (it->second->resetWanted) {
            if (!it->second->resetReported) send({{"type", "contextReset"}, {"agentId", it->first},{"contextId",it->second->contextId}});
            it = contexts.erase(it);
        }
        else ++it;
    }
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    auto cancellations = std::make_shared<CancellationRegistry>();
    Worker worker(cancellations);
    std::thread reader([&worker, &app, cancellations] {
        QByteArray pending;
        char buffer[4096];
        while (true) {
#ifdef Q_OS_WIN
            const int size = _read(_fileno(stdin), buffer, sizeof(buffer));
#else
            const int size = static_cast<int>(read(STDIN_FILENO, buffer, sizeof(buffer)));
#endif
            if (size <= 0) break;
            pending.append(buffer, size);
            if (pending.size() > MaxIpcBytes) std::_Exit(5);
            while (true) {
                const qsizetype newline = pending.indexOf('\n');
                if (newline < 0) break;
                const auto document = QJsonDocument::fromJson(pending.left(newline));
                pending.remove(0, newline + 1);
                if (!document.isObject()) std::_Exit(6);
                const auto message = document.object();
                if (message.value("type").toString() == QStringLiteral("cancel")) cancellations->cancel(message.value("id").toString());
                QMetaObject::invokeMethod(&worker, [&worker, message] { worker.receive(message); }, Qt::QueuedConnection);
            }
        }
        QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
    });
    const int result = app.exec();
    reader.join();
    return result;
}
