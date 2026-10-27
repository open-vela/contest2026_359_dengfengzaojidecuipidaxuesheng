# Candidate 19 timer / disconnect evidence

RUNTIME.json is the current acceptance summary. NEGATIVE-CONTROL.json and host/callout-old-negative/test.log preserve the expected prior-production failure (32 distinct timers, one delivered, representative HCI timer lost). The fixed production dispatcher passes 100 batches of 32 timers under ASan/UBSan.

Real C19: six pure cycles reviewed from original numeric evidence, another six-cycle completed run, eight business-client-exit/reconnect cycles; no extra epoch transitions. The first pure runner failed because a serial field label lost a character. Its JSON is unchanged; fixed-pure-01-REVIEW.json records the exact numeric/accounting review. Do not relabel the original runner as exit zero.

channel-c19-01 / channel-c19-03 are complete eight-round peer-byte comparisons before/after warm reboot. channel-c19-02 completed eight peer-byte rounds then failed to parse the last FIFO diagnostic tag (leading B missing); retained as a failed runner, not used for the primary complete-run count. Console helper now continuously drains serial while typing and retains CR-time replies. Business cycles add 16 verified rounds. Windows pairing API reports NONE protection-level-used, while actual board encryption and HID subscription are checked; no MITM claim.

First deployment attempt rolled back after a console Write timeout without an early streamed boot capture. ATTEMPT1.json / ROLLED-BACK.json and rollback-boot.log preserve that event. Attempt 2 uses the exact same image and identity; streamed boot, app readback and complete data comparison pass. DEPLOY-BACKUP.json references only hashes; no full Flash, app binaries or user data are archived here.

The original historical 520 air trace was not reconstructed, so this confirmed timer race is not claimed as the exclusive cause of every supervision timeout. Cold power, physical screen and three real simultaneous links are not verified. Helper files retain their original output directory conventions.
