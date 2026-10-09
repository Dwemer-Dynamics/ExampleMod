# AGENTS.md

Guidance for coding agents working on this example client, or on a mod copied from it.

## Keep it simple

- C++17 with the Windows SDK and the bundled nlohmann/json header. Do not add package
  managers, networking frameworks or large dependencies unless the user asks.
- Keep the file layout: game code behind `GameAdapter`, protocol code in `ai_client`,
  settings in `config`, the loop in `main.cpp`.
- Never claim the console example works inside a real game. Only claim engine support after
  testing in that engine.

## Stable boundaries

- **GameAdapter** (`src/game_adapter.h`) is the only code that touches the game. A real
  port implements it with engine APIs and leaves `ai_client` unchanged.
- **Game thread**: only the game thread calls `GameAdapter`. Network calls are blocking and
  run on worker threads; results come back through the mailbox in `main.cpp`. Real engines
  usually crash or misbehave when touched from other threads.
- **Stale replies**: the game remembers the `request_id` it waits for and ignores any other
  reply. `/cancel` clears it and tells the server. Keep both. Before applying a reply or its
  actions, `GameAdapter::isSameNpc()` must confirm the captured NPC is still there; never
  retry a failed turn automatically. An action's `args.target_id` must equal that NPC.
- **Actions**: an action runs only if it is in the client's `allowed_actions` *and* has an
  explicit handler in `applyResult()`. Never map action names to console commands, scripts,
  file paths or anything generic.
- **Protocol** (`PROTOCOL.md`, identical in the server repo): `"protocol": 1`. Check the
  reply's shape before using it, as `sendTurn()` does. Bump the version in both repos for
  incompatible changes.
- **Config**: shipped defaults in `config.default.json`; secrets only in the untracked
  `config.json`.
- **Voice is optional**: text first; on any voice failure keep the subtitle or let the
  player type, and say so. Never report voice success without a WAV file. If the user
  wants voice, start with PocketTTS for TTS and Parakeet for STT (the recommended
  DwemerDistro choices) before the supported alternatives. Do not assume they are installed
  or running: within the scope the user has authorized, verify the services are available
  and running before enabling or testing voice, then verify them on the server.
- **Third-party code**: keep `third_party/nlohmann/LICENSE.MIT`. Do not edit `json.hpp`.

## Checks before finishing

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1   # x64 and x86, must have no warnings
```

Then, with ExampleServer running in mock mode, run the table in README "Smoke checks" for
the architectures you changed. Report what ran, and what was not tested (real engine,
voice services).
