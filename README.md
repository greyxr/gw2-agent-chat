# GW2 Agent Chat

GW2 Agent Chat is a Nexus add-on that displays an AI chat panel in Guild Wars 2. It sends user-entered prompts to a small local Go bridge, which connects to OpenCode and the read-only GW2 MCP server. The add-on does not contain an OpenCode provider key or GW2 API key.

## What it does

- Shows a chat panel with a configurable Nexus toggle key (default `Alt+Shift+T`), Send, Ctrl+Enter, Cancel and Clear display history controls.
- Sends prompts to the local bridge over loopback HTTP with a random pairing token.
- Uses Nexus's ImGui context and keybind service. Network work runs outside the render callback.
- Displays the complete assistant response when it arrives. The current bridge returns one final SSE message; it does not stream model tokens into the panel.
- Keeps at most 200 messages and 2 MiB of conversation text in the add-on. Clear display history removes only that visible in-memory copy.

This is an AI-assisted add-on. OpenCode sends prompts and eligible GW2 MCP tool results to the model provider configured by the user. Review that provider's data handling before use. The add-on does not intercept game chat, generate game input, automate gameplay, or read game memory. Guild Wars 2 has no official add-on interface; ArenaNet does not approve this project. See the [third-party program policy](https://help.guildwars2.com/hc/en-us/articles/360013625034-Policy-Third-Party-Programs) and [Nexus add-on policy](https://raidcore.gg/gw2/addon-policy).

OpenCode keeps its session and messages on disk across restarts. Clearing the add-on display does not delete the OpenCode session; OpenCode's Windows data directory is `%USERPROFILE%\.local\share\opencode`. See [OpenCode storage details](https://dev.opencode.ai/docs/troubleshooting/).

## Windows prerequisites

- 64-bit Windows, Guild Wars 2 with Nexus installed, and Visual Studio 2022 with the Desktop development with C++ workload.
- CMake 3.24 or later, Go 1.25 or later, Node.js/npm, and Git.
- The existing GW2 MCP server source. It is a separate project and is not copied into this repository.
- An OpenCode provider configured with `opencode auth login`.

OpenCode CLI `1.19.5` is pinned by `package.json` and `package-lock.json`. The build downloads Nexus-API v6.1 and the template-pinned ImGui 1.80 source by commit SHA. Keep their license notices with redistributed builds.

## Build

Build the companion MCP executable from its existing source tree:

```powershell
Set-Location C:\src\gw2_test
New-Item -ItemType Directory -Force .\bin | Out-Null
go build -o .\bin\gw2-mcp.exe .\cmd\gw2-mcp
```

In `gw2-agent-chat\opencode.json`, set the `mcp.gw2.command` entry to the absolute path of that executable and add the environment mapping below. Do not put the API key itself in this file:

```json
"mcp": {
  "gw2": {
    "type": "local",
    "command": ["C:\\src\\gw2_test\\bin\\gw2-mcp.exe"],
    "environment": { "GW2_API_KEY": "{env:GW2_API_KEY}" },
    "enabled": true,
    "timeout": 30000
  }
}
```

Then install the pinned OpenCode package and build the add-on and bridge:

```powershell
Set-Location C:\src\gw2-agent-chat
npm ci
cmake -S . -B build\windows -A x64 -DBUILD_TESTING=ON
cmake --build build\windows --config Release --parallel
ctest --test-dir build\windows -C Release --output-on-failure
go test ./...
go build -o .\build\gw2-chat-bridge.exe .\cmd\gw2-chat-bridge
```

The resulting add-on is `build\windows\Release\gw2_agent_chat.dll`. Copy it to `<Guild Wars 2>\addons\gw2_agent_chat.dll` and restart the game. The signature in this build is a provisional development identifier; it is not a Nexus library registration.

## Run

Run these commands from the `gw2-agent-chat` project root in one PowerShell window. OpenCode and the bridge inherit the environment from this window. The random OpenCode password stays in process environment; it is not written to the add-on or project config.

```powershell
$secureKey = Read-Host "GW2 API key" -AsSecureString
$keyPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secureKey)
try {
    $env:GW2_API_KEY = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($keyPointer)
} finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($keyPointer)
}
$secureKey.Dispose()
Remove-Variable secureKey

$passwordBytes = New-Object byte[] 32
$random = [Security.Cryptography.RandomNumberGenerator]::Create()
$random.GetBytes($passwordBytes)
$env:OPENCODE_SERVER_PASSWORD = [Convert]::ToBase64String($passwordBytes)
$random.Dispose()
[Array]::Clear($passwordBytes, 0, $passwordBytes.Length)
$env:OPENCODE_SERVER_USERNAME = "opencode"

& .\node_modules\opencode-ai\bin\opencode.exe auth login
$agentServer = Start-Process `
    -FilePath (Resolve-Path .\node_modules\opencode-ai\bin\opencode.exe) `
    -ArgumentList @("serve", "--hostname", "127.0.0.1", "--port", "4096") `
    -WorkingDirectory (Get-Location).Path `
    -PassThru

& .\build\gw2-chat-bridge.exe -listen 127.0.0.1:47153 -agent-url http://127.0.0.1:4096
```

The bridge writes a random-token `connection.conf` under `%APPDATA%\gw2-agent-chat\`. The add-on reads this same user configuration path directly; no copy into the game directory is needed. The Windows file ACL grants access to the current user. The add-on v0.1 reads this default location, so a custom `--connection-file` path will not be discovered by it.

Keep the OpenCode server and bridge on the same Windows host for this first version. Using a bridge or OpenCode server inside WSL requires explicit loopback forwarding and is not a supported seamless setup.

Launch Guild Wars 2 with Nexus and press `Alt+Shift+T`. The panel can show `Configured (not checked)`, `Working`, `Ready`, `Canceled`, `Offline`, or an error. Ctrl+Enter sends a prompt while the input box is focused. Cancel closes the bridge request; Clear display history removes only the add-on's in-memory conversation.

Stop the bridge with Ctrl+C. Then stop the OpenCode server and clear this shell's credentials:

```powershell
Stop-Process -Id $agentServer.Id
Remove-Item Env:OPENCODE_SERVER_PASSWORD, Env:OPENCODE_SERVER_USERNAME, Env:GW2_API_KEY
```

## Tests and current limits

Windows CI builds the x64 DLL with MSVC, runs the portable SSE parser test, and runs `go test ./...` on Windows, including the connection-file ACL test. The native parser test can also be run with CTest as shown above.

The DLL and bridge have compiled for Windows with the Zig cross toolchain, and the parser and Go tests pass. There is no Wine runtime here, and no in-game Nexus runtime validation has been performed yet. Responses are delivered as a complete SSE message rather than incremental model output. The add-on signature is provisional. The historical Fractal/account-value request remains blocked on its personal-history source and authorization; this project must not infer historic loot from current account values.

## Dependencies and licensing

- Nexus-API v6.1 is MIT licensed; ImGui is MIT licensed. CMake install includes their license texts under `licenses/`.
- The pinned OpenCode npm package is MIT licensed and retains its own license in the installed package.
- The bridge uses golang.org/x/sys under its BSD license; its notice is in `licenses/golang-x-sys.txt` and the CMake install package. The existing GW2 MCP source is a separate project; follow its included license notices.
- This repository has no project license file. No license is asserted for this project's original source.

## References

- [Nexus Add-on Quickstart](https://docs.raidcore.gg/addon_quickstart.html)
- [Nexus Keybinding Tutorial](https://docs.raidcore.gg/keybinds.html)
- [Nexus C++ template](https://github.com/RaidcoreGG/Nexus-Template-cpp)
- [OpenCode server](https://opencode.ai/docs/server/)
- [OpenCode local MCP servers](https://opencode.ai/docs/mcp-servers/)
- [OpenCode configuration](https://opencode.ai/docs/config/)
- [ArenaNet third-party program policy](https://help.guildwars2.com/hc/en-us/articles/360013625034-Policy-Third-Party-Programs)
