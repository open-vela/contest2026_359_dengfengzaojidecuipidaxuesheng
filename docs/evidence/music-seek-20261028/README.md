# Music seek prefix replay — host verified

When playback seeks into an undownloaded region and then back into an already
downloaded prefix, the service keeps one HTTP producer and replays the retained
SD prefix. Decoder and PCM state restart at the requested position.

The source hashes and original test results are in `fix-verification.json`.
`loading-seek-after.log` records sample-exact replay through the cached-prefix
and live-ring seam, one stalled GET, pause/rapid/zero seeks, cancellation, and
the inaccessible-cache fallback. The same host test passed again on 2026-11-01:

```
python3 tools/test_music_background.py --apps /home/flash/vela-p4/wifi-throughput-20261027/apps
```

The test uses ASan/UBSan and an offline MP3 fixture. The P4 RV32 object was
compiled; full firmware link, flash, and physical listening remain unverified.

The source hashes in `fix-verification.json` identify the original seek-only checkpoint. The upstream integration also adds offline favorite song and cover caching. Its complete music and lock-screen host suites passed again on 2026-11-02; those results do not imply firmware flash or physical speaker acceptance.
