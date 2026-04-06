# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.


## Project

gaudi-monitor is a single-file C terminal system monitor for Intel Gaudi AI Accelerators. It combines CPU, memory, and HPU monitoring in one ncurses TUI, adapted directly from the nv-monitor architecture.

## Build

```bash
make          # builds gaudi-monitor binary (-O3 -march=native)
make portable # builds without -march=native (for CI/distribution)
make test     # builds and runs unit tests
make clean    # removes binaries
```

Direct compilation: `gcc -O2 -Wall -Wextra -std=gnu11 -o gaudi-monitor gaudi-monitor.c -lncursesw -ldl -lpthread`

Dependencies: `build-essential`, `libncurses-dev`

Works on both **aarch64** and **x86_64**.

## Architecture

Everything is in `gaudi-monitor.c` (~1500 lines). Key sections:

- **HLML dynamic loading** (line ~55): Loads `libhlml.so.1` via `dlopen`/`dlsym` at runtime. All HLML function pointers are prefixed with `p` (e.g. `pHlmlInit`). Falls back to sysfs discovery if HLML is unavailable.
- **CPU sampling**: Reads `/proc/stat` delta between frames to compute per-core usage percentages.
- **Memory**: Parses `/proc/meminfo` for used/available/buffers/cached/swap.
- **CPU thermals/freq**: Reads from `/sys/class/thermal/` and `/sys/devices/system/cpu/`.
- **HPU process info**: Queries compute process list via HLML, resolves PID to command name and user via `/proc/<pid>/`.
- **Sysfs fallback**: When HLML is unavailable, detects Gaudi devices via `/sys/class/habanalabs/` and reads `device_type`.
- **TUI rendering**: ncursesw (wide character support) with color pairs (1=red/critical, 2=green/normal, 3=yellow/medium, 6=cyan/headers). `draw_screen()` is the main render function.
- **History chart**: Ring buffer of last 20 CPU/HPU samples, rendered as full-width vertical bar chart using Unicode block elements (▁▂▃▄▅▆▇█).
- **CSV logging**: Opt-in via `-l FILE`, writes timestamped rows with all CPU/memory/HPU metrics.
- **Prometheus exporter**: Opt-in via `-p PORT`. Runs a minimal HTTP server on a dedicated pthread. All metrics use `gaudi_` prefix.

## HLML vs NVML differences

gaudi-monitor uses the HLML (Habana Labs Management Library) API which closely mirrors NVML:

| NVML | HLML |
|------|------|
| `nvmlInit` | `hlmlInit` |
| `nvmlUtilization_t.gpu` | `hlmlUtilization_t.aip` |
| `NVML_TEMPERATURE_GPU` | `HLML_TEMPERATURE_AIP` |
| `NVML_CLOCK_GRAPHICS` | `HLML_CLOCK_AIC` (AI Core) |
| `NVML_CLOCK_MEM` | `HLML_CLOCK_MME` (Matrix Mult Engine) |
| `nvmlProcessInfo_t.usedGpuMemory` | `hlmlProcessInfo_t.usedHlMemory` |
| No encoder/decoder | N/A — Gaudi is compute-only |

## Locale / decimal separator (CRITICAL for Prometheus)

The Prometheus exposition format **requires** decimal points (`1.23`), never commas (`1,23`). After `setlocale(LC_ALL, "")`, the code calls `setlocale(LC_NUMERIC, "C")` to force POSIX decimal formatting while preserving Unicode support for the TUI.

**Do not remove** the `setlocale(LC_NUMERIC, "C")` call.

## Gaudi specifics

- Gaudi HPUs expose device info via `/sys/class/habanalabs/hl0/`, `/hl1/`, etc.
- `device_type` sysfs file identifies the Gaudi generation (Gaudi, Gaudi2, Gaudi3).
- HLML library is installed as part of the Gaudi SW Suite at `/usr/lib/habanalabs/`.
- Gaudi 3 supports up to 8 HPUs per server with 96GB HBM each.
- No video encoder/decoder on Gaudi — those sections are omitted entirely.
- Token authentication env var is `GAUDI_MONITOR_TOKEN` (vs `NV_MONITOR_TOKEN` in nv-monitor).
