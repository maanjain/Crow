# sysmon — Live System Monitoring Dashboard (C++)

A live system monitoring dashboard with the entire backend written in C++. No Flask, no Express, no Django — just a compiled binary talking directly to the Linux kernel via `/proc` and `statvfs()`, served over HTTP using [Crow](https://github.com/CrowCpp/Crow).

![language](https://img.shields.io/badge/language-C%2B%2B17-blue)
![framework](https://img.shields.io/badge/framework-Crow-green)
![platform](https://img.shields.io/badge/platform-Linux-lightgrey)

---

## What it does

- Reads live **CPU usage** (overall + per-core), **RAM usage**, **disk usage**, and **system uptime** directly from the kernel
- Exposes all of it as a JSON API (`/stats`)
- Serves a live dashboard (`/`) that polls the API every second and renders real-time graphs with Chart.js

No external monitoring library is used — every metric is read straight from the same files the `top`, `free`, and `df` commands use under the hood.

---

## Tech stack

| Layer | Tool |
|---|---|
| Backend language | C++17 |
| Web framework | [Crow](https://github.com/CrowCpp/Crow) (header-only, Flask-like routing for C++) |
| Data sources | `/proc/stat`, `/proc/meminfo`, `/proc/uptime`, `statvfs()` syscall |
| Frontend | Plain HTML/CSS/JS, [Chart.js](https://www.chartjs.org/) via CDN |
| Data format | JSON (built with Crow's `crow::json::wvalue`) |

---

## Project structure

```
dashboard/
├── app.cpp     # entire backend + embedded HTML/JS frontend
└── README.md
```

Everything — backend logic, JSON API, and the HTML page it serves — lives in a single `app.cpp` file for simplicity.

---

## Setup & running

### 1. Install dependencies
```bash
sudo apt update
sudo apt install -y build-essential cmake git libasio-dev
```

### 2. Get Crow
```bash
git clone https://github.com/CrowCpp/Crow.git
```
Crow is header-only — no build/install step needed, just point the compiler at its `include/` folder.

### 3. Compile
```bash
g++ -std=c++17 app.cpp -o app -I /path/to/Crow/include -lpthread
```

### 4. Run
```bash
./app
```

### 5. View it
Open `http://localhost:8080` in a browser. The dashboard updates every second.

---

## How the backend works

The core of the project is five functions, each responsible for reading one piece of system data. They're all called together inside the `/stats` route:

```cpp
double cpuUsage     = getCpuUsagePercent();
double ramUsage      = getRamUsagePercent();
double diskUsage     = getDiskUsagePercent();
double uptime        = getUptimeSeconds();
std::vector<double> coreUsages = getPerCoreCpuUsage();
```

Here's what each one actually does:

### `getCpuUsagePercent()`
Returns overall CPU usage as a percentage.

CPU time in Linux isn't reported as a live percentage anywhere — `/proc/stat` only gives **cumulative totals since boot** (how many "jiffies" the CPU has spent idle, in user mode, in system mode, etc.). To get a usage percentage *right now*, this function:
1. Takes a snapshot of those totals
2. Waits 200ms
3. Takes a second snapshot
4. Calculates how much "idle time" passed vs. total time passed in that window

```
usage% = (1 − idle_delta / total_delta) × 100
```

This delta-based approach is the same technique tools like `top` use internally.

### `getPerCoreCpuUsage()`
Same idea as above, but per-core instead of system-wide.

`/proc/stat` has one line per core (`cpu0`, `cpu1`, `cpu2`...) in addition to the combined `cpu` summary line. This function reads all of them, takes two snapshots 200ms apart (just like the overall version), and returns a `std::vector<double>` — one usage percentage per core, in order. This is what powers the individual core bars on the dashboard.

### `getRamUsagePercent()`
Returns RAM usage as a percentage.

Unlike CPU time, memory stats in `/proc/meminfo` are **instantaneous values**, not cumulative counters — so no delta or waiting is needed here. The function reads just two lines it cares about:
- `MemTotal` — total RAM
- `MemAvailable` — RAM currently available for use (a more accurate "free" measure than `MemFree` alone, since it accounts for reclaimable cache)

```
usage% = (MemTotal − MemAvailable) / MemTotal × 100
```

### `getDiskUsagePercent()`
Returns disk usage of the root filesystem (`/`) as a percentage.

This one doesn't read a `/proc` file at all — it uses `statvfs()`, a POSIX system call that asks the OS directly for filesystem block information (total blocks, available blocks, block size). Disk *capacity* isn't tracked in `/proc/diskstats` (that file tracks I/O activity — reads/writes per second — not how full the disk is), so `statvfs()` is the correct tool for this specific question.

```
usage% = (total_bytes − available_bytes) / total_bytes × 100
```

### `getUptimeSeconds()`
Returns how long the system has been running, in seconds.

`/proc/uptime` contains just two numbers (uptime, and total idle time across all cores) — this function reads only the first one. The dashboard's JavaScript then formats this raw number into a readable "Xd Yh Zm" string.

---

## The `/stats` endpoint

All five values get bundled into one JSON response:

```json
{
  "cpu_percent": 12.4,
  "ram_percent": 58.7,
  "disk_percent": 41.2,
  "uptime_seconds": 93822.5,
  "cores": [10.1, 14.7, 9.3, 15.0]
}
```

The frontend polls this endpoint once per second via `fetch()` and updates the live graphs and numbers on screen — no page reloads, no WebSockets, just simple polling.

---

## Why C++ for this

C++ compiles directly to native machine code with no interpreter or garbage collector in the way — relevant here since the whole program is built around reading raw kernel data and serving it with minimal overhead. Crow gives this Flask-like routing and JSON handling without needing a heavier runtime underneath it.

This project intentionally skips Python/Flask, Node/Express, etc. — not because they're bad choices in general, but as an exercise in working closer to the OS layer directly.

---

## Possible next steps

- Network I/O stats (`/proc/net/dev`)
- Per-process breakdown (top CPU/RAM consumers)
- WebSocket push instead of polling
- Historical data persistence (SQLite) instead of in-memory-only graphs
- Dockerize for easy deployment

---

## License

MIT — use it, fork it, break it, learn from it.
