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

#include <arpa/inet.h>
#include <errno.h>
#include <gio/gio.h>
#include <glib.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "drivers_api.h"
#include "fp-device.h"
#include "fpi-device.h"
#include "glibconfig.h"
#include "goodix.h"
#include "goodixtls.h"

#define GOODIX_TLS_CIPHER_LIST "PSK-AES128-CBC-SHA256"

static GError* err_from_ssl(void)
{
    unsigned long code = ERR_get_error();
    const char* msg = ERR_reason_error_string(code);
    return g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED,
                               msg ? msg : "TLS operation failed");
}

static unsigned int tls_server_psk_server_callback(SSL *ssl,
                                                   const char *identity,
                                                   unsigned char *psk,
                                                   unsigned int max_psk_len) {
  GoodixTlsServer *self = SSL_get_app_data(ssl);

  if (!self || !self->psk || !self->psk_len) {
    fp_dbg("Goodix TLS PSK is not configured");
    return 0;
  }

  fp_dbg("PSK WANTED %d", max_psk_len);
  if (self->psk_len > max_psk_len) {
    fp_dbg("Provided PSK is too long for OpenSSL");
    return 0;
  }

  memcpy(psk, self->psk, self->psk_len);
  return self->psk_len;
}

static SSL_CTX* tls_server_create_ctx(void)
{
    const SSL_METHOD* method;

    method = TLS_server_method();

    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) {
        return NULL;
    }

    return ctx;
}

static void
goodix_tls_close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static gboolean tls_server_config_ctx(SSL_CTX* ctx, GError** error)
{
    SSL_CTX_set_ecdh_auto(ctx, 1);
    SSL_CTX_set_dh_auto(ctx, 1);
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_psk_server_callback(ctx, tls_server_psk_server_callback);
    if (SSL_CTX_set_cipher_list(ctx, GOODIX_TLS_CIPHER_LIST) != 1) {
        *error = err_from_ssl();
        return FALSE;
    }

    return TRUE;
}

int goodix_tls_client_send(GoodixTlsServer* self, guint8* data, guint16 length)
{
    return write(self->client_fd, data, length * sizeof(guint8));
}
int goodix_tls_client_recv(GoodixTlsServer* self, guint8* data, guint16 length) {
    return read(self->client_fd, data, length * sizeof(guint8));
}

int goodix_tls_server_receive(GoodixTlsServer* self, guint8* data,
                              guint32 length, GError** error)
{
    int retr = SSL_read(self->ssl_layer, data, length * sizeof(guint8));
    if (retr <= 0) {
        *error = err_from_ssl();
    }
    return retr;
}

static gboolean tls_config_ssl(SSL* ssl, GError** error)
{
    SSL_set_min_proto_version(ssl, TLS1_2_VERSION);
    SSL_set_max_proto_version(ssl, TLS1_2_VERSION);
    SSL_set_psk_server_callback(ssl, tls_server_psk_server_callback);
    if (SSL_set_cipher_list(ssl, GOODIX_TLS_CIPHER_LIST) != 1) {
        *error = err_from_ssl();
        return FALSE;
    }

    return TRUE;
}

static void* goodix_tls_init_serve(void* me)
{
    GoodixTlsServer* self = me;
    g_autoptr(GError) error = NULL;

    fp_dbg("TLS server waiting to accept...");
    int retr = SSL_accept(self->ssl_layer);
    fp_dbg("TLS server accept done");
    if (g_atomic_int_get(&self->shutting_down))
        return NULL;

    if (retr <= 0) {
        error = err_from_ssl();
        self->connection_callback(self, error, self->user_data);
    }
    else {
        self->connection_callback(self, NULL, self->user_data);
    }
    return NULL;
}

gboolean goodix_tls_server_deinit(GoodixTlsServer* self, GError** error)
{
    gboolean success = TRUE;

    g_atomic_int_set(&self->shutting_down, TRUE);

    if (self->client_fd >= 0)
        shutdown(self->client_fd, SHUT_RDWR);
    if (self->sock_fd >= 0)
        shutdown(self->sock_fd, SHUT_RDWR);

    if (g_atomic_int_get(&self->serve_thread_started)) {
        gint ret = pthread_join(self->serve_thread, NULL);

        if (ret != 0) {
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(ret),
                        "failed to join Goodix TLS server thread: %s",
                        g_strerror(ret));
            success = FALSE;
        }
        g_atomic_int_set(&self->serve_thread_started, FALSE);
    }

    if (self->ssl_layer) {
        SSL_shutdown(self->ssl_layer);
        SSL_free(self->ssl_layer);
        self->ssl_layer = NULL;
    }

    goodix_tls_close_fd(&self->client_fd);
    goodix_tls_close_fd(&self->sock_fd);

    if (self->ssl_ctx) {
        SSL_CTX_free(self->ssl_ctx);
        self->ssl_ctx = NULL;
    }

    return success;
}

gboolean goodix_tls_server_init(GoodixTlsServer* self, GError** error)
{
    g_assert(self->connection_callback);
    self->sock_fd = -1;
    self->client_fd = -1;
    g_atomic_int_set(&self->shutting_down, FALSE);
    g_atomic_int_set(&self->serve_thread_started, FALSE);

    if (!self->psk || !self->psk_len) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Goodix TLS PSK is not configured");
        return FALSE;
    }

    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    SSL_library_init();
    self->ssl_ctx = tls_server_create_ctx();
    if (self->ssl_ctx == NULL) {
        fp_dbg("Unable to create TLS server context\n");
        *error = fpi_device_error_new_msg(FP_DEVICE_ERROR_GENERAL,
                                          "Unable to create TLS server context");
        return FALSE;
    }
    if (!tls_server_config_ctx(self->ssl_ctx, error)) {
        SSL_CTX_free(self->ssl_ctx);
        self->ssl_ctx = NULL;
        return FALSE;
    }

    int socks[2] = {0, 0};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, socks) != 0) {
        g_set_error(error, G_FILE_ERROR, errno,
                    "failed to create socket pair: %s", strerror(errno));
        SSL_CTX_free(self->ssl_ctx);
        self->ssl_ctx = NULL;
        return FALSE;
    }
    self->sock_fd = socks[0];
    self->client_fd = socks[1];

    self->ssl_layer = SSL_new(self->ssl_ctx);
    if (self->ssl_layer == NULL) {
        *error = err_from_ssl();
        goodix_tls_close_fd(&self->client_fd);
        goodix_tls_close_fd(&self->sock_fd);
        SSL_CTX_free(self->ssl_ctx);
        self->ssl_ctx = NULL;
        return FALSE;
    }
    if (!tls_config_ssl(self->ssl_layer, error)) {
        SSL_free(self->ssl_layer);
        self->ssl_layer = NULL;
        goodix_tls_close_fd(&self->client_fd);
        goodix_tls_close_fd(&self->sock_fd);
        SSL_CTX_free(self->ssl_ctx);
        self->ssl_ctx = NULL;
        return FALSE;
    }
    SSL_set_app_data(self->ssl_layer, self);
    SSL_set_fd(self->ssl_layer, self->sock_fd);

    gint ret = pthread_create(&self->serve_thread, 0, goodix_tls_init_serve, self);
    if (ret != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(ret),
                    "failed to start Goodix TLS server thread: %s",
                    g_strerror(ret));
        SSL_free(self->ssl_layer);
        self->ssl_layer = NULL;
        goodix_tls_close_fd(&self->client_fd);
        goodix_tls_close_fd(&self->sock_fd);
        SSL_CTX_free(self->ssl_ctx);
        self->ssl_ctx = NULL;
        return FALSE;
    }
    g_atomic_int_set(&self->serve_thread_started, TRUE);

    return TRUE;
}
