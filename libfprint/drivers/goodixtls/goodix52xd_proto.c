// Goodix TLS 52xd protocol helpers for libfprint

// Copyright (C) 2026

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

#include "fp-device.h"

#include "goodix52xd_proto.h"

#include <string.h>

gboolean
goodix52xd_decode_frame (Goodix52xdPix frame[GOODIX52XD_FRAME_SIZE],
                         const guint8 *raw_frame,
                         guint16       length,
                         GError      **error)
{
  g_return_val_if_fail (frame != NULL, FALSE);
  g_return_val_if_fail (raw_frame != NULL, FALSE);

  if (length == GOODIX52XD_LE16_FRAME_SIZE)
    {
      for (guint i = 0; i != GOODIX52XD_FRAME_SIZE; ++i)
        {
          guint16 value;

          memcpy (&value, raw_frame + i * sizeof (value), sizeof (value));
          frame[i] = GUINT16_FROM_LE (value);
        }

      return TRUE;
    }

  if (length != GOODIX52XD_PACKED_FRAME_SIZE &&
      length != GOODIX52XD_PACKED_FRAME_WITH_TRAILER_SIZE)
    {
      g_set_error (error,
                   FP_DEVICE_ERROR,
                   FP_DEVICE_ERROR_DATA_INVALID,
                   "Image frame has invalid length (got %u, expected %u, %u, or %u)",
                   (guint) length,
                   (guint) GOODIX52XD_PACKED_FRAME_SIZE,
                   (guint) GOODIX52XD_PACKED_FRAME_WITH_TRAILER_SIZE,
                   (guint) GOODIX52XD_LE16_FRAME_SIZE);
      return FALSE;
    }

  Goodix52xdPix uncropped[GOODIX52XD_SCAN_WIDTH * GOODIX52XD_HEIGHT];
  Goodix52xdPix *pix = uncropped;

  for (guint i = 0; i < GOODIX52XD_PACKED_FRAME_SIZE; i += 6)
    {
      const guint8 *chunk = raw_frame + i;

      *pix++ = ((chunk[0] & 0xf) << 8) + chunk[1];
      *pix++ = (chunk[3] << 4) + (chunk[0] >> 4);
      *pix++ = ((chunk[5] & 0xf) << 8) + chunk[2];
      *pix++ = (chunk[4] << 4) + (chunk[5] >> 4);
    }

  for (guint y = 0; y != GOODIX52XD_HEIGHT; ++y)
    {
      for (guint x = 0; x != GOODIX52XD_WIDTH; ++x)
        {
          const guint idx = x + y * GOODIX52XD_SCAN_WIDTH;

          frame[x + y * GOODIX52XD_WIDTH] = uncropped[idx];
        }
    }

  return TRUE;
}

gboolean
goodix52xd_frame_is_empty (const Goodix52xdPix frame[GOODIX52XD_FRAME_SIZE],
                           Goodix52xdFrameStats *stats)
{
  Goodix52xdFrameStats local_stats = {
    .min = G_MAXUINT16,
  };
  guint64 sum = 0;

  g_return_val_if_fail (frame != NULL, TRUE);

  for (guint i = 0; i != GOODIX52XD_FRAME_SIZE; ++i)
    {
      Goodix52xdPix pix = frame[i];

      if (pix != 0)
        local_stats.nonzero++;
      if (pix < local_stats.min)
        local_stats.min = pix;
      if (pix > local_stats.max)
        local_stats.max = pix;
      if (pix > GOODIX52XD_EMPTY_HIGH_PIXEL_THRESHOLD)
        local_stats.high_pixels++;

      sum += pix;
    }

  local_stats.mean = sum / GOODIX52XD_FRAME_SIZE;

  if (stats)
    *stats = local_stats;

  if (local_stats.mean >= GOODIX52XD_EMPTY_MEAN_MIN &&
      local_stats.mean <= GOODIX52XD_EMPTY_MEAN_MAX &&
      local_stats.high_pixels <= GOODIX52XD_EMPTY_HIGH_PIXEL_MAX)
    return TRUE;

  return (local_stats.mean >= GOODIX52XD_EMPTY_SATURATED_MEAN_MIN &&
          local_stats.high_pixels >= GOODIX52XD_EMPTY_SATURATED_HIGH_PIXEL_MIN);
}
