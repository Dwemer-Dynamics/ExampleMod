# Agent playbook: turn the example into a real mod

Paste the prompt below into your coding agent (Claude Code, Codex, or similar) from a
folder that contains your copies of
**[ExampleServer](https://github.com/Dwemer-Dynamics/ExampleServer)** and
**[ExampleMod](https://github.com/Dwemer-Dynamics/ExampleMod)** and your game's modding SDK
or project.

---

```text
You are helping me build an AI NPC mod for my game, starting from two example repos:
ExampleServer (PHP 8.2 + PostgreSQL, runs in DwemerDistro WSL) and ExampleMod
(C++17 Windows console fake game). Read ExampleServer/START_HERE.md, both README.md,
AGENTS.md, PROTOCOL.md and ExampleServer/SETUP.md first, and follow the AGENTS.md rules.

Work in this order and stop to show me the result after each step:

1. Inspect my target game. Find out: the engine and modding SDK; plugin language,
   architecture (x64 or x86) and build tools; how a plugin runs code each frame or tick
   on the main thread; how to read the player name, the NPC being talked to, a stable NPC
   id and location; how to show a subtitle or message; how to make an NPC follow the
   player; whether HTTP from a worker thread is allowed; how to play a WAV on an NPC.
   Cite the SDK files or docs you used. Say plainly what you could not find.

2. Run the examples unchanged. Install the server as SETUP.md says, build the client,
   and run the README smoke checks. Do not change code until they pass. Report output.

3. Rename to my own project. Pick a project name with me. Rename the client and server
   folders, the database name, the session/NPC ids in docs, and the launcher manifest id.
   Add my copyright notice; keep LICENSE (GPL-3.0-only) and every existing and
   third-party notice listed in THIRD_PARTY_NOTICES.md. These templates share no history with any other
   product; do not plan to merge upstream changes automatically.

4. Implement the game adapter. Write a new GameAdapter for my engine using only the
   engine's real APIs. Call GameAdapter only from the game's main thread, keep network
   calls on worker threads, keep the request_id stale check and the action allowlist.
   Implement follow_player with the engine's own follow behaviour. Keep voice optional.
   Do not invent SDK functions: if an API is unknown, leave a clearly marked TODO and
   tell me.

5. Validate. Build for the right architecture with no warnings. Run the smoke checks
   again with the mock server. Then test in the real game: talk to an NPC, see the
   subtitle, ask it to follow, cancel a slow reply, stop the server and check the game
   shows an error instead of crashing or hanging. Report exactly what you tested in game
   and what you did not.
```

---

## Warm-up exercise (before step 4)

A small change that touches each boundary once, on your own copies with a disposable
database, so the agent learns the seams before porting:

1. **Game side** (`src/console_game.h`, behind `GameAdapter`): add a third fictional NPC,
   e.g. `npc_smith` "Smith" at "Forge Lane", and let `/npc smith` face it. Nothing in
   `ai_client` or `PROTOCOL.md` changes: the NPC id is just data.
2. **Server data** (dashboard **NPC bios**, or `scripts/seed_example.php` for samples): give
   `npc_smith` a bio and one fact, e.g. "Forge hours: the forge closes at dusk."
3. **Check**: `/npc smith`, then `When does the forge close?`. Logs shows the fact under
   "Retrieved for this request". Then `/event item_given The player gave the smith ore.`
   (log only) and `/react combat_started Bandits at the forge.` (Smith reacts).
4. **Leave alone**: protocol version, the event type list, action allowlists, provider
   settings in `config/config.php` and the client's `config.json`. A new event type or
   action is a change to both repos and both `PROTOCOL.md` files, not part of this exercise.

If `--health` or the server fails on first run, see
[ExampleServer SETUP.md, Troubleshooting](https://github.com/Dwemer-Dynamics/ExampleServer/blob/main/SETUP.md#troubleshooting): it covers `not_migrated`, `schema_too_new`, a refused `update.sh` and an
older server without a `baseline`.

## What a real engine port must provide

| Need | Why |
|---|---|
| A main-thread tick or callback | `GameAdapter` calls must happen there. |
| A worker thread or async HTTP | So the game never freezes while the AI thinks. |
| Player name, NPC name, stable NPC id | Sent with every turn; the id keys NPC history, bio and knowledge. |
| A `session_id` per save game | Keeps histories apart; server checkpoints are matched to saves by hand (see [README](README.md#npc-bios-knowledge-and-checkpoints)). |
| A way to show text | Subtitles are the primary output. |
| An NPC follow behaviour | The only action in protocol version 1. It targets the NPC the request was sent to; `args.target_id` must equal it. |
| An NPC presence/identity check | `isSameNpc()`: is the captured NPC still loaded and the same instance? Checked before any reply or action is applied. |
| A place for `config.json` | Holds the server URL and token. |
| Optional: WAV playback and microphone capture | For voice. |

## What the services need

| Service | Default | Required |
|---|---|---|
| ExampleServer (Apache in DwemerDistro) | `http://127.0.0.1:8081/ExampleServer` | Yes |
| PostgreSQL in DwemerDistro | `localhost:5432`, database `example_ai_mod` | Yes |
| LLM: OpenAI, OpenRouter, local OpenAI-compatible or DwemerDistro LLM Studio (`127.0.0.1:1234`) | `llm.mode` in `config/config.php`, see ExampleServer/CONNECTORS.md | No (mock otherwise) |
| TTS: PocketTTS (audio.cpp), default | `http://127.0.0.1:8086/v1/audio/speech`, off until enabled | No |
| STT: Parakeet, default | `http://127.0.0.1:8022/v1/audio/transcriptions`, off until enabled | No |
| STT: faster-whisper, supported alternative | `http://127.0.0.1:9876/api/v0/transcribe` | No |
| NPC selection (`decision.php`, mock or OpenRouter decisions) | off | No; selection only, never actions |

All URLs are configurable; provider settings are in
[ExampleServer/CONNECTORS.md](https://github.com/Dwemer-Dynamics/ExampleServer/blob/main/CONNECTORS.md). None of these are installed or
started by the example; start them from the DwemerDistro launcher or your own setup.

The companion [ExampleServer/MAKE_IT_YOURS.md](https://github.com/Dwemer-Dynamics/ExampleServer/blob/main/MAKE_IT_YOURS.md) maps the
source files to customize. [ExampleServer/WAVE_ACTION.md](https://github.com/Dwemer-Dynamics/ExampleServer/blob/main/WAVE_ACTION.md) is a
worked optional action exercise; wave remains disabled in these templates. The port checklist
and evidence levels are in
[ExampleServer/START_HERE.md](https://github.com/Dwemer-Dynamics/ExampleServer/blob/main/START_HERE.md#porting-to-a-real-engine). Use the
existing handoff above rather than adding a second workflow.
