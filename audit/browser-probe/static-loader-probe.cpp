#include <windows.h>
#include <WebView2.h>
int main() {
    LPWSTR version = nullptr;
    const HRESULT result = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    CoTaskMemFree(version);
    return FAILED(result) ? 1 : 0;
}
