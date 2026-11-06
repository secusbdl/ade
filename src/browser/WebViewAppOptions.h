#pragma once
#ifdef Q_OS_WIN
#include <WebView2.h>
#include <QString>
#include <QStringList>
#include <atomic>
#include <cstring>
#include <vector>

namespace ade::webview {
inline HRESULT copyString(const QString& text,LPWSTR* result) {
    if(!result)return E_POINTER;
    *result=nullptr;
    const auto value=text.toStdWString();const size_t bytes=(value.size()+1)*sizeof(wchar_t);
    *result=static_cast<LPWSTR>(CoTaskMemAlloc(bytes));if(!*result)return E_OUTOFMEMORY;
    std::memcpy(*result,value.c_str(),bytes);return S_OK;
}
class Scheme final:public ICoreWebView2CustomSchemeRegistration {
public:
    explicit Scheme(QString name):m_name(std::move(name)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {if(!out)return E_POINTER;*out=nullptr;if(id==IID_IUnknown||id==IID_ICoreWebView2CustomSchemeRegistration){*out=static_cast<ICoreWebView2CustomSchemeRegistration*>(this);AddRef();return S_OK;}return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++m_refs;}
    ULONG STDMETHODCALLTYPE Release() override{const auto left=--m_refs;if(!left)delete this;return left;}
    HRESULT STDMETHODCALLTYPE get_SchemeName(LPWSTR* value) override{return copyString(m_name,value);}
    HRESULT STDMETHODCALLTYPE get_TreatAsSecure(BOOL* value) override{if(!value)return E_POINTER;*value=m_secure;return S_OK;}
    HRESULT STDMETHODCALLTYPE put_TreatAsSecure(BOOL value) override{m_secure=value;return S_OK;}
    HRESULT STDMETHODCALLTYPE get_HasAuthorityComponent(BOOL* value) override{if(!value)return E_POINTER;*value=m_authority;return S_OK;}
    HRESULT STDMETHODCALLTYPE put_HasAuthorityComponent(BOOL value) override{m_authority=value;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetAllowedOrigins(UINT32* count,LPWSTR** origins) override {
        if(!count||!origins)return E_POINTER;
        *count=0;*origins=nullptr;
        auto* result=static_cast<LPWSTR*>(CoTaskMemAlloc(sizeof(LPWSTR)*m_origins.size()));if(!result)return E_OUTOFMEMORY;
        for(qsizetype i=0;i<m_origins.size();++i)if(FAILED(copyString(m_origins[i],&result[i]))){for(qsizetype j=0;j<i;++j)CoTaskMemFree(result[j]);CoTaskMemFree(result);return E_OUTOFMEMORY;}
        *count=static_cast<UINT32>(m_origins.size());*origins=result;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetAllowedOrigins(UINT32 count,LPCWSTR* origins) override {if(count&&!origins)return E_POINTER;QStringList values;for(UINT32 i=0;i<count;++i){if(!origins[i])return E_INVALIDARG;values.append(QString::fromWCharArray(origins[i]));}m_origins=values;return S_OK;}
private:
    std::atomic<ULONG> m_refs{1};QString m_name;BOOL m_secure=TRUE,m_authority=TRUE;
    QStringList m_origins{QStringLiteral("app://plugins"),QStringLiteral("app://agents"),QStringLiteral("ade-app://preview")};
};
class AppOptions final:public ICoreWebView2EnvironmentOptions,public ICoreWebView2EnvironmentOptions4 {
public:
    AppOptions(){m_schemes.push_back(new Scheme(QStringLiteral("app")));m_schemes.push_back(new Scheme(QStringLiteral("ade-app")));}
    ~AppOptions(){for(auto* scheme:m_schemes)scheme->Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {if(!out)return E_POINTER;*out=nullptr;if(id==IID_IUnknown||id==IID_ICoreWebView2EnvironmentOptions)*out=static_cast<ICoreWebView2EnvironmentOptions*>(this);else if(id==IID_ICoreWebView2EnvironmentOptions4)*out=static_cast<ICoreWebView2EnvironmentOptions4*>(this);else return E_NOINTERFACE;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++m_refs;}
    ULONG STDMETHODCALLTYPE Release() override{const auto left=--m_refs;if(!left)delete this;return left;}
    HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR* value) override{return copyString(m_arguments,value);}
    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR value) override{m_arguments=QString::fromWCharArray(value);return S_OK;}
    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* value) override{return copyString(m_language,value);}
    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR value) override{m_language=QString::fromWCharArray(value);return S_OK;}
    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR* value) override{return copyString(m_version,value);}
    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR value) override{m_version=QString::fromWCharArray(value);return S_OK;}
    HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL* value) override{if(!value)return E_POINTER;*value=FALSE;return S_OK;}
    HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL value) override{return value?E_ACCESSDENIED:S_OK;}
    HRESULT STDMETHODCALLTYPE GetCustomSchemeRegistrations(UINT32* count,ICoreWebView2CustomSchemeRegistration*** registrations) override {
        if(!count||!registrations)return E_POINTER;
        *count=0;*registrations=nullptr;
        auto** result=static_cast<ICoreWebView2CustomSchemeRegistration**>(CoTaskMemAlloc(sizeof(ICoreWebView2CustomSchemeRegistration*)*m_schemes.size()));if(!result)return E_OUTOFMEMORY;
        for(size_t i=0;i<m_schemes.size();++i){result[i]=m_schemes[i];result[i]->AddRef();}*count=static_cast<UINT32>(m_schemes.size());*registrations=result;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCustomSchemeRegistrations(UINT32 count,ICoreWebView2CustomSchemeRegistration** registrations) override {
        if(count&&!registrations)return E_POINTER;
        for(UINT32 i=0;i<count;++i)if(!registrations[i])return E_INVALIDARG;
        std::vector<ICoreWebView2CustomSchemeRegistration*> next;for(UINT32 i=0;i<count;++i){registrations[i]->AddRef();next.push_back(registrations[i]);}for(auto* scheme:m_schemes)scheme->Release();m_schemes=std::move(next);return S_OK;
    }
private:
    // Runtime product version, not the NuGet SDK package version.
    std::atomic<ULONG> m_refs{1};QString m_arguments,m_language,m_version=QStringLiteral("156.0.4314.35");
    std::vector<ICoreWebView2CustomSchemeRegistration*> m_schemes;
};
}
#endif
