# slang-cdc

Open-source structural CDC (Clock Domain Crossing) analysis tool for SystemVerilog RTL designs.

Built on [slang](https://github.com/MikePopoloski/slang) — a fast, compliant SystemVerilog compiler library.

## Why

CDC bugs cause non-deterministic metastability failures that are nearly impossible to reproduce in simulation. Commercial CDC tools cost $100K+/year and are inaccessible to small teams, academic users, and open-source silicon projects.

**slang-cdc** provides structural CDC analysis using slang's elaborated design representation — detecting cross-domain paths, verifying synchronizer patterns, and generating actionable reports.

## Features (Phase 1 MVP)

- Clock source auto-detection from port naming patterns
- SDC constraint parsing (`create_clock`, `create_generated_clock`, `set_clock_groups`)
- Flip-flop detection and clock domain classification from `always_ff` sensitivity lists
- Async reset detection
- FF-to-FF connectivity graph construction
- Cross-domain crossing detection with severity classification
- 2-FF / 3-FF synchronizer pattern recognition
- Markdown and JSON report generation

## Quick Start

### Prerequisites

- C++20 compiler (GCC 11+, Clang 17+, Apple Xcode 16+)
- CMake 3.20+
- Python 3 (required by slang code generation)

### Build

```bash
git clone https://github.com/babyworm/slang-cdc.git
cd slang-cdc
make build    # fetches slang v10.0 automatically via CMake FetchContent
```

### Run

```bash
# Basic usage
./build/slang-cdc --top my_soc rtl/*.sv

# With SDC constraints
./build/slang-cdc --top my_soc -f rtl/filelist.f --sdc constraints/clocks.sdc

# Help
./build/slang-cdc --help
```

### Test

```bash
make test
```

### Install

```bash
make install              # installs to ~/.local/bin
INSTALL_PREFIX=/usr/local make install  # custom prefix
```

## Makefile Targets

| Target | Description |
|--------|-------------|
| `make deps` | Fetch slang + dependencies via CMake FetchContent |
| `make build` | Release build |
| `make debug` | Debug build |
| `make test` | Run test suite |
| `make install` | Install binary |
| `make clean` | Remove build directories |

## Architecture

```
SV RTL files
    | slang C++ API
Elaborated Design (full hierarchy)
    | Pass 1
Clock Tree Analysis (sources, divisions, gating, muxing)
    | Pass 2
FF Classification (every FF -> clock domain)
    | Pass 3
Connectivity Graph (FF-to-FF data paths)
    | Pass 4
Cross-Domain Detection (source.domain != dest.domain)
    | Pass 5
Synchronizer Verification (pattern matching)
    | Pass 6
Report Generation (Markdown + JSON)
```

## Violation Categories

| Category | Meaning |
|----------|---------|
| `VIOLATION` | No synchronizer on async crossing |
| `CAUTION` | Synchronizer exists but has quality issue |
| `INFO` | Properly synchronized crossing |

## SDC Support

slang-cdc parses a subset of SDC relevant to CDC analysis:

```tcl
create_clock -name sys_clk -period 10 [get_ports sys_clk]
create_generated_clock -name div_clk -source [get_ports sys_clk] -divide_by 2 [get_pins u_div/Q]
set_clock_groups -asynchronous -group {sys_clk} -group {ext_clk}
```

## Roadmap

- **Phase 1 (current):** Single-module CDC with basic synchronizer detection
- **Phase 2:** Inter-module hierarchy traversal, clock propagation tracking
- **Phase 3:** Advanced synchronizer patterns (Gray code, handshake, async FIFO)
- **Phase 4:** Waiver mechanism, incremental analysis, CI integration

## License

[MIT](LICENSE) — Hyun-Gyu Kim

See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for dependency licenses.

## References

- Clifford Cummings, "Clock Domain Crossing (CDC) Design & Verification Techniques" (SNUG 2008)
- [slang documentation](https://sv-lang.com)
- [slang source](https://github.com/MikePopoloski/slang)
- [OpenTitan CDC methodology](https://opentitan.org/book/hw/methodology/clock_domain_crossing.html)
