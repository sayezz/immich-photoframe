# immich-photoframe

A fullscreen photo frame application for Raspberry Pi (and other Linux/X11 systems) that displays photos directly from your [Immich](https://immich.app) server. No files are copied to the device — images are streamed on demand and held only in RAM.

![Photo frame showing image with date overlay, image counter, folder name and QR code](https://placeholder)

---

## Features

- **Fullscreen display** — OpenCV window covering the entire screen, cursor hidden
- **Immich-native** — pulls photos from your Immich server via its REST API
- **Tag mode or Album mode** — show photos tagged with one or more tags, or photos from one or more shared albums
- **Multi-user** — each user provides their own API key; share links are created by the asset's owner automatically
- **Overlays:**
  - Top-left: photo date and folder name
  - Bottom-right: progress counter (e.g. `12/132`)
  - Bottom-left: QR code that opens the photo in Immich when scanned
- **Touch / mouse navigation** — tap/click left half to go back, right half to advance
- **Smart cycling** — photos are shuffled and every photo is shown before repeating; progress is saved in `db.json` so it survives restarts
- **Live refresh** — asset list is re-fetched from Immich every N minutes, so newly tagged/added photos appear without restarting
- **Preload buffer** — next photos are downloaded in background while the current one is displayed

---

## Requirements

### Hardware
- Raspberry Pi (tested on Pi 4/5 running Debian 12 Bookworm, aarch64)
- Connected display (HDMI or DSI)

### Software (native build)
- Debian 12 Bookworm (or compatible Linux distro)
- X11 display server
- C++17 compiler (GCC 12+)
- Dependencies: `libopencv-dev`, `libcurl4-openssl-dev`, `libqrencode-dev`, `libx11-dev`, `cmake`, `pkg-config`

### Software (Docker build)
- Docker Engine 24+ and Docker Compose v2+
- X11 session running on the host

### Immich server
- Immich v1.90 or later running and reachable on your local network
- One API key per user whose photos you want to display (see [Immich API keys](https://immich.app/docs/features/api-keys))
- Photos must be marked with a **tag** or placed in a **shared album** (see [Immich Setup](#immich-setup))

---

## Immich Setup

### Option A — Shared Album (recommended for multiple users)

1. In Immich, open **Albums → Create Album** and name it (e.g. `Fotorahmen`)
2. Add photos to the album
3. To include photos from other users: open the album → **Share** → invite the other user
4. Each invited user can then add their own photos to the same album
5. In `config.json` set `"sourceMode": "album"` and add the album name to `albumNames`

> The app only needs the **primary user's** API key to read the album. You need additional API keys only for the QR-code feature (each user's share link must be created by that user's key).

### Option B — Tag per user

1. In Immich open **Tags** and create a tag (e.g. `Fotorahmen`)
2. Select photos and apply the tag
3. Repeat for every user who wants to contribute photos under the same tag name
4. In `config.json` set `"sourceMode": "tag"` and add the tag name to `tagNames`

> Each Immich user has their own independent tag namespace. The app resolves the tag ID for each API key separately and merges the results.

---

## Configuration

Edit `config.json` before running.

```json
{
    "timer": 25,
    "enableTouch": true,
    "showDate": true,
    "showImgCount": true,
    "showFolderName": true,
    "showQrCode": true,

    "sourceMode": "album",

    "tagNames":   ["Fotorahmen"],
    "albumNames": ["Fotorahmen"],

    "refreshInterval": 300,

    "immich": {
        "serverUrl": "http://192.168.1.100:2283",
        "apiKeys": [
            "API_KEY_USER_1",
            "API_KEY_USER_2"
        ]
    }
}
```

| Key | Type | Description |
|-----|------|-------------|
| `timer` | int | Seconds each photo is displayed before auto-advancing |
| `enableTouch` | bool | Enable touch/mouse navigation |
| `showDate` | bool | Show photo date overlay (top-left) |
| `showImgCount` | bool | Show progress counter overlay (bottom-right) |
| `showFolderName` | bool | Show folder/album name overlay (top-left) |
| `showQrCode` | bool | Show QR code linking to the photo in Immich (bottom-left) |
| `sourceMode` | string | `"tag"` or `"album"` — which source to use |
| `tagNames` | string \| array | Tag name(s) to display (used when `sourceMode` is `"tag"`) |
| `albumNames` | string \| array | Album name(s) to display (used when `sourceMode` is `"album"`) |
| `refreshInterval` | int | Seconds between asset-list refreshes from Immich |
| `immich.serverUrl` | string | Base URL of your Immich server |
| `immich.apiKeys` | string \| array | API key(s) — one per user. First key is used for all reads; all keys are tried when creating share links |

**Multiple sources:** both `tagNames` and `albumNames` accept a plain string or a JSON array. Photos from multiple sources are merged and deduplicated.

**Legacy format:** a single `"apiKey"` string and single `"tagName"` / `"albumName"` strings still work for backwards compatibility.

---

## Build — Native

```bash
# 1. Install dependencies
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake pkg-config \
    libopencv-dev libqrencode-dev \
    libcurl4-openssl-dev libx11-dev

# 2. Clone
git clone https://github.com/youruser/immich-photoframe.git
cd immich-photoframe

# 3. Edit config
cp config.json config.json  # already present — edit immich.serverUrl and apiKeys

# 4. Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 5. Run
DISPLAY=:0 ./build/immich-photoframe
```

---

## Build — Docker

```bash
# 1. Install Docker
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER
# Log out and back in

# 2. Clone and configure
git clone https://github.com/youruser/immich-photoframe.git
cd immich-photoframe
# Edit config.json

# 3. Allow the container to open a window on your display
xhost +local:docker

# 4. Create db.json if it doesn't exist
touch db.json

# 5. Build image and start
docker compose up --build

# Subsequent starts (no rebuild needed unless source changed):
docker compose up

# Run in background:
docker compose up -d
docker compose logs -f   # follow logs
docker compose down      # stop
```

---

## Auto-start as a systemd service (native)

```bash
# Create the service file
cat > ~/.config/systemd/user/immich-photoframe.service << 'EOF'
[Unit]
Description=Immich Photo Frame
After=graphical-session.target

[Service]
Environment=DISPLAY=:0
WorkingDirectory=%h/git/immich-photoframe
ExecStart=%h/git/immich-photoframe/build/immich-photoframe
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable --now immich-photoframe.service

# Check status and logs
systemctl --user status immich-photoframe.service
journalctl --user -u immich-photoframe.service -f
```

---

## Navigation

| Input | Action |
|-------|--------|
| Tap / click — **right half** | Next photo |
| Tap / click — **left half** | Previous photo (from history) |
| Keyboard `→` / `Space` | Next photo |
| Keyboard `←` | Previous photo |
| Keyboard `Esc` | Quit |

Navigation history holds the last 15 photos so you can page back freely without re-downloading.

---

## How it works

```
┌─────────────────────────────────────────────────────────┐
│ main thread                                             │
│  cv::waitKey loop → renders current image → handles     │
│  user input → calls tryGetNextImage() when timer fires  │
└──────────────────────┬──────────────────────────────────┘
                       │ QueueEntry (ImageInfo + cv::Mat)
              ┌────────▼────────┐
              │  in-memory queue │  max 5 entries
              │  (m_queue)       │
              └────────▲────────┘
                       │ push
┌──────────────────────┴──────────────────────────────────┐
│ preload thread                                          │
│  1. pick random unvisited asset                        │
│  2. GET /api/assets/{id}/thumbnail?size=preview        │
│  3. cv::imdecode → cv::Mat                             │
│  4. (if QR enabled) POST /api/shared-links             │
│  5. mark visited → save to db.json                     │
│  6. push to queue                                      │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ refresh thread                                          │
│  every refreshInterval seconds:                        │
│  GET /api/albums/{id}  OR  POST /api/search/metadata   │
│  → diff against current list → add/remove assets       │
└─────────────────────────────────────────────────────────┘
```

**Disk usage:** only `db.json` is written to disk. It stores visited asset UUIDs (~36 bytes each) and cached share-link keys. With 20,000 photos this file stays under ~2 MB. Images themselves are never written to disk.

**RAM usage:** at most 20 `cv::Mat` objects in memory at once (5 in the preload queue + 15 in the back-navigation history). A 1440×2160 preview JPEG decoded to BGR is ~9 MB, so peak usage is around 180 MB.

---

## File structure

```
immich-photoframe/
├── main.cpp              # Entry point, config loading, event loop
├── DisplayImage.cpp/h    # Rendering engine, preload + refresh threads
├── ImmichClient.cpp/h    # Immich REST API client (libcurl)
├── json.hpp              # nlohmann/json (bundled, header-only)
├── CMakeLists.txt        # Native build
├── Dockerfile            # Multi-stage Docker build
├── docker-compose.yml    # Docker Compose (X11 + volumes)
├── config.json           # User configuration
└── db.json               # Runtime state (auto-created, gitignored)
```

---

## Troubleshooting

**App starts but shows "Verbinde mit Immich..." forever**
- Check `config.json` — is `serverUrl` reachable from the Pi? (`curl http://<host>:2283/api/server/ping`)
- Is the API key valid? Check Immich → Account Settings → API Keys

**Photos from one user show QR codes but another user's don't**
- The QR code is a share link — it must be created by the asset's owner
- Add that user's API key to `apiKeys` in `config.json`

**Docker: "cannot connect to X server"**
- Run `xhost +local:docker` before `docker compose up`
- Make sure `DISPLAY` is set in your shell (`echo $DISPLAY`)

**Docker: OpenCV highgui fails to open window**
- Ensure `/tmp/.X11-unix` exists and is mounted (it is by default in `docker-compose.yml`)
- On Wayland hosts you may need `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY` instead

**"Album/Tag not found"**
- Album and tag names are case-sensitive and must match exactly as shown in Immich
- The first API key in the list is used to look up albums/tags — make sure that user has access to the album
