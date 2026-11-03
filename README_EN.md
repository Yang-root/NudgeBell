**English** | [简体中文](README.md)

# NudgeBell

**A focus bell that never tells you when it'll ring.** It catches you the second you drift.

![size](https://img.shields.io/badge/size-916%20KB-informational) ![platform](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-blue) ![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C) ![license](https://img.shields.io/badge/license-MIT-green)

[![⬇ Download](https://img.shields.io/badge/%E2%AC%87_Download-916%20KB-2ea44f?style=for-the-badge)](https://github.com/Yang-root/NudgeBell/raw/main/NudgeBell.exe)

[Install](#install) · [How it works](#the-three-pieces) · [Screenshots](#also-in-the-box) · [Internals](docs/tech.md)

![Main window](docs/screenshot-main.png)

---

## Why a timer doesn't work

Timers fail for one reason: **you know exactly when they'll fire.**

So you wait for it. You glance at the clock, brace, and drift off anyway. Within a
week the alarm has become another predictable piece of background noise.

NudgeBell never tells you. Every ring re-rolls the interval on the spot. You don't
know if the next one is 3 minutes away or 6 — so a small part of your attention has
nowhere to go but the thing in front of you.

---

## The three pieces

**① Blind-box bell**
Rings once at a random moment inside your range, then re-rolls immediately. 3–6
minutes by default; both ends are sliders, so 15–40 is just as easy if you'd rather
not be interrupted that often.

**② 90 + 20 cycle**
Focus 90 minutes, get told to get up for 20, come back on your own. Bells are fully
suspended during the break and the break doesn't count as focus time. Want a
pomodoro instead? Drag it to 25/5.

![Break state](docs/screenshot-break.png)

**③ Floating card**
Not an overlay inside the main window. Minimised to the tray, buried under other
windows, in a fullscreen video — it still surfaces on top, then leaves after 6
seconds. Click anywhere to dismiss; it never steals focus from what you're typing.

![Floating card](docs/screenshot-card.png)

---

## Install

[Download](https://github.com/Yang-root/NudgeBell/raw/main/NudgeBell.exe) (916 KB) and double-click. No installer, no runtime.

**916 KB / ≈22 MB RAM / instant start** — a thing that occasionally goes *ding*
has no business eating half a browser.

> Windows will flag it on first run (the exe is unsigned): **More info → Run anyway**.
> A code signing certificate costs a few hundred dollars a year; this is a personal
> project. The source is in `src/` if you'd rather build it yourself.

**Closing the window is not quitting.** The X hides it to the tray and it keeps
counting — that's the whole point when the book is on your desk and you're not
looking at the screen. Actual quit is in the tray menu.

---

## Also in the box

- **12 sounds**, all synthesised at runtime, zero audio files in the repo. A
  different one every time — the same sound for a month stops being a signal
- **Monthly heatmap** — one glance tells you which days you were actually working
- **Three ambient beds** — rain, brown noise, low frequency, independent volume
- **Everything adjustable** — drag the slider, or tap − and + on either side

![Heatmap](docs/screenshot-heatmap.png)

---

## Settings

One page for everything: random range, focus/break lengths, which of the 12 sounds
are in the pool, volume, ambient bed, tray behaviour.

Every number has three ways to change it — drag the slider, tap `−`, tap `+`. Nothing
is written to disk until you hit **Save** in the bottom bar.

![Settings](docs/screenshot-settings.png)

---

## Build it yourself

Requires MinGW-w64 (g++ 11+), then `./build.sh` produces the 916 KB exe.

Hand-written Win32 + GDI+, C++17, statically linked — no .NET, no Qt, no Electron,
no browser engine. Internals, how the 12 sounds are synthesised, and what the 67
tests actually check → [**docs/tech.md**](docs/tech.md) *(in Chinese)*

---

## License

MIT. Use it, change it, ship it. No warranty.

If it bought you half an extra hour, it did its job.
