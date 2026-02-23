# GyroScroll

**Chiral (circular) scrolling for Windows Precision Touchpads**

[![Version](https://img.shields.io/badge/version-0.3.3-blue.svg)](https://github.com/rob-vandenberg/gyroscroll/releases)
[![Platform](https://img.shields.io/badge/platform-Windows-lightgrey.svg)]()
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE.txt)

---

## What is chiral scrolling?

Chiral scrolling lets you scroll continuously by moving your finger in a **circle** along the edge of your touchpad — like spinning a wheel. Once you start the circular motion, you can keep going indefinitely without ever lifting your finger.

- Move your finger **up and down** along the **right edge** → vertical scroll
- Move your finger **left and right** along the **bottom edge** → horizontal scroll
- **Circle continuously** in either direction for uninterrupted scrolling
- **Reverse direction** mid-gesture by simply reversing your circular motion

This feature was common on older Synaptics touchpads but disappeared with the introduction of Windows Precision Touchpads. GyroScroll brings it back.

---

## Requirements

- Windows 10 or Windows 11
- A **Windows Precision Touchpad** (WPT) — the built-in touchpad on most modern laptops

> **Not sure if your touchpad qualifies?**  
> Go to *Settings → Bluetooth & devices → Touchpad*. If you see advanced gesture options, you almost certainly have a Precision Touchpad.

---

## Installation

GyroScroll is a single portable executable — no installer needed.

1. Download `GyroScroll.exe` from the [Releases](https://github.com/rob-vandenberg/gyroscroll/releases) page
2. Copy it to any folder you like (e.g. `C:\Tools\GyroScroll\`)
3. Double-click to run — a small icon appears in the system tray

That's it. GyroScroll is now active.

### Run at startup (optional)

To have GyroScroll start automatically with Windows:

1. Press `Win + R`, type `shell:startup`, press Enter
2. Create a shortcut to `GyroScroll.exe` in the folder that opens

---

## Usage

Once running, GyroScroll works silently in the background.

| Gesture | Action |
|---|---|
| Finger touching the **right edge**, moving up/down | Vertical scroll |
| Finger touching the **bottom edge**, moving left/right | Horizontal scroll |
| Continuing in a circle | Continuous scroll |
| Reversing direction | Scroll in opposite direction |

> **Tip:** You don't need to draw a perfect circle. Start at the edge and let your finger flow naturally — the scrolling follows your movement intuitively.

---

## Settings

Right-click the tray icon and choose **Settings** to open the settings window.

![Settings window](docs/settings.png)

### Edge zones

The coloured areas in the touchpad preview show the active scroll zones.

| Setting | Description |
|---|---|
| **Right edge** | Width of the vertical scroll zone (1–30). Default: 8 |
| **Bottom edge** | Height of the horizontal scroll zone (1–30). Default: 8 |

Values represent a percentage of the touchpad dimension. A value of 8 means the zone covers 8% of the pad width/height. Increase if the zone feels too narrow to trigger reliably; decrease if it interferes with normal cursor movement.

You can adjust these values by **typing a number** in the box or **dragging the slider**. The touchpad preview updates live as you change the values.

### Scroll speed

| Setting | Description |
|---|---|
| **Vertical** | How fast vertical scrolling responds (1–40). Default: 20 |
| **Horizontal** | How fast horizontal scrolling responds (1–40). Default: 20 |

Higher values = faster scrolling per finger movement. Adjust to taste.

### Natural (reversed) scroll

| Setting | Description |
|---|---|
| **Reverse vertical** | Inverts the vertical scroll direction |
| **Reverse horizontal** | Inverts the horizontal scroll direction |

Enable these if you prefer the "natural" scrolling style where content follows your finger rather than the traditional direction.

---

## Tray icon

| Action | Result |
|---|---|
| **Left double-click** | Open Settings |
| **Right-click** | Context menu (Settings / Quit) |

---

## Settings file

Settings are stored in `GyroScroll.ini` in the same folder as the executable. You can edit this file in any text editor if you prefer.

```ini
[GyroScroll]
EdgeRight=0.08
EdgeBottom=0.08
SpeedV=20
SpeedH=20
NaturalV=0
NaturalH=0
```

---

## Troubleshooting

**GyroScroll doesn't seem to do anything**  
Make sure your touchpad is a Windows Precision Touchpad (see Requirements above). Older or non-standard touchpads are not supported.

**The scroll zone triggers accidentally during normal use**  
Reduce the Edge zone values in Settings (try 5 or 6 instead of 8).

**Scrolling feels too fast or too slow**  
Adjust the Speed values in Settings. Start around 15 for slower, 30 for faster.

**The tray icon is missing**  
Check the hidden icons area in the taskbar (the `^` arrow). You can drag the icon to the visible tray area to keep it always shown.

**Only one instance can run at a time**  
If you see "GyroScroll is already running", check the system tray — it's already active.

---

## Acknowledgements

Scroll algorithm inspired by [ChiralScroll](https://chiralscroll.sourceforge.net) by Derek Brown, used under the MIT License.

---

## License

GyroScroll is released under the [MIT License](LICENSE.txt).  
Copyright © 2025 rob-vandenberg