# fujinet-midimaze-connect

Small Atari 8‑bit helper that starts a FujiNet NetStream connection for MIDIMaze and then boots into the game.

## What It Does
- Prompts for host, port, transport, and REGISTER flag
- Sends the FUJICMD_ENABLE_UDPSTREAM command with netstream compatible payload
- Mounts FujiNet devices, then warm‑resets so the cart or XEX can boot

## Build
Requires `cc65` and [FujiNet Library](https://github.com/FujiNetWIFI/fujinet-lib/releases)

Modify paths to FujiNet-Lib and CC65_HOME in Makefile if required.

```sh
make
```

Build outputs:
- `mmconncart.xex` (cartridge)
- `mmconndisk.xex` (disk/XEX)

## Run
- Put XEX file on FujiNet SD card
- In FujiNet WebUI Boot Settings, set `Alternate Config Disk` to the XEX path+file on the SD card
- If using disk based MIDI Maze, mount it into D1.
- Reset FujiNet and Atari (insert cartridge)
- Enter host and port in the program
- Choose transport (TCP/UDP) and whether to send REGISTER.
- Select **CONNECT**.

## Notes
- FujiNet Firmware with NETStream support is required. (FujiNet-Firmware or FujiNet-PC) At the time this was created, that only lives at:
  - https://github.com/mozzwald/fujinet-firmware/tree/v1.5.2-fixes-netstream
- Altirra supported with latest test release:
  - https://forums.atariage.com/topic/387055-altirra-440-released/page/3/#findComment-5788781
- And updated netsio device for Altirra from:
  - https://github.com/mozzwald/fujinet-emulator-bridge/blob/clock-in/altirra-custom-device/netsio.atdevice
