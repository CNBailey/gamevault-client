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
3. A notification shows the address GameVault is on (`http://<PS5 address>:8687`); GameVault stays in the background. Open the **GameVault** app on the home screen once it appears, or that address in a browser on a phone or PC. In **Settings**, enter your GameVault server's address (e.g. `http://192.168.0.32:8686`), press **Test connection**, then **Save settings**.

GameVault keeps running until the PS5 is turned off or put in rest mode. Load it again to bring it back; queued downloads carry on. Loading it while it is already running just shows the address again.

## The home-screen app

GameVault puts a native PS5 app, **GameVault**, in `/data/homebrew/PPSA99688`, and ShadowMountPlus adds it to the home screen (load ShadowMountPlus again if it doesn't appear). It is drawn with OpenGL by the [ps5-homebrew-ui](https://github.com/J7mbo/ps5-homebrew-ui) kit, the way [NASBridge](https://github.com/J7mbo/nasbridge-ps5) does it, and has the same sections as the browser version: Library, the game page, Downloads, Storage and Settings (L1 / R1 switch sections; the server address is typed with an on-screen keyboard).

The app is only a window: the downloads run in the background service (`gamevault.elf`), so they carry on when the app is closed. If the service isn't running when you open the app, the app starts it by sending the copy of `gamevault.elf` kept in its folder to the ELF loader on port 9021 (or 9027).

That copy comes from your GameVault server, which fetches the newest release from GitHub every 6 hours. When a newer version is ready, Settings shows **Update GameVault** (and the browser version an **Update to x.y.z** button): GameVault restarts as the new version, a download in progress carries on, and the home-screen app is replaced with the new one.

If you had the 0.2 tile on the home screen and it shows an error, delete the tile from the home screen (Options › Delete) and load ShadowMountPlus again, so it adds the new app.

The app's source is in `app/`; `tools/build-app.sh app` builds it (and `tools/build-app.sh snapshots` renders its screens on a PC, with GameVault running on port 8687; `tools/preview-server.py PORT` is a stand-in server with made-up games to point it at).

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
| `GET /api/log` | Recent log lines (also in Settings › Diagnostics) |
| `POST /api/open` | Opens the browser version in the PS5 browser |
| `POST /api/update-check`, `/api/update` | Checks for, or restarts as, a newer version |
| `POST /api/rescan`, `/api/notify-test`, `/api/quit` | |

## Building

```sh
# Interface
cd web && npm ci && npm run build && cd ..

# Home-screen app (clang-18, lld-18, ninja; fetches the UI kit and its SDKs)
tools/build-app.sh app     # build/app/PPSA99688

# PS5 payload, with the app inside (needs the ps5-payload-sdk)
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

GPL-3.0-or-later. Built with the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk) and the [ps5-homebrew-ui](https://github.com/J7mbo/ps5-homebrew-ui) kit (GPL-3.0-or-later, by BlackBearReloaded); the app's shell change comes from [NASBridge](https://github.com/J7mbo/nasbridge-ps5) (GPL-3.0-or-later). stb_image (public domain / MIT) and zlib (zlib licence) are included in `app/third_party` and `third_party`.
