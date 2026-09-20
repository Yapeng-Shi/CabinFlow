# Sanitizer verification

## AddressSanitizer

Run from `runtime/`:

```bash
cmake --preset linux-asan
cmake --build --preset linux-asan
ctest --preset linux-asan
```

Verified on 2026-09-20 in Ubuntu 24.04 on WSL with GCC 13.3.0: all ten CTest
cases passed under AddressSanitizer. This validates the current local test
paths only; it is not target-hardware validation.

## ThreadSanitizer

The same source built successfully with `-fsanitize=thread` on that WSL
environment, but every executable failed before test code ran with
`FATAL: ThreadSanitizer: unexpected memory mapping`. No race report was
produced, so TSan is recorded as environment-blocked rather than passed or
failed for CabinFlow code. Re-run it in a native Linux environment before
making concurrency-safety claims.
