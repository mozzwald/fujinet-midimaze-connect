# fujinet-midimaze-connect

Small Atari 8‑bit helper that starts a FujiNet NetStream connection for MIDIMaze and then boots into the game.

## What It Does
- Prompts for host, port, and player name for lobby login
- Lets players create games with transport choice (`TCP` default, optional `UDP`)
- Sends the FUJICMD_ENABLE_UDPSTREAM command with netstream compatible payload
- Mounts FujiNet devices, then warm‑resets so the cart or XEX can boot

## Build
Requires `cc65` and [FujiNet Library](https://github.com/FujiNetWIFI/fujinet-lib/releases)

Modify paths to FujiNet-Lib and CC65_HOME in Makefile if required.

```sh
make
```

Build outputs:
- `build/mmconncart.xex` (cartridge)
- `build/mmconndisk.xex` (disk/XEX)
- `build/mmsrv` (server)

## Run
- Put XEX file on FujiNet SD card
- In FujiNet WebUI Boot Settings, set `Alternate Config Disk` to the XEX path+file on the SD card
- If using disk based MIDI Maze, mount it into D1.
- Reset FujiNet and Atari (insert cartridge)
- Enter host and port in the program
- In Create Game, choose transport (`TCP` default or `UDP`).
- Select **CONNECT**.

## Server
The server is a lobby + game relay in one binary. It uses JSON‑lines over TCP for the lobby and spawns game threads on a port range.

Create a config file (key=value):

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

Run:

```sh
./build/mmsrv /path/to/server.cfg
```

## Notes
- FujiNet Firmware with NETStream support is required. (FujiNet-Firmware or FujiNet-PC) At the time this was created, that only lives at:
  - https://github.com/mozzwald/fujinet-firmware/tree/v1.5.2-fixes-netstream
- Altirra supported with latest test release:
  - https://forums.atariage.com/topic/387055-altirra-440-released/page/3/#findComment-5788781
- And updated netsio device for Altirra from:
  - https://github.com/mozzwald/fujinet-emulator-bridge/blob/clock-in/altirra-custom-device/netsio.atdevice
