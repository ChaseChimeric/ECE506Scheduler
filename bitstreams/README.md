## Provided PR images

These artifacts are copied directly from the Vivado project under `../fft_fir_reconfigurable` so they can be consumed by the scheduler without hunting through the `runs/` tree.

- `top_reconfig_wrapper.bit` + `top_reconfig.hwh` Full bitstream + handoff for the static design (`fft_fir_reconfigurable.runs/impl_1/top_reconfig_wrapper.bit`, `fft_fir_reconfigurable.gen/.../top_reconfig.hwh`). The `FpgaSlotAccelerator` uses this for the initial shell when `--static-bitstream` is left at its default.
- `fft_partial.bit` + `fft_partial.hwh` Partial bitstream for the FFT RM (`top_reconfig_i_reconfiguable_region_reconfiguable_region_1_inst_0_partial.bit`). The HWH (`.../reconfiguable_region_1_inst_0.hwh`) shows the RP exposing the `xfft_0` ports.
- `axis_passthrough_partial.bit` + `axis_passthrough_partial.hwh` Partial for the current child RM that wires the RP ports straight through (`top_reconfig_i_reconfiguable_region_reconfiguable_region_inst_0_partial.bit`). It serves as the FIR placeholder until an actual FIR RM exists.
- Each `.bit` has a matching `.bin` (raw binary) generated for Linux `fpga_manager`; when invoking `fpga_loader`, point at the `.bin` filenames.

### FIR status

Vivado exported metadata for `reconfigurable_region_2_inst_0` (`fft_fir_reconfigurable.gen/.../reconfigurable_region_2_inst_0.hwh`), but there is no matching `*_reconfigurable_region_2_inst_0_partial.bit`. Build that RM in the `fft_fir_reconfigurable` project when you are ready, place the resulting partial next to these files (e.g., `fir_partial.bit`), and point the scheduler overlay at it with `--overlay=fir:1:fir_partial.bit`.

### Using with sched_runner

Example invocation after building the repo:

```sh
./build/sched_runner \
  --app-lib=build/libdemo_dash_app.so \
  --backend=fpga \
  --bitstream-dir=bitstreams \
  --fpga-mock \
  -- --overlay=fft:1:fft_partial.bit --overlay=fir:1:axis_passthrough_partial.bit
```

Drop `--fpga-mock` and add `--fpga-real --fpga-manager=/sys/class/fpga_manager/fpga0/firmware` when deploying on the board, or override `--static-bitstream`/`--bitstream-dir` if you relocate these files.

You can also validate the hardware path without the full scheduler by running the standalone loader:

```sh
sudo ./build/fpga_loader \
  --static=bitstreams/top_reconfig_wrapper.bin \
  --partial=bitstreams/fft_partial.bin \
  --manager=/sys/class/fpga_manager/fpga0/firmware
```

The loader stages the `.bin` images into `/lib/firmware`, asserts the DFX decouple signal (AXI GPIO @ 0x4120_0000) during reconfiguration, sets the `fpga_manager` partial flag, and then re-enables traffic once the partial image finishes loading.

For a quick datapath check that also hits the AXI DMA + FFT RP, use `fpga_fft_dma_loader`:

```sh
sudo ./build/fpga_fft_dma_loader \
  --static=bitstreams/top_reconfig_wrapper.bin \
  --partial=bitstreams/fft_partial.bin \
  --mm2s-buf=/dev/udmabuf0 \
  --s2mm-buf=/dev/udmabuf1 \
  --manager=/sys/class/fpga_manager/fpga0/firmware
```

It will stream a ramp from `/dev/udmabuf0` through the FFT RM via the AXI DMA mapped at `0x40400000`, capture the results in `/dev/udmabuf1`, and print the first few words so you can confirm the PR region responds.
