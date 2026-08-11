# WNBD automated test architecture

This directory contains the current driver-backed GoogleTest suite and the first
executable scaffolding for protocol-level and CI diagnostics tests.

## Current suites

- `libwnbd_unit_tests.exe` (`tests/libwnbd_unit_tests`): fast user-mode
  GoogleTest binary for protocol/helper checks that do not require WNBD driver
  installation or qemu.
- `libwnbd_tests.exe` (`tests/libwnbd_tests`): integration-oriented tests that
  exercise the installed WNBD driver, virtual disks, driver options, IOCTLs, and
  optional qemu-backed userspace NBD mappings.
- `nbd_protocol_harness/fake_nbd_server.py`: lightweight NBD wire assertion
  harness for protocol tests that qemu cannot express; CI runs its loopback
  self-test.
- `ci/*.ps1`: CI helper scripts for deterministic readiness checks, diagnostics,
  and process cleanup.

## Target split

The existing `libwnbd_tests.exe` binary should eventually be split into two
explicit targets:

1. `libwnbd_unit_tests.exe`
   - no driver install;
   - no qemu dependency;
   - tests pure protocol helpers, packet encoding/decoding, option parsing, and
     parser helpers extracted from `libwnbd/nbd_protocol.cpp`;
   - CI builds and runs this target before importing the test certificate,
     installing the driver, or installing qemu.
2. `libwnbd_integration_tests.exe`
   - requires the WNBD driver;
   - covers mapping, IOCTLs, disk I/O, qemu-backed NBD, removal, and pending I/O
     race scenarios.

Most existing tests are integration tests even when they use `MockWnbdDaemon`,
because that mock still starts WNBD dispatchers and uses the installed driver.

## Protocol coverage roadmap

Use `nbd_protocol_harness/fake_nbd_server.py` or an equivalent in-process C++
server to add regression tests for:

- graceful shutdown sends `NBD_CMD_DISC`;
- `NBD_CMD_FLUSH` uses zero offset and length;
- invalid `NBDMAGIC` is rejected;
- unsupported oldstyle handshake is rejected clearly;
- fixed-newstyle client flag is masked against server-advertised flags;
- `NBD_OPT_GO` rejects truncated info payloads;
- `NBD_INFO_EXPORT` is required before ACK;
- `NBD_INFO_BLOCK_SIZE` constraints are requested and parsed.

## Negative integration roadmap

Add driver-backed tests for:

- abrupt TCP disconnect while I/O is pending;
- server stalls with many outstanding requests;
- disk removal while I/O is in flight;
- unexpected reply handles;
- short read payloads;
- backend NBD error replies;
- unsupported flush/unmap feature flags.

## Performance regression coverage

Implemented deterministic structural checks:

- read response `DataBufferSize` equals actual request length and oversized
  reads are rejected before submitting the WNBD response.

## Performance regression roadmap

Avoid wall-clock timing gates on shared CI runners. Prefer deterministic counters
and structural assertions, such as:

- write path avoids an extra full-payload copy once scatter/gather is added;
- submitted request lookup reports O(1) path usage once indexed;
- lookaside/pool miss counters are bounded;
- pending request table does not rehash in the hot path.

## Driver verification roadmap

Per-PR CI should stay fast and deterministic. Full SDV and Driver Verifier should
run in scheduled or explicitly dispatched workflows until runtime and artifact
collection are reliable.
