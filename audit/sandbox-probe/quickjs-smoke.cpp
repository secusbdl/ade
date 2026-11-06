#include <quickjs.h>
#include <cstdio>
#include <cstring>
int main() {
    JSRuntime* runtime = JS_NewRuntime();
    JS_SetMemoryLimit(runtime, 32 * 1024 * 1024);
    JSContext* context = JS_NewContext(runtime);
    const char* source = "(async () => { globalThis.saved = 41; return await Promise.resolve(saved + 1); })()";
    JSValue promise = JS_Eval(context, source, std::strlen(source), "smoke.js", JS_EVAL_TYPE_GLOBAL);
    JSContext* pendingContext = nullptr;
    while (JS_ExecutePendingJob(runtime, &pendingContext) > 0) {}
    JSValue result = JS_PromiseResult(context, promise);
    int value = 0;
    JS_ToInt32(context, &value, result);
    std::printf("QuickJS native async/persistent-global probe: %d\n", value);
    JS_FreeValue(context, result);
    JS_FreeValue(context, promise);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return value == 42 ? 0 : 1;
}
