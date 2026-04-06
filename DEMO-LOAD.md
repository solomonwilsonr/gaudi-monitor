# demo-load — Synthetic CPU Load Generator

A lightweight test harness for validating gaudi-monitor across single machines or multi-node clusters. Generates controllable, sinusoidal CPU loads with zero dependencies beyond a C compiler.

Useful for:
- **End-to-end testing** of gaudi-monitor TUI, CSV logging, and Prometheus pipelines
- **Visual health-checks** — verify bars, colors, and history charts are rendering correctly
- **Multi-node validation** — run on every box in a cluster without installing bulky benchmarking tools

## Building

```bash
make demo-load
```

Or directly:

```bash
gcc -O2 -o demo-load demo-load.c -lpthread -lm
```

## Usage

```bash
./demo-load                        # CPU load on all cores
./demo-load --duration 30s         # Run for 30 seconds
./demo-load --duration 5m          # Run for 5 minutes
./demo-load --until 14:30          # Run until 14:30
./demo-load --allow-long-run       # Run indefinitely (Ctrl-C to stop)
```

### Options

| Flag | Description | Default |
|------|-------------|---------|
| `--duration TIME` | Run for specified time (e.g. 30s, 5m, 1h) | 5 min |
| `--until HH:MM` | Run until specified time (24h format) | off |
| `--allow-long-run` | Allow runs longer than 5 minutes | off |
| `-h` | Show help | |

## How It Works

Spawns one thread per CPU core. Each thread runs a busy-wait/sleep duty cycle following a sine wave, with:
- **Phase offset** per core — so all cores show different utilisation levels at any given moment
- **Slightly different frequencies** per core — creating a rolling wave pattern across cores

## HPU (Gaudi) Load Testing

`demo-load` only generates CPU load. For Gaudi HPU load, use:

### Intel Gaudi Benchmarks (habanalabs-qual)

```bash
# Install the qualification package
sudo apt install habanalabs-qual

# Run the built-in HPU stress test
hl_qual -gaudi2 -test all
```

### SynapseAI Model References

```bash
# Clone Intel's official model references
git clone https://github.com/HabanaAI/Model-References
cd Model-References/PyTorch/computer_vision/classification/torchvision

# Run a simple benchmark (requires PyTorch + SynapseAI)
python3 train.py --model resnet50 --batch-size 256 --device hpu
```

### hl-smi for Quick Status

```bash
hl-smi          # like nvidia-smi but for Gaudi
hl-smi dmon     # streaming metrics (utilization, memory, power, temp)
```

## Example: Multi-Node Production Test

Run demo-load on each node while gaudi-monitor exports Prometheus metrics:

```bash
# On each node:
./demo-load --allow-long-run &
./gaudi-monitor -n -p 9101
```

Then scrape all nodes from Prometheus and verify metrics flow end-to-end through to Grafana — without deploying real AI workloads.
