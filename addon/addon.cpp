#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>

#include "Nexus.h"
#include "imgui.h"
#include "sse_parser.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr char kToggleId[] = "gw2-agent-chat.toggle";
constexpr char kWindowName[] = "GW2 Agent Chat";
constexpr std::size_t kMaxPromptBytes = 16 * 1024;
constexpr std::size_t kMaxReplyBytes = 1 << 20;
constexpr std::size_t kMaxMessages = 200;
constexpr std::size_t kMaxHistoryBytes = 2 << 20;

struct Connection {
    std::wstring host;
    INTERNET_PORT port = 0;
    std::string token;
    bool valid = false;
};

struct Message {
    std::string role;
    std::string text;
};

struct State {
    std::mutex mutex;
    std::vector<Message> messages;
    std::size_t historyBytes = 0;
    std::string status = "Bridge offline";
    std::string error;
    bool busy = false;
} state;

AddonAPI* api = nullptr;
std::atomic<bool> cancelRequested{false};
std::thread worker;
Connection connection;
bool visible = false;
std::vector<char> prompt(kMaxPromptBytes + 1);

bool IsTokenChar(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-';
}

bool StartsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool ParseConnection(std::string_view url, std::string_view token, Connection& result) {
    if (token.size() < 32 || token.size() > 128 ||
        !std::all_of(token.begin(), token.end(), IsTokenChar)) return false;
    std::string_view authority;
    std::wstring host;
    if (StartsWith(url, "http://127.0.0.1:")) {
        authority = url.substr(17);
        host = L"127.0.0.1";
    } else if (StartsWith(url, "http://[::1]:")) {
        authority = url.substr(13);
        host = L"::1";
    } else {
        return false;
    }
    if (authority.empty() || authority.find_first_not_of("0123456789") != std::string_view::npos) return false;
    unsigned int port = 0;
    const auto parsed = std::from_chars(authority.data(), authority.data() + authority.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != authority.data() + authority.size() || port == 0 || port > 65535) return false;
    result.host = std::move(host);
    result.port = static_cast<INTERNET_PORT>(port);
    result.token.assign(token);
    result.valid = true;
    return true;
}

std::string_view Trim(std::string_view value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    return value;
}

bool LoadConnection(Connection& result) {
    PWSTR appData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &appData))) return false;
    const std::filesystem::path path = std::filesystem::path(appData) / L"gw2-agent-chat" / L"connection.conf";
    CoTaskMemFree(appData);
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    file.seekg(0, std::ios::end);
    const auto length = file.tellg();
    if (length < 0 || length > 4096) return false;
    file.seekg(0, std::ios::beg);
    std::string contents(static_cast<std::size_t>(length), '\0');
    if (!contents.empty() && !file.read(contents.data(), static_cast<std::streamsize>(contents.size()))) return false;
    std::string_view rest(contents);
    std::string_view url;
    std::string_view token;
    bool gotUrl = false;
    bool gotToken = false;
    while (!rest.empty()) {
        const auto end = rest.find('\n');
        const auto line = Trim(rest.substr(0, end));
        if (StartsWith(line, "url=") && !gotUrl) {
            url = line.substr(4);
            gotUrl = true;
        } else if (StartsWith(line, "token=") && !gotToken) {
            token = line.substr(6);
            gotToken = true;
        } else if (!line.empty()) {
            return false;
        }
        if (end == std::string_view::npos) break;
        rest.remove_prefix(end + 1);
    }
    return gotUrl && gotToken && ParseConnection(url, token, result);
}

void SetStatus(std::string status, std::string error = {}) {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.status = std::move(status);
    state.error = std::move(error);
}

void AddMessageLocked(std::string role, std::string text) {
    const std::size_t size = role.size() + text.size();
    state.historyBytes += size;
    state.messages.push_back({std::move(role), std::move(text)});
    while (state.messages.size() > kMaxMessages || state.historyBytes > kMaxHistoryBytes) {
        state.historyBytes -= state.messages.front().role.size() + state.messages.front().text.size();
        state.messages.erase(state.messages.begin());
    }
}

void AddMessage(std::string role, std::string text) {
    std::lock_guard<std::mutex> lock(state.mutex);
    AddMessageLocked(std::move(role), std::move(text));
}

bool IsCancelled() {
    return cancelRequested.load(std::memory_order_relaxed);
}

std::string RequestError(HINTERNET request, bool& cancelled) {
    if (IsCancelled()) {
        cancelled = true;
        return "";
    }
    DWORD statusCode = 0;
    DWORD size = sizeof(statusCode);
    if (request) WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
    return statusCode ? "Bridge request failed (HTTP " + std::to_string(statusCode) + ")" : "Bridge connection failed";
}

void RunRequest(Connection config, std::string text) {
    HINTERNET session = WinHttpOpen(L"gw2-agent-chat/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        SetStatus("Offline", "Could not open the local bridge connection");
        std::lock_guard<std::mutex> lock(state.mutex);
        state.busy = false;
        return;
    }
    if (!WinHttpSetTimeouts(session, 2000, 2000, 2000, 2000)) {
        WinHttpCloseHandle(session);
        SetStatus("Offline", "Could not configure bridge timeouts");
        std::lock_guard<std::mutex> lock(state.mutex);
        state.busy = false;
        return;
    }
    HINTERNET server = WinHttpConnect(session, config.host.c_str(), config.port, 0);
    HINTERNET request = server ? WinHttpOpenRequest(server, L"POST", L"/message", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    bool safeRequest = request != nullptr;
    if (request) {
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        safeRequest = WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) == TRUE;
    }
    std::wstring headers = L"Authorization: Bearer ";
    for (const char c : config.token) headers.push_back(static_cast<wchar_t>(c));
    headers += L"\r\nContent-Type: text/plain; charset=utf-8\r\nAccept: text/event-stream\r\n";
    const BOOL sent = safeRequest && WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()), text.data(), static_cast<DWORD>(text.size()), static_cast<DWORD>(text.size()), 0);
    const BOOL received = sent && WinHttpReceiveResponse(request, nullptr);
    bool cancelled = false;
    if (!received) {
        const auto error = RequestError(request, cancelled);
        SetStatus(cancelled ? "Canceled" : "Offline", error);
    } else {
        DWORD statusCode = 0;
        DWORD size = sizeof(statusCode);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
        if (statusCode != 200) {
            if (IsCancelled()) SetStatus("Canceled");
            else SetStatus("Offline", "Bridge rejected the message (HTTP " + std::to_string(statusCode) + ")");
        } else {
            wchar_t contentType[128]{};
            DWORD contentTypeBytes = sizeof(contentType);
            const bool isEventStream = WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, contentType, &contentTypeBytes, WINHTTP_NO_HEADER_INDEX) &&
                                      std::wstring_view(contentType).substr(0, 17) == L"text/event-stream";
            if (!isEventStream) {
                SetStatus("Error", "Bridge response was not an event stream");
            } else {
                std::string answer;
                std::string streamError;
                bool done = false;
                gw2chat::SSEParser parser([&](gw2chat::SSEEvent&& event) {
                    if (event.name == "message") {
                        if (event.data.size() <= kMaxReplyBytes) answer = std::move(event.data);
                        else streamError = "The reply exceeded the display limit";
                    } else if (event.name == "error") {
                        streamError = event.data.empty() ? "The agent request failed" : std::move(event.data);
                    } else if (event.name == "done") {
                        done = true;
                    }
                }, kMaxReplyBytes);
                std::vector<char> buffer(4096);
                while (!done && streamError.empty() && !IsCancelled()) {
                    DWORD read = 0;
                    if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read) || !read) break;
                    if (!parser.Feed(std::string_view(buffer.data(), read))) {
                        streamError = "The reply stream was too large or invalid";
                        break;
                    }
                }
                cancelled = IsCancelled();
                if (cancelled) {
                    SetStatus("Canceled");
                } else if (!streamError.empty()) {
                    SetStatus("Error", std::move(streamError));
                } else if (!done) {
                    SetStatus("Offline", "The bridge stream ended unexpectedly");
                } else if (answer.empty()) {
                    SetStatus("Ready", "The agent returned an empty reply");
                } else {
                    AddMessage("Agent", std::move(answer));
                    SetStatus("Ready");
                }
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (server) WinHttpCloseHandle(server);
    WinHttpCloseHandle(session);
    std::lock_guard<std::mutex> lock(state.mutex);
    state.busy = false;
}

void StartRequest() {
    std::string text(prompt.data());
    if (text.empty() || text.size() > kMaxPromptBytes) return;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.busy) return;
        if (!connection.valid) {
            state.status = "Bridge offline";
            state.error = "Start the bridge, then reload the connection";
            return;
        }
        state.busy = true;
        state.status = "Working";
        state.error.clear();
        AddMessageLocked("You", text);
    }
    if (worker.joinable()) worker.join();
    cancelRequested.store(false, std::memory_order_relaxed);
    Connection config = connection;
    prompt[0] = '\0';
    try {
        worker = std::thread(RunRequest, std::move(config), std::move(text));
    } catch (const std::system_error&) {
        SetStatus("Offline", "Could not start the bridge worker");
        std::lock_guard<std::mutex> lock(state.mutex);
        state.busy = false;
    }
}

void ReloadConnection() {
    Connection next;
    const bool loaded = LoadConnection(next);
    if (loaded) connection = std::move(next);
    else connection = {};
    SetStatus(loaded ? "Configured (not checked)" : "Bridge offline", loaded ? "" : "Connection file is missing or invalid");
}

void Toggle(const char*, bool release) {
    if (!release) visible = !visible;
}

void RenderHistory() {
    std::lock_guard<std::mutex> lock(state.mutex);
    ImGui::BeginChild("##conversation", ImVec2(0, 260), true, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& message : state.messages) {
        ImGui::TextUnformatted(message.role.c_str());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(message.text.data(), message.text.data() + message.text.size());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
    }
    ImGui::EndChild();
}

void RenderMain() {
    if (!api) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(api->ImguiContext));
    if (!visible) return;
    if (!ImGui::Begin(kWindowName, &visible, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    RenderHistory();
    bool busy;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        ImGui::Text("Status: %s", state.status.c_str());
        if (!state.error.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(state.error.data(), state.error.data() + state.error.size());
            ImGui::PopTextWrapPos();
        }
        busy = state.busy;
    }
    ImGui::InputTextMultiline("##prompt", prompt.data(), prompt.size(), ImVec2(-1, 100));
    const int enterKey = ImGui::GetIO().KeyMap[ImGuiKey_Enter];
    const bool submit = enterKey >= 0 && ImGui::IsItemFocused() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(enterKey);
    if (ImGui::Button("Send") || submit) StartRequest();
    ImGui::SameLine();
    if (busy) {
        if (ImGui::Button("Cancel")) {
            cancelRequested.store(true, std::memory_order_relaxed);
            SetStatus("Canceling");
        }
    } else if (ImGui::Button("Clear display history")) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.messages.clear();
        state.historyBytes = 0;
        state.error.clear();
    }
    ImGui::End();
}

void RenderOptions() {
    if (!api) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(api->ImguiContext));
    if (ImGui::Button("Reload bridge connection")) ReloadConnection();
    ImGui::SameLine();
    std::lock_guard<std::mutex> lock(state.mutex);
    ImGui::TextUnformatted(state.status.c_str());
}

void AddonLoad(AddonAPI* addonApi) {
    api = addonApi;
    if (!api) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(api->ImguiContext));
    ImGui::SetAllocatorFunctions(reinterpret_cast<void* (*)(size_t, void*)>(api->ImguiMalloc), reinterpret_cast<void (*)(void*, void*)>(api->ImguiFree));
    if (api->Renderer.Register) {
        api->Renderer.Register(ERenderType_Render, RenderMain);
        api->Renderer.Register(ERenderType_OptionsRender, RenderOptions);
    }
    if (api->InputBinds.RegisterWithString) api->InputBinds.RegisterWithString(kToggleId, Toggle, "ALT+SHIFT+T");
    if (api->UI.RegisterCloseOnEscape) api->UI.RegisterCloseOnEscape(kWindowName, &visible);
    ReloadConnection();
}

void AddonUnload() {
    cancelRequested.store(true, std::memory_order_relaxed);
    if (worker.joinable()) worker.join();
    if (api) {
        if (api->UI.DeregisterCloseOnEscape) api->UI.DeregisterCloseOnEscape(kWindowName);
        if (api->InputBinds.Deregister) api->InputBinds.Deregister(kToggleId);
        if (api->Renderer.Deregister) {
            api->Renderer.Deregister(RenderMain);
            api->Renderer.Deregister(RenderOptions);
        }
    }
    api = nullptr;
}

AddonDefinition definition{};

}

extern "C" __declspec(dllexport) AddonDefinition* GetAddonDef() {
    definition.Signature = -73119421;
    definition.APIVersion = NEXUS_API_VERSION;
    definition.Name = "GW2 Agent Chat";
    definition.Version.Major = 0;
    definition.Version.Minor = 1;
    definition.Version.Build = 0;
    definition.Version.Revision = 1;
    definition.Author = "GW2 Agent Chat contributors";
    definition.Description = "A local read-only assistant chat panel";
    definition.Load = AddonLoad;
    definition.Unload = AddonUnload;
    definition.Provider = EUpdateProvider_None;
    return &definition;
}
