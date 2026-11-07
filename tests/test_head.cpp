#include <Windows.h>
#include <winhttp.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "winhttp.lib")

int main() {
    const wchar_t* url = L"https://kvc.pl/esp.tar";
    wchar_t host[256]{}, path[2048]{}, extra[2048]{};
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;     uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;      uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra;   uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        std::printf("WinHttpCrackUrl failed %lu\n", GetLastError());
        return 1;
    }
    HINTERNET session = WinHttpOpen(L"LinuxLive/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    std::wstring target = std::wstring(path) + extra;
    HINTERNET req = WinHttpOpenRequest(connect, L"HEAD", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof redir);

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        std::printf("Send/Receive failed %lu\n", GetLastError());
        return 2;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    ULONGLONG total = 0;
    size = sizeof total;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                        WINHTTP_HEADER_NAME_BY_INDEX, &total, &size, WINHTTP_NO_HEADER_INDEX);
    std::printf("HTTP %lu, Content-Length: %llu\n", status, total);

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return 0;
}
