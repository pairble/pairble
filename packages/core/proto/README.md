# Vendored protobuf schemas

Copied verbatim from ESP-IDF v5.4.4, Apache-2.0:

| File | Origin |
|---|---|
| `constants.proto`, `session.proto`, `sec0.proto`, `sec1.proto`, `sec2.proto` | `components/protocomm/proto/` |
| `wifi_constants.proto`, `wifi_scan.proto`, `wifi_config.proto` | `components/wifi_provisioning/proto/` |

They are here as the reference the hand-written codec in `src/proto/` is
checked against, not as a build input - nothing generates code from them.
Keeping them in the tree means a future IDF change can be diffed against what
the codec was written for.

Only the subset pairble speaks is implemented: security scheme 1, the scan
endpoint and the config endpoint. `sec0.proto` and `sec2.proto` are here
because `session.proto` imports them, not because anything encodes them;
`wifi_ctrl.proto` is absent entirely.

The golden vectors in `../test/fixtures/` are generated from these files with
Google's protobuf implementation, which is what the hand-written codec is
checked against. Regenerate with `tools/gen-vectors.py` after changing a schema.
