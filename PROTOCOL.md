# Protocol, version 1

The game (client) talks to the server over plain HTTP with JSON. This file is the
contract between ExampleMod and ExampleServer; it is identical in both repos.

- Base URL, default for a DwemerDistro install: `http://127.0.0.1:8081/ExampleServer`
- Every request except `health.php` needs `Authorization: Bearer <token>`.
  The token is in the server's `config/config.php`.
- Every JSON request and reply has `"protocol": 1`. The server refuses other versions.
- JSON request bodies are limited to 16 KB.
- Errors always look like this, with a matching HTTP status:

```json
{"protocol":1,"ok":false,"error":{"code":"unauthorized","message":"Missing or wrong token."}}
```

| HTTP | code | Meaning |
|---|---|---|
| 400 | `bad_json`, `bad_request`, `bad_audio` | The request is malformed. Do not retry unchanged. |
| 401 | `unauthorized` | Missing or wrong token. |
| 405 | `method_not_allowed` | Wrong HTTP method. |
| 404 | `unknown_request` | `result.php`: no retained completed turn matches. Do not retry. |
| 409 | `duplicate_request` | This `request_id` was already used. |
| 409 | `action_not_returned`, `already_reported` | `result.php`: see that section. Do not retry. |
| 409 | `stale` | The turn was cancelled or replaced. Drop it. |
| 413 | `too_large` | The body is too large. |
| 500 | `server_not_configured`, `internal_error` | Server problem. Details are only in the server log. |
| 502 | `llm_unavailable`, `tts_unavailable`, `stt_unavailable` | An upstream service failed. |
| 503 | `database_unavailable`, `not_migrated`, `schema_too_new`, `schema_mismatch`, `tts_disabled`, `stt_disabled` | Not ready, or turned off. `not_migrated` names the migration to apply; `schema_too_new` means the server code is older than its database; `schema_mismatch` means the server's own migration list is inconsistent. |

Clients never retry a request automatically: a retry could repeat a reply or an action. After
a timeout or failure, show the error and let the player send again (a new `request_id`).

## GET health.php

No token. Checks config and database. Never applies migrations or contacts providers.

```json
{"protocol":1,"ok":true,"service":"example-ai-server","schema":"007_connector_calls",
 "expected_schema":"007_connector_calls",
 "capabilities":["turn","cancel","result","game_event","action_target","decision","speak","listen",
                 "profiles","memory","trace"],
 "baseline":"baseline-1","server_version":"0.1.0"}
```

`expected_schema`, `capabilities`, `baseline` and `server_version` are optional (added within
version 1); older servers omit them. `server_version` is the server's own release version (set
in its `lib/version.php`). `baseline` names the example template's feature set for support; it
is not the protocol, a schema or a release version. A client that relies on `action_target` or
`game_event` should check the list. `profiles` and `memory` are server-side features that need
nothing from the client; `trace` means `speak.php`, `listen.php` and `decision.php` accept the
optional request ids described under "Optional request ids". A database missing any migration this code needs answers
`503 not_migrated` with the first missing migration's name; a database with migrations this
code does not know answers `503 schema_too_new`; server code whose `sql/` folder does not match
its migration list answers `503 schema_mismatch`. Every endpoint that writes to the database
gives the same answers. A different `protocol` means the client and
server must be updated together.

## POST turn.php

One player line to one NPC.

```json
{
  "protocol": 1,
  "request_id": "req-1a2b3c4d5e6f7a8b9c0d1e2f",
  "session_id": "demo-save-1",
  "npc": {"id": "npc_guide", "name": "Guide"},
  "player": {"name": "Traveler"},
  "context": {"location": "Market Square", "time_of_day": "Morning"},
  "text": "Follow me, please."
}
```

| Field | Rule |
|---|---|
| `request_id` | 8-64 chars of `A-Z a-z 0-9 _ . : -`. New for every turn. Reuse within a day gives `409 duplicate_request`. |
| `session_id`, `npc.id` | 1-64 chars of the same set. History is kept per session + NPC. |
| `npc.name`, `player.name` | 1-64 characters. |
| `text` | 1-1000 characters. |
| `context` | Optional object, at most 10 entries. Keys `a-z` and `_` (max 32), values text (max 200). |

Reply:

```json
{
  "protocol": 1,
  "ok": true,
  "request_id": "req-1a2b3c4d5e6f7a8b9c0d1e2f",
  "npc_id": "npc_guide",
  "generation": 7,
  "reply": "Of course. Lead the way.",
  "actions": [{"name": "follow_player", "args": {"target_id": "npc_guide"}}],
  "rejected_actions": [],
  "logged": true
}
```

- `reply` is at most 1000 characters.
- `actions` only contains names in the server's `allowed_actions`. Version 1 defines one
  action, `follow_player`. The game must still check every name against its own allowlist
  and ignore unknown names. Nothing in this protocol runs commands. The server never repeats
  a name; a client uses at most 4 distinct names and treats a repeated name as malformed:
  it rejects that name once and never runs its handler.
- `args` (optional, added within version 1) holds `target_id` for `follow_player`. The server
  always sets it to this request's `npc.id`; the model cannot choose it, and NPC selection
  (`decision.php`) never changes it. Older servers send name-only actions, which still mean
  "the NPC this request was sent to". A client accepts only `name` and `args`, only the
  `target_id` argument, and only when it equals the NPC it sent the request to; anything
  else is rejected (and may be reported as `rejected`).
- `npc_id` is this request's `npc.id`. Ignore a reply whose `request_id` or `npc_id` is not
  the one you are waiting for.
- The NPC must still be loaded and be the same instance when the reply arrives. If it was
  unloaded, deleted or replaced, drop the reply and its actions and send no result report.
- `rejected_actions` lists names the model asked for that the server refused (for logs).
- `logged` (optional, added within version 1) is `true` when the server stored this turn in
  its event log, `false` when that failed. Older servers omit it. A logged turn can take an
  optional `result.php` report. The log records that the server finished, not that the
  game received the reply.
- The server keeps the last `history_limit` lines (default 10) per session + NPC and sends
  them to the model.
- The server also gives the model the stored biography for `npc.id`, if any, and at most
  three short facts (this NPC's or global) that share words with `text`. These are server-side
  data; the request and reply shape do not change. Use a stable `npc.id`.
- Bios and facts are shared memory: every session of every save sees them, and checkpoint
  restores never change them. History is per session + NPC and is what a restore rolls back.
  The server keeps a copy of what it retrieved for each turn in its event log (Logs page).
- Server-side options that do not change the request or reply shape: a **profile** assigned to
  `npc.id` (or a default profile) adds an operator-written system prompt and may override the
  model or reply mode; **advanced memory** adds this session's saved summary and diary for the
  NPC; **vector search** may pick facts by embedding similarity before the keyword match. All
  are off until the server operator turns them on.

### Mock replies

With `llm.mode = "mock"` (the default) replies are predictable:

| Player text contains | Reply | actions | rejected_actions |
|---|---|---|---|
| `follow` | `Of course. Lead the way.` | `follow_player` | |
| `dance` | `Watch this!` | | `dance` |
| `who are you`, and the NPC has a stored bio | `I am <bio name>, <role>. <bio>` | | |
| `your profile`, and the NPC has a profile | `My profile is <profile name>.` | | |
| `what do you remember`, memory is on and a summary is saved | `I remember: <summary>` | | |
| words matching stored facts | `Here is what I know: <up to three facts>` | | |
| anything else | `<npc name> heard you say: <text>` | | |

## POST event.php

Optional (added within version 1; check `game_event` in `capabilities`). One game event. The
game decides when to send it; the server never triggers anything by itself.

```json
{
  "protocol": 1,
  "request_id": "evt-1a2b3c4d5e6f7a8b9c0d1e2f",
  "session_id": "demo-save-1",
  "type": "item_given",
  "npc": {"id": "npc_guide", "name": "Guide"},
  "text": "The player gave the guide a brass lantern.",
  "context": {"location": "Market Square"},
  "respond": false
}
```

| Field | Rule |
|---|---|
| `request_id` | As for `turn.php`. Turns and events share one id space: reuse within a day gives `409 duplicate_request` and changes nothing. |
| `session_id` | As for `turn.php`. |
| `type` | One of `location_entered`, `item_given`, `combat_started`, `combat_ended`. |
| `text` | 1-300 characters describing what happened. |
| `context` | Optional, as for `turn.php`. |
| `respond` | Optional, default `false`. |
| `npc` | `id` optional for log-only events. With `respond: true`, `npc.id`, `npc.name` and `player.name` are required, as for `turn.php`. |

- `respond: false` (log only): the server stores the event in its event log. It makes no
  model call, writes no conversation history and returns no actions:

  ```json
  {"protocol":1,"ok":true,"request_id":"evt-1a2b3c4d5e6f7a8b9c0d1e2f","respond":false,"logged":true}
  ```

  `logged: true` means the server stored it; it says nothing about the game. If the event
  cannot be stored the reply is an error and the `request_id` stays unused.
- `respond: true` (dialogue): the named NPC reacts. This is a normal turn with the text
  `Game event (<type>): <text>`, with the same reply, errors, history, stale fence, cancel,
  actions and optional `result.php` report as `turn.php`.

## POST cancel.php

Drops any in-flight turn for one NPC.

```json
{"protocol": 1, "session_id": "demo-save-1", "npc_id": "npc_guide"}
```

```json
{"protocol":1,"ok":true,"npc_id":"npc_guide","generation":8}
```

`logged` (optional, added within version 1) is `false` when the server could not store the
cancel in its event log. The cancel itself still took effect.

### How stale replies are prevented

Each turn and each cancel moves the NPC's `generation` up by one. When a turn finishes, the
server saves it only if its generation is still current; otherwise it answers
`409 stale` and saves nothing. A cancel or newer turn received before that save prevents the old turn from being saved.
A server-side checkpoint restore (`scripts/checkpoint.php`) moves every NPC in that session
up, so all of the session's in-flight turns answer `409 stale`.
Cancellation does not necessarily stop computation at the LLM provider.

The game should also remember the `request_id` it is waiting for and ignore any reply with a
different id. That covers replies that were already on their way back.

## Optional: POST result.php

Added within version 1; older servers answer HTTP 404 without this JSON shape, and clients
should then stop sending reports. After the game has applied a turn's actions (including a `respond: true` event), it may report
what it did with them. The server stores the report next to that turn's event for the
dashboard's Logs page, labelled as untrusted client input. It never runs anything and never
changes history.

```json
{
  "protocol": 1,
  "request_id": "req-1a2b3c4d5e6f7a8b9c0d1e2f",
  "session_id": "demo-save-1",
  "npc_id": "npc_guide",
  "results": [{"name": "follow_player", "status": "handled"}]
}
```

| Field | Rule |
|---|---|
| `request_id`, `session_id`, `npc_id` | Must match a completed turn the server still retains. |
| `results` | List of 1-4 objects. `name` must be an action that turn returned, each once. |
| `status` | `handled` (the game ran its handler), `rejected` (the game refused it) or `failed`. |
| `reason` | Optional, 1-120 characters. |

```json
{"protocol":1,"ok":true,"request_id":"req-1a2b3c4d5e6f7a8b9c0d1e2f","repeat":false}
```

- Each turn takes one report. Sending the same report again answers `"repeat":true`; a
  different one is `409 already_reported`.
- Unknown, pruned, failed, duplicate or stale requests are `404 unknown_request`. An action
  the turn did not return is `409 action_not_returned`.
- Only send a report for the reply the game actually used. Never send one for an ignored
  stale reply. A failed report must not undo the reply or its actions.

## Optional voice

Voice is off by default on the server (`tts.enabled`, `stt.enabled`) and the client
(`voice_enabled`). Always show the text first; voice is extra. On any voice error the game
keeps the subtitle (TTS) or lets the player type (STT).

### POST speak.php

```json
{"protocol": 1, "text": "Of course. Lead the way.",
 "request_id": "tts-1a2b3c4d5e6f7a8b9c0d1e2f", "parent_request_id": "req-1a2b3c4d5e6f7a8b9c0d1e2f",
 "npc_id": "npc_guide"}
```

Success: HTTP 200, `Content-Type: audio/wav`, the WAV bytes (`RIFF` at byte 0 and `WAVE` at
byte 8; the server checks this). Failure: a JSON error (`tts_disabled`, `tts_unavailable`).

`request_id`, `parent_request_id` and `npc_id` are optional (added within version 1). `npc_id`
only lets the server use that NPC's profile voice; it never changes what is spoken or anything
else. A successful reply also has the headers `X-Request-Id` and `X-Voice-Source` (`config` or
`profile`).

### POST listen.php

Body: raw WAV bytes (`RIFF` at byte 0, `WAVE` at byte 8), `Content-Type: audio/wav`, at most
10 MB. Optional headers (added within version 1): `X-Request-Id` and `X-Parent-Request-Id` (the
turn the transcript is for). Reply:

```json
{"protocol":1,"ok":true,"text":"follow me please","request_id":"stt-1a2b3c4d5e6f7a8b9c0d1e2f"}
```

`text` is 1-1000 characters of UTF-8. When nothing usable was recognised (empty, too long or
invalid text) the reply is `502 stt_unavailable`, like any other STT failure: let the player
type. Clients should still treat an empty `text` as "type instead". Failure: `bad_audio`,
`stt_disabled`, `stt_unavailable`.

## Optional: POST decision.php

Asks which one of a few NPCs the game offers should answer a line. It is selection only: no
reply text, no history, no conversation or memory writes and no actions (the server only adds
one connector-audit row after replying). The game decides what to do with
the answer; the example client only prints it. Off by default on the server
(`decision.enabled`); it then always answers with the baseline.

Only call it when the player has no target. If the game already has a valid explicit or
crosshair target, skip `decision.php`, do not send it the transcript, and send the turn to
that NPC as usual. The example client's `--decide` only prints the answer.

```json
{
  "protocol": 1,
  "request_id": "dec-1a2b3c4d5e6f7a8b9c0d1e2f",
  "transcript": "Scout, is the north trail safe?",
  "baseline_id": "npc_guide",
  "candidates": [
    {"id": "npc_guide", "name": "Guide", "cues": "market guide in the town square"},
    {"id": "npc_scout", "name": "Scout", "cues": "scout who knows the north trail"}
  ]
}
```

| Field | Rule |
|---|---|
| `request_id` | Optional (added within version 1), 8-64 id characters. See "Optional request ids". |
| `transcript` | 1-2000 characters. |
| `baseline_id` | Must be one of the candidate ids. The answer when nothing better is chosen. |
| `candidates` | List of 1-8 objects. `id` as for `npc.id`, unique, not `abstain`. Optional `name` (max 64) and `cues` (max 200). |

Reply, always HTTP 200 for a valid request:

```json
{"protocol":1,"ok":true,"chosen_id":"npc_scout","source":"provider","reason":"matched",
 "request_id":"dec-1a2b3c4d5e6f7a8b9c0d1e2f"}
```

- `chosen_id` is always one of the offered ids. Check it against your own list anyway.
- `source` is `provider` when the provider picked it, `baseline` for the fallback.
- `reason` is one of `matched` / `no_match` (mock), `chosen`, `disabled`, `abstained`,
  `low_confidence`, `not_offered`, `bad_response`, `provider_error`. Every provider problem,
  including a timeout, gives the baseline.
- A malformed request is `400 bad_request` or `bad_json`.

## Optional request ids (tracing)

Added within version 1; check `trace` in `capabilities`. They help find one request in the
server's error log, its Logs page and its Connector calls page.

- `turn.php` and `event.php` already require `request_id`. `speak.php` and `decision.php` take
  an optional `request_id` field and `listen.php` an optional `X-Request-Id` header, with the same
  rule (8-64 characters of `A-Z a-z 0-9 _ . : -`; anything else is `400 bad_request`). When it is
  missing the server makes one starting with `srv-`, so older clients keep working unchanged.
- `speak.php` (`parent_request_id`) and `listen.php` (`X-Parent-Request-Id`) can name the turn
  the voice belongs to. The example client sends its turn's `request_id`.
- Every JSON reply from these five endpoints, success or error, carries `request_id` once it is
  known, and the reply has an `X-Request-Id` header. A client may check that it is the id it sent.
  An old client simply ignores the extra field.
- The server writes these ids, never tokens, keys, prompts or provider replies, into its error
  log lines. Voice and NPC selection calls also get one row each in a separate connector audit
  (ids, provider name, status, timing, short error code; never audio or transcripts), written
  after the reply was sent, so it adds no wait and a failed write never changes the answer.

## Try it with curl

```bash
TOKEN=...   # from config/config.php
BASE=http://127.0.0.1:8081/ExampleServer
curl -s $BASE/health.php
curl -s -H "Authorization: Bearer $TOKEN" --data \
  '{"protocol":1,"request_id":"req-curl-0001","session_id":"s1","npc":{"id":"npc_guide","name":"Guide"},"player":{"name":"Traveler"},"text":"follow me"}' \
  $BASE/turn.php
```

## Changing the protocol

Add optional fields freely. For anything a version-1 client would misread (renamed or removed
fields, new required fields, new meaning), bump `protocol` in both repos together.

The companion ExampleServer `WAVE_ACTION.md` shows an optional wave extension. It is
not enabled by this template; keep both copied contracts identical when extending it.
