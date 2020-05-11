#ifdef WIN32
#include <windows.h>
#endif

#include "../../../core/util/log.h"
#include "../../../third-party/hidapi/hidapi/hidapi.h"
#include "device_apdu_commands.h"
#include <memory.h>
#include <stdbool.h>
#include <stdlib.h>

static uint8_t CHANNEL[] = {0x01, 0x01};

const uint8_t CLA                = 0x80;
const uint8_t INS_GET_PUBLIC_KEY = 0x04;
const uint8_t INS_SIGN           = 0x02;
const uint8_t P1_MORE            = 0x00;
const uint8_t P1_FINAL           = 0X80;
const uint8_t P2_FINAL           = 0X00;
const uint8_t TAG                = 0x05;

void wrap_apdu(bytes_t i_apdu, uint16_t seq, bytes_t* o_wrapped_hid_cmd) {

  uint16_t apdu_len = (uint16_t) i_apdu.len;
  uint8_t  data[2];
  int      index = 0;
  uint8_t  cmd[HID_CMD_MAX_LEN];

  memset(cmd + index, 0x00, sizeof(cmd));

  memcpy(cmd + index, CHANNEL, sizeof(CHANNEL));
  index += sizeof(CHANNEL);

  memcpy(cmd + index, &TAG, 1);
  index += 1;

  len_to_bytes(seq, data);
  in3_log_debug("data[0] %d data[1] %d\n", data[0], data[1]);
  memcpy(cmd + index, data, sizeof(data));
  index += sizeof(data);

  len_to_bytes(apdu_len, data);
  in3_log_debug("data[0] %d data[1] %d\n", data[0], data[1]);
  memcpy(cmd + index, data, sizeof(data));
  index += sizeof(data);

  memcpy(cmd + index, i_apdu.data, i_apdu.len);

  o_wrapped_hid_cmd->len  = HID_CMD_MAX_LEN;
  o_wrapped_hid_cmd->data = malloc(HID_CMD_MAX_LEN);

  memcpy(o_wrapped_hid_cmd->data, cmd, HID_CMD_MAX_LEN);
}

void unwrap_apdu(bytes_t i_wrapped_hid_cmd, bytes_t* o_apdu_res) {
  uint8_t buf[2];
  buf[0] = i_wrapped_hid_cmd.data[5];
  buf[1] = i_wrapped_hid_cmd.data[6];

  int len = bytes_to_len(buf);

  o_apdu_res->len  = len;
  o_apdu_res->data = malloc(len);
  memcpy(o_apdu_res->data, len, i_wrapped_hid_cmd.data + 7);
}

int len_to_bytes(uint16_t x, uint8_t* buf) {

  buf[1] = (uint8_t)(x & 0xFF);
  buf[0] = (uint8_t)((x >> 8) & 0xFF);
  return 2;
}

uint16_t bytes_to_len(uint8_t* buf) {
  uint16_t number = (buf[1] << 8) + buf[0];
  return number;
}