# GameVault for PS5

The PS5 side of [GameVault](https://github.com/CNBailey/gamevault-server). It is a payload that runs in the background on a jailbroken PS5. It shows your GameVault library on the TV, downloads PS5 games to the console and hands PS4 games to the console's installer.

- **Library**: your GameVault games, with a badge on each game that is already on this PS5 (✓), has a newer version on the NAS (↑) or is downloading (%).
- **Game page**: the Download button turns into *Downloaded* when the game is on the PS5, *Update* when the NAS has a newer version, *Too large* when the drive hasn't enough space, or *Needs firmware X* when the game needs newer firmware than the console has.
- **Downloads**: run in the background while you play, with several connections at once. They can be paused, resumed (also after a restart) and reordered.
- **Storage**: the free space on each drive and the games on it, which you can delete.
- **Settings**: the GameVault address, where PS5 games are saved (internal SSD or an external drive), the speed limit while a game is running, and a controller test.

The same interface also works from a phone or PC at `http://<PS5 address>:8687`.

## Installing

1. Download `gamevault.elf` from the [latest release](https://github.com/CNBailey/gamevault-client/releases/latest), or add it to your payload/package manager using that link. New versions are published there, so the manager can find updates.
2. Load it with your ELF loader (port 9021), e.g. `socat -t 99999999 - TCP:<PS5 address>:9021 < gamevault.elf`, or from the package manager.
3. The PS5's browser opens on GameVault. Open **Settings**, enter your GameVault server's address (e.g. `http://192.168.0.32:8686`), press **Test connection**, then **Save settings**.

GameVault keeps running until the PS5 is turned off or put in rest mode. Load it again to bring it back; queued downloads carry on. Loading it while it is already running just opens the interface again.

## Home-screen tile and updates

Once running, GameVault puts a small app in `/data/homebrew/PPSA99687` (an icon, a background and a tiny fake-signed `eboot.bin`). ShadowMountPlus adds it to the home screen as **GameVault** the next time it scans; load ShadowMountPlus again if it doesn't appear.

Pressing the tile opens GameVault in the PS5 browser. If GameVault isn't running (after a restart, say), the tile starts it first by sending the copy of `gamevault.elf` kept next to it to the ELF loader on port 9021 (or 9027), so it needs an ELF loader running, as after any jailbreak with ShadowMountPlus.

That copy comes from your GameVault server, which fetches the newest release from GitHub every 6 hours (server 2026-11-24 or newer). When a newer version is ready, Settings and the top bar show **Update to x.y.z**: GameVault restarts as the new version, and a download in progress carries on.

## Where games go

| | Saved to | Used by |
|---|---|---|
| PS5 games (`.exfat` images and folder dumps) | `/data/homebrew` on the internal SSD, or `/mnt/usbN/homebrew` / `/mnt/extN/homebrew` on an external drive | ShadowMountPlus |
| PS4 games (`.pkg`) | The PS5's installer, which fetches the PKG from GameVault itself and uses the console's own *Settings › Storage › Installation location* | The PS5 |

Choose the drive for PS5 games in Settings, or for a single game with □ on its page. To install PS4 games to an external drive, change the console's installation location. PS4 games can only be installed from `.pkg` files.

GameVault recognises games already on the console by their title ID, from `.exfat` image names (`PPSA01234-Name (v01.000.000).exfat`), from `sce_sys/param.json` in folders, and from installed PS4 apps.

When an update is downloaded, the older copy is deleted once the new one is complete and has been checked.

## Transfers

Downloads come over HTTP from GameVault. **Settings › Test download speed** reads part of your largest PS5 game with 1, 2, 4 and 6 connections (nothing is written) and keeps the fewest that reach the top speed; *Connections per download: Automatic* (the default) then uses that number, or 2 before the first test. Each connection writes to the drive in 8 MB blocks. NFS isn't supported yet, so the *Automatic* and *NFS* transfer settings use HTTP for now. Your NFS share is saved for when NFS support is added.

Each download is fetched in 64 MB pieces. A `.part` file and a `.part.state` file next to it record which pieces are done, so a paused or interrupted download carries on where it stopped. With *Check each image* on (the default), a finished `.exfat` is checked for the right size and an exFAT header before it is renamed into place.

## Controller

| Button | Does |
|---|---|
| D-pad | Move |
| ✕ | Select |
| L1 / R1 | Previous / next section |
| ○ | Back |
| △ | Favourite (on a game) |
| □ | Change filter (library) or drive (game page) |

The PS5 browser sends the D-pad, ✕, L1, R1, L2 and R2 as key presses. The codes for ○, △ and □ can differ, so **Settings › Controller test** shows the code each button sends and lets you assign the button for Back, Favourite and Change drive.

## API

The payload serves a small JSON API on port 8687, used by the interface:

| Method and path | |
|---|---|
| `GET /api/state` | Version, firmware, settings, drives, games on the console, downloads |
| `GET` / `PUT /api/settings` | Settings |
| `POST /api/test-connection` | `{server}` → checks GameVault |
| `POST /api/downloads` | `{gameId, drive?, files?, replace?}` → queues a game |
| `POST /api/downloads/:id/pause` / `resume` / `cancel` / `up` / `down` | Controls a download |
| `DELETE /api/downloads/:id` | Removes a finished, failed or cancelled download from the list |
| `POST /api/downloads/clear` | Removes finished downloads from the list |
| `DELETE /api/installed?path=` | Deletes a game found on the console (PS5 games only) |
| `POST /api/speed-test` | `{gameId}` → MB/s with 1, 2, 4 and 6 connections |
| `POST /api/open` | Opens the interface in the PS5 browser (used by the tile) |
| `POST /api/update-check`, `/api/update` | Checks for, or restarts as, a newer version |
| `POST /api/rescan`, `/api/notify-test`, `/api/quit` | |

## Building

```sh
# Interface
cd web && npm ci && npm run build && cd ..

# PS5 payload (needs the ps5-payload-sdk, clang-18 and lld-18)
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make                       # gamevault.elf
make send PS5_HOST=192.168.0.50

# Linux build for development, with the console's file system faked under ./gvroot
make -f Makefile.posix
GV_ROOT=./gvroot GV_NO_BROWSER=1 ./build/gamevault
python3 tests/integration.py build/gamevault
```

For the interface on its own, run `npm run dev` in `web/`. It sends `/api` to a GameVault payload on `127.0.0.1:8687`.

GitHub Actions builds the interface, runs the end-to-end test against the Linux build, builds `gamevault.elf` with the latest SDK, and publishes a release whenever the number in `VERSION` changes on `main` (or a `v*` tag is pushed). To release a new version, raise the number in `VERSION`.

## Licence

GPL-3.0-or-later. Built with the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk).
