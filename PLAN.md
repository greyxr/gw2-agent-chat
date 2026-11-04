# GW2 Agent Chat

Nexus C++ x64 add-on → authenticated OpenCode 1.19.5 server on loopback → optional read-only GW2 MCP server.

The DLL calls OpenCode's session, prompt, message, and abort endpoints directly from a worker thread. A random Basic-auth password is held by the OpenCode process and stored for the add-on in `%APPDATA%\gw2-agent-chat\connection.json`; the file DACL grants access only to the current user. The connection file contains no GW2 API key or provider key. `start-agent.ps1 -PrepareOnly` supports Windows ACL checks without launching OpenCode.

The add-on and OpenCode run independently of the GW2 MCP project; account/live queries are optional and require a separately installed `gw2-mcp` executable. OpenCode configuration denies all tools by default and grants the GW2 agent only its explicit read-only MCP tool allowlist. Keep shell, filesystem mutation, game input, gameplay automation, direct game-memory access, and game-chat interception outside the design. Preserve dependency licenses.

The historical Fractal/account-value request needs timestamped personal history. Do not infer historical completion or loot from current progress or account value.

## Verification

Windows CI runs the CMake/MSVC build, OpenCode protocol and authenticated chat HTTP tests, and the connection-file ACL check. The project does not ship or duplicate the separately maintained GW2 MCP server.
