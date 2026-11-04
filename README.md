# GW2 Agent Chat

A Nexus add-on that sends chat prompts directly to a local OpenCode server. The add-on and OpenCode work without the separate GW2 MCP project; account-specific and live-data tools are an optional integration. The DLL never receives the model-provider or GW2 API key.

The add-on renders replies and performs OpenCode HTTP work on a worker thread. It accepts only literal IPv4 or IPv6 loopback, uses HTTP Basic authentication on every request, limits prompt and reply sizes, and supports cancellation. The OpenCode server is started with its own random password. The add-on reads that local connection from `%APPDATA%\gw2-agent-chat\connection.json`, protected by a current-user-only Windows ACL.

OpenCode keeps conversations on disk across restarts. Clearing the add-on display does not delete the OpenCode session. The add-on does not intercept game chat, generate game input, automate gameplay, or read game memory. ArenaNet does not approve this project; see its [third-party program policy](https://help.guildwars2.com/hc/en-us/articles/360013625034-Policy-Third-Party-Programs) and the [Nexus add-on policy](https://raidcore.gg/gw2/addon-policy).

## Requirements

- 64-bit Windows, Guild Wars 2 with Nexus, Visual Studio 2022 with the Desktop development with C++ workload, CMake 3.24+, and Git. Node.js/npm are needed to run OpenCode and the HTTP integration test, but not to build the DLL alone.
- An OpenCode provider configured with `opencode auth login`.
- Optional: the separately built `gw2-mcp` executable on `PATH` for read-only account and live-data queries.

OpenCode CLI 1.19.5 is pinned in `package.json` and `package-lock.json`. CMake fetches pinned Nexus API, ImGui, nlohmann-json, and cpp-httplib sources and preserves their license notices.

## Build

From this project directory:

```powershell
npm ci
cmake -S . -B build\windows -A x64 -DBUILD_TESTING=ON
cmake --build build\windows --config Release --parallel
ctest --test-dir build\windows -C Release --output-on-failure
```

The add-on is `build\windows\Release\gw2_agent_chat.dll`. Copy it to `<Guild Wars 2>\addons\gw2_agent_chat.dll` and restart the game. The development signature is provisional and is not a Nexus library registration.

## Run

Configure an OpenCode provider/model, then log in with `opencode auth login`. The chat works with general game questions without GW2 MCP. For account/live queries, install the separate MCP executable, change `mcp.gw2.enabled` to `true` in `opencode.json`, and set `mcp.gw2.command` to `gw2-mcp` or its full path. Set the GW2 key in this PowerShell session; OpenCode passes the inherited environment to the MCP process. The `gw2` agent denies tools by default and allows only the listed read-only MCP tools. These are application permissions, not an operating-system sandbox.

Start OpenCode:

For optional account access only:

```powershell
$secureKey = Read-Host "GW2 API key" -AsSecureString
$keyPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secureKey)
try { $env:GW2_API_KEY = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($keyPointer) }
finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($keyPointer) }
$secureKey.Dispose()
Remove-Variable secureKey
```

For either standalone chat or account access:

```powershell
$agent = .\start-agent.ps1
```

The script starts OpenCode 1.19.5 on `127.0.0.1:4096`, generates a fresh local password, and writes its username, password, and URL atomically to the protected connection file. It passes the password through the process environment, not command-line arguments, and clears its temporary environment variables when it returns. Use `-Port` or `-Username` to change those settings. `-PrepareOnly` creates and ACL-protects the connection file without starting OpenCode for CI verification.

Launch Guild Wars 2 with Nexus and press `Alt+Shift+T`. Reloading the connection while idle starts a fresh agent session. Ctrl+Enter sends a prompt while the input box is focused. Cancel stops the current OpenCode request; Clear display history clears only the add-on's in-memory display. Stop the server and clear the GW2 key when finished:

```powershell
Stop-Process -Id $agent.ProcessId
Remove-Item Env:GW2_API_KEY
```

Keep OpenCode and Guild Wars 2 on the same Windows host. WSL loopback forwarding is not supported by this setup.

## Tests and limits

Windows CI builds the x64 add-on with MSVC, checks the OpenCode protocol, exercises the authenticated session/message/cancel HTTP flow against a local fixture, and verifies standalone OpenCode startup, authenticated health, and the private connection-file ACL without making a model request. The same HTTP integration test runs on Linux. The add-on displays complete assistant messages when OpenCode finishes; it does not display token-by-token output. The historical Fractal/account-value question remains unsupported without timestamped personal history, and the add-on must not infer old loot from current account values.

## References

- [Nexus add-on quickstart](https://docs.raidcore.gg/addon_quickstart.html)
- [Nexus API](https://github.com/RaidcoreGG/Nexus-API)
- [OpenCode server](https://opencode.ai/docs/server/)
- [OpenCode local MCP servers](https://opencode.ai/docs/mcp-servers/)
- [OpenCode permissions](https://opencode.ai/docs/permissions/)
