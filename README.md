# ExampleMod

A Windows console "fake game" that talks to
**[ExampleServer](https://github.com/Dwemer-Dynamics/ExampleServer)**. It shows how a game
mod can send what the player says to an NPC, show the reply as a subtitle, and run one
allowlisted action (`follow_player`), with optional voice.

This is a reference, **not** a plugin for any real game: it has no engine SDK and no game
hooks. To make a real mod you replace the console `GameAdapter` with your engine's APIs.
It is built separately; the DwemerDistro launcher installs only the server.

## Features

- C++17, CMake, Windows x64 and x86. Uses built-in WinHTTP; no installed dependencies.
- JSON via nlohmann/json 3.11.2 (single header, MIT) in `third_party/nlohmann/`.
- One-shot commands and an interactive mode: talk, game events, NPC selection, voice.
- Game thread and worker threads kept apart, stale-reply checks, cancellation and a
  client-side action allowlist: the patterns a real engine port must keep.

## Documentation

| Read | For |
|---|---|
| [ExampleServer START_HERE.md](https://github.com/Dwemer-Dynamics/ExampleServer/blob/main/START_HERE.md) | **Start here.** Document map, feature status, end-to-end walkthrough, troubleshooting by request ID |
| [ExampleServer SETUP.md](https://github.com/Dwemer-Dynamics/ExampleServer/blob/main/SETUP.md) | Installing the server, building and pairing this client |
| [PROTOCOL.md](PROTOCOL.md) | The request and reply contract (identical in both repositories) |
| [AGENT_PLAYBOOK.md](AGENT_PLAYBOOK.md) | Prompt for porting to your engine with a coding agent |
| [AGENTS.md](AGENTS.md) | Rules for coding agents |
| This README | Commands, settings, smoke checks and limits |

## Fast start

1. Install and start ExampleServer first (its
   [SETUP.md](https://github.com/Dwemer-Dynamics/ExampleServer/blob/main/SETUP.md) covers
   both repos).
2. Clone and build (Visual Studio 2022 with C++ tools):

   ```powershell
   git clone https://github.com/Dwemer-Dynamics/ExampleMod.git
   cd ExampleMod
   powershell -ExecutionPolicy Bypass -File scripts\build.ps1
   ```

   `main` is the stable baseline; `dev` holds development work (`git clone -b dev ...`).

   Output goes to `%LOCALAPPDATA%\DwemerDynamics\ExampleMod\build\x64\Release\example_mod.exe` and
   `...\x86\Release\example_mod.exe`. Change with `-BuildRoot <folder>`.

3. Next to `example_mod.exe`, put a `config.json` with the server token (git-ignored).
   The server's `scripts/pair_client.php` writes one privately (ExampleServer
   [SETUP.md, "Connect them"](https://github.com/Dwemer-Dynamics/ExampleServer/blob/main/SETUP.md#3-connect-them));
   copy that file here. Or write it by hand:

   ```json
   { "token": "paste-the-token-from-the-server-config" }
   ```

4. Run:

   ```text
   example_mod.exe --health
   example_mod.exe --say "Follow me"
   example_mod.exe --decide "Scout, is the trail safe?"   (optional NPC selection, prints only)
   example_mod.exe --event item_given "The player gave a lantern."   (game event, log only)
   example_mod.exe --react item_given "The player gave a lantern."   (game event the Guide reacts to)
   example_mod.exe                  (interactive: type lines and the commands below)
   ```

   Interactive commands: `/cancel`, `/listen <wav>`, `/decide <text>`, `/event <type> <text>`
   (log only), `/react <type> <text>` (the NPC you face reacts), `/npc guide|scout` (face
   another fake NPC), `/session <id>` (switch session, like loading another save), `/unload`,
   `/load`, `/quit`. Event types: `location_entered`, `item_given`, `combat_started`,
   `combat_ended`; text 1-300 characters. Events are sent only when you type them; nothing
   is sent automatically, and the console is not a game engine.

   Switching NPC or session, unloading the NPC or quitting while a reply is pending drops
   that reply and its actions and sends a cancel to the server in the session it was sent
   in, so the server saves no history for it once that cancel arrives. A cancel that fails
   (for example a network error) is reported and not retried; the server may then still
   save that exchange. Requests already out keep the settings they started with.
   `/session` never reads or changes game saves; match server checkpoints to it by hand
   (`php scripts/checkpoint.php list <id>`) or on the dashboard (**Roleplay >
   Checkpoints**). Quit waits for every request still out (at most four normal and two
   cancel requests, see [Limits](#limits)), each bounded by `timeout_seconds`, and applies
   none of their replies.

## Settings

`config.default.json` (shipped) is loaded first, then your `config.json` (or the file given
with `--config <file>`) overrides any key it sets.

| Key | Default | Meaning |
|---|---|---|
| `server_url` | `http://127.0.0.1:8081/ExampleServer` | Where the server is. |
| `token` | empty | Required. From the server's `config/config.php`. |
| `session_id` | `demo-save-1` | Keeps NPC history apart, e.g. one per save game. |
| `timeout_seconds` | `40` | HTTP timeout per phase (connect, send, waiting for the reply headers, each read), 1-300. WinHTTP checks it coarsely (a 1 second setting fired after about 2-4 seconds in tests), so a call can block longer; any reply that completes after `timeout_seconds` is discarded with an error, never applied. A timed-out turn is never retried. |
| `voice_enabled` | `false` | Ask the server for a WAV of each reply. |
| `allowed_actions` | `["follow_player"]` | Actions this game accepts. Set `[]` to see rejection. |

## Files

| File | Purpose |
|---|---|
| `src/game_adapter.h` | **The game boundary.** The only interface that touches the game. |
| `src/console_game.h` | Fake console implementation of `GameAdapter`. |
| `src/ai_client.h/.cpp` | HTTP + JSON client for the protocol. Blocking; used from worker threads. |
| `src/config.h/.cpp` | Loads the two config files. |
| `src/main.cpp` | Game loop, mailbox between threads, stale-reply and action checks, command line. |

## Smoke checks

With the server in mock mode:

| Command | Expect |
|---|---|
| `example_mod.exe --version` | `example_mod 0.1.0 (baseline-1, protocol 1)`, exit 0; needs no config or token. Change the version only in `project(... VERSION)` in `CMakeLists.txt`. |
| `example_mod.exe --health` | `Server is ready.`, then `Versions: client 0.1.0, client baseline baseline-1, server 0.1.0, server baseline baseline-1, schema 007_connector_calls, protocol 1.`, exit 0 |
| `--event location_entered "The player entered the market."` | `Event location_entered logged by the server (log only; no reply or actions; request evt-...).` |
| `--react item_given "The player gave a lantern. follow me"` | Guide reply, follow action and result report, like `--say` |
| `--event dragon_seen "..."` | refused by the client, nothing sent, exit 1 |
| interactive, mock delay 2+: `follow me`, then `/npc scout` | `Dropped the pending reply for Guide: you turned to another NPC.`, cancel confirmed, `Ignored a stale reply` |
| interactive, mock delay 2+: `follow me`, then `/session other-save` | dropped and cancelled in the old session; new lines use `other-save` |
| interactive, mock delay 2+: `follow me`, then `/quit` | `Quitting: dropped the pending reply ...`, then the cancel is confirmed |
| `example_mod.exe --say "hello"` | `[Guide] Guide heard you say: hello`, then `Trace: turn req-... (ok)` |
| `example_mod.exe --say "follow me"` | reply, then `npc_guide is now following you (simulated)` and `Result reported to server: follow_player handled (...)` |
| `--health` against an older server | `Server is ready.`, `server version not reported (older server)` and/or `server baseline not reported (older server)`, and on very old servers a `Note:` that name-only actions still work |
| interactive, mock delay 2+: `follow me`, then `/unload` | `Dropped the pending reply for Guide: Guide is no longer here.`, then `Ignored a stale reply`; no result report |
| interactive: `/unload` and `/load` together while waiting | the new instance does not get the old reply |
| same against a server without `result.php` | reply and action, then `Result reports off: this server has no result.php.` |
| same with `"allowed_actions": []` | `Rejected action not allowed by this mod: follow_player` |
| `example_mod.exe --say "dance"` | reply only; the server rejected `dance` |
| wrong token | `AI unavailable: HTTP 401 unauthorized: ...`, then `Trace: turn req-... (failed)`, exit 1 |
| server stopped | `AI unavailable: cannot reach ...`, exit 1 |
| `"hello","/cancel" \| example_mod.exe` with `mock_delay_seconds` 2+ | `Ignored a stale reply` |
| `example_mod.exe --decide "Scout, is the trail safe?"` | `Decision: npc_guide (baseline, disabled, request dec-...)` |
| same with server `decision` enabled, provider `mock` | `Decision: npc_scout (provider, matched, request dec-...)` |
| `--say "what is your profile?"` after assigning `npc_guide` a profile on the server | `My profile is <name>.` |

## Request ids (tracing)

Every request has an id the client prints, on success and on failure, so it can be found in
the server's Logs, Connector calls and error log (`example-ai [<id>]: ...`):

- turns `req-...` (one line per turn: `Trace: turn req-..., speech-to-text stt-..., voice tts-... (ok)`),
- game events `evt-...`, NPC selection `dec-...` (in the `Decision:` line).

Voice requests send their turn's id as the parent (`parent_request_id` for `speak.php`, the
`X-Parent-Request-Id` header for `listen.php`), and `speak.php` also gets the NPC id so the
server can use that NPC's profile voice. When a reply names a `request_id` that is not the
one sent, the client ignores the reply. Older servers ignore the extra fields and send no id
back; the client still works. Profiles and memory need no client change.

## Optional result reports

After the game thread applies a reply's actions, the client queues one optional report on a
worker thread to the server's `result.php`: each returned action and whether this client
handled or rejected it. The server keeps it as untrusted client input for its Logs page.
It is sent only for the reply the game used (never for an ignored stale reply), never
changes the reply or actions, and stops for the run when the server has no `result.php`.
The console's "handled" means its simulated handler ran, not a real game engine.

## Optional NPC selection

`--decide <text>` and `/decide <text>` send the line with two fictional candidates,
`npc_guide` (Guide, the baseline) and `npc_scout` (Scout), to the server's `decision.php`
and print which one it chose and why. It is display only: the client does not switch the
NPC it talks to and runs no actions. Server settings are in ExampleServer `CONNECTORS.md`.

## Voice examples

Voice is off on both sides by default. The server must have `tts`/`stt` enabled and its
voice services running: PocketTTS (audio.cpp) for speech, and Parakeet (default) or
faster-whisper for transcription. They are started from the DwemerDistro launcher and set
up as described in [CONNECTORS.md](https://github.com/Dwemer-Dynamics/ExampleServer/blob/main/CONNECTORS.md#voice). The client needs
`"voice_enabled": true` only for replies spoken as WAV; `--listen` works without it.

| Command | Expect |
|---|---|
| `--say "follow me"` with `"voice_enabled": true` | reply, action, `Voice saved to ...\dwemer-ai-example\<id>.wav`, `Trace: turn req-..., voice tts-... (ok)` |
| `--listen C:\path\speech.wav` (or `/listen` interactive) | `You said: ...`, then the reply |
| TTS failing | reply text plus `Voice unavailable, text only: ...`; the trace line still names the `tts-...` id |
| STT failing or hearing nothing | `AI unavailable: speech to text failed (...), please type instead (not retried; ...)`, exit 1 |

WAV files must start with `RIFF` and have `WAVE` at byte 8; the client checks both. These
examples describe the client's handling. They are not evidence that real speech works with
your services; test that yourself.

## NPC bios, knowledge and checkpoints

These live on the server; the client needs no changes. The server looks up the bio and facts
by the `npc.id` the game sends (`npc_guide` here), so a real port must send a stable engine
id. In the server folder inside WSL, as `dwemer`:

```bash
php scripts/seed_example.php     # optional: Mira (npc_guide), Oren (npc_scout), a few facts
```

Then on Windows, with the server in mock mode:

| Command | Expect |
|---|---|
| `example_mod.exe --say "Who are you?"` | `[Guide] I am Mira Stonebridge, Market guide. ...` |
| `example_mod.exe --say "When does the market open?"` | `[Guide] Here is what I know: The market square stalls open at dawn ...` |
| `example_mod.exe --say "What happened to the mill?"` | a global fact about the old mill |
| `example_mod.exe --say "Is the north trail safe?"` | echo only: that fact belongs to `npc_scout` |

Edit bios and facts on the dashboard: **Configuration > NPC bios**.

Checkpoints are named copies of one session's conversation, matched to a game save by hand
through `session_id` (default `demo-save-1`; use one per save in a real game). Before a
restore, quit the client or `/cancel` its pending turn. Any turn still in flight for that
session is answered `409 stale` and dropped.

```bash
php scripts/checkpoint.php save    demo-save-1 before-boss
example_mod.exe --say "remember the dragon"   # on Windows: adds lines after the checkpoint
php scripts/checkpoint.php restore demo-save-1 before-boss
php scripts/checkpoint.php list    demo-save-1
```

The same save, restore, delete and list operations are on the server dashboard under
**Roleplay > Checkpoints**, with the same limits and stale handling. Both are manual: the
client never reads or writes save files, and there is no protocol endpoint the game can
call to save or restore a checkpoint. Automating them from game save/load events would
need a new server endpoint plus engine events behind `GameAdapter`; it is not part of these
templates and needs testing in that engine.

## Limits

- Two bounded worker pools: at most four normal workers (turns, events, decisions, voice
  and result reports) and at most two cancel workers reserved for server cancellations. A
  request that finds all four normal workers busy is rejected with a busy message and not
  queued; a result report is then skipped (the reply and actions are kept).
- A new turn starts only if a cancel worker stays free for it (two if it also replaces
  another NPC's pending turn), so dropping the pending turn can send its cancel even
  while every normal worker waits on a slow provider. Otherwise the turn is refused with
  "Waiting for a server cancel to answer" and not queued.
- While a cancel for an NPC in a session is still out, new turns to that same NPC in that
  same session are refused until the server answers it or it fails, so the cancel cannot
  hit the new turn. Other NPCs and sessions are not held up.
- Cancellation always discards the old reply locally. If no cancel worker is free (not
  expected for the pending turn), the server cancellation is not sent and the client says so.
- A server cancel that fails (network error, timeout, server error) is reported and never
  retried; the server may then still finish and save that turn.
- Quit drops pending replies/actions, sends the cancel for the pending turn, and waits for
  both pools (at most six HTTP calls), each bounded by `timeout_seconds`. It prints whether
  each cancel was confirmed or failed; it does not guarantee the server received it.
- Cancellation does not necessarily stop computation at the LLM provider.
- Console input is best with plain ASCII; real games pass UTF-8 text straight to the client.
- Voice replies are saved as WAV files in `%TEMP%\dwemer-ai-example\` and not played.
- The token is sent over plain HTTP. Use it on localhost or a trusted network only.

## Licence

Copyright (C) 2026 Dwemer Dynamics. Licensed under the GNU General Public License,
version 3 only (`GPL-3.0-only`); see [LICENSE](LICENSE). `third_party/nlohmann/json.hpp` is
Copyright (c) 2013-2022 Niels Lohmann, MIT, see
[third_party/nlohmann/LICENSE.MIT](third_party/nlohmann/LICENSE.MIT). All notices are listed
in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
