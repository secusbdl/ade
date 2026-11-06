#include "ProviderCatalog.h"
#include <QUuid>
#include <QUrl>
namespace ade {
const QList<ProviderTemplate>& ProviderCatalog::all() {
    // Endpoint/display source: Stagewise 17a0123, shared-types.ts and coding-plans.ts.
    // Protocol source: providers/official-api.ts, shared.ts, coding-plan.ts,
    // openrouter.ts and ollama.ts. Native Ollama stores the resolved /v1 endpoint.
    static const QList<ProviderTemplate> entries{
        {"codex","Coding Agents","Codex","Use the installed Codex agent and your ChatGPT sign-in.",{},"codex-app-server",{},"Subscription credentials stay with the installed Codex CLI.","codex"},
        {"glm-coding-plan","Subscriptions & Plans","GLM Coding Plan","GLM models through a Z.ai subscription.","https://api.z.ai/api/coding/paas/v4","openai","https://z.ai/manage-apikey/apikey-list","Use your Z.ai Coding Plan key. This plan has a dedicated coding endpoint.","api","https://z.ai/subscribe"},
        {"kimi-plan","Subscriptions & Plans","Kimi","Kimi models through the Moonshot platform.","https://api.moonshot.ai/v1","openai","https://platform.moonshot.ai/console/api-keys","Create a key in the Moonshot Console. Select an available model after fetching models.","api","https://platform.moonshot.ai/pricing"},
        {"qwen-plan","Subscriptions & Plans","Qwen Coding Plan","Qwen and partner models through Coding Plan.","https://coding-intl.dashscope.aliyuncs.com/v1","openai","https://modelstudio.console.alibabacloud.com/","Use your Coding Plan subscription key, commonly beginning sk-sp-. This plan is intended for interactive coding agents.","api","https://www.alibabacloud.com/help/en/model-studio/coding-plan"},
        {"qwen-token-plan","Subscriptions & Plans","Qwen Token Plan","Qwen and partner models through Token Plan.","https://token-plan.ap-southeast-1.maas.aliyuncs.com/compatible-mode/v1","openai","https://modelstudio.console.alibabacloud.com/","Defaults to Singapore. Replace the endpoint with the one in your Model Studio dashboard when it differs.","api","https://www.alibabacloud.com/help/en/model-studio/token-plan",true},
        {"minimax-plan","Subscriptions & Plans","MiniMax Token Plan","MiniMax models through a Token Plan subscription.","https://api.minimax.io/v1","openai","https://platform.minimax.io/user-center/basic-information/interface-key","Use your Token Plan key beginning sk-cp-. Test connection sends a model request; it does not check remaining plan quota.","api","https://platform.minimax.io/subscribe/token-plan"},
        {"mimo-plan","Subscriptions & Plans","Xiaomi MiMo","MiMo models through a Token Plan subscription.","https://token-plan-cn.xiaomimimo.com/v1","openai","https://platform.xiaomimimo.com/#/console/plan-manage","Use your tp- subscription key. Defaults to China; choose your account's regional endpoint if different.","api","https://platform.xiaomimimo.com/#/token-plan"},
        {"openrouter","API Keys","OpenRouter","Models from multiple providers.","https://openrouter.ai/api/v1","openai","https://openrouter.ai/keys","Get your API key at OpenRouter → Keys."},
        {"anthropic-api","API Keys","Anthropic API","Claude models.","https://api.anthropic.com/v1","anthropic","https://console.anthropic.com/settings/keys","Create a key in the Anthropic Console → Settings → API Keys."},
        {"openai-api","API Keys","OpenAI API","GPT and Codex models through the Responses API.","https://api.openai.com/v1","openai-responses","https://platform.openai.com/api-keys","Create a key in the OpenAI Platform → API keys."},
        {"google-api","API Keys","Google API","Gemini models.","https://generativelanguage.googleapis.com/v1beta","google","https://aistudio.google.com/app/apikey","Create a key in Google AI Studio → Get API key."},
        {"deepseek-api","API Keys","DeepSeek API","DeepSeek models.","https://api.deepseek.com/v1","openai","https://platform.deepseek.com/api_keys","Create a key in the DeepSeek Platform → API keys."},
        {"z-ai-api","API Keys","Z.ai API","GLM models.","https://api.z.ai/api/paas/v4","openai","https://z.ai/manage-apikey/apikey-list","Get your key at Z.ai → Manage API keys."},
        {"moonshotai-api","API Keys","Moonshot AI API","Kimi models.","https://api.moonshot.ai/v1","openai","https://platform.moonshot.ai/console/api-keys","Create a key in the Moonshot Console → API keys."},
        {"alibaba-api","API Keys","Alibaba Cloud API","Qwen models.","https://dashscope-intl.aliyuncs.com/compatible-mode/v1","openai","https://dashscope.console.aliyuncs.com/apiKey","Create a key in the DashScope Console → API-KEY."},
        {"minimax-api","API Keys","MiniMax API","MiniMax models.","https://api.minimax.io/v1","openai","https://platform.minimax.io/user-center/basic-information/interface-key","Create a key at MiniMax → Basic Information → Interface Key."},
        {"xiaomi-mimo-api","API Keys","Xiaomi MiMo API","MiMo models.","https://api.xiaomimimo.com/v1","openai","https://platform.xiaomimimo.com/#/console/plan-manage","Get your key at the MiMo Platform."},
        {"mistral-api","API Keys","Mistral API","Mistral AI models.","https://api.mistral.ai/v1","openai","https://console.mistral.ai/api-keys","Create a key in the Mistral Console → API Keys."},
        {"x-ai-api","API Keys","xAI API","Grok models.","https://api.x.ai/v1","openai","https://console.x.ai","Create a key in the xAI Console → API Keys."},
        {"ollama","Self-Hosted","Ollama","Local models through Ollama's OpenAI-compatible API.","http://localhost:11434/v1","openai",{},"Start Ollama locally, then Fetch models. This uses its /v1/models catalog; no API key is required."},
        {"custom","Custom Endpoints","Add custom endpoint","Choose a supported protocol and your own endpoint.",{},"openai",{},"Enter the endpoint, protocol and model supplied by your provider. HTTPS is required except for local HTTP servers."}
    };
    return entries;
}
const ProviderTemplate* ProviderCatalog::find(const QString& id) {
    for (const auto& entry : all()) if (entry.id == id) return &entry;
    return nullptr;
}
QStringList ProviderCatalog::groups() { return {"Coding Agents","Subscriptions & Plans","API Keys","Self-Hosted","Custom Endpoints"}; }
ProviderConfig ProviderCatalog::instantiate(const QString& id) {
    const auto* entry=find(id);if(!entry)return {};
    ProviderConfig provider;provider.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    provider.name=entry->id=="custom"?QStringLiteral("Custom provider"):entry->name;
    provider.baseUrl=entry->baseUrl;provider.protocol=entry->protocol;provider.kind=entry->kind;provider.templateId=entry->id;
    return provider;
}
bool ProviderCatalog::matches(const ProviderConfig& p,const ProviderTemplate& e) {
    if(p.templateId!=e.id||p.kind!=e.kind)return false;
    if(e.id=="custom")return true;
    if(e.configurableEndpoint) {
        const QUrl url(p.baseUrl);
        return p.protocol==e.protocol&&url.isValid()&&url.scheme()=="https"&&!url.host().isEmpty()&&url.userInfo().isEmpty()&&!url.hasQuery()&&!url.hasFragment();
    }
    return QUrl(p.baseUrl)==QUrl(e.baseUrl)&&p.protocol==e.protocol;
}
void ProviderCatalog::clearChangedProvenance(ProviderConfig& provider) {
    const auto* entry=find(provider.templateId);
    if(!entry||!matches(provider,*entry))provider.templateId.clear();
}
}
