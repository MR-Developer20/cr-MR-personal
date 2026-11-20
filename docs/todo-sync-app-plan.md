# Plan: companion app side of the To-do sync (Bluetooth)

Audience: whoever builds the iOS companion app (`PLAN-ios-companion.md` project). The contract is
[todo-sync-protocol.md](todo-sync-protocol.md), sections C1–C7. This plan says what the app must do and in what
order. It does not change the contract. If the app needs a wire change, change the contract first and bump `proto`.

## What the device does (so the app can rely on it)

- The X4 advertises as `X4 To-do` with service `8d4d5e22-336a-4fa2-bbdf-d7d1d6c90cf1` only while **To-do → Sync with
  phone** is open. It stops after 180 s or when the user leaves the screen.
- One phone connection at a time. Pairing uses LE Secure Connections with a 6-digit passkey shown on the X4 screen.
- The device never merges. The app merges (C4) and then pushes with `baseRev`.
- The device sanitizes text again (C3) and returns the stored document. The app adopts that document as truth.

## Phase A — Foundation (no device needed)

1. **Model.** `TodoRecord` (live item or tombstone), `TodoDocument { version, rev, items, deleted }`. Ids are 16 hex
   chars, non-zero u64; accept uppercase, write lowercase.
2. **Sanitizer.** Byte-exact C3 on UTF-8 bytes. Pass all seven test vectors from the contract as unit tests.
3. **Merge.** Implement the C4 table as a pure function `merge(device, phone) -> (document, pushNeeded)`. Unit-test
   every row, plus: tombstone cap of 64, the "more than 100 live → refuse to push" error, and the accepted
   tombstone-eviction edge.
4. **Codec.** JSON encode/decode of C1, the `baseRev` field on requests, missing-field defaults, `version > 1`
   rejected.
5. **Framing.** Encode/decode `[type u8][seq u16 LE][payload]`, `u32` little-endian fields, and IEEE CRC-32
   (poly `0xEDB88320`). Unit-test CRC against a known vector (`"123456789"` → `0xCBF43926`).

**Done when:** all unit tests pass with no Bluetooth code in the target.

## Phase B — Transfer state machine (still no device)

Build the transfer layer against a fake peripheral that replays frames.

- **Read:** send `GET`; expect `CHUNK`×n with `seq` 0,1,2…; then `END(totalLen, crc32)` whose `seq` equals the chunk
  count. Check length and CRC. Any gap in `seq`, a length mismatch, or a CRC mismatch fails the read.
- **Write:** send `PUT_BEGIN(totalLen, crc32)` (`seq` 0), `PUT_CHUNK`×n (`seq` 0,1,2…) (size by `maximumWriteValueLength(for:
  .withResponse)` minus the 3-byte header), then `PUT_COMMIT` (`seq` 0).
  - `OK(newRev)` → then read the streamed stored document (`CHUNK`/`END`) and adopt it.
  - `ERR(1)` conflict → read the streamed current document, re-merge, push again (limit retries to 3).
  - `ERR(2..6)` → surface the message; do not retry blindly. Code 6 (busy/out of memory) may retry once after 2 s.
- **Rules:** one transfer at a time. A new `GET` or `PUT_BEGIN` aborts the partial one. Time out a stalled transfer
  after 10 s without a frame.

**Done when:** the fake-peripheral tests cover success, conflict, each ERR code, dropped chunk, bad CRC and timeout.

## Phase C — Bluetooth layer (CoreBluetooth)

1. Scan for the service UUID only. Show devices named `X4 To-do`. Remember the chosen peripheral identifier.
2. Connect. Read **Info** first. This read is encrypted, so iOS shows the pairing prompt and the user types the
   passkey from the X4 screen. Treat "not yet paired" and "pairing rejected" as distinct, readable errors.
3. Parse Info JSON: require `proto == 1`; keep `rev`, `maxItems`, `maxText`, `maxBody`. Refuse a higher `proto` with a
   message to update the app or firmware. `maxBody` can be below 32768 when the device is short of memory: never send a
   larger body, and tell the user to delete items instead. Read Info again after each connection.
4. Discover Command and Data. Subscribe to Data notifications before sending any command.
5. Write Command with response (`.withResponse`) only.
6. Handle disconnects: drop the partial transfer, keep local data, offer reconnect. Bonded reconnects should work
   without a new passkey while the X4 Sync screen is open.
7. iOS negotiates the MTU itself; size writes from `maximumWriteValueLength(for: .withResponse)`. Never assume 247.
   The device asks for 247, so expect frames of up to 244 bytes.

**Done when:** a real X4 pairs once, then reads and writes a list of 100 items with 120-byte texts (about 15–20 KB),
and an interrupted transfer recovers on the next attempt.

## Phase D — Sync flow in the app

1. **Sync button** → connect → read device document → merge with local → show a summary (added/changed/removed on
   each side) → push if needed → adopt returned document → save locally.
2. **Local edits** bump the record `rev` by 1 (C4); deleting makes a tombstone with `rev + 1`.
3. **Errors users will see:** list too long (over 100 live), text empty after sanitizing, device busy, device not
   found ("Open To-do → Sync with phone on the X4"), pairing failed, conflict loop (3 retries).
4. **Retries:** if the `OK` frame is lost after the device applied a push, the retry still carries the old `baseRev`,
   so the device answers `ERR 1` (conflict) and streams the current document. Re-merge against it: the records match,
   so push again with the new `baseRev`. That second push is answered `OK` and changes nothing.
5. Show the sanitized text the device returned, since it may differ from what the user typed.

## Phase E — Verification with the device

Run these with the X4 on the Sync screen. Expected results are the contract's; a failure means a bug on one side.

| Test | Expected |
|---|---|
| First pairing | Passkey shown on X4; entered on iPhone; Info read succeeds |
| Empty device list, 3 phone items | Push OK; X4 shows the 3 items; `rev` increased |
| Edit same item both sides (equal rev) | Phone content wins, `rev + 1` |
| Delete on phone, edit on device | Higher `rev` wins; tombstone applied |
| 100 items, 120-byte texts | Completes; CRC OK; no ERR 3 |
| 101 live after merge | App refuses to push, shows the error |
| Kill the app mid-PUT | X4 list unchanged; next sync works |
| Leave the X4 Sync screen mid-transfer | App shows a disconnect error; no data loss on either side |
| Retry a push after OK was lost | `ERR 1` plus the current document; the re-merge and second push give `OK`; no duplicate items |
| Bad CRC injected in a test build | ERR 4; nothing applied |

## Open points to settle before coding

- Where the app stores its local document and how it keeps tombstones past the device's 64 cap (the device evicts
  oldest first; the app may keep more but must send at most 64).
- Whether to also support the Wi-Fi transport (C6). It is specified but not part of this plan.
- Background sync is not possible: the X4 only advertises while its Sync screen is open, so syncing is always an
  explicit action on both devices.
