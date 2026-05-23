// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>

#include "goodix_proto.h"

guint8 goodix_calc_checksum(const guint8 *data, guint32 length) {
  guint8 checksum = 0;

  for (guint32 i = 0; i < length; i++) checksum += data[i];

  return checksum;
}

void goodix_encode_pack(guint8 flags, const guint8 *payload, guint16 payload_len,
                        gboolean pad_data, guint8 **data, guint32 *data_len) {
  GoodixPack *pack;

  g_return_if_fail(data != NULL);
  g_return_if_fail(data_len != NULL);
  g_return_if_fail(payload != NULL || payload_len == 0);

  *data_len = sizeof(GoodixPack) + sizeof(guint8) + payload_len;

  if (pad_data && *data_len % GOODIX_EP_OUT_MAX_BUF_SIZE)
    *data_len +=
        GOODIX_EP_OUT_MAX_BUF_SIZE - *data_len % GOODIX_EP_OUT_MAX_BUF_SIZE;

  *data = g_malloc0(*data_len);
  pack = (GoodixPack *)*data;

  pack->flags = flags;
  pack->length = GUINT16_TO_LE(payload_len);
  (*data)[sizeof(GoodixPack)] = goodix_calc_checksum(*data, sizeof(GoodixPack));

  if (payload_len)
    memcpy(*data + sizeof(GoodixPack) + sizeof(guint8), payload, payload_len);
}

void goodix_encode_protocol(guint8 cmd, const guint8 *payload, guint16 payload_len,
                            gboolean calc_checksum, gboolean pad_data,
                            guint8 **data, guint32 *data_len) {
  GoodixProtocol *protocol;

  g_return_if_fail(data != NULL);
  g_return_if_fail(data_len != NULL);
  g_return_if_fail(payload != NULL || payload_len == 0);
  g_return_if_fail(payload_len <= G_MAXUINT16 - sizeof(guint8));

  *data_len = sizeof(GoodixProtocol) + payload_len + sizeof(guint8);

  if (pad_data && *data_len % GOODIX_EP_OUT_MAX_BUF_SIZE)
    *data_len +=
        GOODIX_EP_OUT_MAX_BUF_SIZE - *data_len % GOODIX_EP_OUT_MAX_BUF_SIZE;

  *data = g_malloc0(*data_len);
  protocol = (GoodixProtocol *)*data;

  protocol->cmd = cmd;
  protocol->length = GUINT16_TO_LE(payload_len + sizeof(guint8));

  if (payload_len)
    memcpy(*data + sizeof(GoodixProtocol), payload, payload_len);

  if (calc_checksum)
    (*data)[sizeof(GoodixProtocol) + payload_len] =
        0xaa -
        goodix_calc_checksum(*data, sizeof(GoodixProtocol) + payload_len);
  else
    (*data)[sizeof(GoodixProtocol) + payload_len] = GOODIX_NULL_CHECKSUM;
}

gboolean goodix_decode_pack(const guint8 *data, guint32 data_len, guint8 *flags,
                            guint8 **payload, guint16 *payload_len,
                            gboolean *valid_checksum) {
  const GoodixPack *pack = (const GoodixPack *)data;
  guint16 length;
  guint32 required_len;

  if (payload) *payload = NULL;
  if (payload_len) *payload_len = 0;
  if (valid_checksum) *valid_checksum = FALSE;

  if (!data || !flags || !payload || !payload_len || !valid_checksum)
    return FALSE;

  if (data_len < sizeof(GoodixPack) + sizeof(guint8)) return FALSE;

  length = GUINT16_FROM_LE(pack->length);
  required_len = sizeof(GoodixPack) + sizeof(guint8) + length;

  if (data_len < required_len) return FALSE;

  *flags = pack->flags;
  *payload = length ? g_memdup2(data + sizeof(GoodixPack) + sizeof(guint8), length) : NULL;
  *payload_len = length;
  *valid_checksum = goodix_calc_checksum(data, sizeof(GoodixPack)) ==
                    data[sizeof(GoodixPack)];

  return TRUE;
}

gboolean goodix_decode_protocol(const guint8 *data, guint32 data_len, guint8 *cmd,
                                guint8 **payload, guint16 *payload_len,
                                gboolean *valid_checksum,
                                gboolean *valid_null_checksum) {
  const GoodixProtocol *protocol = (const GoodixProtocol *)data;
  guint16 encoded_length;
  guint16 length;
  guint32 required_len;

  if (payload) *payload = NULL;
  if (payload_len) *payload_len = 0;
  if (valid_checksum) *valid_checksum = FALSE;
  if (valid_null_checksum) *valid_null_checksum = FALSE;

  if (!data || !cmd || !payload || !payload_len || !valid_checksum ||
      !valid_null_checksum)
    return FALSE;

  if (data_len < sizeof(GoodixProtocol) + sizeof(guint8)) return FALSE;

  encoded_length = GUINT16_FROM_LE(protocol->length);
  if (encoded_length < sizeof(guint8))
    return FALSE;

  length = encoded_length - sizeof(guint8);
  required_len = sizeof(GoodixProtocol) + encoded_length;

  if (data_len < required_len) return FALSE;

  *cmd = protocol->cmd;
  *payload = length ? g_memdup2(data + sizeof(GoodixProtocol), length) : NULL;
  *payload_len = length;
  *valid_checksum =
      (guint8)(0xaa - goodix_calc_checksum(data,
                                           sizeof(GoodixProtocol) + length)) ==
      data[sizeof(GoodixProtocol) + length];
  *valid_null_checksum =
      GOODIX_NULL_CHECKSUM == data[sizeof(GoodixProtocol) + length];

  return TRUE;
}
