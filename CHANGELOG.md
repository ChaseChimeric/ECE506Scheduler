# FPGA FFT Investigation – Change Log

Chronological summary of the major edits, experiments, and diagnostics performed while trying to get the radar correlator’s FFT overlay running on the PYNQ board. Each entry includes the motivation, the exact change, and the outcome so we can track what has been tried already.

---

## 1. Replace placeholder bitstreams (Nov 2024)
- **Change**: Copied `top_reconfig_wrapper.bit` and the new FFT partial (`top_reconfig_i_reconfiguable_region_reconfiguable_region_1_inst_0_partial.bit`) from the refreshed Vivado project into `ECE506 Scheduler/bitstreams/` as `static_wrapper.bit` and `fft_partial.bit`.
- **Reason**: The shipped files were dummy placeholders; the correlator needed the latest static shell + FFT RM from the updated `fft_fir_reconfigurable` project.
- **Status**: Scheduler now loads the real shell/partial. README updated to note which bitstreams are real vs. still placeholders (`zip_partial.bit`, `cpu_partial.bit`).

## 2. Add FPGA debug option to sched_runner
- **Change**: Added `--fpga-debug` flag in `apps/sched_runner.cpp` to toggle verbose logging in `FpgaSlotAccelerator` and the FFT runner.
- **Reason**: Needed detailed logs (bitstream loads, `ensure_app_loaded`, fallback decisions) to diagnose why the correlator never left the CPU path.
- **Status**: Passing `--fpga-debug` now emits debug lines for static shell loads, partial loads, FFT task execution, and fallback decisions.

## 3. FFT context buffer sizing fix
- **Change**: Updated `schedule_fft_task` in `apps/radar_correlator.cpp` to describe buffers as `len * 2 * sizeof(float)` bytes (real + imag) instead of `len * sizeof(float)`.
- **Reason**: The hardware runner copies interleaved real/imag samples. The old size check (`sample_count * 2 * sizeof(float)`) failed and forced CPU fallback.
- **Status**: FFT runner now accepts the buffers; no more “output buffer too small” messages.

## 4. FFT DMA runner instrumentation and SIGBUS guard
- **Change**: Added `SigbusScope` around `/dev/mem` mappings so a bad DMA base triggers a clean error instead of crashing. FFT runner now logs udmabuf size/phys and DMA base span at init.
- **Reason**: Avoided silent SIGBUS when `/dev/mem` can’t map the DMA registers (e.g., wrong base or disabled range).
- **Status**: When `SCHEDRT_DMA_BASE` is incorrect, the runner logs a SIGBUS warning and falls back to the CPU path gracefully.

## 5. u-dma-buf configuration
- **Change**: Manually loaded `u_dma_buf` with `udmabuf0=524288`, then later added:
  - `/etc/modules-load.d/u-dma-buf.conf` → `u_dma_buf`
  - `/etc/modprobe.d/u-dma-buf.conf` → `options u_dma_buf udmabuf0=524288`
- **Reason**: FFT runner requires a contiguous DMA buffer (half for input, half for output). After reboots the device disappeared without auto-load.
- **Status**: `/dev/udmabuf0` now appears at boot with the correct 512 KiB size and reported physical address (e.g., `0x15880000`).

## 6. Introduce kernel helper for AXI DMA registers
- **Change**: Wrote `axi_dma_map.c` (a misc device module) that `ioremap`s the DMA register window and exposes it as `/dev/axi_dma_regs` with safe `read`, `write`, and `llseek`.
- **Reason**: `/dev/mem` access to the PL address space (0x4040_0000) caused SIGBUS unless the kernel was patched. The helper keeps the mapping inside the kernel and lets user space talk through a stable interface.
- **Status**: Module builds using the board’s kernel headers, installs under `/lib/modules/<uname -r>/extra/`, and is loaded via `insmod`. Scheduler no longer needs `/dev/mem`.

## 7. Modify AxiDmaController to use /dev/axi_dma_regs
- **Change**: `AxiDmaController` now:
  - Checks `SCHEDRT_DMA_DEVICE` (default `/dev/axi_dma_regs`) before the `/dev/mem` fallback.
  - Uses `pread`/`pwrite` to access registers when the helper exists.
  - Keeps the old `/dev/mem` path as a fallback and retains the SIGBUS guard.
- **Reason**: To leverage the kernel helper and avoid direct `/dev/mem` mappings. Needed for boards where `/dev/mem` is restricted.
- **Status**: When `/dev/axi_dma_regs` is present, the controller logs “using char device …” and avoids SIGBUS. Without it, it falls back to `/dev/mem`.

## 8. dd/hexdump probing and kernel oops
- **Observation**: Running `dd if=/dev/axi_dma_regs …` caused kernel oops (`__copy_to_user_std`) because the helper attempted to copy raw DMA registers into user space while the DMA IP wasn’t clocked.
- **Resolution**: Added `pr_info/pr_debug` logging inside the helper to confirm register access and recommended using simple ioctls or kernel-side logging instead of raw `dd`. (Direct read/write remains risky when the shell hasn’t loaded.)
- **Status**: Still need a safe debug path (e.g., an ioctl that dumps specific registers). Logging shows whether the scheduler is actually talking to the helper.

## 9. FPGA manager file-path fix (pending)
- **Issue**: dmesg reported “Direct firmware load … failed” because FPGA manager looks under `/lib/firmware/`. The scheduler referenced `bitstreams/static_wrapper.bit` relative to its own directory.
- **Planned Fix**: Copy or symlink bitstreams into `/lib/firmware/bitstreams/` and either:
  - Run `--bitstream-dir=/lib/firmware/bitstreams`, or
  - Keep the scheduler’s path but ensure FPGA manager has access to the same file via symlink.
- **Status**: Needs to be done so the static shell and PR partial actually load. Without it, the DMA logic remains stuck in whatever bitstream shipped with the board.

## 10. DMA status logging (pending)
- **Need**: To determine whether the AXI DMA block ever leaves reset or flags errors (bits in `MM2S_DMASR`/`S2MM_DMASR`).
- **Plan**: Add kernel-side logging or ioctls in `axi_dma_map` to dump those registers when `FpgaSlotAccelerator` starts a transfer. Direct `dd` reads proved unreliable (crashed when the block wasn’t configured).
- **Status**: Pending implementation. Once in place, we’ll know if the DMA is truly frozen or if the PL design still needs clock/reset wiring fixes.

## 11. Add bitstream preparation script (Dec 2024)
- **Change**: Created `scripts/prepare_bitstreams.py` to strip the `.bit` headers, byte-swap the payload into `.bin` images, and drop them into `/lib/firmware/bitstreams/`.
- **Reason**: The fpga_manager driver rejects raw `.bit` files (“Invalid bitstream, could not find a sync word…”). It expects headerless, byte-swapped `.bin` files.
- **Status**: Run the script (with sudo when targeting `/lib/firmware`) before launching `sched_runner`, and point `--static-bitstream`/`--bitstream-dir` at the generated `.bin` files.

---

### Environment Variables / Flags
- `SCHEDRT_DMA_BASE`: Override AXI DMA base (default `0x40400000`).
- `SCHEDRT_DMA_DEVICE`: Path to the char device (default `/dev/axi_dma_regs`).
- `SCHEDRT_DMA_DEBUG`: Enable extra logging in the DMA controller.
- `SCHEDRT_UDMABUF`: Override udmabuf name (default `udmabuf0`).
- `--fpga-debug`: Print detailed overlay/FFT runner logs from `sched_runner`.

### Outstanding Tasks
1. Copy or link the bitstream files into `/lib/firmware/bitstreams/` so FPGA manager can load them.
2. Extend `axi_dma_map` with a safe ioctl/logging path to dump DMA status registers instead of relying on `dd`.
3. Verify (once bitstreams load) that the AXI DMA block exits reset; adjust the PL clock/reset nets if needed.

Keep this changelog updated as new experiments land so we avoid repeating the same steps.
