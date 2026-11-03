# GW2 Agent Chat

A Nexus chat panel backed by an out-of-process agent using the existing GW2 MCP server. The MCP server was tested and committed as `29062be` before starting this project.

## Design

Nexus C++ x64 DLL → authenticated loopback Go bridge → OpenCode 1.19.5 → read-only GW2 MCP tools.

The DLL owns display and user text entry. Network/model/MCP work runs outside the render callback; model and GW2 API credentials stay outside the DLL. The bridge exposes a narrow text chat interface and cancellation. Game chat interception, game input generation, combat automation and direct game-memory access are outside this project.

Use Nexus API v6 and the official template-compatible ImGui revision, preserving dependency licenses. The agent has an explicit read-only MCP allowlist and denies shell, filesystem mutation and unrelated tools. Transport uses loopback only, a random local pairing token, bounded input/output and bounded shutdown. Replies are rendered as unformatted text.

The existing historical Fractal/account-value stretch query remains active. Its personal-history source and authorization are awaiting clarification; this project must not invent historical dates or treat interval account-value changes as attributed loot.

## Sources

- https://docs.raidcore.gg/addon_quickstart.html
- https://github.com/RaidcoreGG/Nexus-API
- https://github.com/RaidcoreGG/Nexus-Template-cpp
- https://opencode.ai/docs/server/
- https://opencode.ai/docs/mcp-servers/
- https://opencode.ai/docs/permissions/
- https://help.guildwars2.com/hc/en-us/articles/360013625034-Policy-Third-Party-Programs
- https://raidcore.gg/gw2/addon-policy

ArenaNet does not approve individual third-party addons. The intended design is an informational interface using Nexus services and the official account API; it is not gameplay automation.
