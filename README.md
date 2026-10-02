# devs-cadmium-everything

A **Smart Greenhouse** modelled with the DEVS formalism and simulated with
[Cadmium](https://github.com/SimulationEverywhere/cadmium), built to exercise **every feature of the
library**: both formalisms (Parallel and classic DEVS), both modelling styles (static templates and
the dynamic API), all three execution modes (single thread, Boost.Thread, OpenMP), the whole
logging system, every stock model, compile-time model validation, and Cell-DEVS on a grid *and*
on a graph.

It is written to be read. Every source file starts with *what* it is, *why* it exists and *how*
it works, models carry their formal DEVS definition next to the C++ that implements it, and every
demo checks its own claims, so what this document says is verified by `make test`.

> **Status:** 9 demo programs built into 11 binaries (~700 self-checks), 17 must-not-compile tests,
> an API-coverage audit and a three-way engine-mode trace comparison, all green. Details in
> [Testing](#testing).

---

## Contents

1. [Quick start](#quick-start)
2. [DEVS and Cadmium in five minutes](#devs-and-cadmium-in-five-minutes)
3. [The system we model](#the-system-we-model)
4. [What the simulation does (a timeline you can check by hand)](#what-the-simulation-does)
5. [The models, one by one](#the-models-one-by-one)
6. [The coupled models](#the-coupled-models)
7. [Tour of the demos](#tour-of-the-demos)
8. [Reference tables](#reference-tables) (logging matrix, JSON formats, delay buffers)
9. [Execution modes and concurrency](#execution-modes-and-concurrency)
10. [Testing](#testing)
11. [Findings: pitfalls and upstream issues](#findings-pitfalls-and-upstream-issues)
12. [Cadmium API coverage](#cadmium-api-coverage)
13. [Extending the project](#extending-the-project)
14. [References](#references)

---

## Quick start

```bash
make -j8        # build the 9 demos (+2 parallel variants of demo 03) into build/   (~20 s)
make test       # build, run every demo, compile-fail tests, API audit, mode comparison
make run-02_static_greenhouse     # build and run one demo (any file name in src/)
```

**Requirements:** a C++17 compiler (developed with g++ 14), GNU make, Boost (Thread, System) for the
concurrent mode, OpenMP for the parallel mode. Cadmium itself is **header-only**; the Makefile
looks for it at `../Cadmium-Simulation-Environment/cadmium` (relative to this repository) and you can
point it elsewhere:

```bash
make CADMIUM=/path/to/cadmium test
```

Developed against Cadmium commit `5f6fb2e` ("Include limits", 2021-09-28) with its bundled
nlohmann json v3.8.0. `compile_flags.txt` gives editors (clangd, VS Code) the include paths.

### Repository layout

```
include/greenhouse/                the model library (header-only, heavily commented)
  prelude.hpp                        MUST come first: fixes a Cadmium include-order trap (see Findings)
  fixed_time.hpp                     a user-defined TIME type (integer milliseconds + infinity)
  messages.hpp                       message types (power, heater_command)
  debug_log.hpp                      user-level `logger_debug` channel for models
  logging.hpp                        custom sinks (memory, file, thread-safe) and trace_formatter
  trace.hpp, check.hpp               trace parsing and the tiny CHECK() test helper
  models/                            thermostat, heater, stock-model aliases (+ sensor_feed)
  systems/                           static_greenhouse.hpp, dynamic_greenhouse.hpp
  floor/                             Cell-DEVS: floor_cells/floor_coupled (grid), pipe_network (graph), cell_probe
src/                               the nine demos, numbered in reading order
data/                              sensor_feed.txt, floor.json, pipes.json
tests/compile_fail/                17 snippets that Cadmium must REJECT at compile time + the runner script
tools/coverage_check.sh            audits that every Cadmium header/identifier is covered
docs/cadmium_api.txt               the audited list of Cadmium's public API (253 + 27 + 3 items)
Makefile, compile_flags.txt
```

**Suggested reading order:** this README, then `src/01_atomic_by_hand.cpp`, `models/thermostat.hpp`,
`systems/static_greenhouse.hpp`, `src/02`, `src/03`, then the demo of whatever interests you.

---

## DEVS and Cadmium in five minutes

### The formalism

**DEVS** (Discrete Event System Specification, Zeigler) describes a system as a hierarchy of
*models* that exchange *events*, with simulated time that jumps from event to event (no fixed time
step). Two kinds of model:

* An **atomic model** `M = < X, S, Y, δint, δext, [δcon,] λ, ta >`

  | symbol | meaning |
  |---|---|
  | `X` | the inputs it accepts (values arriving on input *ports*) |
  | `Y` | the outputs it can emit (values on output *ports*) |
  | `S` | its state |
  | `ta(s)` | **time advance**: how long the model stays in state `s` if nothing arrives (may be ∞ = passive) |
  | `λ(s)` | **output function**: what is emitted just *before* an internal transition |
  | `δint(s)` | **internal transition**: new state when `ta(s)` elapses |
  | `δext(s, e, x)` | **external transition**: new state when input `x` arrives after `e` time units in `s` |
  | `δcon(s, x)` | **confluent transition** *(Parallel DEVS only)*: what to do when `x` arrives at the very instant `ta` expires |

* A **coupled model** `N = < X, Y, D, {Md}, EIC, EOC, IC [, Select] >`: a set of sub-models `D`,
  wired by **IC** (internal couplings: sub-model output → sub-model input), **EIC** (external input
  couplings: the coupled model's input → a sub-model's input) and **EOC** (external output couplings:
  a sub-model's output → the coupled model's output). Coupled models nest, so the hierarchy is a tree.

**Classic vs Parallel DEVS.** In classic DEVS only *one* imminent component may act at a time, so a
coupled model needs a `Select` function to break ties, and results can depend on that arbitrary
choice. **Parallel DEVS** (Chow & Zeigler) lets all imminent components act together, delivers inputs
as *bags* (several messages per port), and adds `δcon` for simultaneous internal/external events.
Cadmium's simulators are Parallel DEVS. Demo 06 shows the difference concretely.

**Cell-DEVS** (Wainer) is DEVS for spatial models: a lattice (or any graph) of identical cells, each a
DEVS atomic model with a local rule, a neighbourhood, and an output *delay*.

### The simulation algorithm (what every engine implements)

```
init:      every atomic model m:   next_m := t0 + ta(s_m)
repeat:    t := min over all models of next_m
           1. collect outputs:   every model with next_m == t evaluates λ(s)
           2. route messages:    follow IC / EIC / EOC couplings to fill each model's input bag
           3. advance:           every model with next_m == t or a non-empty input bag
                                   input + next == t   -> δcon(s, x)
                                   input only          -> δext(s, t - last, x)
                                   no input, next == t -> δint(s)
                                 then last := t, next := t + ta(s')
```

### How Cadmium encodes it

| DEVS | Cadmium | where to look |
|---|---|---|
| input / output ports | `struct p : in_port<MSG>`, `struct q : out_port<MSG>` (a port **is a type**) | `modeling/ports.hpp` |
| `X`, `Y` | `input_ports` / `output_ports` = `std::tuple<port types>`; one `message_bag<PORT>` per port | `message_bag.hpp` |
| `S` | `state_type` and a public member `state` (printed by `operator<<` in logs) | any model |
| `δint` `δext` `δcon` | `internal_transition()`, `external_transition(TIME e, bags)`, `confluence_transition(TIME e, bags)` | |
| `λ`, `ta` | `output() const` returning the output bags, `time_advance() const` returning TIME | |
| time | a template parameter `TIME` of every model (float, double, or your own type) | `fixed_time.hpp` |
| coupled model | static: `coupled_model<TIME, IP, OP, models_tuple<...>, EICs, EOCs, ICs>`; dynamic: `dynamic::modeling::coupled<TIME>` | `systems/` |
| EIC/EOC/IC | static: `EIC<…>`, `EOC<…>`, `IC<…>` types; dynamic: `make_EIC/make_EOC/make_IC<PORT_FROM,PORT_TO>(ids)` | |
| `Select` (classic) | `devs::coupling<…, SELECT>` (a type; **no classic simulator is provided**) | `src/06` |
| abstract simulator | `simulator` (atomic), `coordinator` (coupled), `runner` (root coordinator) | `src/04` |

**Two modelling styles.**

* **Static**: the whole model, ports and couplings included, is a *type*. The compiler validates it
  (static_asserts, see `tests/compile_fail`) and the engine is generated code without virtual calls.
  Structure cannot depend on run-time data.
* **Dynamic**: models are objects, ports are `std::type_index`, messages travel type-erased
  (`std::map<type_index, boost::any>`), couplings are run-time `link<PORT_FROM, PORT_TO>` objects.
  Structure can come from a file, and this is the style that supports the concurrent/OpenMP engines
  and Cell-DEVS. `dynamic::translate::make_dynamic_coupled_model<TIME, STATIC_MODEL>()` converts a
  static model into a dynamic one automatically.

---

## The system we model

A greenhouse with an **irrigation counter**, a **climate controller**, and a **thermal floor**.

```
                           greenhouse_top
 ┌───────────────────────────────────────────────────────────────────────────────────────────┐
 │  irrigation                                  climate                                      │
 │  ┌──────────────────────────────┐            ┌───────────────────────────────────────────┐│
 │  │ pulse_clock ─add────► water  │            │ sensor_feed ─reading► thermostat          ││
 │  │ (1 s)                 _meter │            │ (replays a file)        │ heater_command  ││
 │  │ report_clock ─reset──►       │──report──► │                         ▼                 ││
 │  │ (5 s)        │  sum   │      │   tick     │                       heater              ││
 │  └──────────────┼────────┼──────┘   (IC)     │                         │ energy_pulse    ││
 │                 │        └► [water_report]   │ [report_tick_in]─reset─►▼                 ││
 │                 └► [report_tick]             │                    energy_meter ─► [energy_report]
 │                                              │ thermostat ─overheat_alarm► alarm_latch ─► [alarm_raised]
 │                                              │                              └────────► alarm_panel (sink)
 │                                              └───────────────────────────────────────────┘│
 └───────────────────────────────────────────────────────────────────────────────────────────┘

 floor (Cell-DEVS, 9×7 cells):    window │ air · air · air · air · air · air │ heater      → temperature field
                                  (5 °C)                                       (40 °C)

 integrated (demo 09):   floor ─cell (4,3)─► probe ─float─► thermostat ─► heater ─► energy_meter
```

* **Irrigation** counts watering pulses (one per second) and reports the total every five seconds.
  This is Cadmium's own "count fives" example, wrapped as a reusable coupled model.
* **Climate** replays temperature readings from a file; the **thermostat** switches the **heater** ON
  below 18 °C and OFF above 24 °C (a hysteresis band) and raises an overheat alarm above 35 °C; an
  **energy meter** counts the heater's energy pulses per report period; a one-shot **alarm latch**
  makes sure the alarm fires only once.
* **Floor** is a Cell-DEVS lattice of air cells exchanging heat, with windows (cold) on the left and a
  heater (hot) on the right.
* **Why this domain?** It needs every ingredient naturally: periodic generators, accumulators,
  reset ticks, file-driven input, a controller with timing and simultaneous events, a one-shot
  filter, a sink, hierarchy, and a spatial diffusion field that a controller can observe.

---

## What the simulation does

`data/sensor_feed.txt` (time, indoor temperature °C), the input of the climate subsystem:

```
1.0 21.0    2.0 17.0    2.5 17.5    4.0 25.0    7.0 40.0    7.2 38.0
9.0 30.0   12.0 40.0   13.0 10.0   15.0 20.0   17.0 26.0
```

Thermostat: `low 18`, `high 24`, `critical 35`, decision delay `0.5 s`. Heater: one energy unit per
second while ON. The timeline below can be derived by hand from those numbers and is **asserted** in
`src/02_static_greenhouse.cpp` (for `float`, `double` and the custom `fixed_time`):

| t | event | note |
|---|---|---|
| 0 | the feed's first internal event emits nothing | stock `iestream_input` always starts with an empty event at t = 0 |
| 1.0 → 1.5 | reading 21 → decision at 1.5 | in the band: no command |
| 2.0 → 2.5 | reading 17 starts a decision | 17 < 18 → heater ON |
| **2.5** | reading 17.5 arrives *exactly* when the decision is due | **thermostat confluence**: λ already emitted `ON (reading 17)`, δint commits it, δext(0, 17.5) starts a new decision |
| 3.5 | heater pulse #1 | ON at 2.5 + 1 s |
| 4.0 → **4.5** | reading 25 → thermostat emits `OFF` | 25 > 24; at 4.5 the heater is also due to pulse → **heater confluence**: pulse #2 is emitted, *then* it switches off |
| **5** | report tick: water **5**, energy **2** | the 5th water pulse and the tick coincide; PDEVS counts all 5 |
| 7.0 → 7.5 | reading 40, then 38 at 7.2 (deadline stays 7.5, reading refreshed) | 38 > 35 → alarm; the latch emits `1` (second pass at t = 7.5), the panel absorbs it |
| 10 | water 5, energy 0 | heater was off |
| 12.0 → 12.5 | reading 40 → alarm again | **swallowed** by the latch |
| 13.0 → 13.5 | reading 10 → heater ON | pulses at 14.5, 15.5, 16.5, 17.5 |
| 15 | water 5, energy **1** | one pulse (14.5) since t = 5 … |
| 17.0 → **17.5** | reading 26 → `OFF`; heater pulse due too | second **heater confluence** |
| 20 | water 5, energy **3** | pulses at 15.5, 16.5, 17.5 |

So the outputs are `water = 5,5,5,5` and `energy = 2,0,1,3` at t = 5,10,15,20, the alarm latch
fires once (7.5), the heater emits 6 pulses, and there are three confluent events.

---

## The models, one by one

Each entry gives the DEVS definition and how it is done in Cadmium. The same definition is in the
header comment of the file.

### Thermostat: `include/greenhouse/models/thermostat.hpp` (custom, Parallel DEVS)

```
X = { (reading, v) : v ∈ float }     Y = { (heater_command, c) } ∪ { (overheat_alarm, 1) }
S = { (mode, reading, commanded, σ) : mode ∈ {idle, deciding}, commanded ∈ {off, on}, σ ∈ ℝ⁺ ∪ {∞} }
s0 = (idle, 0, off, ∞)
ta(s)  = σ
want(r, c) = on if r < low ∧ c = off;   off if r > high ∧ c = on;   c otherwise
λ(s)   = { heater_command(want(reading, commanded)) if it differs from `commanded`,
           alarm(1) if reading > critical }            (only while deciding)
δint(s)      = (idle, reading, want(reading, commanded), ∞)
δext(s,e,x)  = idle     → (deciding, reading′, commanded, decision_delay)
               deciding → (deciding, reading′, commanded, σ − e)     reading′ = last value in the bag
δcon(s,x)    = δext(δint(s), 0, x)
```

*In Cadmium:* `class thermostat<TIME>` with `state_type = thermostat_state<TIME>`,
`input_ports = tuple<reading>`, `output_ports = tuple<heater_command_out, overheat_alarm>` and the five
functions. The elapsed time `e` is used in `δext` to keep the original deadline; `δcon` is implemented
literally as `internal_transition(); external_transition(TIME{}, bags);`. A second constructor takes a
`thermostat_config`, which the dynamic API forwards (`make_dynamic_atomic_model<thermostat, TIME>(id, cfg)`).

### Heater: `models/heater.hpp` (custom)

```
X = { (command, c) }    Y = { (energy_pulse, 1) }    S = (on, σ, pulses),  s0 = (false, ∞, 0)
ta(s) = σ if on else ∞          λ(s) = { energy_pulse: 1 }           δint(s) = (on, period, pulses+1)
δext((on,σ,n), e, x): σ′ = σ − e if on;  per command in the bag, in order:  ON ∧ ¬on → on:=true, σ′:=period;  OFF → on:=false, σ′:=∞
δcon(s,x) = δext(δint(s), 0, x)
```

*In Cadmium:* shows the textbook use of the elapsed-time argument: a command arriving mid-period does
**not** restart the pulse schedule. An OFF command arriving at the instant a pulse is due is a true
confluent event: λ still emits the pulse (λ runs before δcon), then the heater stops.

### Sensor feed: `stock::iestream_input<float, TIME>` (stock, subclassed)

Replays `<time> <value>` lines as timed output messages. `sensor_feed` only adds a constructor that
opens the file (path from a global for the *static* engine, which default-constructs every model, or
from an argument for the *dynamic* one). **Quirk:** the file must not end with a newline (see
[Findings](#findings-pitfalls-and-upstream-issues)).

### Meters, clocks, latch, panel: **stock models**

| model (alias) | stock class | role | behaviour (Cadmium source) |
|---|---|---|---|
| `pulse_clock` | `int_generator_one_sec` | irrigation pulse | emits `1` every 1 s |
| `report_clock` | `reset_generator_five_sec` | report trigger | emits a `reset_tick` every 5 s |
| `water_meter`, `energy_meter` | `accumulator<int,TIME>` (two tiny subclasses) | per-period totals | sums `add`; on `reset` goes to `ta = 0`, emits the sum, δint zeroes it. Throws if misused (input while reporting). **Two subclasses because static couplings identify sub-models by C++ type**, two aliases of the same class would collide |
| `alarm_latch` | `filter_first_output` | one-shot alarm | first input → emits one `1` → then ignores everything |
| `alarm_panel` | `passive<int,TIME>` | sink | absorbs messages, `ta = ∞`, throws on δint/λ |
| `heartbeat` (demo 01) | `generator<VALUE,TIME>` (abstract) | – | derive and define `period()` / `output_message()` |

### Cell probe: `floor/cell_probe.hpp` (custom adapter)

Bridges two message vocabularies: a Cell-DEVS cell speaks `cell_state_message<position, double>`, the
thermostat wants a `float`. `ta = 0` while a new reading is pending (zero-delay relay), else ∞.

### Floor cells: `floor/floor_cells.hpp` (Cell-DEVS, grid)

State `S = double` (temperature), vicinity `V = double` (conductance). `air_cell` derives from
`grid_cell<T, double, double>` and overrides only two virtuals:

```
local_computation():   T′ = round₀.₁( T + diffusivity · Σ_n  vicinity_n · (T_n − T) )     (n ≠ self)
output_delay(next):    base_delay + delay_per_degree · |next − T|
```

Heaters and windows reuse the stock `grid_cell` unchanged (its default rule is "keep my state").
The rounding is essential: without it the state would creep forever and the lattice never settle.

### Pipe cells: `floor/pipe_network.hpp` (Cell-DEVS, graph)

Same idea on a *graph* with string ids: pressure diffuses from a `source_cell` (the tank) through
`node_cell` junctions to sprinkler heads (which also leak). Each node **lists itself as a neighbour**,
see [the self-loop pattern](#findings-pitfalls-and-upstream-issues).

---

## The coupled models

### Static structure (`systems/static_greenhouse.hpp`)

`coupled_model<TIME, IP, OP, Ms, EICs, EOCs, ICs>` is the Cadmium spelling of `N`.

| coupled model | sub-models | EIC | EOC | IC |
|---|---|---|---|---|
| `irrigation_model` | pulse_clock, report_clock, water_meter | – | water_meter.sum → `water_report`; report_clock.out → `report_tick` | pulse_clock.out → water_meter.add; report_clock.out → water_meter.reset |
| `climate_model` | sensor_feed, thermostat, heater, energy_meter, alarm_latch, alarm_panel | `report_tick_in` → energy_meter.reset | energy_meter.sum → `energy_report`; alarm_latch.out → `alarm_raised` | feed→thermostat.reading; thermostat→heater.command; heater→meter.add; thermostat.alarm→latch.in; latch.out→panel.in |
| `greenhouse_top` | irrigation_model, climate_model | – | water_report, energy_report, alarm | irrigation.report_tick → climate.report_tick_in |

Note the fan-out of `report_clock.out` (both an IC *and* an EOC) and of `alarm_latch.out` (an IC *and* an
EOC). `climate_standalone` is `climate` alone with nothing connected, used to demonstrate
`run_until_passivate()`, which the full greenhouse (endless clocks) can never reach.

### Dynamic structure (`systems/dynamic_greenhouse.hpp`)

The same hierarchy two ways: **translated** (`make_dynamic_coupled_model<TIME, greenhouse_top>()`, ids =
C++ type names) and **hand built** (`build_dynamic_greenhouse`: readable ids, constructor arguments,
the vector *and* initializer-list `coupled<TIME>` constructors, which validate every link and throw
`std::domain_error` on a bad one). Demo 03 proves static, translated and hand-built runs are the
**same simulation** (416 events compared), and that changing a constructor argument
(`heater.pulse_period_s = 0.5`) changes the result as predicted by hand: energy `4, 0, 3, 5`.

### Integrated structure (demo 09)

```
floor (grid_coupled + 1 EOC) ─IC→ probe ─IC→ thermostat ─IC→ heater ─IC→ energy_meter ←IC─ report_clock
```
A Cell-DEVS lattice is an ordinary dynamic coupled model, so it can be coupled to anything: we declare
an output port on the lattice (`expose_cell`) and add an EOC from the sensor cell. The thermostat
(thresholds 17/20 °C) switches ON at t = 0.5 and OFF at t = 38.4, half a second after the sensor cell
first published a value above 20 °C; 37 energy pulses are emitted and every one is accounted for.

---

## Tour of the demos

Run any of them with `make run-<name>`; each prints what it does and ends with `N/N checks passed`.

| demo | topic | highlights |
|---|---|---|
| `01_atomic_by_hand` | **Models without an engine** | ports, bags (`message_bag`, `make_message_bags`, `get_messages`), boxes (`message_box`…); the five DEVS functions called by hand, including `e` and `δcon`; **every stock model** driven manually (accumulator's error cases, the file `Parser`, `iestream_input` replaying the feed); concept checks and helper traits; tuple printing, `helper::for_each/join` |
| `02_static_greenhouse` | **Static engine** | compile-time validation, `runner` (`run_until`, resuming, `run_until_passivate`), default logger, the timeline above asserted for **float, double and fixed_time** (606 trace events identical), debug channel |
| `03_dynamic_greenhouse` | **Dynamic engine** (×3 modes) | translation vs hand-built, constructor arguments, run-time link validation, progress meter, `run_until_passivate`, determinism over 25 repetitions, the parallel building blocks called directly |
| `04_engine_internals` | **The abstract simulator, step by step** | one `simulator` stepped by hand; a `coordinator` stepped like `runner` does; every coordinator helper (`min_next_in_tuple`, `route_*`, `collect_messages_by_eoc`, …); the three `domain_error` refusals; the dynamic mirror (type-erased bags, `link`, validators, translator pieces) |
| `05_logging_tour` | **Logging** | the full source × event matrix on both engines, custom sinks/formatters, `not_logger`, `verbatim_formatter`, printing helpers, default loggers, `logger_debug`, thread-safe sinks |
| `06_classic_devs` | **Classic DEVS** | `message_box` models, `devs::coupling<…, SELECT>`, a tiny classic simulator (Cadmium has none), and three Select policies giving three different answers |
| `07_celldevs_floor` | **Cell-DEVS grid** | `grid_scenario`/`cell_map` toolbox, the three delay buffers, the JSON-driven floor, the JSON default/merge machinery, building a lattice in code, delay semantics changing the dynamics |
| `08_celldevs_pipes` | **Cell-DEVS graph** | a single cell by hand, `cells_coupled` + `data/pipes.json`, `add_cell`, cell messages and ports |
| `09_integrated_greenhouse` | **Everything together** | Cell-DEVS floor feeding the DEVS controller |

### Demo 04 in one picture: what `runner::run_until` hides

```
runner ─owns→ coordinator(greenhouse_top) ─owns→ coordinator(irrigation) ─owns→ simulator(pulse_clock)
                                           │                              └──→ simulator(water_meter) …
                                           └─owns→ coordinator(climate)    ─owns→ simulator(thermostat) …
each loop iteration:   t = top.next();  top.collect_outputs(t);  top.advance_simulation(t);
```
`coordinator::collect_outputs` asks every sub-engine for outputs and applies the EOCs;
`coordinator::advance_simulation` first routes IC and EIC messages into the sub-engines' inboxes, then
advances them all. Part 1c of the demo performs exactly those steps on a hand-made pair
(thermostat → heater) so you can see each message appear.

### Demo 06: why Parallel DEVS exists

Irrigation in classic DEVS: at t = 5 the 5th pulse and the report request are simultaneous.

| `Select` policy | meter reports at t = 5, 10, 15, 20 |
|---|---|
| pulse first | 5, 5, 5, 5 |
| report, then meter, then pulse | **4**, 5, 5, 5 (the simultaneous pulse lands in the *next* window) |
| report, then pulse, then meter | **exception** `External transition called while on reset state` |

Parallel DEVS (demo 02) reports 5 every time, with no Select and no arbitrary ordering.

### Demo 07: delay buffers change the dynamics

The same 5×3 floor with each delay type (`window | sensor | heater`, 30 s):

| delay | events | sensor reaches 20 °C | final sensor |
|---|---|---|---|
| inertial | 1530 | t = 12.8 | 22.3 |
| transport | 4965 | t = 5.19 | 22.3 |
| hybrid | 4215 | t = 5.19 | 22.3 |

Inertial drops superseded outputs (a low-pass filter: slow, cheap); transport delivers everything
(fast, 3× the events); hybrid cancels only the pending outputs a new one supersedes. All settle to the
same equilibrium: *delays shape transients, not steady states.* On the full 9×7 floor the effect is
bigger: about 25× more events with transport than with inertial in 30 simulated seconds (measured
while developing, not asserted by a test).

---

## Reference tables

### The logging system (demo 05 verifies every cell of this table)

`logger<SOURCE, FORMATTER, SINK>::log<SOURCE, EVENT>(args…)`. A logger reacts to **one source**, which
`if constexpr` makes free for all the others. `multilogger<L1, L2, …>` fans out to several.

| source | events emitted under it | static engine | dynamic engine |
|---|---|---|---|
| `logger_info` | `run_info`, `coor_info_init`, `coor_info_collect`, `coor_info_advance`, `sim_info_init`, `sim_info_collect`, `sim_info_advance` | ✓ | ✓ |
| `logger_state` | `sim_state` | `(state, id)` | `(t, id, state)` |
| `logger_messages` | `sim_messages_collect` | `(messages, id)` | `(t, id, outbox)` |
| `logger_message_routing` | `coor_routing_eoc_collect`, `coor_routing_ic_collect`, `coor_routing_eic_collect` (headers) + detail: `coor_routing_collect_ic`, `_eic`, `_eoc` | 3 detail events | one `coor_routing_collect` |
| `logger_global_time` | `run_global_time` | ✓ | ✓ |
| `logger_local_time` | `sim_local_time` (elapsed time per model) | ✓ | ✓ |
| `logger_debug` | *none, reserved for model authors* (`greenhouse::debug_note`) | – | – |

A **formatter** is a class with one static function per event; Cadmium ships `formatter<TIME>` and
`dynamic::logger::formatter<TIME>`. A **sink provider** has `static std::ostream& sink()`; Cadmium
ships `cout_sink_provider` and `cerr_sink_provider`. This project adds `memory_sink<Tag>`
(line-atomic, thread-safe), `file_sink<Tag>`, `trace_formatter<TIME>` (one parseable line per event,
overloaded for both engines) and the debug channel. `not_logger` (a logger bound to a source nothing
emits) switches logging off at zero cost. Messages are printed with `operator<<` when it exists, and as
`obscure message of type <T>` when it does not (the stock `reset_tick`).

### Grid scenario JSON (`data/floor.json`, `grid_coupled::add_lattice_json`)

| key | meaning |
|---|---|
| `shape` | lattice size, e.g. `[9, 7]` (any dimension) |
| `wrapped` | `true` = torus (distances and neighbours wrap around) |
| `cells.default` | the base cell: `cell_type`, `delay`, `state`, `neighborhood`, `config` |
| `cells.<group>` | overrides of any of those; `state` is merge-patched, `config` is merge-patched key by key (so `{"diffusivity": 0.05}` keeps the other config values) |
| `cell_map` | `{ "<group>": [[x,y], …] }`, which positions belong to which group |

Neighbourhood entries (each with a `vicinity`, and a `range` that defaults to 1):

| `type` | neighbours |
|---|---|
| `von_neumann` | the cells within Manhattan distance `range` (the plus shape) |
| `moore` | the cells within Chebyshev distance `range` (the square) |
| `relative` | an explicit list of offsets `neighbors: [[1,0], …]` (`custom` is the deprecated spelling; it warns on stderr) |
| `absolute`, `remove` | announced, **not implemented** (throw `std::logic_error`) |
| anything else | `std::bad_typeid` |

Neighbours that fall outside a non-wrapped lattice are silently dropped, which gives the border cells
fewer neighbours (insulating boundary).

### Graph scenario JSON (`data/pipes.json`, `cells_coupled::add_cells_json`)

`cells.default` plus **one key per cell** (the key is the cell id); `neighborhood` is a plain object
`{ "neighbour id": vicinity }`. Everything else (delay, state, config merging) is as above.

### Delay buffers (`celldevs/delay_buffer/`)

A cell does not send its new state at once: it schedules it `output_delay(state)` into the future.

| `delay` | rule when `add_to_buffer(state, t)` is called | ordering |
|---|---|---|
| `inertial` | keep only the **latest**; it replaces whatever was pending | one pending output |
| `transport` | keep **all**; a same-time entry replaces the previous | delivered in time order |
| `hybrid` | drop pending entries scheduled **at or after** `t`, keep earlier ones | transport for the past, inertial for the future |

`delay_buffer_factory::create_delay_buffer("inertial"|"transport"|"hybrid")` builds them from the JSON
string (anything else throws `std::out_of_range`). `next_state()` falls back to the last published
state once the buffer is empty.

### Custom TIME type (`fixed_time.hpp`)

Every model is `template<typename TIME>`. `greenhouse::fixed_time` (int64 milliseconds, saturating
±∞) is a drop-in replacement for `float`/`double`: integer time makes simultaneity exact (no `0.1 + 0.2 ≠
0.3`) and the three types produce identical traces (demo 02). What Cadmium requires of a TIME type is
listed in the file header: zero default value, implicit construction from a floating literal,
`+ − += −=`, comparisons, `<<` and `>>`, `numeric_limits::infinity`, and `std::hash` (for the transport
buffer). Note that Cadmium's concept checks always instantiate models with `float`, so models must
compile with it.

---

## Execution modes and concurrency

The dynamic engine has three execution modes selected **when compiling**; demo 03 is built in all
three (`make` produces `03_dynamic_greenhouse`, `…_concurrent`, `…_openmp`).

| mode | flags | how it works |
|---|---|---|
| sequential | – | default |
| concurrent | `-DCADMIUM_EXECUTE_CONCURRENT -DBOOST_THREAD_PROVIDES_EXECUTORS -DBOOST_THREAD_PROVIDES_FUTURE_CONTINUATION -DBOOST_THREAD_USES_MOVE … -lboost_system -lboost_thread` | each coupled model advances its sub-models on a Boost thread pool (`runner(model, t0, threads)`) |
| OpenMP | `-DCPU_PARALLEL -fopenmp` | each coupled model advances its sub-models in an `omp parallel` region (`runner(model, t0, threads)`) |

Within one simulation step the models are independent (couplings are routed *between* steps), so
parallelism changes speed, not results. `make test` checks that the three modes produce **byte-identical**
canonical traces, and demo 03 repeats the run 25 times per mode to give any data race room to show up.

Three things you must know (all found while building this project, all in [Findings](#findings-pitfalls-and-upstream-issues)):
the three Boost macros, `omp_set_max_active_levels` for nested coupled models, and thread-safe sinks.

**Is it faster?** Not for models this small. Measured on the 63-cell floor (60 simulated seconds,
`-O2`, 8 cores, 4 worker threads): sequential **267 ms**, Boost.Thread 408 ms, OpenMP 338 ms. Per-model
work is far smaller than the cost of coordinating threads; the modes pay off when individual
transitions are expensive.

---

## Testing

`make test` runs five independent layers:

1. **Self-checking demos.** Each program `CHECK`s every claim it prints and exits non-zero on failure
   (~700 checks). Expected values are derived by hand (see [the timeline](#what-the-simulation-does)),
   not copied from output.
2. **Cross-engine equivalence.** The static engine, the translated dynamic model and the hand-built
   dynamic model produce the same canonical trace (416 events), as do `float`, `double` and `fixed_time`.
3. **Compile-fail tests** (`tests/compile_fail/`). Cadmium promises to reject invalid models at compile
   time; 17 snippets check that, for IC/EIC/EOC message-type mismatches, wrong port directions,
   self-coupling, undeclared ports, missing or mistyped DEVS functions, duplicate ports, classic models
   using bags, dynamic `link` mismatches, and `run_until_passivate` on a time type without infinity.
   Each snippet is compiled **twice**: without `-DBREAK` (must compile, so the snippet is otherwise
   valid) and with it (must fail *with the expected static_assert message*).
4. **Execution-mode equality.** Sequential, concurrent and OpenMP traces are compared byte for byte.
5. **API coverage audit** (`tools/coverage_check.sh`): see below.

---

## Findings: pitfalls and upstream issues

Everything here was **observed**, not assumed, while building the project. They are the things a
newcomer to Cadmium will hit; items marked 🐞 are defects in this Cadmium revision.

### Building and including

1. **Include order matters.** `dynamic_atomic.hpp` prints a model's state with an *unqualified*
   `oss << state`. For `std::tuple` states (the stock accumulator) the printer lives in
   `cadmium/logger/tuple_to_ostream.hpp` and, `std::tuple` being in `std`, ADL cannot find it: it must be
   declared **before** any dynamic Cadmium header, or you get an opaque "no match for operator<<".
   Worse, once that tuple printer exists in namespace `cadmium`, ordinary lookup from `cadmium::celldevs`
   stops there and **hides the global `vector<int>` printer** that Cell-DEVS uses for cell ids, so
   combining tuple-state models with Cell-DEVS (demo 09) breaks unless a vector printer is also visible
   inside `namespace cadmium`. A third, smaller trap: the stock `iestream.hpp` uses `std::ostringstream`
   without including `<sstream>`, so it fails to compile on its own unless something earlier pulled that in.
   `include/greenhouse/prelude.hpp` handles all three and every header includes it first.
2. **Boost executors need three macros**, not just `CADMIUM_EXECUTE_CONCURRENT`: also
   `BOOST_THREAD_PROVIDES_EXECUTORS`, `BOOST_THREAD_PROVIDES_FUTURE_CONTINUATION`, `BOOST_THREAD_USES_MOVE`.
   Otherwise `boost::basic_thread_pool` is silently not declared.
3. **C++17 only.** `cadmium::concept` collides with the C++20 `concept` keyword.
4. **Single translation unit.** Several functions in `dynamic_models_helpers.hpp` and `common_helpers.hpp`
   are non-inline, so including the dynamic headers from two `.cpp` files gives multiple-definition link errors.

### Threads

5. **OpenMP mode hangs on nested coupled models** 🐞. Every coupled model opens an `omp parallel`
   region, nested regions are serialised by default, and `cpu_parallel_for_each` gives "the remainder" of
   the work to the *last thread of the team it asked for*, which then never exists: the remaining
   sub-models are silently never advanced, simulated time stops advancing and the program hangs. Fix: `omp_set_max_active_levels(16)`
   (demo 03 does it, with the explanation).
6. **Console sinks are not thread-safe.** `cout_sink_provider` is `std::cout`; the logger writes a line
   with *two* stream operations, so lines from concurrent models interleave mid-line (seen in OpenMP
   output). This project's `memory_sink` gives each thread a private buffer published line by line.
   *(A stringbuf with a lock in `overflow()` is not enough: its inline `sputc` fast path bypasses it;
   an earlier version of `locked_stream` lost over half the characters in the stress test.)*

### Models

7. **`iestream_input` needs a file without a trailing newline.** It tests `file.eof()` *before* reading;
   with a final newline the next failed extraction leaves an uninitialised time and a bogus event can appear.
   It also always starts with an **empty output at t = 0**.
8. **A Cell-DEVS cell recomputes only when a message arrives.** A cell that listens to others but not to
   itself stops iterating after its first update (the 2-cell pipe test stalled at 5 instead of 10).
   The pattern: **list the cell itself as a neighbour** (the grid gets it from the centre of a von Neumann
   neighbourhood); it then hears its own delayed publication and keeps relaxing until its state stops changing.
   Also: states must be *rounded* or the lattice never settles.
9. **A static coupled model identifies its sub-models by C++ type, and a duplicate is a *silent*
   error.** Two `accumulator<int,T>` in one coupled model compile and run, but both sets of couplings are
   resolved to the same engine: it receives every message twice (reports `10`), and the second
   sub-model never takes part. Nothing diagnoses it (demo 02, section 7, reproduces it). An alias of the
   same class does not help, derive a tiny subclass instead (`water_meter`, `energy_meter`). The
   dynamic translator has the same limit (`models_by_type` is keyed by type). Related: `coordinate_tuple`'s
   engine types are built from an internal alias template, so compare engines through `model_type`.
10. **A static coupled model is logged under its full C++ type name** (hundreds of characters). The dynamic
    builder with readable ids is the practical choice for debugging.
11. **Dynamic inbox semantics:** an inbox *map entry* means "input available", even if its bag is empty
    (the engine tests whether the map is empty, not the bags).
12. `run_until` always emits an ANSI reset (`ESC[0m`) through `turn_progress_off()`, even with the meter off.
13. Relative neighbour offsets are `assert`ed to be smaller than the lattice size (a 1×1 lattice can only list itself).

### Defects in the Cell-DEVS code 🐞

14. `grid_coupled::add_lattice(scenario, …)` calls `scenario.get_states()`, which `grid_scenario` does not
    have; it is a template that was never instantiated, so it never failed. **Replacement:**
    `floor_coupled::add_scenario` (it does what `add_lattice_json` does after parsing).
15. The `cell` constructor taking `std::vector<C> neighbors` (and `cells_coupled::add_cell(id, vector, …)`
    which uses it) iterates its own empty local map instead of `neighbors` and does not compile when used.
    Use the `unordered_map` overloads.
16. `absolute` and `remove` neighbourhood types are advertised but throw `std::logic_error` (not implemented).

These three are the "excluded" entries of the coverage audit, each with its reason.

---

## Cadmium API coverage

"Everything Cadmium offers" is a checked claim: `docs/cadmium_api.txt` lists **every public item** in
`cadmium/include/cadmium` (54 headers) and `tools/coverage_check.sh` verifies each one:

* **253** items are used directly in this project's code,
* **27** are exercised through another call (e.g. the private `*_impl` helper structs, through the public
  function that drives them) with a regex proving the call exists,
* **3** are excluded with a documented reason (the upstream defects 14 and 15),
* and, in reverse, **every header** of the include tree must appear in the list, so an upstream header added
  later fails the audit until someone decides how to cover it.

Where each family is covered:

| Cadmium area | headers | demos |
|---|---|---|
| ports, bags, boxes | `modeling/{ports,message_bag,message_box}` | 01, 06 |
| static coupling | `modeling/coupling` | 02, 06, compile-fail |
| dynamic modeling | `modeling/dynamic_*` | 03, 04, 09 |
| translator | `modeling/dynamic_model_translator` | 03, 04 |
| compile-time checks | `concept/*` | 01, 06, compile-fail |
| static engine | `engine/pdevs_{runner,coordinator,simulator,engine_helpers}` | 02, 04 |
| dynamic engine | `engine/pdevs_dynamic_*` | 03, 04 |
| concurrency | `engine/{concurrency,parallel}_helpers` | 03 (concurrent, OpenMP builds) |
| logging | `logger/*` | 05 (+ every demo) |
| stock PDEVS models | `basic_model/pdevs/*` | 01, 02, 03 |
| stock classic models | `basic_model/devs/*` | 06 |
| Cell-DEVS | `celldevs/*` | 07, 08, 09 |
| JSON | `json/json.hpp` | 07, 08 |

---

## Extending the project

* **Close the loop.** In demo 09 the heater does not act back on the floor. Make a floor cell that
  listens to a heater model: give it a neighbour id for the heater, add the heater as a model of the
  lattice, and let `couple_cells()` create the link (or write an IC from the heater's output to a
  cell's `cell_in`, with the heater impersonating one of that cell's neighbours).
* **Wrapped lattice / 3-D lattice.** Set `"wrapped": true` or a 3-element `shape` in `floor.json`; the
  grid toolbox and neighbourhoods are dimension-generic (demo 07, section 1).
* **A new cell type.** Derive from `grid_cell`, override `local_computation()` and `output_delay()`, add
  a branch to `floor_coupled::add_grid_cell_json`, and use the new `cell_type` string in JSON.
* **A real classic simulator.** `06_classic_devs.cpp` shows the algorithm; generalising `classic_driver`
  to arbitrary `devs::coupling` types is a nice exercise in Cadmium's template machinery.

---

## References

* B. P. Zeigler, T. G. Kim, H. Praehofer, *Theory of Modeling and Simulation*, 2nd ed., 2000 (DEVS).
* A. C. H. Chow, B. P. Zeigler, "Parallel DEVS: a parallel, hierarchical, modular modeling formalism", WSC 1994.
* G. Wainer, *Discrete-Event Modeling and Simulation: a Practitioner's Approach*, CRC Press, 2009 (Cell-DEVS).
* G. Wainer, "CD++: a toolkit to develop DEVS models", *Software: Practice and Experience*, 2002.
* D. Vicino, D. Niyonkuru, G. Wainer, O. Dalle, "Sequential PDEVS architecture", 2015.
* Cadmium: <https://github.com/SimulationEverywhere/cadmium>.
