
#include "../../core/util/error.h"
#include "../../core/util/mem.h"
#include "../../core/util/utils.h"
#include "../../third-party/crypto/sha2.h"
#include "../../third-party/libbase58/libbase58.h"
#include "../../third-party/multihash/hashes.h"
#include "../../third-party/multihash/multihash.h"
#include "../../third-party/nanopb/pb_decode.h"
#include "../../third-party/nanopb/pb_encode.h"
#include "ipfs.pb.h"
#include <stdio.h>
#include <stdlib.h>

#define GOTO_RET(label, val) \
  do {                       \
    ret = val;               \
    goto label;              \
  } while (0)

typedef struct data_ {
  size_t         len;
  const uint8_t* buf;
} cb_arg_bytes_t;

static bool cb_encode_bytes(pb_ostream_t* stream, const pb_field_t* field, void* const* arg) {
  cb_arg_bytes_t* data = *arg;
  return pb_encode_tag_for_field(stream, field) && pb_encode_string(stream, data->buf, data->len);
}

static size_t pb_encode_size(const pb_msgdesc_t* fields, const void* src_struct) {
  pb_ostream_t s_ = PB_OSTREAM_SIZING;
  if (pb_encode(&s_, fields, src_struct))
    return s_.bytes_written;
  return 0;
}

static in3_ret_t ipfs_create_hash(const uint8_t* content, size_t len, int hash, char** b58) {
  in3_ret_t      ret = IN3_OK;
  cb_arg_bytes_t tmp = {.buf = NULL, .len = 0};
  pb_ostream_t   stream;
  size_t         wlen = 0;
  uint8_t *      buf1 = NULL, *buf2 = NULL, *out = NULL;

  Data data              = Data_init_zero;
  data.Type              = Data_DataType_File;
  data.has_filesize      = true;
  data.filesize          = len;
  data.Data.funcs.encode = &cb_encode_bytes;
  tmp.buf                = content;
  tmp.len                = len;
  data.Data.arg          = &tmp;

  wlen = pb_encode_size(Data_fields, &data);
  if ((buf1 = _malloc(wlen)) == NULL)
    GOTO_RET(EXIT, IN3_ENOMEM);

  stream = pb_ostream_from_buffer(buf1, wlen);
  if (!pb_encode(&stream, Data_fields, &data))
    GOTO_RET(EXIT, IN3_EUNKNOWN);

  PBNode node            = PBNode_init_zero;
  node.Data.funcs.encode = &cb_encode_bytes;
  tmp.buf                = buf1;
  tmp.len                = stream.bytes_written;
  node.Data.arg          = &tmp;

  wlen = pb_encode_size(PBNode_fields, &node);
  if ((buf2 = _malloc(wlen)) == NULL)
    GOTO_RET(EXIT, IN3_ENOMEM);

  stream = pb_ostream_from_buffer(buf2, wlen);
  if (!pb_encode(&stream, PBNode_fields, &node))
    GOTO_RET(EXIT, IN3_EUNKNOWN);

  uint8_t* digest     = NULL;
  size_t   digest_len = 0;
  if (hash == MH_H_SHA2_256) {
    uint8_t d_[32] = {0};
    digest_len     = 32;
    SHA256_CTX ctx;
    sha256_Init(&ctx);
    sha256_Update(&ctx, buf2, stream.bytes_written);
    sha256_Final(&ctx, d_);
    digest = d_;
  }

  if (digest == NULL)
    GOTO_RET(EXIT, IN3_ENOTSUP);

  size_t mhlen = mh_new_length(hash, digest_len);
  if ((out = _malloc(mhlen)) == NULL)
    GOTO_RET(EXIT, IN3_ENOMEM);

  if (mh_new(out, hash, digest, digest_len) < 0)
    GOTO_RET(EXIT, IN3_EUNKNOWN);

  size_t b58sz = 64;
  *b58         = _malloc(b58sz);
  if (!b58enc(*b58, &b58sz, out, mhlen))
    ret = IN3_EUNKNOWN;

EXIT:
  _free(out);
  _free(buf2);
  _free(buf1);
  return ret;
}

in3_ret_t ipfs_verify_hash(const char* content, const char* encoding, const char* requsted_hash) {
  bytes_t* buf;
  if (!strcmp(encoding, "hex"))
    buf = hex_to_new_bytes(content, strlen(content));
  else if (!strcmp(encoding, "utf8"))
    buf = b_new(content, strlen(content));
  else
    return IN3_ENOTSUP;

  if (buf == NULL)
    return IN3_ENOMEM;

  char*     out = NULL;
  in3_ret_t ret = ipfs_create_hash(buf->data, buf->len, MH_H_SHA2_256, &out);
  if (ret == IN3_OK)
    ret = !strcmp(requsted_hash, out) ? IN3_OK : IN3_EINVALDT;
  b_free(buf);
  return ret;
}
