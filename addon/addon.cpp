#include <windows.h>
#include <shlobj.h>

#include "Nexus.h"
#include "imgui.h"
#include "opencode_protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <exception>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr char toggleId[] = "gw2-agent-chat.toggle";
constexpr char windowName[] = "GW2 Agent Chat";
constexpr std::size_t promptLimit = 16 * 1024;
constexpr std::size_t historyLimit = 256 << 10;
constexpr std::size_t messageLimit = 200;

struct Message {
    std::string role;
    std::string text;
};

struct AppState {
    std::mutex mutex;
    std::vector<Message> messages;
    std::size_t bytes = 0;
    std::string status = "OpenCode offline";
    std::string error;
    std::string sessionId;
    bool busy = false;
    bool scrollToEnd = false;
} state;

AddonAPI* api = nullptr;
std::atomic<bool> cancel{false};
std::thread worker;
gw2chat::Connection connection;
bool visible = false;
bool refocus = false;
std::array<char, promptLimit + 1> prompt{};

bool ReadConnection(gw2chat::Connection& out) {
    PWSTR appData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &appData))) return false;
    const auto path = std::filesystem::path(appData) / L"gw2-agent-chat" / L"connection.json";
    CoTaskMemFree(appData);
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::string contents(4097, '\0');
    file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (file.bad() || file.gcount() > 4096) return false;
    contents.resize(static_cast<std::size_t>(file.gcount()));
    return gw2chat::ParseConnection(contents, out);
}

void AddMessageLocked(std::string role, std::string text) {
    state.bytes += role.size() + text.size();
    state.messages.push_back({std::move(role), std::move(text)});
    state.scrollToEnd = true;
    while (state.messages.size() > messageLimit || state.bytes > historyLimit) {
        state.bytes -= state.messages.front().role.size() + state.messages.front().text.size();
        state.messages.erase(state.messages.begin());
    }
}

void Finish(std::string status, std::string error = {}, std::string answer = {}, bool newSession = false) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (newSession) AddMessageLocked("Note", "Started a new session; earlier context is not available");
    if (!answer.empty()) AddMessageLocked("Agent", std::move(answer));
    state.status = std::move(status);
    state.error = std::move(error);
    state.busy = false;
}

void SetStatus(std::string status, std::string error = {}) {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.status = std::move(status);
    state.error = std::move(error);
}

void RunRequest(gw2chat::Connection config, std::string text) {
    try {
        std::string sessionId;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            sessionId = state.sessionId;
        }
        const auto previous = sessionId;
        auto reply = gw2chat::Prompt(config, text, sessionId, cancel);
        const bool newSession = !previous.empty() && !sessionId.empty() && sessionId != previous;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.sessionId = std::move(sessionId);
        }
        if (reply.text.empty() && cancel.load()) Finish("Canceled", {}, {}, newSession);
        else if (reply.error.empty()) Finish("Ready", {}, std::move(reply.text), newSession);
        else Finish(reply.offline ? "OpenCode offline" : "Error", std::move(reply.error), std::move(reply.text), newSession);
    } catch (const std::exception&) {
        Finish("Error", "OpenCode request failed");
    }
}

void StartRequest() {
    std::string text(prompt.data());
    if (text.empty() || text.size() > promptLimit) return;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.busy) return;
    }
    gw2chat::Connection latest;
    const bool loaded = ReadConnection(latest);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (loaded && !(latest == connection)) {
            connection = std::move(latest);
            state.sessionId.clear();
        }
        if (!connection) {
            state.status = "OpenCode offline";
            state.error = "Run start-agent.ps1 to start OpenCode, then send again";
            return;
        }
        state.busy = true;
        state.status = "Working";
        state.error.clear();
        AddMessageLocked("You", text);
    }
    if (worker.joinable()) worker.join();
    cancel.store(false);
    prompt[0] = '\0';
    refocus = true;
    try { worker = std::thread(RunRequest, connection, std::move(text)); }
    catch (const std::system_error&) { Finish("Error", "Could not start the OpenCode worker"); }
}

void ReloadConnection() {
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.busy) return;
        state.sessionId.clear();
    }
    gw2chat::Connection next;
    const bool loaded = ReadConnection(next);
    connection = loaded ? std::move(next) : gw2chat::Connection{};
    SetStatus(loaded ? "Configured (not checked)" : "OpenCode offline",
              loaded ? "" : "Connection file is missing or invalid");
}

void Toggle(const char*, bool release) {
    if (!release) visible = !visible;
}

void RenderMain() {
    if (!api) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(api->ImguiContext));
    if (!visible) return;
    ImGui::SetNextWindowSize(ImVec2(520, 440), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(windowName, &visible)) { ImGui::End(); return; }
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        const float below = 100 + ImGui::GetFrameHeightWithSpacing() * (state.error.empty() ? 3 : 5);
        ImGui::BeginChild("##conversation", ImVec2(0, std::max(80.0f, ImGui::GetContentRegionAvail().y - below)), true);
        for (const auto& message : state.messages) {
            ImGui::TextUnformatted(message.role.c_str());
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(message.text.data(), message.text.data() + message.text.size());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
        }
        if (state.scrollToEnd) ImGui::SetScrollHereY(1.0f);
        state.scrollToEnd = false;
        ImGui::EndChild();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::Text("Status: %s", state.status.c_str());
        if (!state.error.empty()) ImGui::TextUnformatted(state.error.data(), state.error.data() + state.error.size());
        ImGui::PopTextWrapPos();
    }
    if (refocus) ImGui::SetKeyboardFocusHere();
    refocus = false;
    ImGui::InputTextMultiline("##prompt", prompt.data(), prompt.size(), ImVec2(-1, 100));
    const int enter = ImGui::GetIO().KeyMap[ImGuiKey_Enter];
    const bool submit = enter >= 0 && ImGui::IsItemFocused() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(enter);
    if (ImGui::Button("Send") || submit) StartRequest();
    ImGui::SameLine();
    bool busy;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        busy = state.busy;
    }
    if (busy) {
        if (ImGui::Button("Cancel")) {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.busy) {
                cancel.store(true);
                state.status = "Canceling";
            }
        }
    } else if (ImGui::Button("New chat")) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.messages.clear();
        state.bytes = 0;
        state.error.clear();
        state.sessionId.clear();
    }
    ImGui::End();
}

void RenderOptions() {
    if (!api) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(api->ImguiContext));
    if (ImGui::Button("Reload OpenCode connection")) ReloadConnection();
    ImGui::SameLine();
    std::lock_guard<std::mutex> lock(state.mutex);
    ImGui::TextUnformatted(state.status.c_str());
}

void AddonLoad(AddonAPI* addonApi) {
    api = addonApi;
    if (!api) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(api->ImguiContext));
    ImGui::SetAllocatorFunctions(reinterpret_cast<void* (*)(size_t, void*)>(api->ImguiMalloc),
                                 reinterpret_cast<void (*)(void*, void*)>(api->ImguiFree));
    ReloadConnection();
    api->Renderer.Register(ERenderType_Render, RenderMain);
    api->Renderer.Register(ERenderType_OptionsRender, RenderOptions);
    api->InputBinds.RegisterWithString(toggleId, Toggle, "ALT+SHIFT+T");
    api->UI.RegisterCloseOnEscape(windowName, &visible);
}

void AddonUnload() {
    if (api) {
        api->UI.DeregisterCloseOnEscape(windowName);
        api->InputBinds.Deregister(toggleId);
        api->Renderer.Deregister(RenderMain);
        api->Renderer.Deregister(RenderOptions);
    }
    api = nullptr;
    cancel.store(true);
    if (worker.joinable()) worker.join();
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
