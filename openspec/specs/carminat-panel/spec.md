# carminat-panel Specification

## Purpose

The Carminat / AFFA3 family: the colour two-row window with a menu, popups, a fullscreen
screen and a confirm box. Sync on `0x3AF` / `0x3CF`, data on `0x151` and `0x1F1`, keys
inbound on `0x1C1`.

The panel class supplies four things and nothing more: its `SyncProfile` and function table,
its filler and key transmit id, the frame builders, and the menu seams.

## Requirements

### Requirement: The Panel Class Contains No Application Coupling

A panel SHALL NOT reference NVS, media state, phone integrations, diagnostics, voltage
helpers, or any car-specific sequence.

Those are application policy: one car, one radio, one phone.

#### Scenario: Password sequences are application code

- **WHEN** an application needs to impersonate a panel to drive a real radio
- **THEN** it builds that from public API — `subscribe()` plus `pressKey(..., KeySource::Wire)`
- **AND** produces identical bytes without any library change

### Requirement: Capability Is Answerable Before The Call

`supports(Feature)` SHALL answer for each panel, and an unsupported render SHALL return
`NotSupported`.

The legacy interface gave unsupported operations silent no-op bodies returning success, so
calling one looked exactly like it worked.

#### Scenario: An unsupported feature is not silently ignored

- **WHEN** a render is invoked on a family that does not implement it
- **THEN** the call returns `NotSupported` rather than `Ok`

### Requirement: Text Renders On 0x151 With The Windowed Command

`setText()` SHALL build a first frame declaring content length 14, command `0x77`, and
SHALL NUL-pad or truncate the payload to the panel's cell count.

`0x74` is the full-window overlay. Sending `0x77` while no window is applied has been
observed to leave the main screen frozen.

#### Scenario: Text is truncated, not overflowed

- **WHEN** text longer than the cell count is supplied
- **THEN** it is truncated to the cell count and the remaining cells are NUL

#### Scenario: The digit argument is ignored here

- **WHEN** `setText(text, digit)` is called on Carminat
- **THEN** `digit` has no effect; it exists for the UpdateList signature

### Requirement: The Clock Takes Exactly Four ASCII Digits

`setTime()` SHALL accept `"HHMM"` and SHALL return `BadArgument` for anything shorter.

The legacy builder indexed four characters unconditionally and read past a short string.

#### Scenario: Ten o'clock

- **WHEN** `setTime("1000")` is called
- **THEN** the payload is `05 56 '1' '0' '0' '0' 00 00` on `0x151` in `RenderSlot::Clock`

#### Scenario: A short string is rejected

- **WHEN** `setTime("100")` is called
- **THEN** it returns `BadArgument` and nothing is enqueued

### Requirement: Power Control Is Its Own Slot

`setPower()` SHALL build `03 52 <09|00> FF FF` on the display-control id in
`RenderSlot::Control`.

This is deliberately NOT unified with UpdateList: Carminat declares `0x03` for four bytes
after the PCI, UpdateList declares `0x04` for the same shape. Only UpdateList's is
self-consistent, and both have been accepted by their panels for months.

#### Scenario: Text is invisible while the display is off

- **WHEN** `setText()` succeeds and reaches the panel while power is off
- **THEN** the render is delivered and nothing is visible
- **AND** this is not a defect in the text command; the display must be powered first

### Requirement: Menu Geometry Is The Panel's, Not The Widget's

The menu renderer SHALL inject its geometry so the shared widget truncates rows at the
panel's real width.

#### Scenario: Carminat rows

- **WHEN** the Carminat renderer reports its geometry
- **THEN** it is 2 rows of 26 characters
- **AND** rows are truncated at 26 rather than at a widget-internal maximum

### Requirement: The Menu Widget Is Opt-In And Not Protocol

The menu SHALL be compiled only when `AFFA_ENABLE_MENU` is set, and the model SHALL live in
`src/widget/` behind a renderer interface.

The state machine is display-agnostic; only the adapter is panel-specific.

#### Scenario: A build without the menu still renders everything else

- **WHEN** the library is built with `AFFA_ENABLE_MENU=0`
- **THEN** every render primitive still works
- **AND** navigation returns `NotSupported` and the menu seams are inert

### Requirement: Fullscreen Replaces, Popup Overlays

The fullscreen primitive SHALL need no teardown, and the popup SHALL require an explicit
hide.

Measured on a real Carminat 2026-07-28: any other full-screen render REPLACES a fullscreen
render, while a popup survives a redraw underneath it.

#### Scenario: Popup survives a redraw beneath it

- **WHEN** a popup is shown and the base screen is redrawn
- **THEN** the popup remains until `hidePopup()`

### Requirement: A Golden Vector Must Account For Self-Ack

A test pinning frame counts SHALL state whether it ran against hardware or against
`setSelfAck`.

A real panel terminates at the declared length, so `showMenu` is 13 frames on hardware and
14 under self-ack. The declared length `0x5A` = 90 with 94 bytes built is correct: the panel
stops as soon as it holds the declared count.

#### Scenario: Frame count differs by ack source

- **WHEN** the same `showMenu` payload is transmitted to hardware and under self-ack
- **THEN** the counts are 13 and 14 respectively
- **AND** neither is a defect

### Requirement: Key Frames Are Guarded Before Decoding

Inbound key decoding SHALL require the `03 89` prefix before interpreting the key bytes.

The key id also carries `70 A3..`, `02 64 0F A3..` and `05 63 "0037"`, out of which a
decoder without the guard invents keys `0x640F` and `0x3030`.

#### Scenario: Non-key traffic on the key id is ignored

- **WHEN** `1C1 70 A3 A3 A3 A3 A3 A3 A3` arrives
- **THEN** no key event is produced

#### Scenario: Wheel codes are exempt from the hold mask

- **WHEN** a wheel-down code `0x0141` is decoded
- **THEN** the hold mask is not applied
- **AND** it is not rewritten to `0x0101`, which would report every wheel-down as a wheel-up

### Requirement: Emulated Keys Default To Local

`pressKey()` and `nav()` SHALL default to `KeySource::Local`.

In the radio role key frames only ever come IN: the joystick is wired to the PANEL, which
encodes and transmits; the radio receives. Transmitting a key frame is meaningful only when
impersonating the panel at a REAL radio.

#### Scenario: Local emulation puts nothing on the bus

- **WHEN** `pressKey(Key::Load, KeyEdge::Click)` is called with the default source
- **THEN** the key takes the identical path to a key off the wire
- **AND** no frame is transmitted

#### Scenario: Wire source is refused for an ambiguous hold

- **WHEN** `pressKey()` is asked to transmit a Hold edge on a wheel code
- **THEN** it returns `NotSupported`
- **AND** the reason is that `0x0101|0xC0` and `0x0141|0xC0` are both `0x01C1`, so the
  click form would step fine where the caller asked for coarse
