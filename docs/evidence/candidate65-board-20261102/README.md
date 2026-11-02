# Candidate 65 board evidence

This directory records the frozen candidate 65 firmware and source inputs. The 789 source SHA-256 values came from the verified firmware build metadata. `PHYSICAL65.json` is the original user acceptance record. `BOARD-VERIFICATION.json` is a portable summary of the original board and build records; its `evidence_sha256` fields identify those records.

The board run verified SPI3 DMA at 20 MHz with zero reported errors/timeouts, no native-browser executable pool, the measured user heap, a bounded INT8 LLM/cache diagnostic, and screen, backlight, and touch response. The production model was Q4_32 with scalar backend. The diagnostic did not execute a real Home Assistant action, microphone capture, or speaker playback. Full-resident Q4 was not possible in the measured ordinary user heap. The later rollback to this image did not receive a new physical screen confirmation.

This commit contains the source inputs that differ from the published candidate 56 baseline and the corresponding build tooling. It does not contain the firmware binary, SD model weights, machine-local build workspace, authentication files, or generated caches.
