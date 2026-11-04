#include <windows.h>
#include <shlobj.h>

#include "Nexus.h"
#include "imgui.h"
#include "opencode_protocol.hpp"

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
constexpr std::size_t historyLimit = 2 << 20;
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
} state;

AddonAPI* api = nullptr;
std::atomic<bool> cancel{false};
std::thread worker;
gw2chat::Connection connection;
bool visible = false;
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
    while (state.messages.size() > messageLimit || state.bytes > historyLimit) {
        state.bytes -= state.messages.front().role.size() + state.messages.front().text.size();
        state.messages.erase(state.messages.begin());
    }
}

void Finish(std::string status, std::string error = {}, std::string answer = {}) {
    std::lock_guard<std::mutex> lock(state.mutex);
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
        auto reply = gw2chat::Prompt(config, text, sessionId, cancel);
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.sessionId = std::move(sessionId);
        }
        if (cancel.load()) Finish("Canceled");
        else if (!reply.error.empty()) Finish(reply.offline ? "OpenCode offline" : "Error", std::move(reply.error));
        else if (reply.text.empty()) Finish("Ready", "The agent returned an empty reply");
        else Finish("Ready", {}, std::move(reply.text));
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
        if (!connection) {
            state.status = "OpenCode offline";
            state.error = "Start OpenCode, then reload the connection";
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
    try { worker = std::thread(RunRequest, connection, std::move(text)); }
    catch (const std::system_error&) { Finish("Offline", "Could not start the OpenCode worker"); }
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
    if (!ImGui::Begin(windowName, &visible, ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
    {
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
        ImGui::Text("Status: %s", state.status.c_str());
        if (!state.error.empty()) ImGui::TextUnformatted(state.error.data(), state.error.data() + state.error.size());
    }
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
        if (ImGui::Button("Cancel")) { cancel.store(true); SetStatus("Canceling"); }
    } else if (ImGui::Button("Clear display history")) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.messages.clear();
        state.bytes = 0;
        state.error.clear();
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
    api->Renderer.Register(ERenderType_Render, RenderMain);
    api->Renderer.Register(ERenderType_OptionsRender, RenderOptions);
    api->InputBinds.RegisterWithString(toggleId, Toggle, "ALT+SHIFT+T");
    api->UI.RegisterCloseOnEscape(windowName, &visible);
    ReloadConnection();
}

void AddonUnload() {
    cancel.store(true);
    if (worker.joinable()) worker.join();
    if (api) {
        api->UI.DeregisterCloseOnEscape(windowName);
        api->InputBinds.Deregister(toggleId);
        api->Renderer.Deregister(RenderMain);
        api->Renderer.Deregister(RenderOptions);
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
