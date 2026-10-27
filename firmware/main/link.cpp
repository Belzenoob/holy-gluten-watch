#include "link.h"
#include "protocol.h"
#include <ArduinoJson.h>
#include <stdarg.h>
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// The BLE service on NimBLE directly. The stack is started and stopped many
// times a day (linkStart / linkStop): the service table is static and nothing
// is allocated per start, so cycles cost no memory (the Arduino BLE wrapper
// leaked its server on every stop and ran the heap dry after a few dozen).

static volatile bool connected = false;
static volatile bool running = false;  // the stack is up (linkStart)
static uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t statusHandle = 0;
static void (*onPayload)() = nullptr;

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static std::string rx;           // payload being received (BLE task only)
static uint32_t rxExpected = 0;  // 0 = no transfer in progress
static std::string ready;        // complete payload waiting for the loop
static bool hasReady = false;

static uint32_t readU32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Bitwise CRC-32 (IEEE 802.3, reflected, as zlib). Payloads are a few KB.
static uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
  }
  return ~crc;
}

// What happens on the link, queued from the BLE task for the loop to print.
static char events[16][72];
static uint32_t evHead = 0, evTail = 0;

static void note(const char *fmt, ...) {
  char buf[72];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  portENTER_CRITICAL(&lock);
  memcpy(events[evHead % 16], buf, sizeof(buf));
  evHead++;
  if (evHead - evTail > 16) evTail = evHead - 16;
  portEXIT_CRITICAL(&lock);
}

bool linkTakeEvent(char *out, size_t size) {
  bool got = false;
  portENTER_CRITICAL(&lock);
  if (evTail != evHead) {
    snprintf(out, size, "%s", events[evTail % 16]);
    evTail++;
    got = true;
  }
  portEXIT_CRITICAL(&lock);
  return got;
}

// ─── STATUS ─────────────────────────────────────────────────────────────────

// Last reported refreshes and battery, so an error report does not hide them,
// and the JSON itself: read by the phone, and kept across stack restarts.
static int64_t heldFed[8];
static size_t heldCount = 0;
static int heldBattery = -1;
static char heldStatus[256] = "";
static SemaphoreHandle_t statusMutex = nullptr;  // loop and BLE task both report

static void notifyStatus() {
  if (!running || !connected || !statusHandle) return;
  xSemaphoreTake(statusMutex, portMAX_DELAY);
  struct os_mbuf *om = ble_hs_mbuf_from_flat(heldStatus, strlen(heldStatus));
  xSemaphoreGive(statusMutex);
  if (om) ble_gatts_notify_custom(connHandle, statusHandle, om);
}

void linkReportStatus(const int64_t *fed, size_t n, int battery, bool ok, const char *err) {
  if (!statusMutex) statusMutex = xSemaphoreCreateMutex();
  xSemaphoreTake(statusMutex, portMAX_DELAY);
  if (fed != heldFed) {
    heldCount = n < 8 ? n : 8;
    for (size_t i = 0; i < heldCount; i++) heldFed[i] = fed[i];
  }
  heldBattery = battery;
  JsonDocument doc;
  doc["p"] = MADRE_PROTOCOL;
  doc["fw"] = MADRE_FW_VERSION;
  JsonArray list = doc["fed"].to<JsonArray>();
  for (size_t i = 0; i < heldCount; i++) list.add(heldFed[i]);
  doc["bat"] = battery;
  doc["ok"] = ok;
  if (err && *err) doc["err"] = err;
  serializeJson(doc, heldStatus, sizeof(heldStatus));
  xSemaphoreGive(statusMutex);
  notifyStatus();
}

static void reportError(const char *err) {
  note("errore: %s", err);
  linkReportStatus(heldFed, heldCount, heldBattery, false, err);
}

// ─── STATE ──────────────────────────────────────────────────────────────────

static uint32_t dataFrames = 0;

static void onStateFrame(const uint8_t *d, size_t n) {
  if (n == 0) return;
  switch (d[0]) {
    case FRAME_BEGIN:
      if (n < 5) return reportError("frame begin");
      rxExpected = readU32(d + 1);
      if (rxExpected == 0 || rxExpected > MADRE_MAX_PAYLOAD) {
        rxExpected = 0;
        return reportError("dimensione");
      }
      rx.clear();
      rx.reserve(rxExpected);
      dataFrames = 0;
      note("inizio trasferimento: %u byte", (unsigned)rxExpected);
      break;
    case FRAME_DATA:
      if (!rxExpected) return reportError("dati senza inizio");
      if (rx.size() + n - 1 > rxExpected) {
        rxExpected = 0;
        return reportError("troppi dati");
      }
      rx.append((const char *)d + 1, n - 1);
      dataFrames++;
      break;
    case FRAME_END: {
      if (!rxExpected || n < 5) return reportError("fine inattesa");
      bool ok = rx.size() == rxExpected && crc32((const uint8_t *)rx.data(), rx.size()) == readU32(d + 1);
      note("fine trasferimento: %u/%u byte in %u pezzi, crc %s", (unsigned)rx.size(), (unsigned)rxExpected, (unsigned)dataFrames, ok ? "ok" : "sbagliato");
      rxExpected = 0;
      if (!ok) return reportError("crc");
      portENTER_CRITICAL(&lock);
      ready.swap(rx);
      hasReady = true;
      portEXIT_CRITICAL(&lock);
      if (onPayload) onPayload();
      rx.clear();
      break;
    }
  }
}

// ─── GATT ───────────────────────────────────────────────────────────────────

static ble_uuid128_t serviceUuid, stateUuid, statusUuid;

// "8f1c0001-5b7e-…" → NimBLE's little-endian bytes.
static void parseUuid(const char *s, ble_uuid128_t &out) {
  uint8_t bytes[16];
  int n = 0;
  for (const char *p = s; *p && n < 16; p++) {
    if (*p == '-') continue;
    auto hex = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    bytes[n++] = (uint8_t)(hex(p[0]) << 4 | hex(p[1]));
    p++;
  }
  out.u.type = BLE_UUID_TYPE_128;
  for (int i = 0; i < 16; i++) out.value[i] = bytes[15 - i];
}

static int onAccess(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    // Raw bytes: BEGIN/END carry binary integers that may contain zeros.
    uint8_t buf[MADRE_MAX_PAYLOAD < 600 ? MADRE_MAX_PAYLOAD : 600];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    onStateFrame(buf, len);
    return 0;
  }
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    note("stato letto dal telefono");
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    int rc = os_mbuf_append(ctxt->om, heldStatus, strlen(heldStatus));
    xSemaphoreGive(statusMutex);
    return rc ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

static struct ble_gatt_chr_def characteristics[3];
static struct ble_gatt_svc_def services[2];

static void buildServices() {
  parseUuid(MADRE_SERVICE_UUID, serviceUuid);
  parseUuid(MADRE_STATE_UUID, stateUuid);
  parseUuid(MADRE_STATUS_UUID, statusUuid);
  memset(characteristics, 0, sizeof(characteristics));
  memset(services, 0, sizeof(services));
  characteristics[0].uuid = &stateUuid.u;
  characteristics[0].access_cb = onAccess;
  characteristics[0].flags = BLE_GATT_CHR_F_WRITE;
  characteristics[1].uuid = &statusUuid.u;
  characteristics[1].access_cb = onAccess;
  characteristics[1].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY;
  characteristics[1].val_handle = &statusHandle;
  services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  services[0].uuid = &serviceUuid.u;
  services[0].characteristics = characteristics;
}

// ─── GAP ────────────────────────────────────────────────────────────────────

static int onGap(struct ble_gap_event *event, void *arg);

static void advertise() {
  if (!running || connected) return;
  uint8_t ownAddr;
  if (ble_hs_id_infer_auto(0, &ownAddr)) return;
  // Name and service in the advert itself (3 + 7 + 18 bytes of 31): the app
  // scans for the service.
  struct ble_hs_adv_fields fields = {};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.name = (const uint8_t *)MADRE_DEVICE_NAME;
  fields.name_len = strlen(MADRE_DEVICE_NAME);
  fields.name_is_complete = 1;
  fields.uuids128 = &serviceUuid;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  if (ble_gap_adv_set_fields(&fields)) return;
  // The radio is only on for a short window while the watch is awake, so it
  // advertises often (100–150 ms) to be found within a second.
  struct ble_gap_adv_params params = {};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  params.itvl_min = 160;  // × 0.625 ms = 100 ms
  params.itvl_max = 240;  // 150 ms
  ble_gap_adv_start(ownAddr, nullptr, BLE_HS_FOREVER, &params, onGap, nullptr);
}

static int onGap(struct ble_gap_event *event, void *arg) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        connected = true;
        connHandle = event->connect.conn_handle;
        note("telefono collegato");
      } else {
        advertise();
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      connected = false;
      connHandle = BLE_HS_CONN_HANDLE_NONE;
      rxExpected = 0;
      note("telefono scollegato");
      advertise();
      break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      advertise();
      break;
    case BLE_GAP_EVENT_MTU:
      note("mtu %u", event->mtu.value);
      break;
  }
  return 0;
}

static void onSync() {
  ble_hs_util_ensure_addr(0);
  advertise();
}

static void hostTask(void *) {
  nimble_port_run();  // returns on nimble_port_stop()
  nimble_port_freertos_deinit();
}

// ─── Start / stop ───────────────────────────────────────────────────────────

void linkStart() {
  if (running) return;
  if (!statusMutex) statusMutex = xSemaphoreCreateMutex();
  if (nimble_port_init() != ESP_OK) {
    note("bluetooth: avvio fallito");
    return;
  }
  ble_hs_cfg.sync_cb = onSync;
  ble_hs_cfg.reset_cb = [](int reason) { note("bluetooth: reset %d", reason); };
  ble_svc_gap_init();
  ble_svc_gatt_init();
  buildServices();
  if (ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services)) note("bluetooth: servizio non registrato");
  ble_svc_gap_device_name_set(MADRE_DEVICE_NAME);
  ble_att_set_preferred_mtu(517);
  rxExpected = 0;
  running = true;
  nimble_port_freertos_init(hostTask);
  note("bluetooth acceso");
}

void linkStop() {
  if (!running) return;
  running = false;
  if (connected) ble_gap_terminate(connHandle, BLE_ERR_REM_USER_CONN_TERM);
  ble_gap_adv_stop();
  nimble_port_stop();
  nimble_port_deinit();
  connected = false;
  connHandle = BLE_HS_CONN_HANDLE_NONE;
  rxExpected = 0;
  note("bluetooth spento");
}

bool linkRunning() {
  return running;
}

void linkOnPayload(void (*cb)()) {
  onPayload = cb;
}

void linkAdvertise(bool on) {
  if (!running) return;
  if (on) advertise();
  else ble_gap_adv_stop();
}

bool linkConnected() {
  return connected;
}

bool linkTakePayload(std::string &out) {
  bool got = false;
  portENTER_CRITICAL(&lock);
  if (hasReady) {
    out.swap(ready);
    ready.clear();
    hasReady = false;
    got = true;
  }
  portEXIT_CRITICAL(&lock);
  return got;
}
