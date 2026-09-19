# CabinFlow Runtime development

## Day-one build scaffold

The repository root builds only the new runtime foundation during the migration.
Existing components remain on their standalone build paths until they have a
reproducible dependency definition and automated test coverage.

From the repository root, run:

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

The same workflow is available from any directory through:

```bash
./scripts/build.sh
./scripts/test.sh
```

The generated files are placed under `build/`, which is already ignored by Git.

## Legacy baseline

The current standalone `infra-controller` configuration depends on `eventpp`.
It is deliberately not included in the root build until that dependency is
captured by a reproducible development environment.
