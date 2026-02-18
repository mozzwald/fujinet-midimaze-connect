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
idle_timeout_sec=600
udp_dup_enabled=1
udp_dup_delay_ms=15
```

`udp_dup_enabled` controls whether UDP duplicate forwarding is enabled (`0` or `1`).
`udp_dup_delay_ms` controls delayed duplicate timing when enabled. Valid range: `0..1000` ms.

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
{"ok":true,"games":[{"id":"G1","name":"Game","players":2,"max":4,"active":false,"transport":"tcp"}]}
```

### Create Game
`/create?client_id=ABC12345&name=Ring%201&max_players=8&transport=tcp`

`transport` supports:
- `tcp` (default if omitted)
- `udp` (UDP NetStream relay with 1 duplicate forwarded packet at +15ms)
  - Duplicate behavior is controlled by `udp_dup_enabled` and `udp_dup_delay_ms`.

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
{"cmd":"start","host":"your.dns.name","port":5123,"transport":"tcp","token":""}
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
- TCP mode:
  - Clients connect to the game port over TCP.
  - First message must be the literal string `REGISTER` (no newline required).
  - Ring forwarding is TCP stream to next player.
- UDP mode:
  - Clients send UDP datagrams to the game port.
  - First packet from each player must contain `REGISTER` (raw or with 2-byte sequence prefix).
  - Ring forwarding is UDP datagram to next player.
  - Server sends one duplicate of each forwarded datagram after `udp_dup_delay_ms` (default 15ms).
- TCP sockets use `TCP_NODELAY` and reduced socket buffers to reduce queueing latency.

## Diagnostics
- During active games, server prints one stats line every 10 seconds:
  - `rx`: packets/datagrams received
  - `tx`: forwards sent (includes delayed duplicates)
  - `dup_tx`: delayed duplicate sends
  - `reg`: registration packets accepted
  - `drop`: send failures, disconnect-triggered drops, queue-full duplicate drops
  - `unknown`: UDP packets from unregistered endpoints
  - `seq_i`: in-order UDP sequence observations
  - `seq_o`: out-of-order-ahead sequence observations
  - `seq_l`: late/behind sequence observations
  - `seq_dup`: exact duplicate sequence observations
  - `seq_gap`: total missing sequence count inferred from ahead jumps
  - `seq_maxgap`: largest single inferred gap
  - `seq_short`: UDP payloads shorter than 2 bytes (cannot decode sequence)
- A final stats line is printed when each game thread exits.

## Behavior Notes
- Pending games expire after `join_timeout_sec`.
- If any client drops during a game, the game ends after `drop_timeout_sec`.
- Active games with no traffic end after `idle_timeout_sec`.
- When a game ends, its lobby listing is removed.
