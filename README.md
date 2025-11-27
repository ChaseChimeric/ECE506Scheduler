## Build

- Install dependencies (C++17 toolchain, CMake, zlib, pthreads).
- From repo root run:
    - `cmake -S . -B build`
    - `cmake --build build`
    - This builds libschedrt.so, the runner sched_runner, and the shared app plugins (libdemo_dash_app.so, libradar_correlator_app.so).

## Runner usage (sched_runner)

- `--app-lib=PATH` (required) shared object implementing app_initialize/app_run.
- `--backend={auto|cpu|fpga}` choose scheduler backend preference.
- `--cpu-workers=N` number of worker threads (default = hardware concurrency).
- `--preload-threshold=N` how many ready tasks trigger overlay preload (default 3).
- `--bitstream-dir=DIR` directory plugins use to resolve <app>_partial.bit.
- `--fpga-manager=PATH` sysfs path to write partial bitstreams (defaults to /sys/class/fpga_manager/fpga0/firmware).
- `--fpga-real/--fpga-mock` whether FpgaSlotAccelerator actually writes to the manager or stays mock.
- everything after -- is passed verbatim to the plugin as its own arguments (e.g., --input=...).

## DASH plugin flags (libdemo_dash_app.so)

- `--overlay=name[:count[:bit]]` add overlay slots (defaults to fft:1:fft_partial.bit and fir:1:axis_passthrough_partial.bit).
- `--static-bitstream=PATH` override the static shell (defaults to bitstreams/top_reconfig_wrapper.bit).
- `--cpu-workers, --preload-threshold, --fpga-manager, --fpga-real/--fpga-mock, --bitstream-dir` as above.

## Radar correlator plugin flags (libradar_correlator_app.so)

- `--input=DIR` override where time_input.txt/received_input.txt live (default searches near executable).

## Pre-built PR artifacts (bitstreams/)

- `top_reconfig_wrapper.bit` is the static full bitstream for the Zynq shell. `top_reconfig.hwh` is the matching hardware handoff describing the static design.
- `fft_partial.bit` + `fft_partial.hwh` are the FFT RM partial reconfig image and handoff (Vivado imports `reconfiguable_region_1_inst_0.dcp` for this partition).
- `axis_passthrough_partial.bit` + `axis_passthrough_partial.hwh` capture the current child RM that wires the RP ports to an AXI-Stream pass-through (placeholder for FIR). Use this until the FIR implementation produces a `*_reconfigurable_region_2_inst_0_partial.bit`.
- Point `sched_runner` at `--bitstream-dir=bitstreams` so each overlay resolves `<name>_partial.bit` inside this repo, and keep the `.hwh` files nearby for XRT/PetaLinux metadata.
