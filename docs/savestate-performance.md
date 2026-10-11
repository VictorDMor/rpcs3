# Savestate performance and integrity

## Transport changes

The Zstandard writer uses a continuous lossless stream at compression level 1,
with a content checksum, at most four workers and 1 MiB jobs. Zstandard owns the
job queue and backpressure. The serialization buffer is reused rather than moved
into a separate 16 MiB frame for each worker. Short writes prevent successful
finalization and publication of the pending file.

The reader reuses a bounded compressed input buffer, drains buffered decoder
output even when no compressed input remains, and stops immediately on decoder
errors. EOF inside a frame is an error; EOF after a completed frame is valid.
Compression and decompression contexts have automatic lifetime management,
including when a reader is discarded without explicit finalization.

`serial::try_read` now propagates failed reads and rejects position overflow.
Failed raw reads do not advance the cursor or copy partial data into the caller's
destination. Invalid handlers remain invalid even when their buffer contains
partially decoded data.

Guest payload layout and serialization versions are unchanged. The reader still
accepts legacy concatenated Zstandard frames. An actual pre-change build also
loaded a new checksummed FIFA 10 state successfully. Older gzip and uncompressed
transports are not rewritten.

API reference: [Zstandard streaming and context API](https://github.com/facebook/zstd/blob/dev/lib/zstd.h).

## Reproducible benchmark

Build the existing unit-test target with `BUILD_RPCS3_TESTS=ON` and an installed
GTest package. The benchmark is disabled during ordinary test runs:

```sh
build/bin/rpcs3_test --gtest_filter=zstd_serialization.DISABLED_Benchmark --gtest_also_run_disabled_tests
```

`RPCS3_SAVESTATE_BENCH_KIND` selects `zero`, `mixed`, or `random` synthetic
64 MiB data. `RPCS3_SAVESTATE_BENCH_INPUT` selects a local existing Zstandard
savestate instead, sampling at most the first 256 MiB of its decoded payload.
Guest data is never printed or retained. Only disposable output files are written.

The benchmark uses the production file handler on a named capture thread,
16 MiB writes, finalization and file synchronization. It restores through the
production reader in deliberately uneven chunks, checking every byte. Input
preparation is outside the timed interval; read timing includes comparison with
the expected bytes. Peak RSS covers the entire test process, including the input
sample and fixture. The `peak_rss` field uses the platform's `getrusage` units
(bytes on macOS, KiB on Linux); it is omitted on Windows.

Use separate processes and alternate baseline/candidate execution. Close the
emulator and avoid concurrent compilation before measuring. Keep input size,
chunking, compiler flags, synchronization and verification identical.

## Measurements: October 11, 2026

Apple Silicon, ARM64 Release build, bundled Zstandard, baseline `8f2fe1a99`.
The final comparison alternated execution order over three repetitions per
version/profile. Values below are medians. Earlier compression-level trials
selected level 1 over levels 2 and 3 on the real sample.

| Payload | Save before / after | Read + verify before / after | Peak RSS before / after |
| --- | ---: | ---: | ---: |
| 64 MiB zero | 14.97 / 6.18 ms | 6.93 / 11.02 ms | 331.75 / 199.34 MiB |
| 64 MiB mixed | 36.48 / 14.11 ms | 11.10 / 11.31 ms | 340.44 / 200.47 MiB |
| 64 MiB random | 15.74 / 15.95 ms | 12.31 / 12.58 ms | 340.38 / 205.05 MiB |
| 256 MiB FIFA 10 prefix | 287.58 / 82.73 ms | 121.07 / 117.07 ms | 908.53 / 397.70 MiB |

The real-prefix writer is **3.48 times faster**, with **56.2% lower process peak
RSS**. Its file grows from 225,178,483 to 236,476,155 bytes (**5.0%**). The zero
profile demonstrates a real read regression from checksum work; this is not a
universal load-speed improvement. Random-data throughput is essentially unchanged
in this small sample.

### Full emulator observations

FIFA 10 (BLES00615), Vulkan, with a disposable portable configuration and cloned
game/cache data:

- Two captures with the new writer completed in **1.15712 s** and **1.12934 s**.
  Their resulting approximately 1.4 GiB states both restored, with the expected
  clock, score, teams and scene visible afterward.
- A baseline capture from the same portable configuration completed in
  **1.24312 s**. This single comparison indicates only a modest full-save gain;
  it does not establish a 3.48-times faster complete emulator save.
- The baseline executable loaded the second new state, then successfully created
  and reloaded a legacy state. This exercises both transport directions in the
  real emulator, in addition to synthetic legacy-frame tests.
- One capture while emulation was paused was rejected because SPUs were not at
  safe capture points. Resuming emulation allowed the next capture to succeed.
  The SPU safety gate was preserved.
- New-state reloads reached the automatic pause after approximately **19.17 s**
  and **24.01 s**, measured from their logged savestate information messages.
  These timings include graphics preparation and are not isolated decoder times.

The first exploratory launch used the normal configuration because the portable
directory was initially placed inside the bundle. Subsequent measurements verified
the portable configuration and HDD paths in the log. On macOS the `portable`
directory belongs **beside the `.app`**, not inside `Contents/MacOS`.

Raw measurements, logs, original executables and disposable runtime data remain
under the ignored local `build-arm64/savestate-results/` directory. Game states,
firmware and local configuration are not repository artifacts.

## Reusing native SPU objects within a session

The SPU disk cache stores guest programs, which were translated and compiled
again after every reset. On ARM64, successful LLVM objects can now be retained
in a process-local cache. Its SHA-256 key covers the complete generated bitcode,
CPU, target features, code model, relocation model and optimization level.
Changed IR or target settings cause a miss. Objects are immutable, copied into
the new execution engine and relocated again; live code, patched branch
trampolines and dispatcher tables are not retained. The cache has a 128 MiB
object-data limit and a 65,536-entry limit, with FIFO eviction. Closing RPCS3
discards it. The existing SPU Cache option controls its use; debug builds keep
their diagnostic cache path.

Generating and finalizing an object now share one LLVM recovery thread on the
normal ARM64 path. Module verification remains enabled, while large IR and
disassembly strings are generated only for diagnostics that will be used.
The existing TBL2 retry and SPU capture safety checks remain in place.

FIFA 10 was restored from the same private state twice using the same executable
and configuration, first with a cold native-object cache and then in the same
process with that cache warm:

| Measurement | Cold native cache | Warm native cache |
| --- | ---: | ---: |
| SPU cache initialization | 10.708 s | 1.597 s |
| Native cache hits / misses | 7 / 13,333 | 13,338 / 3 |
| Savestate information to automatic first-frame pause | 19.316 s | 9.493 s |

About 39.6 MiB of object data was retained. After the warm restore, the expected
Cruzeiro versus Atletico Mineiro 0-0 scene at 2:10 was visible, the keyboard
resumed the match, and the clock advanced to 5:35. A further capture completed
in 1.177 s and its automatic restore reached the pause in 11.546 s, including
2.657 s of SPU cache initialization (13,353 hits / 1 miss). These are native
observations for one title, not statistical guarantees for other workloads.

The cache interface follows LLVM's [MCJIT object-cache flow](https://llvm.org/docs/MCJITDesignAndImplementation.html).
Additional graphics-preload experiments were discarded after a measured
regression. The Vulkan loading behavior is unchanged in this iteration.

## Validation and remaining work

The ARM64 application and test executable build successfully. All **223 enabled
unit tests** pass. Eight serialization tests cover roundtrip boundaries, reader
finalization, malformed magic, legacy concatenated frames, truncated headers and
trailers, checksum corruption, clean EOF/empty frames, and peek overflow. Six
native-object tests execute compiled functions and cover reuse after a JIT
reset, changed IR, changed target, external-symbol relinking, disabling the
cache, and concurrent immutable-object reuse.

Checksum failures are detected when the decoder reaches the affected frame data
or trailer. A small prefix read cannot certify the integrity of an unread tail.
This work does not prove arbitrary malformed guest-object deserialization safe,
cross-build guest compatibility beyond the tested baseline, deterministic gameplay
replay, or stability across other titles.

The overall optimization goal remains open: the user wants save/load to feel
near-instant, comparable to SNES/Genesis emulators. A 9-12 second warm load is
still far from that target. The next loops must reduce the remaining preparation
and state-reconstruction costs, and the cold load still compiles missing SPUs.
`Savestate phase` log entries now report elapsed time between existing capture
progress stages. One instrumented capture spent 1.034 s saving HDD1, 0.244 s
saving VM memory, 0.002 s collecting/writing the header and 0.010 s finalizing
compression. The logged save total was 1.409 s; subsequent retention cleanup
added about 0.438 s. In particular, Vulkan interpreter pipeline preload reconstructs
thousands of base variants during restart; codec-only improvements do not remove
that cost. Existing rendering artifacts and unrelated Qt/MoltenVK issues are not
claimed fixed by this change.
