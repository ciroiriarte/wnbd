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

Implemented deterministic protocol checks cover:

- graceful shutdown sends `NBD_CMD_DISC`;
- `NBD_CMD_FLUSH` uses zero offset and length;
- invalid `NBDMAGIC` is rejected;
- fixed-newstyle client flag is masked against server-advertised flags;
- `NBD_OPT_GO` rejects truncated info payloads;
- `NBD_INFO_EXPORT` is required before ACK.

Remaining protocol coverage candidates:

- unsupported oldstyle handshake is rejected clearly;
- `NBD_INFO_BLOCK_SIZE` constraints are requested and parsed.

## Negative integration roadmap

Implemented fake-server integration checks cover abrupt disconnect, stalled reads
with several outstanding requests, disk removal while I/O is in flight,
unexpected reply handles, short read payloads, and backend NBD error replies.

Remaining driver-backed negative coverage candidates:

- unsupported flush/unmap feature flags.

## Performance regression coverage

Implemented deterministic structural checks:

- read response `DataBufferSize` equals actual request length and oversized
  reads are rejected before submitting the WNBD response;
- userspace NBD writes send header and payload without staging an extra
  full-payload copy;
- the userspace NBD pending-request table is reserved up front and unit-tested
  against rehashing within the expected reserve.

## Performance regression roadmap

Avoid wall-clock timing gates on shared CI runners. Prefer deterministic counters
and structural assertions, such as:

- submitted request lookup reports O(1) path usage once indexed;
- lookaside/pool miss counters are bounded.

## Driver verification roadmap

Per-PR CI should stay fast and deterministic. Full SDV and Driver Verifier should
run in scheduled or explicitly dispatched workflows until runtime and artifact
collection are reliable.
