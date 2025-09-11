# ftpc — Tiny FTP client for Nintendo 3DS (homebrew)

A minimal, controller-friendly FTP **client** for the 3DS.  
- Anonymous login (user: `anonymous`, blank password)  
- Default port **5000** (editable)  
- Passive mode (PASV), IPv4  
- Simple file browser with **A = enter/ download**, **B = up**  
- Downloads files to **`sdmc:/3ds/`**  
- Remembers last host/port in `sdmc:/3ds/ftpc/config.ini`

> ⚠️ Plain FTP (no TLS). Use on trusted local networks only.

---

## Features

- **Browse current directory only** using `NLST` (fast, predictable)
- **Enter directories / go up** with A / B
- **Download files** with A (when the selection isn’t a directory)
- **30-ish visible rows** per screen (27 content lines + header)
- **Flicker-free UI** (only redraws on change)
- **Config persistence** (`host`/`port`) between runs

---

## Controls

### Before connect
- **X** — Edit `IP:Port` (touch keyboard)
- **A** — Connect
- **START** — Exit

### After connect (browser)
- **D-Pad Up/Down** — Move selection
- **A** — Enter directory / **download file**
- **B** — Up one directory (`CDUP`)
- **START** — Exit

---

## Requirements

- A 3DS capable of running homebrew (`.3dsx`) via Homebrew Launcher
- Wi-Fi network access
- An FTP server that supports:
  - **Anonymous** login (user `anonymous`, no password)
  - **Passive (PASV)** mode
  - **IPv4**
- Default server port is **5000** (you can change it)

---

## Build

### Windows (recommended)

1. Install **devkitPro** (includes MSYS2 and pacman).  
   <https://devkitpro.org/wiki/Getting_Started>
2. Open the **MSYS2** shell from the devkitPro start menu.
3. Install 3DS tools (if you haven’t already):
   ```bash
   pacman -S 3ds-dev devkitarm-rules
