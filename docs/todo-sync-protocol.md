# To-do sync protocol (v1)

The contract between the X4 firmware and its companion app for the to-do list. It is implemented by `lib/TodoList`
(`applyIncoming`), `src/TodoStore` (`parseIncoming`) and, in later phases, the Wi-Fi and Bluetooth transports. The file
format of `/.crosspoint/todo.json` is described in [file-formats.md](file-formats.md). The section below is copied
unchanged from the project's plan; the plan files it names belong to the firmware and companion-app projects and are
not in this repository.

## Shared contract — To-do sync v1

> This section is duplicated **verbatim** in `PLAN-crosspoint-todo.md` (firmware) and `PLAN-ios-companion.md` (iOS). Change both together. Bump `version` (document) or `proto` (BLE) whenever the wire format changes.

### C1. Document

Used for the device file `/.crosspoint/todo.json`, HTTP bodies and BLE payloads. The device file already uses this shape (`src/TodoStore.cpp` from patch 0002).

```json
{
  "version": 1,
  "rev": 42,
  "items": [
    { "id": "5f431f29eab058ba", "text": "Buy milk", "done": false, "rev": 3, "seq": 40, "order": 2048 }
  ],
  "deleted": [
    { "id": "484d45672c4f9869", "rev": 2, "seq": 41 }
  ]
}
```

| Field | Type | Rule |
|---|---|---|
| `version` | int | Always `1`. Readers refuse higher values. |
| `rev` | u32 | Device list revision. +1 for every device-side change. Ignored when sent by the phone. |
| `id` | string | 16 lowercase hex chars = non-zero u64. Unique across `items` + `deleted`. Readers accept uppercase. |
| `text` | string | Sanitized per C3. Never empty for live items. Absent in `deleted`. |
| `done` | bool | Live items only. |
| `rev` (item) | u32 | Per-record revision. +1 on every edit by whichever side edits it. |
| `seq` | u32 | Device-local ordering of changes. Assigned by the device only. The phone may echo it; the device ignores it. |
| `order` | i32 | Display position, ascending. Ties broken by `id` as unsigned 64-bit, ascending. |

Missing numbers/bools read as `0`/`false`. A missing array reads as empty.

### C2. Limits

| Limit | Value |
|---|---|
| Live items | 100 |
| Tombstones (`deleted`) | 64 (device drops lowest `seq` first) |
| Text | 120 bytes UTF-8 after sanitizing |
| Request body (HTTP or BLE) | 32768 bytes |

### C3. Text sanitizing (byte-exact on both sides)

Operate on UTF-8 **bytes**, not characters:

1. A byte `b` is *blank* if `b == 0x20 || b < 0x20 || b == 0x7F`. Bytes ≥ 0x80 are never blank.
2. Drop leading blanks. Replace each run of blanks between non-blank bytes with one `0x20`. Drop trailing blanks.
3. If longer than 120 bytes: set `cut = 120`; while the byte at `cut` is a continuation byte (`(b & 0xC0) == 0x80`) decrement `cut`; truncate to `cut`; drop trailing `0x20`.
4. An empty result is invalid text.

Test vectors (both codebases must pass all of them):

| Input | Output |
|---|---|
| `"  buy\n\tmilk \r\n and  eggs  "` | `"buy milk and eggs"` |
| `"a\x7Fb"` | `"a b"` |
| `" \n\t "` | `""` (invalid) |
| 500 × `x` | 120 × `x` |
| 119 × `a` + `é` + `tail` | 119 × `a` |
| 41 × `€` (3 bytes each) | 40 × `€` (120 bytes) |
| 119 × `a` + `" bbbb"` | 119 × `a` |

### C4. Revisions and merge (the phone merges; the device never merges)

- A *record* is a live item or a tombstone. Its *content* is `(isDeleted, text, done, order)`.
- Any local edit on either side: `rev += 1`.
- Deleting turns the item into a tombstone with `rev = item.rev + 1`.

`merge(D = device snapshot, P = phone state)`, per id in the union of both:

| Case | Result |
|---|---|
| Only D has the id | D's record |
| Only P has the id | P's record |
| `D.rev > P.rev` | D's record |
| `P.rev > D.rev` | P's record |
| Equal `rev`, equal content | D's record |
| Equal `rev`, different content | P's content with `rev = rev + 1` |

Then sort live records by (`order`, `id`). Keep at most 64 tombstones (most recently changed). If more than 100 live records remain, do **not** push. Show an error and ask the user to delete items.

Known, accepted edge: if the device evicted a tombstone (64+ newer deletions) and the phone still has that item live, the item comes back.

### C5. Device apply rules (HTTP POST and BLE PUT share this)

The request body is a C1 document plus `"baseRev": <u32>`.

1. Parse. `version` must be `1`, else **400**.
2. If `baseRev` ≠ device `rev`: **409**, response body = current document.
3. Validate everything before changing anything: ids valid and unique across both arrays, every live text non-empty after C3, ≤ 100 live, ≤ 64 tombstones. Any failure: **400**, nothing applied.
4. Every id currently live on the device must appear in `items` or `deleted`, else **400** `missing id`. This guards against a phone bug wiping the list.
5. If the incoming records equal the stored ones (same ids, same `rev`, same content after sanitizing): **200** with the stored document. No SD write, `rev` unchanged. This makes retries idempotent.
6. Otherwise replace the list. Records that are new or whose `rev`/content changed get `seq = ++rev`, in request order. Unchanged records keep their `seq`. Store sanitized text. Sort, cap tombstones, save to SD, **200** with the stored document.

The phone always adopts the document in a 200/409 response as the device's truth. For example, the device may have re-sanitized text.

### C6. HTTP (Wi-Fi)

Available only while the X4 shows its **File Transfer** screen.

| Request | Responses |
|---|---|
| `GET /api/todos` | 200 `application/json` document. A missing file returns `{"version":1,"rev":0,"items":[],"deleted":[]}`. |
| `POST /api/todos` (`Content-Type: application/json`, ≤ 32768 bytes) | 200 document · 400 `text/plain` reason · 409 document · 413 too large |

Hosts:
- **Hotspot mode:** `192.168.4.1`. The SSID is `CrossPoint-Reader`, an open network with no password.
- **Home Wi-Fi:** `crosspoint.local` or the IP shown on screen.

`GET /api/status` reports `"mode": "AP" | "STA"`.

### C7. BLE GATT

Advertising happens only while the X4 shows **To-do → Sync with phone**. It stops after 180 s or on leaving the screen.

| Thing | Value |
|---|---|
| Advertised name | `X4 To-do` (advertisement includes the service UUID) |
| Service | `8d4d5e22-336a-4fa2-bbdf-d7d1d6c90cf1` |
| Info (read, encrypted) | `648fb5e2-f2fd-43e7-8a21-f7a6be62e27c` → UTF-8 JSON `{"proto":1,"fw":"<CROSSPOINT_VERSION>","rev":N,"maxItems":100,"maxText":120,"maxBody":32768}` |
| Command (write with response, encrypted + MITM) | `11c415fe-5334-40c6-b168-48ee5b1a4036` (phone → X4) |
| Data (notify, encrypted + MITM) | `1a2cebb4-cbf0-45fc-9d53-f0564b7e87bc` (X4 → phone) |
| Security | LE Secure Connections, bonding, MITM. The X4 is DisplayOnly and shows a 6-digit passkey. iOS prompts automatically on first access. |

**Frame:** `[type: u8][seq: u16 little-endian][payload]`.
- The whole frame must be ≤ ATT_MTU − 3.
- The phone sizes writes with `maximumWriteValueLength(for: .withResponse)`.
- `seq` restarts at 0 for each message and increments by 1 per chunk.

| Type | Dir | Payload |
|---|---|---|
| `0x01 GET` | P→X | none |
| `0x02 CHUNK` | X→P | document bytes |
| `0x03 END` | X→P | `u32 totalLen`, `u32 crc32` (LE) |
| `0x10 PUT_BEGIN` | P→X | `u32 totalLen`, `u32 crc32` (LE) |
| `0x11 PUT_CHUNK` | P→X | request bytes (C5 body) |
| `0x12 PUT_COMMIT` | P→X | none |
| `0x20 OK` | X→P | `u32 newRev` (LE) |
| `0x7F ERR` | X→P | `u8 code` + UTF-8 message |

- **ERR codes:** 1 conflict, 2 invalid, 3 too large, 4 CRC mismatch, 5 sequence error, 6 busy/out of memory.
- **CRC-32:** IEEE / zlib (poly `0xEDB88320`, init `0xFFFFFFFF`, final XOR `0xFFFFFFFF`) over the JSON bytes.

Flows:
- **Read:** P sends `GET`. X replies `CHUNK`×n then `END`. P checks length and CRC.
- **Write:** P sends `PUT_BEGIN`, `PUT_CHUNK`×n, `PUT_COMMIT`. X checks length/CRC and applies C5.
  - On success X sends `OK(newRev)`, then streams the stored document (`CHUNK`/`END`).
  - On conflict X sends `ERR(1)`, then streams the current document.
  - On any other error X sends `ERR(code)`.
- One transfer at a time. A new `GET` or `PUT_BEGIN` aborts any partial transfer.

