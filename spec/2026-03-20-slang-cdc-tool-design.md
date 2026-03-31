# sv-cdccheck: Open-Source Structural CDC Analysis Tool

> Design Spec — 2026-03-20
> Status: DRAFT
> Implementation: Standalone C++ project (separate repository)
> Dependencies: slang (C++ library, not JSON)

## 1. Problem Statement

CDC (Clock Domain Crossing) bugs are among the most dangerous in silicon design — they cause
non-deterministic metastability failures that are nearly impossible to reproduce in simulation.
Commercial CDC tools (SpyGlass CDC, Questa CDC, VC CDC) cost $100K+/year and are inaccessible
to small teams, academic users, and open-source projects.

Currently, the RTL Agent Team plugin's `structural` CDC mode is a grep-based placeholder
that only lists clock signal names without any actual crossing analysis.

**Goal**: Build a production-quality open-source structural CDC analyzer that provides
meaningful cross-domain path detection and synchronizer verification using slang's
elaborated design representation.

## 2. Target Users

- RTL designers performing pre-synthesis CDC sign-off
- Open-source silicon projects (OpenTitan, PULP, etc.)
- Academic/educational use
- rtl-agent-team plugin users (via `run_cdc.sh --tool sv-cdccheck`)

## 3. Architecture Overview

```
SV RTL files
    ↓ slang C++ API
Elaborated Design (full hierarchy)
    ↓ Pass 1
Clock Tree Analysis (sources, divisions, gating, muxing)
    ↓ Pass 2
FF Classification (every FF → clock domain)
    ↓ Pass 3
Connectivity Graph (FF-to-FF data paths across modules)
    ↓ Pass 4
Cross-Domain Detection (source.domain ≠ dest.domain)
    ↓ Pass 5
Synchronizer Verification (pattern matching)
    ↓ Pass 6
Report Generation + SDC Constraints
```

### 3.1 Why C++ with slang as library (not JSON)

- **Performance**: Large designs (100K+ FFs) require efficient graph traversal. JSON parsing
  adds 10-100x overhead vs direct AST access.
- **slang API**: Provides elaborated, resolved, type-checked design. No need to re-implement
  elaboration, parameter resolution, or generate-block expansion.
- **Existing ecosystem**: slang is already a dependency of rtl-agent-team plugin.
  Building on it avoids introducing a new parser.

### 3.2 Why elaborated design (not module-level)

CDC fundamentally occurs at **module boundaries** — a single module typically uses one clock,
but the interconnect between modules creates crossings. Module-level analysis cannot see these.

Additionally, clocks from a single source (e.g., PLL output) may propagate through the hierarchy
with different names at each level. Only elaborated analysis can track these relationships.

## 4. Core Analysis Passes

### Pass 1: Clock Tree Analysis

**Input**: Elaborated design from slang
**Output**: Clock domain map with source relationships

#### 4.1.1 Clock Source Identification

- Top-level input ports matching `*_clk` or `clk` patterns
- PLL/MMCM output ports (recognized by module type or attribute)
- Clock divider outputs (`always_ff` with toggle pattern)
- Clock gate outputs (ICG cells)

#### 4.1.2 Clock Propagation Tracking

A clock from a single PLL may propagate as:
```
pll_out_clk → core_clk (at top) → sys_clk (inside subsystem) → proc_clk (inside core)
```

These are the **same physical clock** but with different hierarchical names.
The tool must track propagation through:
- Port connections (direct wire-through)
- Clock buffer/gate cells (recognized by pattern or attribute)
- Clock mux cells (creates conditional domain)

#### 4.1.3 Clock Domain Classification

| Relationship | Definition | CDC Required? |
|-------------|------------|---------------|
| Same source, same edge | Identical domain | No |
| Same source, different edge (posedge/negedge) | Related | Yes (simplified) |
| Same source, divided | Related (harmonic) | Yes |
| Different sources | Asynchronous | Yes (full) |
| Muxed clock | Conditional | Depends on mux control |

#### 4.1.4 User-Supplied Clock Relationships (optional)

```
# clock_domains.sdc or clock_domains.yaml
create_clock -name sys_clk -period 10 [get_ports sys_clk]
create_clock -name axi_clk -period 8  [get_ports axi_clk]

# Declare same-source relationship (tool cannot always infer this)
set_clock_groups -asynchronous -group {sys_clk} -group {axi_clk}
# Or: declare as related
set_clock_groups -physically_exclusive -group {mux_clk_a} -group {mux_clk_b}
```

### Pass 2: FF Classification

**Input**: Elaborated design + clock domain map
**Output**: Every FF mapped to its clock domain

#### 4.2.1 FF Detection

Identify flip-flops from:
- `always_ff @(posedge xxx_clk)` — explicit FF
- `always @(posedge xxx_clk)` — legacy FF style
- Library cell instances (recognized by cell name patterns or `(* cdc_ff *)` attributes)

#### 4.2.2 FF-to-Domain Mapping

Each FF is assigned a clock domain based on its sensitivity list clock signal.
Track the full hierarchical clock path:
```
u_subsys.u_core.u_regfile.always_ff @(posedge proc_clk)
  → proc_clk traces back to → sys_clk at top
  → domain: sys_clk
```

#### 4.2.3 Ambiguous Cases

- FF with multiple clock edges → ERROR (design issue)
- FF inside `generate` block → resolve per-instance
- Latch (`always_latch`) → flag as WARNING (not a proper FF for CDC)
- Async reset FF → track reset domain separately

### Pass 3: Connectivity Graph

**Input**: Elaborated design + FF map
**Output**: Directed graph: FF → combinational logic → FF

This is the most complex pass. Must build a signal-level DAG that tracks
data flow from each FF's output (Q) to the next FF's input (D).

#### 4.3.1 Intra-Module Connectivity

Within a module, trace assignments:
```systemverilog
always_ff @(posedge clk_a) q_a <= d_a;     // FF in domain A
assign wire_x = q_a & some_signal;          // combinational
always_ff @(posedge clk_b) q_b <= wire_x;  // FF in domain B — CROSSING!
```

#### 4.3.2 Inter-Module Connectivity (Port Connections)

```systemverilog
module top;
  module_a u_a (.o_data(wire_ab), .a_clk(clk_a));
  module_b u_b (.i_data(wire_ab), .b_clk(clk_b));
  // wire_ab crosses from domain A to domain B
endmodule
```

slang's elaborated design resolves these connections, so the graph can be
built by walking the elaborated hierarchy.

#### 4.3.3 Graph Representation

```cpp
struct FFNode {
    std::string hier_path;     // e.g., "top.u_a.q_data"
    ClockDomain domain;        // resolved clock domain
    std::vector<FFEdge> fanin; // source FFs feeding this FF
};

struct FFEdge {
    FFNode* source;
    FFNode* dest;
    std::vector<std::string> comb_path; // intermediate combinational signals
    bool has_synchronizer;
    SyncType sync_type;        // NONE, TWO_FF, GRAY, HANDSHAKE, FIFO, MUX
};
```

### Pass 4: Cross-Domain Detection

**Input**: Connectivity graph + domain map
**Output**: List of cross-domain paths

For each edge in the graph where `source.domain ≠ dest.domain`:
1. Check if domains are truly asynchronous (different sources)
2. Check if domains are related (same source, divided/gated)
3. Record the crossing path with full hierarchical signal names

#### 4.4.1 Crossing Classification

| Category | Criteria | Severity |
|----------|----------|----------|
| ASYNC_CROSSING | Different clock sources | HIGH |
| HARMONIC_CROSSING | Same source, integer-divided | MEDIUM |
| GATED_CROSSING | Same source, clock-gated | LOW |
| SAME_DOMAIN | Same source, same characteristics | NONE (not a crossing) |

### Pass 5: Synchronizer Verification

**Input**: Cross-domain paths
**Output**: Annotated paths with synchronizer status

For each crossing, check the destination-domain path for synchronizer patterns:

#### 4.5.1 Recognized Synchronizer Patterns

| Pattern | Detection Heuristic | Valid For |
|---------|--------------------|---------|
| **2-FF synchronizer** | Two consecutive FFs in dest domain with direct connection (no logic between) | Single-bit signals |
| **3-FF synchronizer** | Three consecutive FFs (high-frequency designs) | Single-bit, high MTBF |
| **Gray code** | Multi-bit register where only 1 bit changes per cycle + 2-FF sync per bit | Counters (FIFO pointers) |
| **Handshake (REQ/ACK)** | REQ signal from src→dest (2-FF synced), ACK from dest→src (2-FF synced), data stable while REQ asserted | Multi-bit data |
| **Async FIFO** | Gray-coded write/read pointers + dual-port memory | Streaming data |
| **MUX synchronizer** | Data path through MUX controlled by synced select signal | Multi-bit, quasi-static |
| **Pulse synchronizer** | Toggle in source domain + edge detect in dest domain | Single pulses |

#### 4.5.2 Synchronizer Quality Checks

Even when a synchronizer is present, check for:
- **Reconvergence**: Multiple signals from same source crossing independently
  (must be synchronized together, e.g., via handshake or FIFO)
- **Combinational logic before sync FF**: Logic between source FF and first sync FF
  introduces glitch risk
- **Fan-out after first sync FF**: Data used before completing sync chain
- **Reset synchronizer**: Async reset crossing must use reset synchronizer
  (async assert, sync deassert)

### Pass 6: Report Generation

#### 4.6.1 Violation Categories

| Category | Meaning | Gate? |
|----------|---------|-------|
| `VIOLATION` | No synchronizer on async crossing | FAIL |
| `CAUTION` | Synchronizer exists but has quality issue (reconvergence, glitch path) | WARNING |
| `CONVENTION` | Non-standard clock/reset naming | WARNING |
| `INFO` | Properly synchronized crossing | PASS |
| `WAIVED` | User-waived crossing (via waiver file) | SKIP |

#### 4.6.2 Output Formats

| Format | File | Purpose |
|--------|------|---------|
| Markdown report | `cdc_report.md` | Human-readable summary |
| JSON report | `cdc_report.json` | Tool integration (rtl-agent-team, CI) |
| SDC constraints | `cdc_constraints.sdc` | Synthesis tool input |
| Waiver template | `cdc_waivers.yaml` | User waiver definitions |

#### 4.6.3 Markdown Report Structure

```markdown
# CDC Analysis Report
## Summary
| Category | Count |
|----------|-------|
| VIOLATION | 3 |
| CAUTION | 5 |
| INFO | 42 |

## Clock Domains
| Domain | Source | Type | Signals |
| sys_clk | PLL0 | primary | 1247 FFs |
| axi_clk | PLL0 | divided (1/2) | 523 FFs |
| pixel_clk | ext_osc | async | 89 FFs |

## Crossings
### VIOLATION-001: sys_clk → pixel_clk (no synchronizer)
- Source: top.u_ctrl.o_frame_start (sys_clk, posedge)
- Dest:   top.u_display.i_frame_start (pixel_clk, posedge)
- Path:   top.u_ctrl.q_frame_start → wire_frame_start → top.u_display.d_frame_start
- Fix:    Insert 2-FF synchronizer at top.u_display.i_frame_start
```

## 5. CLI Interface

```
sv-cdccheck [OPTIONS] <SV_FILES...>

Required:
  <SV_FILES...>           SystemVerilog source files or -f <filelist>
  --top <module>          Top-level module name

Output:
  -o, --output <dir>      Output directory (default: ./cdc_reports/)
  --format <fmt>          md|json|sdc|all (default: all)

Clock specification:
  --sdc <file>            SDC file with clock definitions
  --clock-yaml <file>     YAML file with clock domain relationships
  --auto-clocks           Auto-detect clocks from port names (default)

Analysis control:
  --waiver <file>         Waiver file (YAML) for known crossings
  --sync-stages <n>       Required synchronizer stages (default: 2)
  --ignore-gated          Don't flag gated-clock crossings
  --strict                Treat CAUTION as VIOLATION

Slang options (pass-through):
  -I <dir>                Include directory
  -D <macro>=<val>        Define preprocessor macro
  --std <ver>             SystemVerilog standard version

Verbosity:
  -v, --verbose           Detailed output
  -q, --quiet             Only violations and summary
  --dump-graph <file>     Dump connectivity graph (DOT format for debug)
```

### Example Usage

```bash
# Basic usage
sv-cdccheck --top soc_top -f rtl/filelist_top.f -o sim/cdc/reports/

# With clock constraints
sv-cdccheck --top soc_top -f rtl/filelist_top.f --sdc syn/constraints/clocks.sdc

# With waivers
sv-cdccheck --top soc_top -f rtl/filelist_top.f --waiver sim/cdc/waivers.yaml

# CI mode (exit code = violation count)
sv-cdccheck --top soc_top -f rtl/filelist_top.f --format json --quiet
```

## 6. Waiver Mechanism

```yaml
# cdc_waivers.yaml
waivers:
  - id: WAIVE-001
    crossing: "top.u_ctrl.o_cfg_data -> top.u_slow.i_cfg_data"
    reason: "Quasi-static configuration, stable before use"
    owner: "john@example.com"
    date: "2026-03-20"

  - id: WAIVE-002
    pattern: "top.u_debug.*"
    reason: "Debug-only signals, not in production path"
    owner: "team-lead@example.com"
```

## 7. Clock Specification Format

When auto-detection is insufficient (e.g., clocks from same PLL with different names):

```yaml
# clock_domains.yaml
clock_sources:
  - name: pll0
    outputs:
      - signal: sys_clk
        frequency: 200MHz
      - signal: axi_clk
        frequency: 100MHz
        relationship: divided_by_2  # same source as sys_clk
      - signal: pixel_clk
        frequency: 74.25MHz
        relationship: independent   # truly async

  - name: ext_osc
    outputs:
      - signal: ref_clk
        frequency: 25MHz

domain_groups:
  # Explicitly declare async groups
  async:
    - [sys_clk, pixel_clk]
    - [sys_clk, ref_clk]

  # Explicitly declare related (same source)
  related:
    - [sys_clk, axi_clk]  # from pll0, harmonic
```

## 8. Integration with rtl-agent-team

### 8.1 Installation via rat-setup

`rat-setup` Phase 1 (tool audit) checks for `sv-cdccheck`:
```bash
sv-cdccheck --version 2>&1 || echo "NOT_FOUND"
```

If not found, offer installation:
```bash
# Option 1: Pre-built binary (when available)
# Option 2: Build from source
git clone https://github.com/<org>/sv-cdccheck.git ~/tools/sv-cdccheck
cd ~/tools/sv-cdccheck
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j$(nproc)
cmake --install build
```

### 8.2 run_cdc.sh Integration

Replace current grep-based `structural` mode:
```bash
# In run_cdc.sh, structural case:
structural|sv-cdccheck)
    CMD="sv-cdccheck --top $TOP -f $FILELIST -o $OUTDIR --format all"
    [ -n "$WAIVER_FILE" ] && CMD="$CMD --waiver $WAIVER_FILE"
    run_tool $CMD
    ;;
```

### 8.3 cdc-checker Agent Integration

The `cdc-checker` agent invokes `run_cdc.sh --tool sv-cdccheck` and parses the JSON report
to classify violations for the orchestrator.

## 9. Implementation Phases

### Phase 1: Foundation (MVP)
- slang library integration, design elaboration
- Clock source identification (port name patterns)
- FF detection and domain classification
- Basic connectivity graph (direct FF-to-FF, same module)
- Cross-domain detection (different clock name = different domain)
- 2-FF synchronizer pattern detection
- Markdown + JSON report generation
- **Deliverable**: Catches missing synchronizers in simple designs

### Phase 2: Hierarchy & Clock Tree
- Inter-module connectivity (port-level path tracking)
- Clock propagation tracking (same source, different names)
- Clock division/gating recognition
- Related vs async domain classification
- SDC constraint generation
- User-supplied clock specification (YAML/SDC)
- **Deliverable**: Handles real multi-module designs

### Phase 3: Advanced Synchronizer Analysis
- Gray code / async FIFO pattern recognition
- Handshake (REQ/ACK) pattern recognition
- Pulse synchronizer detection
- Reconvergence detection
- Combinational logic before sync FF (glitch risk)
- Fan-out-before-sync detection
- Reset synchronizer verification
- **Deliverable**: Quality analysis comparable to commercial tools (structural)

### Phase 4: Usability & Integration
- Waiver mechanism (YAML)
- Incremental analysis (cache, only re-analyze changed modules)
- DOT graph export for visualization
- CI integration (exit codes, GitHub Actions example)
- Performance optimization for 100K+ FF designs
- **Deliverable**: Production-ready tool

## 10. Build System

```cmake
cmake_minimum_required(VERSION 3.20)
project(sv-cdccheck VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

# slang as dependency (FetchContent or find_package)
find_package(slang REQUIRED)
# Or: FetchContent from github

add_executable(sv-cdccheck
    src/main.cpp
    src/clock_tree.cpp
    src/ff_classifier.cpp
    src/connectivity.cpp
    src/crossing_detector.cpp
    src/sync_verifier.cpp
    src/report_generator.cpp
)

target_link_libraries(sv-cdccheck PRIVATE slang::slang)
```

## 11. Testing Strategy

| Level | What | How |
|-------|------|-----|
| Unit | Each pass independently | Minimal SV snippets → expected output |
| Integration | Full pipeline | Small multi-clock designs → known violations |
| Regression | Correctness over time | Golden reference reports |
| Benchmark | Performance | OpenTitan / PULP Platform designs |

### Test Cases (Phase 1 MVP)

```
tests/
  basic/
    01_no_crossing.sv          — single domain, expect 0 violations
    02_missing_sync.sv         — direct FF-to-FF cross-domain, expect VIOLATION
    03_two_ff_sync.sv          — proper 2-FF sync, expect INFO
    04_three_ff_sync.sv        — 3-FF sync, expect INFO
    05_comb_before_sync.sv     — logic before sync FF, expect CAUTION
  hierarchy/
    10_port_crossing.sv        — cross-domain via port connection
    11_nested_modules.sv       — 3-level hierarchy crossing
    12_same_source_clocks.sv   — divided clock from same PLL
  advanced/
    20_gray_code_fifo.sv       — async FIFO with gray code
    21_handshake.sv            — REQ/ACK handshake
    22_reconvergence.sv        — multiple signals from same source
    23_reset_crossing.sv       — async reset CDC
```

## 12. Non-Goals (explicitly out of scope)

- **Formal CDC proof**: This is structural analysis (pattern matching), not formal verification.
  Commercial tools use formal engines to prove synchronizer correctness — we do structural detection.
- **Timing analysis**: No setup/hold/MTBF calculation. This is a structural checker.
- **Auto-fix**: The tool reports violations. It does NOT insert synchronizers automatically.
- **Gate-level analysis**: Input is RTL (behavioral), not synthesized netlist.
- **Metastability MTBF calculation**: Requires technology library data we don't have.

## 13. Success Criteria

| Metric | Target |
|--------|--------|
| Phase 1 MVP: catches direct FF-to-FF missing sync | 100% on test suite |
| Phase 2: handles 10+ module hierarchy | OpenTitan ibex core as benchmark |
| Phase 3: recognizes 5 synchronizer patterns | 2-FF, 3-FF, gray, handshake, pulse |
| Phase 4: processes 100K FF design in <60s | Performance benchmark |
| False positive rate | <10% on real designs (with waivers) |
| False negative rate (missed violations) | 0% for direct FF-to-FF crossings |

## 14. References

- Clifford Cummings, "Clock Domain Crossing (CDC) Design & Verification Techniques" (SNUG 2008)
- slang documentation: https://sv-lang.com
- slang C++ API: https://github.com/MikePopoloski/slang
- OpenTitan CDC methodology: https://opentitan.org/book/hw/methodology/clock_domain_crossing.html
- IEEE 1800-2017 SystemVerilog standard (sensitivity lists, always_ff semantics)
