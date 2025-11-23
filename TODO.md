# Project Progress Summary

## Phase 0: Scheduler Foundations
- Built the initial scheduler runtime (ready queue, dependency manager, CPU mock accelerator) in C++17.
- Added the DASH APIs (`dash::fft_execute`, `dash::zip_execute`) and the promise/future completion bus, plus early demo apps running purely on CPU mocks.

## Initial Setup for FPGA Integration
- Configured project path `ECE506 Scheduler`.
- Explored repository structure and Vivado project (`fft_fir_reconfigurable/fft_fir_reconfigurable.xpr`).
- Confirmed access to `bitstreams/` directory for static and partial bitstreams.

## Task 1: Integrate FPGA Bitstreams for FFT/FIR Overlay
- Copied Vivado-generated partial bitstreams into `bitstreams/fft_partial.bit` and `bitstreams/fir_partial.bit`.
- Added `ResourceKind::FIR` and extended `ExecutionResult` to track accelerator names.
- Added CLI flags to `sched_runner` for `--overlay=name[:count[:bitstream]]`, `--bitstream-dir`, `--static-bitstream`, `--fpga-real`, and `--fpga-manager`.
- Implemented static shell loading (`FpgaSlotAccelerator::prepare_static`) before partial reconfiguration.
- Added CSV reporting (`--csv-report`) with accelerator/FPGA indicators.

## Task 2: Radar Correlator and SAR Plugins
- Converted radar correlator and SAR apps into scheduler plugins.
- Added padding to radar inputs to match 65,536-point FFT hardware requirement.
- Created helper (`schedule_fft_task`) to submit FFT jobs via scheduler.
- Ensured apps now remain hardware-agnostic while scheduler handles overlays.

## Task 3: Scheduler Runner Enhancements
- Runner now registers all overlays/accelerators centrally and adds CPU fallbacks.
- Apps no longer register FPGA slots; they simply submit tasks via DASH API.
- Added global CLI parsing, overlay registration, and CPU mock fallback control.

## Task 4: Diagnostics & Logging
- Each task now reports the accelerator that executed it (`accel="fpga-slot-0"` or `cpu-mock`).
- Non-CSV output tags whether the FPGA ran (`(fpga)` vs `(cpu)`).
- Runner command example:  
  ```
  ./build/sched_runner --app-lib=build/libradar_correlator_app.so --backend=fpga \
    --bitstream-dir=bitstreams --static-bitstream=bitstreams/static_wrapper.bit \
    --fpga-real --fpga-manager=/sys/class/fpga_manager/fpga0/firmware \
    --overlay=fft:1 --overlay=fir:1 -- --input=input
  ```

## Outstanding Items
- Confirm partial bitstream writes succeed (check `/sys/class/fpga_manager/...` permissions and dmesg).
- Integrate AXI driver interactions (current code only handles PR via sysfs).
- Add richer logging for bitstream loads and hardware failures.
