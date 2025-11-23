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
- `--fpga-pr-gpio=N` assert GPIO `N` during static/partial bitstream loads (decouples the PR region). Add `--fpga-pr-gpio-active-low` if the GPIO is active-low, and use `--fpga-pr-gpio-delay-ms=NUM` to control how long we wait after each toggle (default 5 ms).
- everything after -- is passed verbatim to the plugin as its own arguments (e.g., --input=...).

## DASH plugin flags (libdemo_dash_app.so)

- `--overlay=name[:count]` add overlay slots (default zip:2, fft:1).
- `--cpu-workers, --preload-threshold, --fpga-manager, --fpga-real/--fpga-mock, --bitstream-dir` as above.

## Radar correlator plugin flags (libradar_correlator_app.so)

- `--input=DIR` override where time_input.txt/received_input.txt live (default searches near executable).

## Bitstream placeholding

- `bitstreams/static_wrapper.bit` comes from the `fft_fir_reconfigurable` top-level run (`fft_fir_reconfigurable.runs/impl_1/top_reconfig_wrapper.bit`) and must be converted to a `.bin` file before loading it with the FPGA manager.
- `bitstreams/fft_partial.bit` is the FFT partial produced by that same Vivado project (`fft_fir_reconfigurable.runs/impl_1/top_reconfig_i_reconfiguable_region_reconfiguable_region_1_inst_0_partial.bit`) and likewise needs a `.bin` conversion.
- `bitstreams/zip_partial.bit` and `bitstreams/cpu_partial.bit` remain placeholders; replace them with the real partial images and regenerate their `.bin` counterparts once the overlays are ready.

## Preparing bitstreams for fpga_manager

The Xilinx fpga_manager driver only accepts byte-swapped `.bin` images that expose the raw configuration data without the `.bit` header. Use the helper script to generate the `.bin` files (and place them where the driver looks, typically `/lib/firmware/bitstreams/`):

```bash
./scripts/prepare_bitstreams.py \
  bitstreams/static_wrapper.bit \
  bitstreams/fft_partial.bit \
  --dst-dir /lib/firmware/bitstreams --force
```

After running the script, point the scheduler at the converted files:

```bash
sudo ./build/sched_runner --app-lib=build/libradar_correlator_app.so \
  --backend=fpga \
  --bitstream-dir=/lib/firmware/bitstreams \
  --static-bitstream=/lib/firmware/bitstreams/static_wrapper.bin \
  --fpga-real \
  --fpga-manager=/sys/class/fpga_manager/fpga0/firmware \
  --overlay=fft:1 --overlay=fir:1 \
  --fpga-debug -- --input=input
```

> Tip: You need write access to `/lib/firmware/bitstreams/`. Run the script with `sudo` when targeting that directory, or use `--dst-dir` to stage the `.bin` files somewhere else first.
