#include <vector>

#include "bt.h"
#include "console/console.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

struct ble_hs_cfg;
struct ble_gatt_register_ctxt;

#define GATT_SVR_SVC_ALERT_UUID 0x1811

// BLE_UUID128_INIT isn't c++11 compatible
#define BT_UUID128_INIT(uuid128...)                                                                \
  { .u = {.type = BLE_UUID_TYPE_128}, .value = {uuid128}, }

/* 59462f12-9543-9999-12c8-58b459a2712d */
static const ble_uuid128_t gatt_svr_svc_uuid = BT_UUID128_INIT(
    0x2d, 0x71, 0xa2, 0x59, 0xb4, 0x58, 0xc8, 0x12, 0x99, 0x99, 0x43, 0x95, 0x12, 0x2f, 0x46, 0x59);

/* 5c3a659e-897e-45e1-b016-007107c96d00 */
static ble_uuid128_t gatt_svr_chr_base = BT_UUID128_INIT(
    0x00, 0x6d, 0xc9, 0x07, 0x71, 0x00, 0x16, 0xb0, 0xe1, 0x45, 0x7e, 0x89, 0x9e, 0x65, 0x3a, 0x5c);

/* 5c3a659e-897e-45e1-b016-007107c86d00 */
static ble_uuid128_t gatt_svr_desc_base = BT_UUID128_INIT(
    0x00, 0x6d, 0xc8, 0x07, 0x71, 0x00, 0x16, 0xb0, 0xe1, 0x45, 0x7e, 0x89, 0x9e, 0x65, 0x3a, 0x5c);

struct bt_chr_entry {
  const ble_uuid128_t chr_uuid;
  const ble_uuid128_t desc_uuid;
  const bt_chr chr;
  ble_gatt_dsc_def dscs[2];
};

static const char *tag = "BLE";
static int bt_gap_event(struct ble_gap_event *event, void *arg);
static uint8_t s_own_addr_type;
static bool s_is_enabled;
static std::vector<bt_chr_entry> s_chr_entries;
static ble_gatt_svc_def s_gatt_svr_svcs[2]{
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &gatt_svr_svc_uuid.u}, {0} // No more services
};
static std::vector<ble_gatt_chr_def> s_gatt_svr_chrs;

extern "C" void ble_store_config_init(void);

static int gatt_svr_chr_write(struct os_mbuf *om, uint16_t min_len, uint16_t max_len, void *dst,
                              uint16_t *len) {
  uint16_t om_len;
  int rc;

  om_len = OS_MBUF_PKTLEN(om);
  if (om_len < min_len || om_len > max_len) {
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  }

  rc = ble_hs_mbuf_to_flat(om, dst, max_len, len);
  if (rc != 0) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  return 0;
}

static int gatt_svr_desc_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
  const char *name = s_chr_entries.at((size_t)arg).chr.name;

  assert(ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC);
  int rc = os_mbuf_append(ctxt->om, name, strlen(name));
  return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
  int rc;
  const bt_chr *chr = &s_chr_entries.at((size_t)arg).chr;

  switch (ctxt->op) {
  case BLE_GATT_ACCESS_OP_READ_CHR:
    assert(chr->readable);

    size_t bytes;
    rc = chr->access_cb(&bytes, chr, BtOp::REQUEST_READ);
    if (rc > 0) {
      return rc;
    }
    assert(bytes <= chr->bufferSize);

    rc = os_mbuf_append(ctxt->om, chr->buffer, bytes);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  case BLE_GATT_ACCESS_OP_WRITE_CHR:
    assert(chr->writable);
    uint16_t written;
    rc = gatt_svr_chr_write(ctxt->om, 0, chr->bufferSize, (void *)chr->buffer, &written);
    if (rc != 0) {
      return rc;
    }

    bytes = written;
    rc = chr->access_cb(&bytes, chr, BtOp::WRITTEN);

    return rc;
  default:
    assert(0);
    return BLE_ATT_ERR_UNLIKELY;
  }
}

void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg) {
  char buf[BLE_UUID_STR_LEN];

  switch (ctxt->op) {
  case BLE_GATT_REGISTER_OP_SVC:
    MODLOG_DFLT(DEBUG, "registered service %s with handle=%d\n",
                ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf), ctxt->svc.handle);
    break;

  case BLE_GATT_REGISTER_OP_CHR:
    MODLOG_DFLT(DEBUG,
                "registering characteristic %s with "
                "def_handle=%d val_handle=%d\n",
                ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf), ctxt->chr.def_handle,
                ctxt->chr.val_handle);
    break;

  case BLE_GATT_REGISTER_OP_DSC:
    MODLOG_DFLT(DEBUG, "registering descriptor %s with handle=%d\n",
                ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf), ctxt->dsc.handle);
    break;

  default:
    assert(0);
    break;
  }
}

/**
 * Enables advertising with the following parameters:
 *     o General discoverable mode.
 *     o Undirected connectable mode.
 */
static void bt_advertise(void) {
  struct ble_gap_adv_params adv_params;
  struct ble_hs_adv_fields fields;
  const char *name;
  int rc;

  /**
     *  Set the advertisement data included in our advertisements:
     *     o Flags (indicates advertisement type and other general info).
     *     o Advertising tx power.
     *     o Device name.
     *     o 16-bit service UUIDs (alert notifications).
     */

  memset(&fields, 0, sizeof fields);

  /* Advertise two flags:
     *     o Discoverability in forthcoming advertisement (general)
     *     o BLE-only (BR/EDR unsupported).
     */
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

  /* Indicate that the TX power level field should be included; have the
     * stack fill this value automatically.  This is done by assigning the
     * special value BLE_HS_ADV_TX_PWR_LVL_AUTO.
     */
  fields.tx_pwr_lvl_is_present = 1;
  fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

  name = ble_svc_gap_device_name();
  fields.name = (uint8_t *)name;
  fields.name_len = strlen(name);
  fields.name_is_complete = 1;

  ble_uuid16_t uuids[]{{
      .u = {BLE_UUID_TYPE_16},
      .value = GATT_SVR_SVC_ALERT_UUID,
  }};

  fields.uuids16 = uuids;
  fields.num_uuids16 = 1;
  fields.uuids16_is_complete = 1;

  rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) {
    MODLOG_DFLT(ERROR, "error setting advertisement data; rc=%d\n", rc);
    return;
  }

  /* Begin advertising. */
  memset(&adv_params, 0, sizeof adv_params);
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, bt_gap_event, NULL);
  if (rc != 0) {
    MODLOG_DFLT(ERROR, "error enabling advertisement; rc=%d\n", rc);
    return;
  }
}

/**
 * The nimble host executes this callback when a GAP event occurs.  The
 * application associates a GAP event callback with each connection that forms.
 * bleprph uses the same callback for all connections.
 *
 * @param event                 The type of event being signalled.
 * @param ctxt                  Various information pertaining to the event.
 * @param arg                   Application-specified argument; unused by
 *                                  bleprph.
 *
 * @return                      0 if the application successfully handled the
 *                                  event; nonzero on failure.  The semantics
 *                                  of the return code is specific to the
 *                                  particular GAP event being signalled.
 */
static int bt_gap_event(struct ble_gap_event *event, void *arg) {
  struct ble_gap_conn_desc desc;
  int rc;

  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    /* A new connection was established or a connection attempt failed. */
    MODLOG_DFLT(INFO, "connection %s; status=%d ",
                event->connect.status == 0 ? "established" : "failed", event->connect.status);
    if (event->connect.status == 0) {
      rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
      assert(rc == 0);
    }
    MODLOG_DFLT(INFO, "\n");

    if (event->connect.status != 0) {
      /* Connection failed; resume advertising. */
      bt_advertise();
    }
    return 0;

  case BLE_GAP_EVENT_DISCONNECT:
    MODLOG_DFLT(INFO, "disconnect; reason=%d ", event->disconnect.reason);
    MODLOG_DFLT(INFO, "\n");

    /* Connection terminated; resume advertising. */
    bt_advertise();
    return 0;

  case BLE_GAP_EVENT_CONN_UPDATE:
    /* The central has updated the connection parameters. */
    MODLOG_DFLT(INFO, "connection updated; status=%d ", event->conn_update.status);
    rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
    assert(rc == 0);
    MODLOG_DFLT(INFO, "\n");
    return 0;

  case BLE_GAP_EVENT_ADV_COMPLETE:
    MODLOG_DFLT(INFO, "advertise complete; reason=%d", event->adv_complete.reason);
    bt_advertise();
    return 0;

  case BLE_GAP_EVENT_ENC_CHANGE:
    /* Encryption has been enabled or disabled for this connection. */
    MODLOG_DFLT(INFO, "encryption change event; status=%d ", event->enc_change.status);
    rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
    assert(rc == 0);
    MODLOG_DFLT(INFO, "\n");
    return 0;

  case BLE_GAP_EVENT_SUBSCRIBE:
    MODLOG_DFLT(INFO,
                "subscribe event; conn_handle=%d attr_handle=%d "
                "reason=%d prevn=%d curn=%d previ=%d curi=%d\n",
                event->subscribe.conn_handle, event->subscribe.attr_handle, event->subscribe.reason,
                event->subscribe.prev_notify, event->subscribe.cur_notify,
                event->subscribe.prev_indicate, event->subscribe.cur_indicate);
    return 0;

  case BLE_GAP_EVENT_MTU:
    MODLOG_DFLT(INFO, "mtu update event; conn_handle=%d cid=%d mtu=%d\n", event->mtu.conn_handle,
                event->mtu.channel_id, event->mtu.value);
    return 0;

  case BLE_GAP_EVENT_REPEAT_PAIRING:
    /* We already have a bond with the peer, but it is attempting to
         * establish a new secure link.  This app sacrifices security for
         * convenience: just throw away the old bond and accept the new link.
         */

    /* Delete the old bond. */
    rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
    assert(rc == 0);
    ble_store_util_delete_peer(&desc.peer_id_addr);

    /* Return BLE_GAP_REPEAT_PAIRING_RETRY to indicate that the host should
         * continue with the pairing operation.
         */
    return BLE_GAP_REPEAT_PAIRING_RETRY;

  case BLE_GAP_EVENT_PASSKEY_ACTION:
    ESP_LOGE(tag, "PASSKEY_ACTION_EVENT started \n");
    struct ble_sm_io pkey = {0};

    if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
      pkey.action = event->passkey.params.action;
      pkey.passkey = 123456; // This is the passkey to be entered on peer
      ESP_LOGI(tag, "Enter passkey %lu on the peer side", pkey.passkey);
      rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
      ESP_LOGI(tag, "ble_sm_inject_io result: %d\n", rc);
    } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
      ESP_LOGI(tag, "Passkey BLE_SM_IOACT_NUMCMP not supported");
    } else if (event->passkey.params.action == BLE_SM_IOACT_OOB) {
      static uint8_t tem_oob[16] = {0};
      pkey.action = event->passkey.params.action;
      for (int i = 0; i < 16; i++) {
        pkey.oob[i] = tem_oob[i];
      }
      rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
      ESP_LOGI(tag, "ble_sm_inject_io result: %d\n", rc);
    } else if (event->passkey.params.action == BLE_SM_IOACT_INPUT) {
      ESP_LOGI(tag, "Passkey BLE_SM_IOACT_INPUT not supported");
    }
    return 0;
  }

  return 0;
}

static void bt_on_reset(int reason) { MODLOG_DFLT(ERROR, "Resetting state; reason=%d\n", reason); }

static void bt_on_sync(void) {
  int rc;

  rc = ble_hs_util_ensure_addr(0);
  assert(rc == 0);

  /* Figure out address to use while advertising (no privacy for now) */
  rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
  if (rc != 0) {
    MODLOG_DFLT(ERROR, "error determining address type; rc=%d\n", rc);
    return;
  }

  /* Printing ADDR */
  uint8_t addr_val[6] = {0};
  rc = ble_hs_id_copy_addr(s_own_addr_type, addr_val, NULL);
  MODLOG_DFLT(INFO, "\n");
  /* Begin advertising. */
  bt_advertise();
}

void bt_host_task(void *param) {
  ESP_LOGI(tag, "BLE Host Task Started");
  /* This function will return only when nimble_port_stop() is executed */
  nimble_port_run();

  nimble_port_freertos_deinit();
  ESP_ERROR_CHECK(esp_nimble_hci_deinit());
}

void bt_register(bt_chr chr) {
  size_t idx = s_chr_entries.size();

  ble_uuid128_t chr_uuid = gatt_svr_chr_base;
  chr_uuid.value[0] = idx;

  ble_uuid128_t desc_uuid = gatt_svr_desc_base;
  desc_uuid.value[0] = idx;

  s_chr_entries.push_back(bt_chr_entry{
      .chr_uuid = chr_uuid,
      .desc_uuid = desc_uuid,
      .chr = chr,
      .dscs = {{.att_flags = BLE_ATT_F_READ, .access_cb = gatt_svr_desc_access, .arg = (void *)idx},
               {0}}});
}

void bt_init() { /* Initialize the NimBLE host configuration. */
  ble_hs_cfg.reset_cb = bt_on_reset;
  ble_hs_cfg.sync_cb = bt_on_sync;
  ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;

  ble_svc_gap_init();
  ble_svc_gatt_init();

  for (bt_chr_entry &entry : s_chr_entries) {
    ble_gatt_chr_flags flags = 0;

    if (entry.chr.readable) {
      flags |= BLE_GATT_CHR_F_READ;
    }
    if (entry.chr.writable) {
      flags |= BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC;
    }

    entry.dscs[0].uuid = &entry.desc_uuid.u;
    s_gatt_svr_chrs.push_back(ble_gatt_chr_def{.uuid = &entry.chr_uuid.u,
                                               .access_cb = gatt_svr_chr_access,
                                               .arg = (void *)entry.chr_uuid.value[0],
                                               .descriptors = entry.dscs,
                                               .flags = flags});
  }

  s_gatt_svr_chrs.push_back({0}); // No more characteristics in this service.

  s_gatt_svr_svcs[0].characteristics = &s_gatt_svr_chrs[0];
  int rc = ble_gatts_count_cfg(s_gatt_svr_svcs);
  assert(rc == 0);

  rc = ble_gatts_add_svcs(s_gatt_svr_svcs);
  assert(rc == 0);

  rc = ble_svc_gap_device_name_set("wake-bunny");
  assert(rc == 0);

  ble_store_config_init();
}

void bt_start() {
  if (bt_is_enabled()) {
    return;
  }

  ESP_ERROR_CHECK(esp_nimble_hci_init());
  nimble_port_init();

  bt_init();

  nimble_port_freertos_init(bt_host_task);
  s_is_enabled = true;
}

void bt_stop() {
  if (!bt_is_enabled()) {
    return;
  }
  s_is_enabled = false;
  int ret = nimble_port_stop();
  if (ret == 0) {
    nimble_port_deinit();
    ret = esp_nimble_hci_deinit();
    if (ret != ESP_OK) {
      ESP_LOGE(tag, "esp_nimble_hci_and_controller_deinit() failed with error: %d", ret);
    }
  }
}

bool bt_is_enabled() { return s_is_enabled; }
