# Sanitizer verification

## AddressSanitizer

Run from `runtime/`:

```bash
cmake --preset linux-asan
cmake --build --preset linux-asan
ctest --preset linux-asan
```

Verified on 2026-09-29 in WSL2 Ubuntu 24.04 on x86 with GCC 13.3.0:
`cmake --build --preset linux-asan -j2` and `ctest --preset linux-asan
--output-on-failure` passed all 26 current CTest cases. This validates the
executed local test paths, not untested interleavings or target hardware.

On 2026-10-02, after the external one-shot lifecycle fix, the standard
`cmake --preset linux-asan`, `cmake --build --preset linux-asan -j 4`, and
`ctest --preset linux-asan --output-on-failure` workflow passed 29/29 cases.
See [`runtime-migration-closeout.md`](runtime-migration-closeout.md) for the
new contract coverage and remaining unverified paths.

## ThreadSanitizer

The initial 2026-09-20 WSL run failed before test code with
`FATAL: ThreadSanitizer: unexpected memory mapping`; that run produced no
CabinFlow race result. A later network-migration run recorded 8/8 selected
network tests passing under TSan after using `setarch x86_64 -R` to avoid the
startup mapping failure (see `capability-migration.md`). On 2026-09-29, the
current `target_runtime_contract_test`, `session_ledger_test`, and
`runtime_demo` were rebuilt in the manually configured `build/linux-tsan`
directory and each passed once through `setarch x86_64 -R`, with no TSan race
report. These are selected runs, not a full-Runtime TSan pass. There is no
`linux-tsan` project preset and the then-current 26-case suite had not been
verified under TSan. Do not infer race freedom from either the ASan run or
the selected TSan tests.

On 2026-10-02, the existing manual `build/linux-tsan` cache was verified to
use `-fsanitize=thread` in both compile and link flags. After regenerating
that same cache to add the new target, `runtime_test` and
`runtime_lifecycle_contract_test` were each run once with `setarch x86_64 -R`.
Both exited 0 without a TSan report. This is a two-test subset, not a full
29-case TSan run; the exact commands and initial missing-target failure are
recorded in the migration closeout document.
