# Copilot code review instructions

## Backwards compatibility for telemetry and messages

When reviewing pull requests, pay special attention to backwards compatibility of the
host-facing telemetry interface and message interface. These are consumed by external
tools (host software, `tt-flash`, `tt-smi`, etc.).

Flag any change that could break compatibility, including:

- **Telemetry tags** (`lib/tenstorrent/bh_arc/telemetry.h`): reusing or renumbering an
  existing `TAG_*` value, changing the meaning, units, encoding, or bit-layout of an
  existing tag, or removing a tag. New tags must be appended with a new number and must
  not shift existing values. `TELEMETRY_VERSION` should be bumped when the meaning of an
  existing tag is redefined, and `TAG_COUNT` (and any related counts) updated correctly.

- **Host messages** (`include/tenstorrent/msgqueue.h`, `include/tenstorrent/smc_msg.h`):
  reusing or renumbering an existing `TT_SMC_MSG_*` command code, changing the layout,
  field sizes, padding, or semantics of an existing request/response struct in
  `union request` / `struct response`, or changing `REQUEST_MSG_LEN` / `RESPONSE_MSG_LEN`.
  New request structs should append fields into existing reserved/padding space where
  possible and add new command codes rather than repurposing old ones.

- **Response encodings and error codes**: changing the meaning of existing response
  `data[]` fields or existing error-code enum values (e.g. `gddr_reset_err`,
  `eth_reset_err`) that host software already interprets.

For any such change, call it out explicitly and ask whether host-side consumers have
been updated and whether the change is intentionally breaking. Prefer additive,
backwards-compatible changes.
