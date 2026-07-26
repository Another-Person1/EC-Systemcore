# ec-systemcore Python client

This dependency-free Python 3.11+ SDK uses the local ECSC v1 Unix-domain
socket. It has bounded framing, command, completion, listener, retained-state,
and PDO-reassembly queues. Connection loss or an epoch change invalidates all
queued/in-flight commands. Output writes and enables are never replayed, and a
new connection always starts disabled.

`SystemCoreClient.connect()` returns immediately. Command methods return
`concurrent.futures.Future` objects and do not block the caller. Robot loops
should poll `outputs_enabled` and `latest_input(...)`, explicitly enable once
per robot-enable transition, send complete SubDevice output images within the
daemon's output-command timeout, and never call `Future.result()` from the
periodic loop.
