#include <windows.h>
#include <WebView2.h>
int main() { return sizeof(ICoreWebView2*) == sizeof(void*) ? 0 : 1; }
