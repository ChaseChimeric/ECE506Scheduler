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

- `--overlay=name[:count]` add overlay slots (default zip:2, fft:1).
- `--cpu-workers, --preload-threshold, --fpga-manager, --fpga-real/--fpga-mock, --bitstream-dir` as above.

## Radar correlator plugin flags (libradar_correlator_app.so)

- `--input=DIR` override where time_input.txt/received_input.txt live (default searches near executable).

## Bitstream placeholding

- `bitstreams/static_wrapper.bit` now comes from the `fft_fir_reconfigurable` top-level run (`fft_fir_reconfigurable.runs/impl_1/top_reconfig_wrapper.bit`), so it contains the actual shell used by the overlay.
- `bitstreams/fft_partial.bit` is no longer a dummy—it is the FFT partial produced by that same Vivado project (`fft_fir_reconfigurable.runs/impl_1/top_reconfig_i_reconfiguable_region_reconfiguable_region_1_inst_0_partial.bit`).
- `bitstreams/zip_partial.bit` and `bitstreams/cpu_partial.bit` are still placeholders; replace them with their respective Vitis partial images when those overlays are ready and point `--bitstream-dir` at the directory that contains them.
