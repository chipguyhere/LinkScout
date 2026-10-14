// CresnetMonitor — passive Cresnet bus observer for ESP32 family.
//
// Spins up a FreeRTOS task that reads the Cresnet bus through a UART
// configured for 38400 8N1 and maintains a 256-entry status table
// describing each possible 8-bit Cresnet ID. The status table is a flat
// array of bytes; a single-byte read from another thread is atomic on
// the targeted hardware, so callers can poll status() without locking.
//
// Status semantics (window is configurable; default 60 s):
//   NOT_POLLED  master has not polled this ID inside the window
//   NEVER_SEEN  master polled this ID inside the window, but this ID has
//               never been observed responding since the monitor started
//   SILENT      master polled this ID inside the window and this ID has
//               responded at some point earlier in the program's life,
//               but no response from it has fallen inside the window
//   ALIVE       this ID responded to a poll inside the window and the
//               most recent poll observed got its response back
//   MISSED      this ID responded inside the window, but the most recent
//               poll observed did NOT get its response back yet (fast
//               yellow-flag signal: a green ID flips here as soon as it
//               misses a single poll, then drops to SILENT once the last
//               good response ages out of the window)
//
// The class also exposes per-ID poll/response counters and timestamps
// for diagnostic display. Those are 32-bit and so reads across threads
// can theoretically tear; treat them as informational only.

#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_intr_alloc.h>

class CresnetMonitor {
public:
  enum Status : uint8_t {
    NOT_POLLED = 0,
    SILENT     = 1,
    ALIVE      = 2,
    MISSED     = 3,
    NEVER_SEEN = 4,
  };

  CresnetMonitor() = default;

  // Configure the UART and start the background task. Safe to call once.
  //
  //   uart_port  : ESP32 UART number (0..n-1). Pick one not used elsewhere.
  //   rx_pin     : GPIO connected to the transceiver's RO (receive) line.
  //   tx_pin     : GPIO connected to the transceiver's DI line. Held idle;
  //                with an auto-direction transceiver this leaves DE off.
  //   baud       : Cresnet bit rate. 38400 confirmed on the live system.
  //   window_ms  : recency window for ALIVE / SILENT classification.
  //   core       : CPU core to pin the task to. -1 = no affinity.
  //   priority   : task priority. Modest by default because all the
  //                latency-critical work now runs in the UART RX ISR
  //                (IRAM, immune to FreeRTOS scheduling). The task
  //                just walks the 256-entry status table every
  //                ~100 ms to age out devices that have fallen out
  //                of the recency window.
  //   stack      : task stack size in bytes.
  //
  // Returns true on success. If begin() has already been called, returns
  // false without touching anything.
  bool begin(int uart_port,
             int rx_pin, int tx_pin,
             uint32_t    baud      = 38400,
             uint32_t    window_ms = 60000,
             int         core      = 1,
             UBaseType_t priority  = 5,
             uint32_t    stack     = 4096);

  // Atomic single-byte read of status for a given ID. Safe from any thread.
  Status status(uint8_t id) const { return (Status)_status[id]; }

  // Copy the entire 256-entry status table into a caller buffer.
  void snapshot(Status out[256]) const {
    for (int i = 0; i < 256; ++i) out[i] = (Status)_status[i];
  }

  // Diagnostic accessors. 32-bit reads — not atomic relative to the
  // background task. Acceptable for human-facing display.
  uint32_t pollCount (uint8_t id) const { return _poll_count[id]; }
  uint32_t respCount (uint8_t id) const { return _resp_count[id]; }
  uint32_t lastPollMs(uint8_t id) const { return _last_poll_ms[id]; }
  uint32_t lastRespMs(uint8_t id) const { return _last_resp_ms[id]; }
  uint32_t totalBytes()           const { return _total_bytes; }
  uint32_t totalSegments()        const { return _total_segments; }
  bool     isRunning()            const { return _started; }

private:
  static void taskTrampoline(void* arg);
  void taskLoop();
  void ageAllStatus();
  void drainPollEventsToPsram();
  bool deviceIsPolled(uint8_t id, uint32_t now_ms) const;
  bool deviceRecentlyHeard(uint8_t id, uint32_t now_ms) const;
  static uint32_t minRespForHeard();

  // ISR path. Everything reachable from the ISR is placed in IRAM
  // via IRAM_ATTR in the .cpp so it can run while the flash cache is
  // unavailable. The ISR captures per-byte arrival timestamps from
  // esp_timer_get_time() directly, so the parser is immune to
  // FreeRTOS scheduling jitter. The ISR never touches PSRAM — that
  // could stall the core if the PSRAM cache is being invalidated by
  // another core. Instead, poll events are enqueued into the SRAM
  // SPSC ring below and the aging task drains them into the per-ID
  // PSRAM history.
  static void isrTrampoline(void* arg);
  void isrHandler();
  void processByteIRAM(uint8_t b, uint32_t now_us);
  void finalizeBurstIRAM();
  void recordPollIRAM(uint8_t id, bool responded);
  void enqueuePollEventIRAM(uint8_t id, uint32_t now_ms, bool responded);

  int      _uart_port = -1;
  int      _rx_pin    = -1;
  int      _tx_pin    = -1;
  uint32_t _baud      = 38400;
  uint32_t _window_ms = 60000;

  // Public state. Reader threads see one byte per ID, atomic per ID.
  volatile uint8_t _status[256] = {};

  // Task-private state. Only the background task writes these.
  uint32_t _last_poll_ms[256] = {};
  uint32_t _last_resp_ms[256] = {};
  uint32_t _poll_count[256]   = {};
  uint32_t _resp_count[256]   = {};

  uint32_t _total_bytes    = 0;
  uint32_t _total_segments = 0;

  // Master-poll pairing state. When a burst classifies as a master
  // poll (`<valid ID> 00`), we stash the ID here and wait for either
  // (a) the next burst, which is then attributed as the slave's
  // response, or (b) a timeout, which marks the device silent.
  // _pending_master_ms == 0 means "no master poll currently pending".
  // Single thread (the monitor task) owns these.
  uint8_t  _pending_master_id = 0;
  uint32_t _pending_master_ms = 0;

  // Per-byte burst buffer. The ISR captures bytes with per-arrival
  // microsecond timestamps; consecutive bytes accumulate here and the
  // buffer is finalized into a burst when the inter-byte gap exceeds
  // BURST_GAP_US (see the .cpp file).
  static constexpr size_t BURST_BUF_LEN = 64;
  uint8_t  _burst_buf[BURST_BUF_LEN] = {};
  size_t   _burst_len                = 0;
  uint32_t _last_byte_us             = 0;

  // Per-ID circular buffers (one of poll timestamps, one of response
  // timestamps) — both in PSRAM, allocated in begin(). 256 IDs ×
  // POLL_HISTORY_LEN × 4 bytes each = ~1 MB per buffer; PSRAM is the
  // only sensible home. Owned by the aging task; the ISR never
  // touches either directly. deviceIsPolled() and
  // deviceRecentlyHeard() read these from task context only.
  uint32_t* _poll_ts                 = nullptr;
  uint16_t  _poll_history_idx[256]   = {};
  uint16_t  _poll_history_count[256] = {};
  uint32_t* _resp_ts                 = nullptr;
  uint16_t  _resp_history_idx[256]   = {};
  uint16_t  _resp_history_count[256] = {};

  // SPSC ring of pending poll events: ISR pushes (id, ms, responded)
  // here, aging task drains and copies into _poll_ts (and, if
  // responded, _resp_ts). Lives in SRAM so the ISR never has to
  // touch PSRAM. POLL_RING_LEN at ~300 events/sec gives us ~850 ms
  // of buffering before drops — way more than the 100 ms aging-task
  // cadence.
  static constexpr size_t POLL_RING_LEN = 256;
  uint32_t _poll_ring_ms[POLL_RING_LEN]        = {};
  uint8_t  _poll_ring_id[POLL_RING_LEN]        = {};
  uint8_t  _poll_ring_responded[POLL_RING_LEN] = {};
  std::atomic<uint16_t> _poll_ring_head{0};   // ISR writes
  std::atomic<uint16_t> _poll_ring_tail{0};   // aging task reads

  intr_handle_t _intr_handle = nullptr;
  TaskHandle_t  _task_handle = nullptr;
  bool          _started     = false;
};
