#pragma once

enum BtOp {
  REQUEST_READ,
  WRITTEN,
};

struct bt_chr;

// On read, bytes should be set to the number of bytes available
// for reading in the buffer.
// On write, bytes will contain the number of bytes written to the
// buffer.
typedef int (*bt_access_fn)(size_t *bytes, const bt_chr *chr, BtOp op);

struct bt_chr {
  const char *name;
  char *buffer;
  size_t bufferSize;
  bool readable;
  bool writable;
  bt_access_fn access_cb;
};

void bt_init();
void bt_register(bt_chr chr);
void bt_start();
void bt_stop();
bool bt_is_enabled();
