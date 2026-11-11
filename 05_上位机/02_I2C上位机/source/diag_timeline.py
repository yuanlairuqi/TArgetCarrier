"""Bounded, opt-out diagnostic timeline for the X-axis BLE link (evidence only).

Design constraints (task 03): logging must never change control flow, never grow
without bound and never become a new source of congestion.

  * ``TC_DIAG_TIMELINE=0`` turns the whole timeline into a no-op.
  * Every public method swallows its own exceptions (a diagnostic bug can only
    lose a line, never break the control loop).
  * Only two bounded structures are kept: a ring of the last MAX_EVENTS events
    (in memory, for export) and small fixed counters/deques.
  * Lines are rate limited to MAX_LINES_PER_SEC; the per-second summary is
    emitted at most once per 0.9 s.  The dedicated handler in unified.py rotates
    at 2 MiB x 5, so a wedged environment cannot fill the disk.

What is measured (all timings monotonic):
  * one line per GATT write: submit (seq/op), done (ok + ms), fail (kind + ms),
    deadline (write_deadline hit) and quarantine;
  * notification counters per type (status / pulse / diag) with 1 Hz rates and
    PULSE bytes/s + peak packets per 100 ms -- the direct PC-side evidence for a
    PULSE burst;
  * full-field status line 1 Hz (every parsed Status field, including version,
    uptime, battery, mode, speed, current/target Hz);
  * full-field ESP diagnostic line at most 1 Hz;
  * ACK stall warnings: while motion is commanded, when stm_ack has not advanced
    for >= 300 ms and then >= 500 ms (once per threshold per stall episode);
  * EXT1/EXT2 samples (cumulative ESP counters) with raw hex always logged.

Log lines go to the ``targetcarrier.diag`` logger; connection.log is untouched.
"""

import collections
import logging
import os
import time

from ext_diag import EXT_UUIDS, EXT_PARSERS, derive_rates

MAX_EVENTS = 4096          # in-memory ring, bounded
MAX_LINES_PER_SEC = 20     # diagnostic file write budget
WRITE_MS_KEEP = 600        # bounded write-duration history for avg/max
ACK_STALL_MS = (300, 500)  # lead-requested thresholds (warn once each)
STATUS_MAX_GAP = 0.9       # seconds between 1 Hz full-field status lines
DIAG_MAX_GAP = 4.9         # ESP snapshot lines are additionally change-gated

FAIL_KINDS = {0: 'none', 1: 'status', 2: 'pulse', 3: 'diag'}


def _switch(name, default=True):
    """Environment switches are read once per Timeline; anything unexpected = on."""
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() not in ('0', 'off', 'false', 'no', 'disabled')


class Timeline:
    def __init__(self, logger=None, clock=time.monotonic, enabled=None, ext_enabled=None):
        self.log = logger or logging.getLogger('targetcarrier.diag')
        self.clock = clock
        self.enabled = _switch('TC_DIAG_TIMELINE', True) if enabled is None else bool(enabled)
        self.ext_enabled = _switch('TC_DIAG_EXT', True) if ext_enabled is None else bool(ext_enabled)
        self.events = collections.deque(maxlen=MAX_EVENTS)
        self.counts = collections.Counter()
        self.session = collections.Counter()
        self.notif = collections.Counter()
        self.notif_bytes = collections.Counter()
        self.write_ms = collections.deque(maxlen=WRITE_MS_KEEP)
        self.pulse_buckets = collections.deque(maxlen=10)
        self._pulse_bucket = None
        self._pulse_bucket_count = 0
        self.suppressed = 0
        self.ext_last = {}
        self.ext_seen = {}
        self._fail_logged = set()
        self._line_window = 0.0
        self._lines = 0
        self.last_status_line = -1e9
        self.last_diag_line = -1e9
        self.last_pulse_line = -1e9
        self.ack_mark = None
        self.ack_at = 0.0
        self.ack_stall_ms = 0
        self.ack_warned = set()
        self.started = self.clock()
        self.last_second = self.started
        self._prev_notif = collections.Counter()
        self._prev_at = self.started

    # ------------------------------------------------------------------ helpers
    def reset_session(self, now=None):
        """Start a new BLE session: per-session counters restart, EXT baselines stay."""
        try:
            now = self.clock() if now is None else now
            self.session.clear()
            self.notif.clear()
            self.notif_bytes.clear()
            self.write_ms.clear()
            self.pulse_buckets.clear()
            self._pulse_bucket = None
            self._pulse_bucket_count = 0
            self.ack_mark = None
            self.ack_at = now
            self.ack_stall_ms = 0
            self.ack_warned.clear()
            self.last_second = now
            self._prev_notif = collections.Counter()
            self._prev_at = now
        except Exception:
            pass

    def _now(self, now=None):
        return self.clock() if now is None else now

    def _record(self, label, now, **fields):
        try:
            self.events.append((now, label, fields))
        except Exception:
            pass

    def _emit(self, message):
        """One rate-limited INFO line; never raises, never blocks."""
        try:
            if not self.enabled:
                return
            now = self.clock()
            if now - self._line_window >= 1.0:
                self._line_window = now
                self._lines = 0
            if self._lines >= MAX_LINES_PER_SEC:
                self.suppressed += 1
                return
            self._lines += 1
            self.log.info(message)
        except Exception:
            pass

    def _warn_once(self, key, message):
        try:
            if key in self._fail_logged:
                return
            self._fail_logged.add(key)
            self.log.warning(message)
        except Exception:
            pass

    # ------------------------------------------------------------ write lifecycle
    def write_submit(self, seq, motion, stop_seq, now=None):
        try:
            if not self.enabled:
                return
            now = self._now(now)
            self.counts['write_submit'] += 1
            self.session['write_submit'] += 1
            self._record('submit', now, seq=seq, motion=motion, stop_seq=stop_seq)
            self._emit('X TX submit seq=%s op=%s stop_seq=%s' % (seq, motion, stop_seq))
        except Exception:
            pass

    def write_done(self, seq, elapsed_ms, ok=True, kind=None, motion=None, now=None):
        try:
            if not self.enabled:
                return
            now = self._now(now)
            elapsed_ms = int(elapsed_ms)
            self.counts['write_done' if ok else 'write_fail'] += 1
            self.session['write_done' if ok else 'write_fail'] += 1
            self.write_ms.append(elapsed_ms)
            self._record('done' if ok else 'fail', now, seq=seq, ms=elapsed_ms, kind=kind)
            if ok:
                self._emit('X TX done seq=%s ok=1 ms=%s' % (seq, elapsed_ms))
            else:
                self._emit('X TX fail seq=%s ok=0 kind=%s elapsed_ms=%s' % (seq, kind, elapsed_ms))
        except Exception:
            pass

    def write_deadline(self, seq, elapsed_ms, deadline_ms, now=None):
        try:
            if not self.enabled:
                return
            now = self._now(now)
            self.counts['write_deadline'] += 1
            self.session['write_deadline'] += 1
            self._record('deadline', now, seq=seq, ms=int(elapsed_ms), deadline_ms=int(deadline_ms))
            self._emit('X TX deadline seq=%s elapsed_ms=%s deadline_ms=%s quarantined=1'
                       % (seq, int(elapsed_ms), int(deadline_ms)))
        except Exception:
            pass

    def write_native(self, seq, state, now=None):
        """Record the settled state of a quarantined native write (done/cancelled/error)."""
        try:
            if not self.enabled:
                return
            now = self._now(now)
            self._record('native', now, seq=seq, state=state)
            self._emit('X TX native seq=%s state=%s' % (seq, state))
        except Exception:
            pass

    def quarantine(self, reason, now=None):
        try:
            if not self.enabled:
                return
            now = self._now(now)
            self.counts['quarantine'] += 1
            self.session['quarantine'] += 1
            self._record('quarantine', now, reason=reason)
            self._emit('QUARANTINE reason=%s old_session_fenced=1' % reason)
        except Exception:
            pass

    def stop_queued(self, stop_seq, force, blocked_by_write=False, now=None):
        try:
            if not self.enabled:
                return
            now = self._now(now)
            self.counts['stop_queued'] += 1
            self.session['stop_queued'] += 1
            self._record('stop', now, seq=stop_seq, force=bool(force), blocked=bool(blocked_by_write))
            self._emit('STOP queued seq=%s force=%s blocked_by_write=%s'
                       % (stop_seq, bool(force), bool(blocked_by_write)))
        except Exception:
            pass

    # ------------------------------------------------------------- notifications
    def notify(self, kind, nbytes=0, now=None):
        try:
            if not self.enabled:
                return
            now = self._now(now)
            self.notif[kind] += 1
            self.notif_bytes[kind] += int(nbytes)
            if kind == 'pulse':
                bucket = int(now * 10)
                if bucket != self._pulse_bucket:
                    if self._pulse_bucket is not None:
                        self.pulse_buckets.append(self._pulse_bucket_count)
                    self._pulse_bucket = bucket
                    self._pulse_bucket_count = 0
                self._pulse_bucket_count += 1
        except Exception:
            pass

    def _notification_rates(self, now):
        elapsed = max(1e-6, now - self._prev_at)
        rates = {}
        for kind in ('status', 'pulse', 'diag', 'other'):
            delta = self.notif[kind] - self._prev_notif.get(kind, 0)
            rates[kind] = delta / elapsed
        bytes_delta = self.notif_bytes['pulse'] - self._prev_notif.get('pulse_bytes', 0)
        rates['pulse_bytes'] = bytes_delta / elapsed
        self._prev_notif = collections.Counter(self.notif)
        self._prev_notif['pulse_bytes'] = self.notif_bytes['pulse']
        self._prev_at = now
        return rates

    # ------------------------------------------------------------------ status
    def status_ack(self, now, status):
        """Track stm_ack progress and warn once at each stall threshold."""
        try:
            if not self.enabled:
                return
            mark = (status.ack, status.stm_ack)
            if self.ack_mark is None or mark != self.ack_mark:
                self.ack_mark = mark
                self.ack_at = now
                self.ack_stall_ms = 0
                self.ack_warned.clear()
                return
            self.ack_stall_ms = int(max(0.0, now - self.ack_at) * 1000)
            moving = bool(status.motion or status.requested)
            if not moving:
                return
            for threshold in ACK_STALL_MS:
                if self.ack_stall_ms >= threshold and threshold not in self.ack_warned:
                    self.ack_warned.add(threshold)
                    self._record('ack_stall', now, ack=status.ack, stm_ack=status.stm_ack,
                                 motion=status.motion, stall_ms=self.ack_stall_ms, threshold=threshold)
                    self._emit('ACK stall ack=%s stm_ack=%s motion=%s requested=%s stall_ms=%s threshold=%s'
                               % (status.ack, status.stm_ack, status.motion, status.requested,
                                  self.ack_stall_ms, threshold))
        except Exception:
            pass

    def full_status(self, now, status, extra=None):
        """1 Hz full-field status line (all Status fields plus channel context)."""
        try:
            if not self.enabled or status is None:
                return
            if now - self.last_status_line < STATUS_MAX_GAP:
                return
            self.last_status_line = now
            fields = dict(version=status.version, motion=status.motion, requested=status.requested,
                          speed=status.speed, mode=status.mode, phase=status.phase,
                          flags=status.flags, error=status.error, ack=status.ack,
                          stm_ack=status.stm_ack, current_hz=status.current_hz,
                          target_hz=status.target_hz, uptime=status.uptime,
                          battery=status.battery, uart=int(status.uart),
                          online=int(status.online), demo=int(status.demo),
                          can_move=int(status.can_move), stop_feedback=int(status.stop_feedback))
            if extra:
                fields.update(extra)
            self._record('status_full', now, **fields)
            self._emit('STATUS ' + ' '.join('%s=%s' % (k, v) for k, v in fields.items()))
        except Exception:
            pass

    def full_diag(self, now, detail, raw=None):
        """Full ESP first-fault snapshot, at most one line per second."""
        try:
            if not self.enabled or not detail:
                return
            key = tuple(sorted((k, str(v)) for k, v in detail.items()))
            if now - self.last_diag_line < 1.0 and key == self.ext_last.get('diag_key'):
                return
            self.last_diag_line = now
            self.ext_last['diag_key'] = key
            self._record('diag_full', now, **detail)
            self._emit('ESPDIAG ' + ' '.join('%s=%s' % (k, v) for k, v in detail.items()))
        except Exception:
            pass

    def pulse_capture(self, now, capture):
        """One bounded line per complete PULSE capture (rate limited)."""
        try:
            if not self.enabled or not capture:
                return
            if now - self.last_pulse_line < 0.9:
                return
            self.last_pulse_line = now
            self._record('pulse_capture', now, id=capture.get('id'), edges=len(capture.get('edges', ())))
            self._emit('PULSE capture id=%s flags=%s hz=%s duration=%s pul=%s direction=%s edges=%s'
                       % (capture.get('id'), capture.get('flags'), capture.get('hz'),
                          capture.get('duration'), capture.get('pul'), capture.get('direction'),
                          len(capture.get('edges', ()))))
        except Exception:
            pass

    # ----------------------------------------------------------------- EXT reads
    def ext_unavailable(self, names=None):
        try:
            names = list(names) if names else [name for name, _uuid in EXT_UUIDS]
            self._warn_once('ext_unavailable',
                            'EXT diagnostics unavailable/missing %s (degraded; old firmware is expected to lack them)'
                            % ', '.join(names))
        except Exception:
            pass

    def ext_read(self, name, data, error, now=None):
        """Record one EXT read: decoded fields, derived per-second rates, raw hex."""
        try:
            if not self.enabled or not self.ext_enabled:
                return
            now = self._now(now)
            if error is not None:
                self.counts['ext_fail'] += 1
                self._record('ext_fail', now, name=name, kind=error)
                self._warn_once(('ext_fail', name), 'EXT read fail name=%s kind=%s (degraded, control path untouched)'
                                % (name, error))
                return
            raw = bytes(data)
            parser = EXT_PARSERS.get(name)
            if parser is None:
                self.counts['ext_bad_layout'] += 1
                return
            try:
                decoded = parser(raw)
            except ValueError as exc:
                self.counts['ext_bad_layout'] += 1
                self._record('ext_bad_layout', now, name=name, raw=raw.hex())
                self._warn_once(('ext_layout', name), 'EXT layout unknown name=%s %s raw=%s'
                                % (name, exc, raw.hex().upper()))
                return
            previous = self.ext_last.get(name)
            elapsed = (now - previous['at']) if previous is not None else 0.0
            rates = derive_rates(name, previous, decoded, elapsed)
            self.counts['ext_ok'] += 1
            self.ext_last[name] = dict(decoded, at=now)
            self.ext_seen[name] = decoded
            fields = dict(decoded, since_s=round(elapsed, 2), **rates)
            self._record('ext', now, name=name, **fields)
            self._emit('EXT%s %s raw=%s'
                       % (name[-1], ' '.join('%s=%s' % (k, v) for k, v in fields.items()),
                          raw.hex().upper()))
        except Exception:
            pass

    # ------------------------------------------------------------- per-second
    def second(self, now, status=None, channel=None, health=None):
        """1 Hz bounded summary + full-field status line."""
        try:
            if not self.enabled:
                return
            rates = self._notification_rates(now)
            buckets = list(self.pulse_buckets)
            if self._pulse_bucket is not None:
                buckets.append(self._pulse_bucket_count)
            peak = (max(buckets) * 10) if buckets else 0
            self._emit('RX rate status=%.1f/s pulse=%.1f/s diag=%.1f/s other=%.1f/s '
                       'pulse_bytes=%.0f/s pulse_peak_100ms=%s/s'
                       % (rates['status'], rates['pulse'], rates['diag'], rates['other'],
                          rates['pulse_bytes'], peak))
            total = self.counts
            avg = (sum(self.write_ms) / len(self.write_ms)) if self.write_ms else 0
            self._emit('LINK submit=%s done=%s fail=%s deadline=%s quarantine=%s stop=%s '
                       'write_ms_avg=%.1f write_ms_max=%s ack_stall_ms=%s notif_status=%s '
                       'notif_pulse=%s notif_diag=%s ext_ok=%s ext_fail=%s suppressed=%s'
                       % (total['write_submit'], total['write_done'], total['write_fail'],
                          total['write_deadline'], total['quarantine'], total['stop_queued'],
                          avg, (max(self.write_ms) if self.write_ms else 0), self.ack_stall_ms,
                          self.notif['status'], self.notif['pulse'], self.notif['diag'],
                          total['ext_ok'], total['ext_fail'], self.suppressed))
            if status is not None:
                extra = {}
                if channel is not None:
                    extra = dict(stop_seq=channel.stop_seq, barrier=int(channel.barrier),
                                 synced=int(channel.synced), timed_out=int(channel.timed_out),
                                 writing=int(channel.writing is not None),
                                 pending=int(channel.pending is not None),
                                 unconfirmed=len(channel.unconfirmed),
                                 ack_delay_ms=getattr(channel, 'ack_delay_ms', None))
                self.full_status(now, status, extra)
            if health:
                self._emit('HEALTH ' + ' '.join('%s=%s' % (k, health[k]) for k in sorted(health)))
            self.last_second = now
        except Exception:
            pass

    def close(self, reason='session_end', now=None):
        """Final bounded flush; never raises."""
        try:
            if not self.enabled:
                return
            now = self._now(now)
            self._emit('SESSION end reason=%s submit=%s done=%s fail=%s deadline=%s quarantine=%s '
                       'stop=%s notif_status=%s notif_pulse=%s notif_diag=%s ext_ok=%s ext_fail=%s'
                       % (reason, self.counts['write_submit'], self.counts['write_done'],
                          self.counts['write_fail'], self.counts['write_deadline'],
                          self.counts['quarantine'], self.counts['stop_queued'],
                          self.notif['status'], self.notif['pulse'], self.notif['diag'],
                          self.counts['ext_ok'], self.counts['ext_fail']))
        except Exception:
            pass

    def snapshot(self):
        """Small JSON-able view for exports (bounded)."""
        try:
            return dict(enabled=self.enabled, ext_enabled=self.ext_enabled,
                        counts=dict(self.counts), session=dict(self.session),
                        notif=dict(self.notif), notif_bytes=dict(self.notif_bytes),
                        write_ms=list(self.write_ms), ack_stall_ms=self.ack_stall_ms,
                        ext1=self.ext_last.get('ext1'), ext2=self.ext_last.get('ext2'),
                        suppressed=self.suppressed,
                        events=[dict(at=round(at, 3), kind=kind, **fields)
                                for at, kind, fields in self.events])
        except Exception:
            return {}
