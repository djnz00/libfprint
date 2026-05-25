/*
 * Goodix TLS 52xd frame helper tests
 * Copyright (C) 2026
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <glib.h>
#include <string.h>

#include "fp-device.h"
#include "libfprint/drivers/goodixtls/goodix52xd_proto.h"

static void
encode_packed_pixels (guint8         chunk[6],
                      Goodix52xdPix p0,
                      Goodix52xdPix p1,
                      Goodix52xdPix p2,
                      Goodix52xdPix p3)
{
  chunk[0] = ((p0 >> 8) & 0x0f) | ((p1 & 0x0f) << 4);
  chunk[1] = p0 & 0xff;
  chunk[2] = p2 & 0xff;
  chunk[3] = (p1 >> 4) & 0xff;
  chunk[4] = (p3 >> 4) & 0xff;
  chunk[5] = ((p2 >> 8) & 0x0f) | ((p3 & 0x0f) << 4);
}

static void
test_decode_le16_frame (void)
{
  g_autofree guint8 *raw = g_malloc0 (GOODIX52XD_LE16_FRAME_SIZE);
  Goodix52xdPix frame[GOODIX52XD_FRAME_SIZE] = { 0 };

  for (guint i = 0; i < GOODIX52XD_FRAME_SIZE; i++)
    {
      guint16 value = GUINT16_TO_LE ((i * 17) & 0x0fff);

      memcpy (raw + i * sizeof (value), &value, sizeof (value));
    }

  g_assert_true (goodix52xd_decode_frame (frame,
                                          raw,
                                          GOODIX52XD_LE16_FRAME_SIZE,
                                          NULL));
  g_assert_cmpuint (frame[0], ==, 0);
  g_assert_cmpuint (frame[1], ==, 17);
  g_assert_cmpuint (frame[GOODIX52XD_FRAME_SIZE - 1], ==,
                    ((GOODIX52XD_FRAME_SIZE - 1) * 17) & 0x0fff);
}

static void
test_decode_packed_frame (void)
{
  g_autofree guint8 *raw = g_malloc0 (GOODIX52XD_PACKED_FRAME_SIZE);
  Goodix52xdPix frame[GOODIX52XD_FRAME_SIZE] = { 0 };

  encode_packed_pixels (raw, 0x001, 0x234, 0xabc, 0xfff);

  g_assert_true (goodix52xd_decode_frame (frame,
                                          raw,
                                          GOODIX52XD_PACKED_FRAME_SIZE,
                                          NULL));
  g_assert_cmpuint (frame[0], ==, 0x001);
  g_assert_cmpuint (frame[1], ==, 0x234);
  g_assert_cmpuint (frame[2], ==, 0xabc);
  g_assert_cmpuint (frame[3], ==, 0xfff);
}

static void
test_decode_packed_frame_with_trailer (void)
{
  g_autofree guint8 *raw = g_malloc0 (GOODIX52XD_PACKED_FRAME_WITH_TRAILER_SIZE);
  Goodix52xdPix frame[GOODIX52XD_FRAME_SIZE] = { 0 };

  encode_packed_pixels (raw, 0x010, 0x020, 0x030, 0x040);
  memset (raw + GOODIX52XD_PACKED_FRAME_SIZE, 0xff, 4);

  g_assert_true (goodix52xd_decode_frame (frame,
                                          raw,
                                          GOODIX52XD_PACKED_FRAME_WITH_TRAILER_SIZE,
                                          NULL));
  g_assert_cmpuint (frame[0], ==, 0x010);
  g_assert_cmpuint (frame[1], ==, 0x020);
  g_assert_cmpuint (frame[2], ==, 0x030);
  g_assert_cmpuint (frame[3], ==, 0x040);
}

static void
test_decode_rejects_short_le16_frame (void)
{
  g_autofree guint8 *raw = g_malloc0 (GOODIX52XD_LE16_FRAME_SIZE);
  Goodix52xdPix frame[GOODIX52XD_FRAME_SIZE] = { 0 };
  g_autoptr(GError) error = NULL;

  g_assert_false (goodix52xd_decode_frame (frame,
                                           raw,
                                           GOODIX52XD_LE16_FRAME_SIZE - 1,
                                           &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID);
  g_assert_nonnull (strstr (error->message, "invalid length"));
}

static void
test_decode_rejects_oversized_frame (void)
{
  g_autofree guint8 *raw = g_malloc0 (GOODIX52XD_LE16_FRAME_SIZE + 1);
  Goodix52xdPix frame[GOODIX52XD_FRAME_SIZE] = { 0 };
  g_autoptr(GError) error = NULL;

  g_assert_false (goodix52xd_decode_frame (frame,
                                           raw,
                                           GOODIX52XD_LE16_FRAME_SIZE + 1,
                                           &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID);
}

static void
test_empty_heuristic_accepts_static_empty_envelope (void)
{
  Goodix52xdPix frame[GOODIX52XD_FRAME_SIZE];
  Goodix52xdFrameStats stats = { 0 };

  for (guint i = 0; i < GOODIX52XD_FRAME_SIZE; i++)
    frame[i] = 2015 + (i % 64);

  g_assert_true (goodix52xd_frame_is_empty (frame, &stats));
  g_assert_cmpuint (stats.mean, >=, GOODIX52XD_EMPTY_MEAN_MIN);
  g_assert_cmpuint (stats.mean, <=, GOODIX52XD_EMPTY_MEAN_MAX);
  g_assert_cmpuint (stats.high_pixels, ==, 0);
}

static void
test_empty_heuristic_rejects_high_pixels (void)
{
  Goodix52xdPix frame[GOODIX52XD_FRAME_SIZE];
  Goodix52xdFrameStats stats = { 0 };

  for (guint i = 0; i < GOODIX52XD_FRAME_SIZE; i++)
    frame[i] = 2015;

  for (guint i = 0; i <= GOODIX52XD_EMPTY_HIGH_PIXEL_MAX; i++)
    frame[i] = GOODIX52XD_EMPTY_HIGH_PIXEL_THRESHOLD + 1;

  g_assert_false (goodix52xd_frame_is_empty (frame, &stats));
  g_assert_cmpuint (stats.high_pixels, >, GOODIX52XD_EMPTY_HIGH_PIXEL_MAX);
}

static void
test_empty_heuristic_rejects_mean_shift (void)
{
  Goodix52xdPix frame[GOODIX52XD_FRAME_SIZE];
  Goodix52xdFrameStats stats = { 0 };

  for (guint i = 0; i < GOODIX52XD_FRAME_SIZE; i++)
    frame[i] = GOODIX52XD_EMPTY_MEAN_MIN - 1;

  g_assert_false (goodix52xd_frame_is_empty (frame, &stats));
  g_assert_cmpuint (stats.mean, <, GOODIX52XD_EMPTY_MEAN_MIN);
}

static void
test_empty_heuristic_accepts_saturated_empty (void)
{
  Goodix52xdPix frame[GOODIX52XD_FRAME_SIZE];
  Goodix52xdFrameStats stats = { 0 };

  for (guint i = 0; i < GOODIX52XD_FRAME_SIZE; i++)
    frame[i] = 4095;

  g_assert_true (goodix52xd_frame_is_empty (frame, &stats));
  g_assert_cmpuint (stats.mean, >=, GOODIX52XD_EMPTY_SATURATED_MEAN_MIN);
  g_assert_cmpuint (stats.high_pixels, >=,
                    GOODIX52XD_EMPTY_SATURATED_HIGH_PIXEL_MIN);
}

static void
test_empty_heuristic_accepts_saturated_with_sparse_noise (void)
{
  Goodix52xdPix frame[GOODIX52XD_FRAME_SIZE];
  Goodix52xdFrameStats stats = { 0 };

  for (guint i = 0; i < GOODIX52XD_FRAME_SIZE; i++)
    frame[i] = 4095;

  for (guint i = 0;
       i < GOODIX52XD_FRAME_SIZE - GOODIX52XD_EMPTY_SATURATED_HIGH_PIXEL_MIN;
       i++)
    frame[i] = 0;

  g_assert_true (goodix52xd_frame_is_empty (frame, &stats));
  g_assert_cmpuint (stats.high_pixels, >=,
                    GOODIX52XD_EMPTY_SATURATED_HIGH_PIXEL_MIN);
}

static void
test_empty_heuristic_rejects_saturated_with_ridges (void)
{
  Goodix52xdPix frame[GOODIX52XD_FRAME_SIZE];
  Goodix52xdFrameStats stats = { 0 };

  for (guint i = 0; i < GOODIX52XD_FRAME_SIZE; i++)
    frame[i] = 4095;

  for (guint i = 0;
       i <= GOODIX52XD_FRAME_SIZE - GOODIX52XD_EMPTY_SATURATED_HIGH_PIXEL_MIN;
       i++)
    frame[i] = 0;

  g_assert_false (goodix52xd_frame_is_empty (frame, &stats));
  g_assert_cmpuint (stats.high_pixels, <,
                    GOODIX52XD_EMPTY_SATURATED_HIGH_PIXEL_MIN);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/goodixtls52xd/frame/decode-le16",
                   test_decode_le16_frame);
  g_test_add_func ("/goodixtls52xd/frame/decode-packed",
                   test_decode_packed_frame);
  g_test_add_func ("/goodixtls52xd/frame/decode-packed-with-trailer",
                   test_decode_packed_frame_with_trailer);
  g_test_add_func ("/goodixtls52xd/frame/reject-short-le16",
                   test_decode_rejects_short_le16_frame);
  g_test_add_func ("/goodixtls52xd/frame/reject-oversized",
                   test_decode_rejects_oversized_frame);
  g_test_add_func ("/goodixtls52xd/frame/empty-heuristic/static-envelope",
                   test_empty_heuristic_accepts_static_empty_envelope);
  g_test_add_func ("/goodixtls52xd/frame/empty-heuristic/high-pixels",
                   test_empty_heuristic_rejects_high_pixels);
  g_test_add_func ("/goodixtls52xd/frame/empty-heuristic/mean-shift",
                   test_empty_heuristic_rejects_mean_shift);
  g_test_add_func ("/goodixtls52xd/frame/empty-heuristic/saturated-empty",
                   test_empty_heuristic_accepts_saturated_empty);
  g_test_add_func ("/goodixtls52xd/frame/empty-heuristic/saturated-sparse-noise",
                   test_empty_heuristic_accepts_saturated_with_sparse_noise);
  g_test_add_func ("/goodixtls52xd/frame/empty-heuristic/saturated-with-ridges",
                   test_empty_heuristic_rejects_saturated_with_ridges);

  return g_test_run ();
}
