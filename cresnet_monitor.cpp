// CresnetMonitor implementation — ISR-driven parser.
//
// The complete byte-level parser now runs inside the UART RX
// interrupt service routine. Per-byte arrival timestamps come
// directly from esp_timer_get_time() (microsecond-accurate, IRAM-
// safe) so burst-gap detection is immune to FreeRTOS scheduling
// jitter: even if WiFi, LVGL, or MQTT hog the CPU for tens of ms,
// our ISR still fires per byte and burst boundaries stay accurate.
//
// All functions in the ISR chain (isrHandler, processByteIRAM,
// finalizeBurstIRAM, recordPollIRAM, updateStatusIRAM) are
// IRAM_ATTR so they run from instruction RAM and cannot be stalled
// by flash-cache invalidations (e.g. during OTA or NVS writes).
//
// Wire-level protocol (38400 8N1, observed on live bus):
//   Master poll      : <ID> 00                (2 bytes, intra ~310 µs)
//   Master->slave    : ~900 µs turnaround
//   Slave response   : <length> <payload...>  (length INCLUDES itself)
//   Inter-poll idle  : ~1.5 ms before next master poll
//
// Burst detection: any inter-byte gap > BURST_GAP_US (500 µs) closes
// the current burst. With ISR-captured timestamps this reliably
// separates master polls from slave responses.
//
// Burst classification (length-prefix walker):
//   * <valid ID 0x03..0xFE> <00>       => master poll (2 bytes)
//   * anything else                    => length-prefixed slave
//                                         message (consume `len` bytes)
//
// Pairing across bursts: master poll sets `_pending_master_*`; the
// next slave-shaped burst within PENDING_RESPONSE_WINDOW_MS marks the
// device alive. Otherwise the next master poll marks it silent.
//
// The (light) FreeRTOS task only runs the periodic 100 ms aging walk
// so that devices age out of the recency window even when no further
// bus traffic arrives for them.

#include "cresnet_monitor.h"
#include <driver/uart.h>
#include <hal/uart_ll.h>
#include <soc/uart_struct.h>
#include <soc/uart_periph.h>
#include <esp_timer.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>

namespace {

// Anything > this between two byte timestamps starts a new burst.
// Comfortably above the ~310 µs intra-frame wire gap and below the
// ~900 µs master->slave turnaround.
constexpr uint32_t BURST_GAP_US                = 500;

// Bus-idle backstop for a stuck pending poll. Long enough that
// normal operation never trips it; only fires when bus stops
// entirely with a poll outstanding.
constexpr uint32_t PENDING_TIMEOUT_MS          = 50;

// Tolerance for "is this slave-shaped burst the response to the
// currently-pending master poll?" — measured as ms between when the
// master poll burst was finalized and when this slave burst is
// finalized. Has to comfortably exceed the longest bus idle that can
// follow a slave response before the *next* byte triggers the slave
// burst's finalize: that's the ~21 ms inter-cycle gap that always
// follows the last poll of a cycle (e.g. 0x03 in the low range, and
// the equivalent last-IDs of other ranges). With a tighter window,
// the parser correctly sees the slave bytes but their burst only
// closes when the next-cycle master byte arrives ~21 ms later, so
// the elapsed check would fail and the end-of-cycle device would be
// left unpaired — showing magenta forever. Matched to
// PENDING_TIMEOUT_MS so "pair if pending is still alive at all"
// has uniform semantics.
constexpr uint32_t PENDING_RESPONSE_WINDOW_MS  = 50;

// Sanity bound for a slave length byte in the burst walker.
constexpr uint8_t  MAX_PLAUSIBLE_LEN           = 64;

// Aging task cadence.
constexpr uint32_t AGE_TICK_MS                 = 100;

// --- Anti-noise filtering thresholds -----------------------------
// A single byte-sequence on the wire that happens to look like a
// master poll for some random ID (no checksum exists at the Cresnet
// protocol layer to rule this out) shouldn't be enough to light up
// that ID in the UI. We require sustained, repeated evidence.
//
// MIN_POLLS_FOR_POLLED: minimum number of polls for the same ID
// observed inside POLL_CONFIRM_WINDOW_MS for the ID to be considered
// "actually polled by the master." A single bogus poll never reaches
// 3; even a small burst of noise won't reliably hit the same ID
// three times in five minutes.
constexpr uint8_t  MIN_POLLS_FOR_POLLED        = 3;
constexpr uint32_t POLL_CONFIRM_WINDOW_MS      = 5UL * 60UL * 1000UL;

// Anti-noise threshold for response counts. A device only counts as
// "we have heard from it" once we've recorded enough responses since
// boot. The threshold grows over time:
//
//   threshold = min(MAX, BASE + GROWTH_PER_HOUR * hours_since_boot)
//
// so a slow trickle of false-positive "responses" from line noise
// can never crawl past the bar over a long run. After
// LATCH_HOURS the threshold latches at MAX, which also makes the
// computation immune to the millis()-style 49.7-day uint32 wrap
// (we already use the 64-bit esp_timer_get_time() for the hour
// count, but the latch is belt-and-suspenders).
constexpr uint32_t MIN_RESP_FOR_HEARD_BASE          = 5;
constexpr uint32_t MIN_RESP_FOR_HEARD_GROWTH_HOUR   = 2;
constexpr uint32_t MIN_RESP_FOR_HEARD_MAX           = 100;
constexpr uint64_t MIN_RESP_FOR_HEARD_LATCH_HOURS   = 48;  // 5 + 2*48 == 101 -> cap

// Alternate "we've heard from this ID" path so that a freshly-
// plugged-in device doesn't have to wait for its lifetime response
// count to clear the (growing) MIN_RESP_FOR_HEARD threshold. If we've
// heard from the ID at least MIN_RESPS_IN_RECENT_WINDOW times within
// the last RESP_RECENT_WINDOW_MS, we trust it immediately. (Same
// shape as the poll-history gate.)
constexpr uint8_t  MIN_RESPS_IN_RECENT_WINDOW       = 5;
constexpr uint32_t RESP_RECENT_WINDOW_MS            = 5UL * 60UL * 1000UL;

// During the first BOOT_HEARD_GRACE_US after boot we relax the
// "heard from" requirement to BOOT_HEARD_MIN_RESP responses. This
// keeps slowly-polled devices (those polled at the master's
// high-range cycle, ~1/sec or rarer) from showing magenta during
// the warm-up. The threshold isn't *1* because a single phantom
// noise-driven "response" was producing the occasional ghost-green
// or ghost-yellow cell during boot; requiring 2 hits inside the
// window is enough to filter that out without inconveniencing any
// real device that's actually being polled. After the grace ends
// the normal two-path gate (lifetime threshold OR recent-window)
// takes over.
constexpr uint64_t BOOT_HEARD_GRACE_US              = 120ULL * 1000000ULL;
constexpr uint32_t BOOT_HEARD_MIN_RESP              = 2;

// Depth of the per-ID poll-timestamp ring (PSRAM). 1024 gives us
// plenty of headroom over MIN_POLLS_FOR_POLLED and is convenient
// to address. Memory cost: 256 * 1024 * 4 B = 1 MB in PSRAM.
constexpr int      POLL_HISTORY_LEN            = 1024;

// RX timeout threshold in bit-times. Above the intra-byte ~2 bits
// of wire idle but below the master->slave turnaround (~35 bits).
// This makes the IDF RX timeout interrupt fire shortly after a burst
// ends, which we use as an extra opportunity to close any open
// burst even if no further bytes arrive.
constexpr uint8_t  RX_TIMEOUT_BITS             = 30;

}  // namespace

bool CresnetMonitor::begin(int uart_port, int rx_pin, int tx_pin,
                           uint32_t baud, uint32_t window_ms,
                           int core, UBaseType_t priority, uint32_t stack) {
  if (_started) return false;
  _uart_port = uart_port;
  _rx_pin    = rx_pin;
  _tx_pin    = tx_pin;
  _baud      = baud;
  _window_ms = window_ms;

  uart_port_t port = (uart_port_t)_uart_port;

  // If something else (HardwareSerial, an earlier begin attempt, etc.)
  // already installed the IDF UART driver, tear it down so our raw
  // ISR install is clean.
  if (uart_is_driver_installed(port)) {
    uart_driver_delete(port);
  }

  uart_config_t cfg = {};
  cfg.baud_rate  = (int)_baud;
  cfg.data_bits  = UART_DATA_8_BITS;
  cfg.parity     = UART_PARITY_DISABLE;
  cfg.stop_bits  = UART_STOP_BITS_1;
  cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  // uart_param_config enables the UART peripheral clock and
  // configures baud/framing. We do not install the high-level
  // driver; we own the ISR ourselves.
  if (uart_param_config(port, &cfg) != ESP_OK) return false;
  if (uart_set_pin(port, _tx_pin, _rx_pin,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
    return false;
  }

  // Everything from here goes through uart_ll_* (raw hardware access
  // via a uart_dev_t* obtained from UART_LL_GET_HW). The IDF's
  // high-level uart_disable_intr_mask / uart_clear_intr_status /
  // uart_intr_config / uart_enable_intr_mask wrappers route through
  // uart_context[port].hal which on Arduino-ESP32 3.3.7's P4 build
  // can be null when no driver has been installed, causing a null
  // dereference inside a ROM-resident hal helper. uart_ll_* bypasses
  // uart_context entirely and operates directly on the peripheral.
  uart_dev_t* hw = UART_LL_GET_HW(port);

  // Disable any leftover interrupts and clear any pending status.
  uart_ll_disable_intr_mask(hw, UART_INTR_RXFIFO_FULL |
                                UART_INTR_RXFIFO_TOUT);
  uart_ll_clr_intsts_mask(hw, UART_INTR_RXFIFO_FULL |
                              UART_INTR_RXFIFO_TOUT);

  // RX FIFO threshold of 1: every byte received fires an interrupt.
  // RX timeout: an additional interrupt fires after RX_TIMEOUT_BITS
  // of post-byte idle, which we use as a "burst-ended" signal so the
  // ISR closes the current burst even when no further byte arrives.
  uart_ll_set_rxfifo_full_thr(hw, 1);
  uart_ll_set_rx_tout(hw, RX_TIMEOUT_BITS);

  // Allocate the per-ID poll- and response-history rings in PSRAM
  // *before* enabling the UART ISR, so the very first poll handled
  // by the ISR has valid buffers to drain into. Each is ~1 MB.
  auto alloc_history = [](void) -> uint32_t* {
    uint32_t* p = (uint32_t*)heap_caps_calloc(
        (size_t)256 * (size_t)POLL_HISTORY_LEN,
        sizeof(uint32_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
      // Fall back to internal RAM if PSRAM is unavailable. The
      // anti-noise filter still works; just costs more SRAM.
      p = (uint32_t*)heap_caps_calloc(
          (size_t)256 * (size_t)POLL_HISTORY_LEN,
          sizeof(uint32_t),
          MALLOC_CAP_DEFAULT);
    }
    return p;
  };
  _poll_ts = alloc_history();
  _resp_ts = alloc_history();
  // If either is null, the corresponding helper detects it and
  // degrades gracefully — don't fail begin() over it.

  // Install our IRAM-resident ISR. ESP_INTR_FLAG_IRAM is critical:
  // it tells esp_intr_alloc that this ISR (and everything it calls)
  // must be reachable from IRAM even with the flash cache off.
  if (esp_intr_alloc(uart_periph_signal[port].irq,
                     ESP_INTR_FLAG_IRAM,
                     &CresnetMonitor::isrTrampoline, this,
                     &_intr_handle) != ESP_OK) {
    if (_poll_ts) { heap_caps_free(_poll_ts); _poll_ts = nullptr; }
    if (_resp_ts) { heap_caps_free(_resp_ts); _resp_ts = nullptr; }
    return false;
  }
  // Enable the RX interrupts we want.
  uart_ll_ena_intr_mask(hw, UART_INTR_RXFIFO_FULL |
                            UART_INTR_RXFIFO_TOUT);

  BaseType_t r;
  if (core >= 0) {
    r = xTaskCreatePinnedToCore(taskTrampoline, "cresnet_age",
                                stack, this, priority,
                                &_task_handle, core);
  } else {
    r = xTaskCreate(taskTrampoline, "cresnet_age",
                    stack, this, priority, &_task_handle);
  }
  if (r != pdPASS) {
    esp_intr_free(_intr_handle);
    _intr_handle = nullptr;
    _task_handle = nullptr;
    return false;
  }
  _started = true;
  return true;
}

void CresnetMonitor::taskTrampoline(void* arg) {
  static_cast<CresnetMonitor*>(arg)->taskLoop();
}

// Lightweight aging task. Bus parsing happens in the ISR. The task
// (a) moves the SRAM ring of poll events into the per-ID PSRAM
// history, and (b) walks the table to compute current statuses.
// Together they form the only PSRAM-touching path in this module.
void CresnetMonitor::taskLoop() {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(AGE_TICK_MS));
    drainPollEventsToPsram();
    ageAllStatus();
  }
}

// -----------------------------------------------------------------
// ISR-side code below. Every function from here on is IRAM_ATTR.
// -----------------------------------------------------------------

void IRAM_ATTR CresnetMonitor::isrTrampoline(void* arg) {
  static_cast<CresnetMonitor*>(arg)->isrHandler();
}

void IRAM_ATTR CresnetMonitor::isrHandler() {
  uart_dev_t* hw = UART_LL_GET_HW((uart_port_t)_uart_port);
  uint32_t intr_status = uart_ll_get_intsts_mask(hw);

  // Drain whatever is currently in the RX FIFO, timestamping each
  // byte at its individual drain time. With RX FIFO full threshold
  // of 1 the FIFO is usually empty or has just one byte when we
  // arrive, so the per-byte timestamp closely tracks wire-arrival.
  while (uart_ll_get_rxfifo_len(hw) > 0) {
    uint8_t b = 0;
    uart_ll_read_rxfifo(hw, &b, 1);
    uint32_t now_us = (uint32_t)esp_timer_get_time();
    processByteIRAM(b, now_us);
  }

  // RX_TIMEOUT signals end-of-burst even when no further byte is
  // coming. Use it to close any burst that's been hanging open
  // (e.g. the very last poll before a long bus idle).
  if (intr_status & UART_INTR_RXFIFO_TOUT) {
    uint32_t now_us = (uint32_t)esp_timer_get_time();
    if (_burst_len > 0 && _last_byte_us != 0 &&
        (uint32_t)(now_us - _last_byte_us) > BURST_GAP_US) {
      finalizeBurstIRAM();
    }
  }

  uart_ll_clr_intsts_mask(hw, intr_status);
}

void IRAM_ATTR CresnetMonitor::processByteIRAM(uint8_t b, uint32_t now_us) {
  _total_bytes++;
  uint32_t gap = (_last_byte_us == 0) ? UINT32_MAX
                                      : (uint32_t)(now_us - _last_byte_us);
  _last_byte_us = now_us;

  // Gap above threshold => the previous burst is complete and a new
  // one starts with this byte.
  if (gap > BURST_GAP_US) {
    finalizeBurstIRAM();
  }
  if (_burst_len < BURST_BUF_LEN) {
    _burst_buf[_burst_len++] = b;
  }
}

void IRAM_ATTR CresnetMonitor::finalizeBurstIRAM() {
  // A single-byte burst is almost always noise (or a one-byte glitch
  // from the transceiver). Drop it without changing any state, so it
  // can't false-green whatever poll is currently pending.
  if (_burst_len < 2) {
    _burst_len = 0;
    return;
  }

  uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

  // State-aware length-prefix walker. The fundamental ambiguity we
  // resolve here is the byte sequence `<X> <00>`: it could be a
  // master poll for ID X, or it could be the start of a slave
  // length-prefixed message of length X with a 0x00 first payload
  // byte. This bit specifically: a slave response of length 3 with
  // payload starting `00` is byte-for-byte identical to a master
  // poll for 0x03.
  //
  // Heuristic:
  //   * When we're already waiting on a pending master poll, the
  //     next message on the wire is — by protocol — the slave's
  //     reply. Prefer the slave interpretation. Only fall back to
  //     master poll if the slave-length doesn't actually fit the
  //     burst (e.g. `[04, 00]` after a silent 0x03 poll: length 4
  //     can't fit a 2-byte burst, so it's the next master poll
  //     and the previous poll was silent).
  //   * When no master is pending, prefer master-poll. Only fall
  //     back to length-prefix if that fails.
  size_t i = 0;
  while (i + 2 <= _burst_len) {
    uint8_t b0 = _burst_buf[i];
    uint8_t b1 = _burst_buf[i + 1];

    bool is_master_shape =
        (b0 >= 0x03 && b0 < 0xFF && b1 == 0x00);
    bool is_slave_valid =
        (b0 >= 2 && b0 <= MAX_PLAUSIBLE_LEN && i + b0 <= _burst_len);

    bool do_master = false;
    bool do_slave  = false;
    size_t advance = 1;

    if (_pending_master_ms != 0) {
      // Pending poll waiting for response → slave preferred.
      if (is_slave_valid) {
        do_slave  = true;
        advance   = b0;
      } else if (is_master_shape) {
        do_master = true;
        advance   = 2;
      }
    } else {
      // No pending poll → master preferred.
      if (is_master_shape) {
        do_master = true;
        advance   = 2;
      } else if (is_slave_valid) {
        do_slave  = true;
        advance   = b0;
      }
    }

    if (do_master) {
      _total_segments++;
      if (_pending_master_ms != 0) {
        // Previous pending master never got a response.
        recordPollIRAM(_pending_master_id, /*responded=*/false);
      }
      _pending_master_id = b0;
      _pending_master_ms = (now_ms == 0) ? 1 : now_ms;
    } else if (do_slave) {
      _total_segments++;
      if (_pending_master_ms != 0) {
        uint32_t elapsed = (uint32_t)(now_ms - _pending_master_ms);
        if (elapsed <= PENDING_RESPONSE_WINDOW_MS) {
          recordPollIRAM(_pending_master_id, /*responded=*/true);
          _pending_master_ms = 0;
          _pending_master_id = 0;
        }
        // Stale slave-shaped burst: leave pending intact. The next
        // real master poll (or the bus-idle timeout) resolves it.
      }
    }
    // else: nothing fits → fall through and skip a single byte to
    // try to resync at i+1.

    i += advance;
  }

  // After walking, opportunistically clean up a stuck pending master
  // poll if too much time has elapsed since it was set.
  if (_pending_master_ms != 0) {
    uint32_t pending_age =
        (uint32_t)(now_ms - _pending_master_ms);
    if (pending_age > PENDING_TIMEOUT_MS) {
      recordPollIRAM(_pending_master_id, /*responded=*/false);
      _pending_master_ms = 0;
      _pending_master_id = 0;
    }
  }

  _burst_len = 0;
}

void IRAM_ATTR CresnetMonitor::recordPollIRAM(uint8_t id, bool responded) {
  uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
  _last_poll_ms[id] = now_ms;
  _poll_count[id]++;
  if (responded) {
    _last_resp_ms[id] = now_ms;
    _resp_count[id]++;
  }
  // SRAM ring only — the aging task copies into PSRAM later.
  enqueuePollEventIRAM(id, now_ms, responded);
  // NB: we deliberately do NOT update _status[id] here. _status is
  // sole-written by the aging task running in task context — that
  // way the ISR never has to consult deviceIsPolled() (which reads
  // the PSRAM history) or otherwise touch PSRAM. Worst-case
  // observable latency for a status transition is one aging tick.
}

// ISR side of the history pipeline: append the (id, ms, responded)
// event to the SRAM SPSC ring. The aging task drains this ring and
// splits each entry into the per-ID poll-history PSRAM ring and
// (if responded) the response-history PSRAM ring.
// Single-producer (this ISR) / single-consumer (the aging task) so
// we can do this lock-free with acquire/release on the head/tail
// counters.
void IRAM_ATTR CresnetMonitor::enqueuePollEventIRAM(uint8_t id,
                                                   uint32_t now_ms,
                                                   bool responded) {
  uint16_t head = _poll_ring_head.load(std::memory_order_relaxed);
  uint16_t next = (uint16_t)((head + 1) % POLL_RING_LEN);
  uint16_t tail = _poll_ring_tail.load(std::memory_order_acquire);
  if (next == tail) {
    // Ring full — aging task hasn't drained yet. Drop this event;
    // a momentary blip in history is preferable to corrupting the
    // SPSC invariant. Should never happen under any sane load
    // (256 entries vs. ~30 events per 100 ms aging tick).
    return;
  }
  _poll_ring_ms[head]        = now_ms;
  _poll_ring_id[head]        = id;
  _poll_ring_responded[head] = responded ? 1 : 0;
  _poll_ring_head.store(next, std::memory_order_release);
}

// Task side of the pipeline: pull events off the SRAM ring and
// append each into the per-ID poll-history PSRAM ring (always)
// and the response-history PSRAM ring (if the event represented
// an alive response). Only this function ever touches _poll_ts /
// _resp_ts, so PSRAM access is always in task context.
void CresnetMonitor::drainPollEventsToPsram() {
  if (!_poll_ts && !_resp_ts) {
    // Neither PSRAM nor DRAM fallback available. Drain the SRAM
    // ring anyway so it doesn't fill up; deviceIsPolled() and
    // deviceRecentlyHeard() will detect the null buffers and
    // degrade gracefully.
    _poll_ring_tail.store(
        _poll_ring_head.load(std::memory_order_acquire),
        std::memory_order_release);
    return;
  }
  uint16_t tail = _poll_ring_tail.load(std::memory_order_relaxed);
  uint16_t head = _poll_ring_head.load(std::memory_order_acquire);
  while (tail != head) {
    uint8_t  id        = _poll_ring_id[tail];
    uint32_t ms        = _poll_ring_ms[tail];
    bool     responded = _poll_ring_responded[tail] != 0;

    if (_poll_ts) {
      uint16_t idx = _poll_history_idx[id];
      _poll_ts[(size_t)id * POLL_HISTORY_LEN + idx] = ms;
      _poll_history_idx[id] = (uint16_t)((idx + 1) % POLL_HISTORY_LEN);
      if (_poll_history_count[id] < POLL_HISTORY_LEN) {
        _poll_history_count[id]++;
      }
    }
    if (responded && _resp_ts) {
      uint16_t idx = _resp_history_idx[id];
      _resp_ts[(size_t)id * POLL_HISTORY_LEN + idx] = ms;
      _resp_history_idx[id] = (uint16_t)((idx + 1) % POLL_HISTORY_LEN);
      if (_resp_history_count[id] < POLL_HISTORY_LEN) {
        _resp_history_count[id]++;
      }
    }
    tail = (uint16_t)((tail + 1) % POLL_RING_LEN);
  }
  _poll_ring_tail.store(tail, std::memory_order_release);
}

// True iff we have at least MIN_POLLS_FOR_POLLED polls for `id` in
// the last POLL_CONFIRM_WINDOW_MS. Looks back at the
// MIN_POLLS_FOR_POLLED-th most recent entry in the ring; if even
// *that* is inside the window then there are at least that many in
// window. If the PSRAM ring couldn't be allocated, falls back to
// the simple poll-age check against the legacy _window_ms.
//
// Task-context only — reads _poll_ts (PSRAM). Never call from ISR.
bool CresnetMonitor::deviceIsPolled(uint8_t id, uint32_t now_ms) const {
  if (!_poll_ts) {
    if (_last_poll_ms[id] == 0) return false;
    return (uint32_t)(now_ms - _last_poll_ms[id]) <= _window_ms;
  }
  if (_poll_history_count[id] < MIN_POLLS_FOR_POLLED) return false;
  uint16_t off = (uint16_t)((_poll_history_idx[id] + POLL_HISTORY_LEN
                             - MIN_POLLS_FOR_POLLED) % POLL_HISTORY_LEN);
  uint32_t ts = _poll_ts[(size_t)id * POLL_HISTORY_LEN + off];
  return (uint32_t)(now_ms - ts) <= POLL_CONFIRM_WINDOW_MS;
}

// True iff we've recorded at least MIN_RESPS_IN_RECENT_WINDOW
// responses for `id` within the last RESP_RECENT_WINDOW_MS — same
// shape as deviceIsPolled() but on the response ring. This gives a
// fresh device a fast path to ALIVE: as soon as it answers five
// polls in five minutes it's "heard from," without waiting for the
// growing lifetime threshold to climb. Task-context only — reads
// _resp_ts (PSRAM).
bool CresnetMonitor::deviceRecentlyHeard(uint8_t id, uint32_t now_ms) const {
  if (!_resp_ts) return false;
  if (_resp_history_count[id] < MIN_RESPS_IN_RECENT_WINDOW) return false;
  uint16_t off = (uint16_t)((_resp_history_idx[id] + POLL_HISTORY_LEN
                             - MIN_RESPS_IN_RECENT_WINDOW) % POLL_HISTORY_LEN);
  uint32_t ts = _resp_ts[(size_t)id * POLL_HISTORY_LEN + off];
  return (uint32_t)(now_ms - ts) <= RESP_RECENT_WINDOW_MS;
}

// Dynamic threshold for "we've heard from this ID enough times to
// trust SILENT/MISSED on it." Grows linearly from BASE at boot,
// caps at MAX, latches at MAX after LATCH_HOURS so that the
// computation can't be affected by any conceivable timer wrap.
// Uses esp_timer_get_time() (int64_t µs) so it's already
// overflow-safe before the latch kicks in.
uint32_t CresnetMonitor::minRespForHeard() {
  static bool latched_at_max = false;
  if (latched_at_max) return MIN_RESP_FOR_HEARD_MAX;

  uint64_t us    = (uint64_t)esp_timer_get_time();
  uint64_t hours = us / (3600ULL * 1000000ULL);
  if (hours >= MIN_RESP_FOR_HEARD_LATCH_HOURS) {
    latched_at_max = true;
    return MIN_RESP_FOR_HEARD_MAX;
  }
  uint32_t v = MIN_RESP_FOR_HEARD_BASE +
               MIN_RESP_FOR_HEARD_GROWTH_HOUR * (uint32_t)hours;
  if (v > MIN_RESP_FOR_HEARD_MAX) v = MIN_RESP_FOR_HEARD_MAX;
  return v;
}

// updateStatusIRAM was removed: status updates now happen
// exclusively in ageAllStatus() (task context), so that the only
// PSRAM consumer (deviceIsPolled) is never invoked from the ISR.

// -----------------------------------------------------------------
// Task-side: status aging. Runs from the lightweight aging task,
// NOT from the ISR. Reads timestamps (atomic uint32_t) written by
// the ISR and writes _status[] (atomic uint8_t). No locking needed
// because all individual reads/writes are naturally atomic on
// aligned 32-bit (and 8-bit) accesses, and a brief inconsistency
// between an ID's timestamps and its status auto-corrects on the
// next aging tick.
// -----------------------------------------------------------------

void CresnetMonitor::ageAllStatus() {
  uint32_t now            = millis();
  uint32_t resp_threshold = minRespForHeard();

  // One-shot latch: true until the first aging tick that happens
  // after BOOT_HEARD_GRACE_US since boot. Once cleared it stays
  // cleared, so this can never re-arm after a millis()-style
  // overflow. (We read the 64-bit esp_timer_get_time() to decide.)
  static bool boot_heard_grace_active = true;
  if (boot_heard_grace_active &&
      (uint64_t)esp_timer_get_time() >= BOOT_HEARD_GRACE_US) {
    boot_heard_grace_active = false;
  }

  for (int i = 0; i < 256; ++i) {
    // Skip IDs we've literally never touched at all. Once the poll
    // history has anything in it we always re-evaluate, so a device
    // that drops below the 3-in-5-min threshold ages back to grey.
    if (_last_poll_ms[i] == 0 && _last_resp_ms[i] == 0 &&
        _poll_history_count[i] == 0) {
      continue;
    }

    // Anti-noise gate 1: an ID only counts as polled if we've seen
    // at least MIN_POLLS_FOR_POLLED polls for it in the last
    // POLL_CONFIRM_WINDOW_MS. A bogus byte sequence that decodes as
    // a single master poll for some random ID can never reach this.
    if (!deviceIsPolled((uint8_t)i, now)) {
      _status[i] = NOT_POLLED;
      continue;
    }
    // Anti-noise gate 2: don't promote the ID to any "we've heard
    // from it" state (MISSED / SILENT / ALIVE) until one of these
    // three conditions is met:
    //   (a) lifetime response count >= the (growing) threshold; or
    //   (b) at least MIN_RESPS_IN_RECENT_WINDOW responses within
    //       the last RESP_RECENT_WINDOW_MS; or
    //   (c) we're still inside BOOT_HEARD_GRACE_US and lifetime
    //       responses >= BOOT_HEARD_MIN_RESP. Stops the grid
    //       filling with magenta during boot for slowly-polled
    //       devices, without single-shot noise being able to ghost
    //       a device into ALIVE/MISSED.
    // (a) is the slow-and-steady proof; (b) is the fast path for
    // freshly-plugged-in devices on a running system; (c) is the
    // warm-up grace at startup.
    bool heard = (_resp_count[i] >= resp_threshold) ||
                 deviceRecentlyHeard((uint8_t)i, now) ||
                 (boot_heard_grace_active &&
                  _resp_count[i] >= BOOT_HEARD_MIN_RESP);
    if (!heard) {
      _status[i] = NEVER_SEEN;
      continue;
    }
    uint32_t resp_age = (uint32_t)(now - _last_resp_ms[i]);
    if (resp_age > _window_ms) {
      _status[i] = SILENT;
      continue;
    }
    _status[i] = (_last_poll_ms[i] != _last_resp_ms[i]) ? MISSED : ALIVE;
  }
}
