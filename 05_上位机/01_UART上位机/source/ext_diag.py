"""Optional read-only extended diagnostics (EXT1/EXT2) exposed by the ESP firmware.

Both characteristics are *optional*: firmware that does not implement them is a
normal, supported case.  The PC polls them, logs every decoded field plus the raw
hex frame, and degrades to a single "EXT unavailable / EXT read fail" notice.
Nothing in this module may raise into the control path.

Byte layout contract (layout=1, little endian) -- agreed with the firmware task:

EXT1 = 6e400006-b5a3-f393-e0a9-e50e24dcca9e, 20 bytes, read only
  [0]      0xE1 marker
  [1]      0x01 layout
  [2..3]   u16 status_attempt      status notification attempts
  [4..5]   u16 status_fail         attempts with NimBLE rc != 0
  [6..7]   u16 status_enomem       attempts with rc == 6 (BLE_HS_ENOMEM)
  [8..9]   u16 status_mbuf_null    ble_hs_mbuf_from_flat() returned NULL
  [10..11] u16 pulse_ok            PULSE notifications sent successfully
  [12..13] u16 pulse_fail          PULSE notification failures
  [14..15] u16 pulse_enomem        PULSE failures with rc == 6
  [16]     u8  pulse_mbuf_null     saturated at 255
  [17]     u8  last_fail_kind      0 none / 1 status / 2 pulse / 3 diag
  [18]     u8  last_fail_rc        raw NimBLE rc of the most recent failure
  [19]     u8  last_fail_age_ds    saturated 0.1 s units since that failure
  status_ok and pulse_attempt are derived on the PC (attempt-fail / ok+fail).

EXT2 = 6e400007-b5a3-f393-e0a9-e50e24dcca9e, 20 bytes, read only
  [0]      0xE2 marker
  [1]      0x01 layout
  [2..5]   u32 ble_cmd_count      ATT writes that entered access_command and parsed
  [6..9]   u32 ble_cmd_last_seq   seq field of that command frame (data[4..7])
  [10..11] u16 ble_cmd_last_age_ms saturated at 65535
  [12..13] u16 uart_tx_ok
  [14..15] u16 uart_tx_fail
  [16..17] u16 att_lock_wait_max_ms  max time the ATT callback waited for the lock
  [18]     u8  att_lock_wait_last_ms saturated at 255
  [19]     u8  telemetry_gap_max_ms saturated at 255

EXT3 = 6e400008-b5a3-f393-e0a9-e50e24dcca9e, 20 bytes, read only
  [0]      0xE3 marker
  [1]      0x01 layout
  [2]      u8  conn_itvl_ms        negotiated connection interval (link budget)
  [3]      u8  conn_latency        peripheral latency in connection events
  [4..5]   u16 att_mtu             negotiated ATT MTU
  [6..7]   u16 msys_free_min       lowest free NimBLE mbuf/msys count seen
  [8..9]   u16 msys_free_now
  [10..11] u16 msys_total
  [12..13] u16 uart_rx_events      STM32 frames received from the UART
  [14..15] u16 diag_ok             diag notification success count
  [16..17] u16 diag_fail           diag notification failure count
  [18..19] u16 pulse_transfer_abort_count
  conn_itvl_ms/att_mtu are the key fields for deciding whether a ~200 packet/s
  PULSE stream can fit the observed air interface at all.

If a live device answers with an unknown marker/layout the PC logs the raw frame
once and keeps counting failures, so a firmware/PC mismatch cannot be mistaken
for a silent link problem.
"""

EXT1 = '6e400006-b5a3-f393-e0a9-e50e24dcca9e'
EXT2 = '6e400007-b5a3-f393-e0a9-e50e24dcca9e'
EXT3 = '6e400008-b5a3-f393-e0a9-e50e24dcca9e'
EXT1_MARKER = 0xE1
EXT2_MARKER = 0xE2
EXT3_MARKER = 0xE3
EXT_LAYOUT = 1
EXT_LENGTH = 20
EXT_UUIDS = (('ext1', EXT1), ('ext2', EXT2), ('ext3', EXT3))
EXT_PARSERS = {}


def _frame(data, marker, name):
    b = bytes(data)
    if len(b) != EXT_LENGTH:
        raise ValueError('%s 长度 %d != %d' % (name, len(b), EXT_LENGTH))
    if b[0] != marker:
        raise ValueError('%s 标记 0x%02X != 0x%02X' % (name, b[0], marker))
    if b[1] != EXT_LAYOUT:
        raise ValueError('%s layout %d != %d' % (name, b[1], EXT_LAYOUT))
    return b


def _u16(b, offset):
    return b[offset] | (b[offset + 1] << 8)


def _u32(b, offset):
    return _u16(b, offset) | (_u16(b, offset + 2) << 16)


def parse_ext1(data):
    """Decode an EXT1 frame; raises ValueError for unknown layout or length."""
    b = _frame(data, EXT1_MARKER, 'EXT1')
    status_attempt, status_fail = _u16(b, 2), _u16(b, 4)
    pulse_ok, pulse_fail = _u16(b, 10), _u16(b, 12)
    return dict(
        layout=b[1],
        status_attempt=status_attempt,
        status_fail=status_fail,
        status_enomem=_u16(b, 6),
        status_mbuf_null=_u16(b, 8),
        status_ok=max(0, status_attempt - status_fail),
        pulse_ok=pulse_ok,
        pulse_fail=pulse_fail,
        pulse_enomem=_u16(b, 14),
        pulse_mbuf_null=b[16],
        pulse_attempt=pulse_ok + pulse_fail,
        last_fail_kind=b[17],
        last_fail_rc=b[18],
        last_fail_age_ms=b[19] * 100,
    )


def parse_ext2(data):
    """Decode an EXT2 frame; raises ValueError for unknown layout or length."""
    b = _frame(data, EXT2_MARKER, 'EXT2')
    return dict(
        layout=b[1],
        ble_cmd_count=_u32(b, 2),
        ble_cmd_last_seq=_u32(b, 6),
        ble_cmd_last_age_ms=_u16(b, 10),
        uart_tx_ok=_u16(b, 12),
        uart_tx_fail=_u16(b, 14),
        att_lock_wait_max_ms=_u16(b, 16),
        att_lock_wait_last_ms=b[18],
        telemetry_gap_max_ms=b[19],
    )


def parse_ext3(data):
    """Decode an EXT3 frame; raises ValueError for unknown layout or length."""
    b = _frame(data, EXT3_MARKER, 'EXT3')
    return dict(
        layout=b[1],
        conn_itvl_ms=b[2],
        conn_latency=b[3],
        att_mtu=_u16(b, 4),
        msys_free_min=_u16(b, 6),
        msys_free_now=_u16(b, 8),
        msys_total=_u16(b, 10),
        uart_rx_events=_u16(b, 12),
        diag_ok=_u16(b, 14),
        diag_fail=_u16(b, 16),
        pulse_transfer_abort_count=_u16(b, 18),
    )


EXT_PARSERS = {'ext1': parse_ext1, 'ext2': parse_ext2, 'ext3': parse_ext3}

# Cumulative counters whose per-second rate is the deliverable (width in bits).
EXT_COUNTERS = {
    'ext1': {'status_attempt': 16, 'status_fail': 16, 'status_enomem': 16,
             'status_mbuf_null': 16, 'status_ok': 16, 'pulse_ok': 16,
             'pulse_fail': 16, 'pulse_enomem': 16, 'pulse_attempt': 16},
    'ext2': {'ble_cmd_count': 32, 'uart_tx_ok': 16, 'uart_tx_fail': 16},
    'ext3': {'uart_rx_events': 16, 'diag_ok': 16, 'diag_fail': 16,
             'pulse_transfer_abort_count': 16},
}


def derive_rates(name, previous, current, elapsed_s):
    """Per-second rate of each cumulative counter from two consecutive samples.

    Wrapping counters are unwrapped with their width; a reordered/rebooted
    counter (delta far above a sane rate) is reported as 0 instead of a spike.
    """
    if previous is None or elapsed_s <= 0:
        return {}
    rates = {}
    for key, width in EXT_COUNTERS.get(name, {}).items():
        if key not in previous or key not in current:
            continue
        mask = (1 << width) - 1
        delta = (current[key] - previous[key]) & mask
        if delta > mask // 2:      # counter went backwards: reboot / reorder
            delta = 0
        rates[key + '_per_s'] = round(delta / elapsed_s, 2)
    return rates

