/*
 * tool_cresnet.ino - the Cresnet Monitor tool, from 6115-cresnet-86-monitor.
 *
 * Listens to the bus through an RS485 transceiver on UART1 and paints a 16x16
 * grid of every possible Cresnet ID.  Everything after begin() runs on its
 * own: the monitor in its ISR and aging task, the grid on an LVGL timer.  Tap
 * the grid for the colour legend.
 *
 *   UART1, RX=GPIO48, TX=GPIO47 (TX held idle by the auto-direction transceiver)
 */

static CresnetMonitor cresnetMonitor;
static CresnetGrid    cresnetGrid;

void cresnet_tool_begin(lv_obj_t *scr) {
    if (!cresnetMonitor.begin(/*uart_port=*/1, /*rx_pin=*/48, /*tx_pin=*/47)) {
        Serial0.println("ERROR: CresnetMonitor failed to start");
    }
    cresnetGrid.begin(scr, &cresnetMonitor);
    Serial0.println("Cresnet Monitor started");
}
