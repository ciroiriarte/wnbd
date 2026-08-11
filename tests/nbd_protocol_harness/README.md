# NBD protocol assertion harness

`fake_nbd_server.py` is a minimal NBD wire-protocol harness for tests that need
to assert client bytes or inject malformed server replies. It is not a full NBD
server and should not replace the qemu-backed functional tests.

Initial scenarios:

- `record`: perform a fixed-newstyle handshake and print transmission requests.
- `assert-disc`: fail unless the client sends `NBD_CMD_DISC` with zero offset and length.
- `assert-flush-zero`: fail unless `NBD_CMD_FLUSH` uses zero offset and length.
- `invalid-init-magic`: send malformed initial magic for handshake rejection tests.
- `ack-without-export`: send `NBD_REP_ACK` without `NBD_INFO_EXPORT`.
- `truncated-info`: send a too-short `NBD_REP_INFO` payload.

Example:

```powershell
python tests\nbd_protocol_harness\fake_nbd_server.py --self-test
python tests\nbd_protocol_harness\fake_nbd_server.py --port 10809 --scenario assert-flush-zero
```

The PR workflow runs `--self-test` to prove the harness can perform a loopback
handshake and record a request. Future GoogleTest integration should launch this
harness as a child process, wait for its listening port, run `WnbdRunNbdDaemon`
against it, and assert the harness exit code for each scenario.
