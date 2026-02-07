# MIDIMaze Server

Lobby + game relay server for MIDIMaze NetStream. The lobby uses JSON‑lines over TCP. When a game is full, the server spawns a game thread on a dedicated port and closes the lobby connection to those players.

## Build
From repo root:

```sh
make
```

Server binary output:
- `build/mmsrv`

## Config
The server requires a config file with `key=value` pairs:

```ini
host_name=your.dns.name
lobby_port=5000
game_port_min=5100
game_port_max=5199
max_games=5
max_players_default=10
join_timeout_sec=600
drop_timeout_sec=15
```

## Run

```sh
./build/mmsrv /path/to/server.cfg
```

## Lobby Protocol (JSON‑Lines)
Each request/response is one JSON object per line.

### Client Hello
Client must send name first. Name must be alphanumeric and max 8 chars.

Request:
```json
{"cmd":"hello","name":"ALICE"}
```

Response:
```json
{"ok":true,"name":"ALICE"}
```

### List Games
Request:
```json
{"cmd":"list"}
```

Response:
```json
{"ok":true,"games":[{"id":"ABC12345","name":"Game","players":2,"max":4,"active":false,"player_names":["ALICE","BOB"]}]}
```

### Create Game
Request:
```json
{"cmd":"create","name":"Ring 1","max_players":8}
```

Response:
```json
{"ok":true,"game_id":"ABC12345","status":"waiting"}
```

### Join Game
Request:
```json
{"cmd":"join","game_id":"ABC12345"}
```

Response:
```json
{"ok":true,"status":"waiting"}
```

When a game fills, the server responds with `start` and closes the lobby connection:
```json
{"cmd":"start","host":"your.dns.name","port":5123,"token":"T0K3N123"}
```

### Leave Game
Request:
```json
{"cmd":"leave","game_id":"ABC12345"}
```

Response:
```json
{"ok":true}
```

### Heartbeat
Request:
```json
{"cmd":"heartbeat"}
```

Response:
```json
{"ok":true}
```

## Game Connection
- Clients connect to the game port using TCP.
- First message must be the literal string `REGISTER` (no newline required).
- The server forwards packets in a one‑way ring.

## Behavior Notes
- Pending games expire after `join_timeout_sec`.
- If any client drops during a game, the game ends after `drop_timeout_sec`.
- When a game ends, its lobby listing is removed.
