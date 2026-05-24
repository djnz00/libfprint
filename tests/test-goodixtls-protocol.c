/*
 * Goodix TLS protocol helper tests
 * Copyright (C) 2026
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <glib.h>

#include "libfprint/drivers/goodixtls/goodix_proto.h"

static void
test_decode_pack_valid (void)
{
  const guint8 input[] = { 0x01, 0x02, 0x03 };
  g_autofree guint8 *encoded = NULL;
  g_autofree guint8 *payload = NULL;
  guint32 encoded_len = 0;
  guint16 payload_len = 0;
  guint8 flags = 0;
  gboolean valid_checksum = FALSE;

  goodix_encode_pack (GOODIX_FLAGS_MSG_PROTOCOL, input, sizeof (input), FALSE,
                      &encoded, &encoded_len);

  g_assert_true (goodix_decode_pack (encoded, encoded_len, &flags, &payload,
                                     &payload_len, &valid_checksum));
  g_assert_cmpint (flags, ==, GOODIX_FLAGS_MSG_PROTOCOL);
  g_assert_cmpuint (payload_len, ==, sizeof (input));
  g_assert_true (valid_checksum);
  g_assert_cmpmem (payload, payload_len, input, sizeof (input));
}

static void
test_decode_pack_rejects_short_payload (void)
{
  const guint8 input[] = { 0x01, 0x02, 0x03 };
  g_autofree guint8 *encoded = NULL;
  g_autofree guint8 *payload = NULL;
  guint32 encoded_len = 0;
  guint16 payload_len = 99;
  guint8 flags = 0xff;
  gboolean valid_checksum = TRUE;

  goodix_encode_pack (GOODIX_FLAGS_MSG_PROTOCOL, input, sizeof (input), FALSE,
                      &encoded, &encoded_len);

  g_assert_false (goodix_decode_pack (encoded, encoded_len - 1, &flags,
                                      &payload, &payload_len,
                                      &valid_checksum));
  g_assert_null (payload);
  g_assert_cmpuint (payload_len, ==, 0);
  g_assert_false (valid_checksum);
}

static void
test_decode_pack_reports_bad_checksum (void)
{
  const guint8 input[] = { 0x01 };
  g_autofree guint8 *encoded = NULL;
  g_autofree guint8 *payload = NULL;
  guint32 encoded_len = 0;
  guint16 payload_len = 0;
  guint8 flags = 0;
  gboolean valid_checksum = TRUE;

  goodix_encode_pack (GOODIX_FLAGS_MSG_PROTOCOL, input, sizeof (input), FALSE,
                      &encoded, &encoded_len);
  encoded[sizeof (GoodixPack)] ^= 0x01;

  g_assert_true (goodix_decode_pack (encoded, encoded_len, &flags, &payload,
                                     &payload_len, &valid_checksum));
  g_assert_false (valid_checksum);
}

static void
test_decode_pack_rejects_declared_length_overflow (void)
{
  const guint8 encoded[] = { GOODIX_FLAGS_MSG_PROTOCOL, 0xff, 0xff, 0x9e };
  g_autofree guint8 *payload = NULL;
  guint16 payload_len = 99;
  guint8 flags = 0xff;
  gboolean valid_checksum = TRUE;

  g_assert_false (goodix_decode_pack (encoded, sizeof (encoded), &flags,
                                      &payload, &payload_len,
                                      &valid_checksum));
  g_assert_null (payload);
  g_assert_cmpuint (payload_len, ==, 0);
  g_assert_false (valid_checksum);
}

static void
test_decode_protocol_valid_checksum (void)
{
  const guint8 input[] = { 0x10, 0x20 };
  g_autofree guint8 *encoded = NULL;
  g_autofree guint8 *payload = NULL;
  guint32 encoded_len = 0;
  guint16 payload_len = 0;
  guint8 cmd = 0;
  gboolean valid_checksum = FALSE;
  gboolean valid_null_checksum = TRUE;

  goodix_encode_protocol (GOODIX_CMD_NOP, input, sizeof (input), TRUE, FALSE,
                          &encoded, &encoded_len);

  g_assert_true (goodix_decode_protocol (encoded, encoded_len, &cmd, &payload,
                                         &payload_len, &valid_checksum,
                                         &valid_null_checksum));
  g_assert_cmpint (cmd, ==, GOODIX_CMD_NOP);
  g_assert_cmpuint (payload_len, ==, sizeof (input));
  g_assert_true (valid_checksum);
  g_assert_false (valid_null_checksum);
  g_assert_cmpmem (payload, payload_len, input, sizeof (input));
}

static void
test_decode_protocol_valid_null_checksum (void)
{
  const guint8 input[] = { 0x10, 0x20 };
  g_autofree guint8 *encoded = NULL;
  g_autofree guint8 *payload = NULL;
  guint32 encoded_len = 0;
  guint16 payload_len = 0;
  guint8 cmd = 0;
  gboolean valid_checksum = TRUE;
  gboolean valid_null_checksum = FALSE;

  goodix_encode_protocol (GOODIX_CMD_NOP, input, sizeof (input), FALSE, FALSE,
                          &encoded, &encoded_len);

  g_assert_true (goodix_decode_protocol (encoded, encoded_len, &cmd, &payload,
                                         &payload_len, &valid_checksum,
                                         &valid_null_checksum));
  g_assert_false (valid_checksum);
  g_assert_true (valid_null_checksum);
}

static void
test_decode_protocol_valid_wrapped_checksum (void)
{
  const guint8 encoded[] = { GOODIX_CMD_RESET, 0x04, 0x00, 0x01, 0x00, 0x08,
                             0xfb };
  g_autofree guint8 *payload = NULL;
  guint16 payload_len = 0;
  guint8 cmd = 0;
  gboolean valid_checksum = FALSE;
  gboolean valid_null_checksum = TRUE;

  g_assert_true (goodix_decode_protocol (encoded, sizeof (encoded), &cmd,
                                         &payload, &payload_len,
                                         &valid_checksum,
                                         &valid_null_checksum));
  g_assert_cmpint (cmd, ==, GOODIX_CMD_RESET);
  g_assert_cmpuint (payload_len, ==, 3);
  g_assert_true (valid_checksum);
  g_assert_false (valid_null_checksum);
}

static void
test_decode_protocol_rejects_zero_length_field (void)
{
  guint8 encoded[] = { GOODIX_CMD_NOP, 0x00, 0x00, 0x00 };
  g_autofree guint8 *payload = NULL;
  guint16 payload_len = 99;
  guint8 cmd = 0xff;
  gboolean valid_checksum = TRUE;
  gboolean valid_null_checksum = TRUE;

  g_assert_false (goodix_decode_protocol (encoded, sizeof (encoded), &cmd,
                                          &payload, &payload_len,
                                          &valid_checksum,
                                          &valid_null_checksum));
  g_assert_null (payload);
  g_assert_cmpuint (payload_len, ==, 0);
  g_assert_false (valid_checksum);
  g_assert_false (valid_null_checksum);
}

static void
test_decode_protocol_rejects_short_payload (void)
{
  const guint8 input[] = { 0x10, 0x20 };
  g_autofree guint8 *encoded = NULL;
  g_autofree guint8 *payload = NULL;
  guint32 encoded_len = 0;
  guint16 payload_len = 99;
  guint8 cmd = 0xff;
  gboolean valid_checksum = TRUE;
  gboolean valid_null_checksum = TRUE;

  goodix_encode_protocol (GOODIX_CMD_NOP, input, sizeof (input), TRUE, FALSE,
                          &encoded, &encoded_len);

  g_assert_false (goodix_decode_protocol (encoded, encoded_len - 1, &cmd,
                                          &payload, &payload_len,
                                          &valid_checksum,
                                          &valid_null_checksum));
  g_assert_null (payload);
  g_assert_cmpuint (payload_len, ==, 0);
  g_assert_false (valid_checksum);
  g_assert_false (valid_null_checksum);
}

static void
test_decode_protocol_reports_bad_checksum (void)
{
  const guint8 input[] = { 0x10 };
  g_autofree guint8 *encoded = NULL;
  g_autofree guint8 *payload = NULL;
  guint32 encoded_len = 0;
  guint16 payload_len = 0;
  guint8 cmd = 0;
  gboolean valid_checksum = TRUE;
  gboolean valid_null_checksum = TRUE;

  goodix_encode_protocol (GOODIX_CMD_NOP, input, sizeof (input), TRUE, FALSE,
                          &encoded, &encoded_len);
  encoded[encoded_len - 1] ^= 0x01;

  g_assert_true (goodix_decode_protocol (encoded, encoded_len, &cmd, &payload,
                                         &payload_len, &valid_checksum,
                                         &valid_null_checksum));
  g_assert_false (valid_checksum);
  g_assert_false (valid_null_checksum);
}

static void
test_decode_protocol_rejects_declared_length_overflow (void)
{
  const guint8 encoded[] = { GOODIX_CMD_NOP, 0xff, 0xff, 0x00 };
  g_autofree guint8 *payload = NULL;
  guint16 payload_len = 99;
  guint8 cmd = 0xff;
  gboolean valid_checksum = TRUE;
  gboolean valid_null_checksum = TRUE;

  g_assert_false (goodix_decode_protocol (encoded, sizeof (encoded), &cmd,
                                          &payload, &payload_len,
                                          &valid_checksum,
                                          &valid_null_checksum));
  g_assert_null (payload);
  g_assert_cmpuint (payload_len, ==, 0);
  g_assert_false (valid_checksum);
  g_assert_false (valid_null_checksum);
}

static void
test_decode_tls_data_strips_prefix (void)
{
  const guint8 encoded[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x16, 0x03, 0x03,
  };
  const guint8 *tls_data = NULL;
  guint16 tls_data_len = 0;

  g_assert_true (goodix_decode_tls_data (encoded, sizeof (encoded), &tls_data,
                                         &tls_data_len));
  g_assert_cmpuint (tls_data_len, ==, 3);
  g_assert_cmpmem (tls_data, tls_data_len, encoded + GOODIX_TLS_DATA_PREFIX_LEN,
                   3);
}

static void
test_decode_tls_data_rejects_short_payload (void)
{
  const guint8 encoded[GOODIX_TLS_DATA_PREFIX_LEN] = { 0 };
  const guint8 *tls_data = (const guint8 *) 0x1;
  guint16 tls_data_len = 99;

  g_assert_false (goodix_decode_tls_data (encoded, sizeof (encoded), &tls_data,
                                          &tls_data_len));
  g_assert_null (tls_data);
  g_assert_cmpuint (tls_data_len, ==, 0);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/goodixtls/protocol/decode-pack-valid",
                   test_decode_pack_valid);
  g_test_add_func ("/goodixtls/protocol/decode-pack-short-payload",
                   test_decode_pack_rejects_short_payload);
  g_test_add_func ("/goodixtls/protocol/decode-pack-bad-checksum",
                   test_decode_pack_reports_bad_checksum);
  g_test_add_func ("/goodixtls/protocol/decode-pack-length-overflow",
                   test_decode_pack_rejects_declared_length_overflow);
  g_test_add_func ("/goodixtls/protocol/decode-protocol-valid-checksum",
                   test_decode_protocol_valid_checksum);
  g_test_add_func ("/goodixtls/protocol/decode-protocol-valid-null-checksum",
                   test_decode_protocol_valid_null_checksum);
  g_test_add_func ("/goodixtls/protocol/decode-protocol-valid-wrapped-checksum",
                   test_decode_protocol_valid_wrapped_checksum);
  g_test_add_func ("/goodixtls/protocol/decode-protocol-zero-length-field",
                   test_decode_protocol_rejects_zero_length_field);
  g_test_add_func ("/goodixtls/protocol/decode-protocol-short-payload",
                   test_decode_protocol_rejects_short_payload);
  g_test_add_func ("/goodixtls/protocol/decode-protocol-bad-checksum",
                   test_decode_protocol_reports_bad_checksum);
  g_test_add_func ("/goodixtls/protocol/decode-protocol-length-overflow",
                   test_decode_protocol_rejects_declared_length_overflow);
  g_test_add_func ("/goodixtls/protocol/decode-tls-data-strips-prefix",
                   test_decode_tls_data_strips_prefix);
  g_test_add_func ("/goodixtls/protocol/decode-tls-data-short-payload",
                   test_decode_tls_data_rejects_short_payload);

  return g_test_run ();
}
