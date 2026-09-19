# Day-one baseline

Captured on 2026-09-19 before introducing the CabinFlow runtime scaffold.

## Repository state

- Source revision: `367d8a0` on `master`.
- Existing tracked modifications: 82 files.
- Existing untracked paths: 2.
- Existing diff SHA-256:
  `4f5005e3194c07ce15b407fc9c51bc53ac6335d8050ee30bbae6321804c81628`.

The existing modifications are user-owned baseline state. The runtime migration
does not reset, stage, format, or otherwise rewrite them.

## Toolchain discovered

- CMake 3.28.3
- GCC/G++ 13.3.0
- GNU Make and CTest available
- Ninja, pkg-config, Protobuf compiler, ZeroMQ development package, and
  eventpp development package were not found

## Legacy build result

Configuring `infra-controller` independently fails at `find_package(eventpp)`.
No legacy source file was changed to work around this. The root build initially
contains only the migration scaffold and its standard-library build sanity test.
