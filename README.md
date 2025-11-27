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

## FPGA loader utility (`fpga_loader`)

- Build with `cmake --build build --target fpga_loader` (the default configure step already created the target).
- Run on the Zynq board as root, e.g.:
  ```
  sudo ./build/fpga_loader \
    --static=bitstreams/top_reconfig_wrapper.bin \
    --partial=bitstreams/fft_partial.bin \
    --manager=/sys/class/fpga_manager/fpga0/firmware
  ```
- The tool copies the requested `.bin` containers into `/lib/firmware`, writes their filenames to the Linux `fpga_manager`, drives the `fpga0/flags` node high for partial reconfiguration, and automatically toggles the AXI-GPIO decouple signal at `0x41200000` around each partial load so the DFX decouplers isolate the reconfigurable partition.
- Linux `fpga_manager` requires the raw binary (`*.bin`) bitstreams generated via `bootgen`; the `bitstreams/` directory contains `.bin` siblings for the `.bit` files so the loader defaults to those paths.

## FFT DMA probe (`fpga_fft_dma_loader`)

- Build with `cmake --build build --target fpga_fft_dma_loader`.
- This executable performs the same static/partial loading sequence as `fpga_loader`, then programs the AXI DMA at `0x40400000` to push data through the FFT RP using the `u-dma-buf` devices you created (defaults: `/dev/udmabuf0` for MM2S input, `/dev/udmabuf1` for S2MM output).
- Example usage (adjust DMA buffers if yours differ):
  ```
  sudo ./build/fpga_fft_dma_loader \
    --static=bitstreams/top_reconfig_wrapper.bin \
    --partial=bitstreams/fft_partial.bin \
    --mm2s-buf=/dev/udmabuf0 \
    --s2mm-buf=/dev/udmabuf1 \
    --manager=/sys/class/fpga_manager/fpga0/firmware
  ```
- After the partial image is loaded and decouple released, the tool resets the DMA, streams `--samples` 32-bit words into the FFT overlay, waits for MM2S/S2MM completion, and dumps the first few output samples so you can confirm the fabric datapath is alive.
- Add `--dry-run` on a development host to see the sequence without touching `/dev/mem` or sysfs, or adjust `--gpio-base`, `--firmware-dir`, or `--wait-ms` if your platform differs.
