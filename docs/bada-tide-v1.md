# bada-tide v1

An optional link between a **harness** and a **keyboard**: the keyboard tells the harness about its
language keys directly, and can leave them out of what it sends the OS, so a key is read once, by the
harness, with no OS permission. A small **translator** file describes each keyboard's wire protocol;
the harness interprets it. The engine is not involved: it still gets only `key`, `action` and
`select` events (the standard, §2, §9).

bada-tide is a part of [bada-compatible v2](bada-compatible-v2.md) that a harness may implement. A
harness that does says so in its state (`harness.tide`, §8 below). The words **must**, **should** and
**may** are used as in RFC 2119.

- [1. Rules](#1-rules)
- [2. Messages](#2-messages)
- [3. The lease](#3-the-lease)
- [4. Take](#4-take)
- [5. Conflicts](#5-conflicts)
- [6. Translators](#6-translators)
- [7. Safety](#7-safety)
- [8. The harness's state](#8-the-harnesss-state)
- [9. Where translators live](#9-where-translators-live)

## 1. Rules

bada-tide is **stateless**. A crash, a quit, an unplug or a power cycle on either side leaves nothing
to undo, and the harness recognises the keyboard again on reconnect.

1. **The keyboard stores nothing tide sends.** No flash or EEPROM write, no pairing, no remembered
   harness. Tide state is in RAM and is gone after an unplug, a power cycle or a reset.
2. **A lease, not a switch.** `hello` starts a lease of a few seconds; every `state` renews it. When
   the harness stops (crash, quit, unplug), the lease runs out and the keyboard behaves as if tide had
   never been used. There is no message that turns tide off.
3. **Complete, idempotent messages.** `state` carries the harness's whole state. Sending a message
   twice, or after a reconnect, gives the same result. No sequence number or session id survives
   anything.
4. **Recognition never depends on tide.** A harness finds a keyboard by its USB descriptors (§6) and
   sends `hello` after every connect. `hello` is answered the same way from any state.
5. **The harness keeps no device state.** What it knows about a connected keyboard comes from its
   `hello` and is published in `harness.tide`, never saved.
6. **Tide reaches nothing else.** A keyboard's firmware update, keymap and settings stay on their own
   paths. A harness sends only `hello` and `state` (§7).

## 2. Messages

| Message | Direction | Content |
|---|---|---|
| `hello` | harness → keyboard | tide version; starts or renews the lease |
| `hello` reply | keyboard → harness | status, tide version, features, lease length |
| `state` | harness → keyboard | `mode`, `profile`, `caps`, `take`: the harness's full state; renews the lease |
| `state` reply | keyboard → harness | status, the state as stored |
| `key` | keyboard → harness, unasked | a language key or Caps Lock went down or up |

- **Versions.** `hello` carries the harness's tide major and minor. A keyboard with another major
  refuses it (`unsupported`) and its reply still carries its own version.
- **Features**, in the `hello` reply, are what the keyboard can do: `keys` (it sends `key` events)
  and `take` (§4). Later minors may add others; a harness ignores the ones it doesn't know.
- **Status** of a reply: `ok`; `unsupported` (another major, a request the keyboard doesn't know);
  `no-lease` (`state` with no lease: send `hello`); `bad-value`; `blocked` (§5: another program used
  the keyboard's channel a moment ago; try again later).
- **`state`**: `mode` is `muhenkan` or `henkan`; `profile` is the index of the profile in the key
  table's `cycle`, or none; `caps` is whether Caps Lock is on; `take` is §4. A keyboard may show these
  (a later feature); v1 keyboards store them and report them back.
- **`key`**: the key's code (the standard, §4: `Backquote`, `NonConvert`, `Convert`, `KanaMode`,
  `Lang2`, `Lang1`, `CapsLock`), `down` or `up`, the modifiers held at that moment, a counter that
  starts at 0 after each `hello` (a gap means events were lost), the keyboard's clock in
  milliseconds, and a flag that events were dropped before this one. The key is what the keyboard
  sent after its own keymap, so a remapped key reports what it types.

## 3. The lease

- The `hello` reply names the lease length. A harness **must** send `state` at least every third of
  it (every second for a 3 s lease), and at once when its mode, profile, Caps Lock or take changes.
- `state` with no lease is refused with `no-lease`; the harness sends `hello` again.
- When the lease ends, the keyboard forgets the harness's state, takes nothing (§4) and drops the
  events it hadn't sent.
- A harness that stops renewing for any reason (a conflict, §5; the user turned tide off) does
  nothing else: the lease runs out on its own.
- **A link that hears nothing takes nothing.** Every renewal is answered. A harness that has heard
  nothing from a keyboard for two renewals stops renewing, so the lease runs out and the keys go back
  to the OS; it starts again with `hello`. Otherwise a link that can send but not receive would take
  the user's keys and lose them.
- **Retries.** A keyboard that answers no `hello` (another firmware with the same ids) is tried a few
  times, then left alone for a while.
- **Reconnect.** When a keyboard disappears, the harness forgets it. When it comes back, it is a new
  keyboard: match (§6), conflicts (§5), `hello`.

## 4. Take

`take` names the language keys the harness takes while its lease holds. The keyboard leaves a taken
key out of what it sends the OS, and reports it only as `key` events, so the key is read once, by the
harness, and macOS needs no Input Monitoring for it. When the lease ends, nothing is taken and the
keys go back to the OS.

- A harness takes only the keys it handles: those its key table binds (§9). A key it doesn't take
  still sends `key` events, which the harness ignores (the OS has the key too).
- `take` is part of every `state`, so a harness restates it every renewal.
- A key held when a lease begins or ends reaches the OS (or stops reaching it) with the keyboard's
  next report.

## 5. Conflicts

Some programs use the same channel as tide. A translator lists them as **conflicts**, and a harness
**must not** hold a lease while one runs.

- Before every `hello`, the harness checks the running programs against the translator's conflicts.
- While it holds a lease, it checks again whenever the platform says a program started, and at least
  every 5 s otherwise.
- If one matches, it sends no `hello`, or stops renewing (the lease runs out, §3). It publishes the
  conflict's id in `harness.tide` and tries again once that program has quit.
- A check only reads the list of running programs. It never touches the other program.

A keyboard may also have its own backstop: on another program's request on the channel, it ends the
lease and answers `hello` with `blocked` for a while. A harness that gets `blocked` waits and tries
again (§6: `retry_ms`).

## 6. Translators

A translator is a JSON file, `bada-tide-translator/1` ([schema](schemas/tide-translator.schema.json)),
that describes one keyboard's tide protocol. It is data: a harness interprets it and runs no code from
it.

```json
{"format": "bada-tide-translator/1", "id": "ikarosu", "name": "ikarosu (HHKB Professional HYBRID Type-S, JIS)",
 "match": {"vendor_id": 1278, "product_id": 34, "usage_page": 65280, "usage": 1, "report_size": 64},
 "version": {"major": 1, "minor": 0},
 "hello": {"send": "AA AA F1 01 02 {major} {minor}",
           "reply": "55 55 F1 {status} 01 06 {major} {minor} {features} {lease_ms:le16}"},
 "state": {"send": "AA AA F1 02 04 {mode} {profile} {caps} {take}",
           "reply": "55 55 F1 {status} 02 06"},
 "key": {"recv": "55 55 F1 00 10 09 {usage} {edge} {mods} {seq} {time_ms:le32} {flags}"},
 "values": {"status": {"ok": 0, "unsupported": 1, "no-lease": 2, "bad-value": 3, "blocked": 4},
            "mode": {"muhenkan": 0, "henkan": 1}, "profile_none": 255,
            "features": {"keys": 1, "take": 16},
            "usage": {"Backquote": 53, "NonConvert": 139, "Convert": 138, "KanaMode": 136,
                      "Lang2": 145, "Lang1": 144, "CapsLock": 57},
            "take": {"Backquote": 1, "NonConvert": 2, "Convert": 4, "KanaMode": 8,
                     "Lang2": 16, "Lang1": 32, "CapsLock": 64},
            "edge": {"up": 0, "down": 1}, "mods": "hid", "flags": {"dropped": 1}},
 "retry_ms": 10000,
 "conflicts": [{"id": "pfu-hhkb-keymap-tool", "name": "HHKB Keymap Tool",
                "reason": "uses the same vendor channel",
                "match": {"macos": {"bundle_id": ["jp.co.pfu.hhkb-keymap-tool"]},
                          "windows": {"process": ["HHKBKeymapTool.exe"]}}}]}
```

- **`match`**: the HID interface that carries tide: vendor and product id, usage page and usage, and
  the report size in bytes. Optional `version_min`/`version_max` compare with the device's release
  number (`bcdDevice`). A keyboard matches when every given field does.
- **Templates** (`send`, `reply`, `recv`) are the report's bytes, space-separated: a hex byte (`AA`),
  or a field in braces (`{mode}`). A field is one byte unless it says `:le16`, `:le32` (little-endian)
  or `:be16`, `:be32`. A report is padded with zeros to `report_size`. In `send`, a field is filled
  from the message; in `reply` and `recv`, a hex byte must match and a field is read. Fields a
  template doesn't name are absent.
- **`values`** maps the messages' words to the keyboard's numbers. `mods: "hid"` means the HID
  modifier byte (bit 0 left Control … bit 7 right GUI).
- A reply is the next report that matches the `reply` template; a report that matches `recv` is a
  `key` event, wherever it comes; anything else is ignored.
- A harness **must** open the interface **non-exclusively**, and never block on it: a send is
  asynchronous, with a short timeout, and a renewal that can't go now goes at the next one. (Some HID
  libraries open exclusively by default, hidapi on macOS among them: a tool that does takes the
  channel from the harness, whose lease then runs out.)
- Where the platform delivers a keyboard's input reports unreliably to the input method's own
  process, or where the input method runs in many processes, the harness holds the link in a helper
  process. bada's macOS harness does: an input method is an NSApplication, and there macOS mostly
  doesn't deliver them (its README). So does bada's Windows harness: its text service runs in every
  app, and a keyboard has one link (its README, "bada-tide").
- **`retry_ms`**: how long to wait after `blocked` before the next `hello` (default 10000).
- **`conflicts`**: §5. Each has an `id`, a `name`, a `reason`, and per platform what identifies it:
  `macos.bundle_id`, `windows.process` (executable file names), `linux.process`.

## 7. Safety

- A harness sends a keyboard only `hello` and `state`, built from the translator's two `send`
  templates. A translator can't add a message.
- A harness **must** refuse a translator whose `send` template begins like a known dangerous
  request: PFU's firmware update and settings writes (`AA AA E0`–`E7`, `AA AA 86`, `AA AA 04`,
  `AA AA 07`, `AA AA 08`, `AA AA D0`) and VIA's keymap and bootloader requests (`05`, `07`, `0B`,
  `0C`, `12`, `13` as the first byte of a 32-byte report). The list is the standard's; a later
  version may add to it.
- Tide is **off** unless the user turns it on: a translator is used only when it is in the settings
  folder (§9).

## 8. The harness's state

A harness that implements bada-tide adds `tide` to `harness` in the published state (§6.7 of the
standard), a list of the keyboards it sees:

```json
"tide": [{"translator": "ikarosu", "state": "held", "version": "1.0", "features": ["keys", "take"],
          "take": ["Lang1", "Lang2", "KanaMode"], "conflict": null}]
```

`state` is one of `connecting` (`hello` sent), `held` (a lease is held), `conflict` (`conflict` names
it), `blocked` (the keyboard answered `blocked`), `unsupported` (another major) or `error`. An empty
list: no keyboard matches. Like the rest of the state, it is read-only and never saved.

## 9. Where translators live

- **In the settings folder**, `tide/<id>.json`. A translator there is on. Removing it turns it off.
- **The keyboard's project** publishes its translator: ikarosu's is in its repository. bada's
  repository holds translators for keyboards it supports itself, in `tide/`, each pinned to a firmware
  version.
- A translator that doesn't validate is not used, and the harness lists it in `harness.problems`
  (file `tide/<id>.json`).
