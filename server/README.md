# MIDIMaze Server

HTTP lobby + game relay server for MIDIMaze NetStream.

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

## Lobby API (HTTP GET)
Responses are JSON.

### Hello
`/hello?name=ALICE`

Response:
```json
{"ok":true,"client_id":"ABC12345","name":"ALICE"}
```

### List Games
`/list?client_id=ABC12345`

Response:
```json
{"ok":true,"games":[{"id":"G1","name":"Game","players":2,"max":4,"active":false}]}
```

### Create Game
`/create?client_id=ABC12345&name=Ring%201&max_players=8`

Response:
```json
{"ok":true,"game_id":"G1","status":"waiting"}
```

### Join Game
`/join?client_id=ABC12345&game_id=G1`

Response:
```json
{"ok":true,"status":"waiting"}
```

### Leave Game
`/leave?client_id=ABC12345&game_id=G1`

Response:
```json
{"ok":true}
```

### Wait For Start (poll)
`/wait?client_id=ABC12345`

If ready:
```json
{"cmd":"start","host":"your.dns.name","port":5123,"token":""}
```

If waiting:
```json
{"ok":true,"status":"waiting"}
```

### Ping
`/ping?client_id=ABC12345`

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
