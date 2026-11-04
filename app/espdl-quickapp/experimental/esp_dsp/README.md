# Pinned ESP-DSP vector subset for NuttX ASR

Upstream: https://github.com/espressif/esp-dsp, commit
`3c8ac0fdfec83740b783e200862c8d0c056de0ad`, Apache-2.0.
`UPSTREAM.json` records original paths, file lengths, Git blob IDs and SHA256.
The twelve upstream files are byte-for-byte unmodified; retain `LICENSE`.

The firmware compiles `dsps_add_f32_ansi`, `dsps_sub_f32_ansi` and
`dsps_mul_f32_ansi` at O3, without fast-math or fused arithmetic. Their
symbols are prefixed `glv_esp_dsp_` through compiler definitions to isolate
them from existing ESP-SR/ESP-DL libraries. No model tensors are changed.
`compat/` contains repository-native, private error/configuration headers;
it is not an ESP-IDF runtime or a replacement for the board configuration.

The new broadcast adapter batches contiguous/scalar suffixes into bounded
calls of at most1024 elements. Existing ownership, cancellation, exact
in-place alias validation and scalar fallback remain in the caller.

This subset is the official portable C implementation, not a claim that
ESP-DL executes the entire ONNX graph or that floating point uses PIE SIMD.
The inspected official P4 float dot/matrix assembly uses HWLP; its context
preservation and look-ahead accesses are not enabled by this integration.
The previously qualified integer PIE/double-core backend remains separate.
