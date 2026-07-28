# LTP differential lane

This manual lane builds the curated Linux Test Project registrations in
`tests.list` for both guest ISAs and compares engine verdicts with a native or
QEMU oracle. The upstream revision is pinned by `LTP_PIN` in `build.sh`.

`build.sh` prepares the binaries; `run.sh` runs one-at-a-time and
`run-parallel.sh` runs bounded category batches. These shell prototypes are
scheduled for replacement by the repository's C matrix tooling and are not CI
entry points.

An oracle result other than PASS is `skip`; matching complete assertions and
exit status are `ok`; matching assertions with a bad teardown are `TEARDOWN`;
all other oracle-PASS mismatches are `DD-GAP`. Select work with `LTP_ARCHES`,
`LTP_CATEGORY`, `LTP_ONLY`, and `LTP_TIMEOUT`.

The list intentionally excludes tests requiring external helpers, devices,
huge pages, cgroups, kernel modules, or elevated privileges. Generated output
belongs under `out/` and is never an input to the repository.
