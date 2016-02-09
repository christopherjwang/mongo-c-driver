/*
 * Copyright 2016 MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mongoc-config.h"

#ifdef MONGOC_ENABLE_OPENSSL

#include <bson.h>

#include <errno.h>
#include <string.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "mongoc-counters-private.h"
#include "mongoc-errno-private.h"
#include "mongoc-stream-tls.h"
#include "mongoc-stream-private.h"
#include "mongoc-stream-tls-openssl-bio-private.h"
#include "mongoc-stream-tls-openssl-private.h"
#include "mongoc-ssl-private.h"
#include "mongoc-trace.h"
#include "mongoc-log.h"


#undef MONGOC_LOG_DOMAIN
#define MONGOC_LOG_DOMAIN "stream-tls-openssl"

#define MONGOC_STREAM_TLS_OPENSSL_BUFFER_SIZE 4096

/* Magic vtable to make our BIO shim */
static BIO_METHOD gMongocStreamTlsOpenSslRawMethods = {
   BIO_TYPE_FILTER,
   "mongoc-stream-tls-glue",
   mongoc_stream_tls_openssl_bio_write,
   mongoc_stream_tls_openssl_bio_read,
   mongoc_stream_tls_openssl_bio_puts,
   mongoc_stream_tls_openssl_bio_gets,
   mongoc_stream_tls_openssl_bio_ctrl,
   mongoc_stream_tls_openssl_bio_create,
   mongoc_stream_tls_openssl_bio_destroy
};


/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_stream_tls_openssl_destroy --
 *
 *       Cleanup after usage of a mongoc_stream_tls_openssl_t. Free all allocated
 *       resources and ensure connections are closed.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static void
_mongoc_stream_tls_openssl_destroy (mongoc_stream_t *stream)
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;
   mongoc_stream_tls_openssl_t *openssl = (mongoc_stream_tls_openssl_t *) tls->ctx;

   BSON_ASSERT (tls);

   BIO_free_all (openssl->bio);
   openssl->bio = NULL;

   mongoc_stream_destroy (tls->base_stream);
   tls->base_stream = NULL;

   SSL_CTX_free (openssl->ctx);
   openssl->ctx = NULL;

   bson_free (openssl);
   bson_free (stream);

   mongoc_counter_streams_active_dec();
   mongoc_counter_streams_disposed_inc();
}


/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_stream_tls_openssl_failed --
 *
 *       Called on stream failure. Same as _mongoc_stream_tls_openssl_destroy()
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static void
_mongoc_stream_tls_openssl_failed (mongoc_stream_t *stream)
{
   _mongoc_stream_tls_openssl_destroy (stream);
}


/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_stream_tls_openssl_close --
 *
 *       Close the underlying socket.
 *
 *       Linus dictates that you should not check the result of close()
 *       since there is a race condition with EAGAIN and a new file
 *       descriptor being opened.
 *
 * Returns:
 *       0 on success; otherwise -1.
 *
 * Side effects:
 *       The BIO fd is closed.
 *
 *--------------------------------------------------------------------------
 */

static int
_mongoc_stream_tls_openssl_close (mongoc_stream_t *stream)
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;
   int ret = 0;
   ENTRY;

   BSON_ASSERT (tls);

   ret = mongoc_stream_close (tls->base_stream);
   RETURN (ret);
}


/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_stream_tls_openssl_flush --
 *
 *       Flush the underlying stream.
 *
 * Returns:
 *       0 if successful; otherwise -1.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static int
_mongoc_stream_tls_openssl_flush (mongoc_stream_t *stream)
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;
   mongoc_stream_tls_openssl_t *openssl = (mongoc_stream_tls_openssl_t *) tls->ctx;

   BSON_ASSERT (openssl);

   return BIO_flush (openssl->bio);
}


static ssize_t
_mongoc_stream_tls_openssl_write (mongoc_stream_tls_t *tls,
                                  char                *buf,
                                  size_t               buf_len)
{
   mongoc_stream_tls_openssl_t *openssl = (mongoc_stream_tls_openssl_t *) tls->ctx;
   ssize_t ret;
   int64_t now;
   int64_t expire = 0;
   ENTRY;

   BSON_ASSERT (tls);
   BSON_ASSERT (buf);
   BSON_ASSERT (buf_len);

   if (tls->timeout_msec >= 0) {
      expire = bson_get_monotonic_time () + (tls->timeout_msec * 1000UL);
   }

   ret = BIO_write (openssl->bio, buf, buf_len);
   TRACE("BIO_write returned %ld", ret);

   TRACE("I got ret: %ld and retry: %d", ret, BIO_should_retry (openssl->bio));
   if (ret <= 0) {
      return ret;
   }

   if (expire) {
      now = bson_get_monotonic_time ();

      if ((expire - now) < 0) {
         if (ret < buf_len) {
            mongoc_counter_streams_timeout_inc();
         }

         tls->timeout_msec = 0;
      } else {
         tls->timeout_msec = (expire - now) / 1000L;
      }
   }

   RETURN (ret);
}


/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_stream_tls_openssl_writev --
 *
 *       Write the iovec to the stream. This function will try to write
 *       all of the bytes or fail. If the number of bytes is not equal
 *       to the number requested, a failure or EOF has occurred.
 *
 * Returns:
 *       -1 on failure, otherwise the number of bytes written.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static ssize_t
_mongoc_stream_tls_openssl_writev (mongoc_stream_t *stream,
                                   mongoc_iovec_t  *iov,
                                   size_t           iovcnt,
                                   int32_t          timeout_msec)
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;
   char buf[MONGOC_STREAM_TLS_OPENSSL_BUFFER_SIZE];
   ssize_t ret = 0;
   ssize_t child_ret;
   size_t i;
   size_t iov_pos = 0;

   /* There's a bit of a dance to coalesce vectorized writes into
    * MONGOC_STREAM_TLS_OPENSSL_BUFFER_SIZE'd writes to avoid lots of small tls
    * packets.
    *
    * The basic idea is that we want to combine writes in the buffer if they're
    * smaller than the buffer, flushing as it gets full.  For larger writes, or
    * the last write in the iovec array, we want to ignore the buffer and just
    * write immediately.  We take care of doing buffer writes by re-invoking
    * ourself with a single iovec_t, pointing at our stack buffer.
    */
   char *buf_head = buf;
   char *buf_tail = buf;
   char *buf_end = buf + MONGOC_STREAM_TLS_OPENSSL_BUFFER_SIZE;
   size_t bytes;

   char *to_write = NULL;
   size_t to_write_len;

   BSON_ASSERT (tls);
   BSON_ASSERT (iov);
   BSON_ASSERT (iovcnt);
   ENTRY;

   tls->timeout_msec = timeout_msec;

   for (i = 0; i < iovcnt; i++) {
      iov_pos = 0;

      while (iov_pos < iov[i].iov_len) {
         if (buf_head != buf_tail ||
             ((i + 1 < iovcnt) &&
              ((buf_end - buf_tail) > (iov[i].iov_len - iov_pos)))) {
            /* If we have either of:
             *   - buffered bytes already
             *   - another iovec to send after this one and we don't have more
             *     bytes to send than the size of the buffer.
             *
             * copy into the buffer */

            bytes = BSON_MIN (iov[i].iov_len - iov_pos, buf_end - buf_tail);

            memcpy (buf_tail, (char *) iov[i].iov_base + iov_pos, bytes);
            buf_tail += bytes;
            iov_pos += bytes;

            if (buf_tail == buf_end) {
               /* If we're full, request send */

               to_write = buf_head;
               to_write_len = buf_tail - buf_head;

               buf_tail = buf_head = buf;
            }
         } else {
            /* Didn't buffer, so just write it through */

            to_write = (char *)iov[i].iov_base + iov_pos;
            to_write_len = iov[i].iov_len - iov_pos;

            iov_pos += to_write_len;
         }

         if (to_write) {
            /* We get here if we buffered some bytes and filled the buffer, or
             * if we didn't buffer and have to send out of the iovec */

            child_ret = _mongoc_stream_tls_openssl_write (tls, to_write, to_write_len);
            if (child_ret != to_write_len) {
               TRACE("Got child_ret: %ld while to_write_len is: %ld",
                     child_ret, to_write_len);
            }

            if (child_ret < 0) {
               TRACE("Returning what I had (%ld) as apposed to the error (%ld, errno:%d)", ret, child_ret, errno);
               RETURN (ret);
            }

            ret += child_ret;

            if (child_ret < to_write_len) {
               /* we timed out, so send back what we could send */

               RETURN (ret);
            }

            to_write = NULL;
         }
      }
   }

   if (buf_head != buf_tail) {
      /* If we have any bytes buffered, send */

      child_ret = _mongoc_stream_tls_openssl_write (tls, buf_head, buf_tail - buf_head);

      if (child_ret < 0) {
         RETURN (child_ret);
      }

      ret += child_ret;
   }

   if (ret >= 0) {
      mongoc_counter_streams_egress_add (ret);
   }

   RETURN (ret);
}


/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_stream_tls_openssl_readv --
 *
 *       Read from the stream into iov. This function will try to read
 *       all of the bytes or fail. If the number of bytes is not equal
 *       to the number requested, a failure or EOF has occurred.
 *
 * Returns:
 *       -1 on failure, 0 on EOF, otherwise the number of bytes read.
 *
 * Side effects:
 *       iov buffers will be written to.
 *
 *--------------------------------------------------------------------------
 */

static ssize_t
_mongoc_stream_tls_openssl_readv (mongoc_stream_t *stream,
                                  mongoc_iovec_t  *iov,
                                  size_t           iovcnt,
                                  size_t           min_bytes,
                                  int32_t          timeout_msec)
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;
   mongoc_stream_tls_openssl_t *openssl = (mongoc_stream_tls_openssl_t *) tls->ctx;
   ssize_t ret = 0;
   size_t i;
   int read_ret;
   size_t iov_pos = 0;
   int64_t now;
   int64_t expire = 0;
   ENTRY;

   BSON_ASSERT (tls);
   BSON_ASSERT (iov);
   BSON_ASSERT (iovcnt);

   tls->timeout_msec = timeout_msec;

   if (timeout_msec >= 0) {
      expire = bson_get_monotonic_time () + (timeout_msec * 1000UL);
   }

   for (i = 0; i < iovcnt; i++) {
      iov_pos = 0;

      while (iov_pos < iov[i].iov_len) {
         read_ret = BIO_read (openssl->bio, (char *)iov[i].iov_base + iov_pos,
                              (int)(iov[i].iov_len - iov_pos));

         /* https://www.openssl.org/docs/crypto/BIO_should_retry.html:
          *
          * If BIO_should_retry() returns false then the precise "error
          * condition" depends on the BIO type that caused it and the return
          * code of the BIO operation. For example if a call to BIO_read() on a
          * socket BIO returns 0 and BIO_should_retry() is false then the cause
          * will be that the connection closed.
          */
         if (read_ret < 0 || (read_ret == 0 && !BIO_should_retry (openssl->bio))) {
            return -1;
         }

         if (expire) {
            now = bson_get_monotonic_time ();

            if ((expire - now) < 0) {
               if (read_ret == 0) {
                  mongoc_counter_streams_timeout_inc();
#ifdef _WIN32
                  errno = WSAETIMEDOUT;
#else
                  errno = ETIMEDOUT;
#endif
                  RETURN (-1);
               }

               tls->timeout_msec = 0;
            } else {
               tls->timeout_msec = (expire - now) / 1000L;
            }
         }

         ret += read_ret;

         if ((size_t)ret >= min_bytes) {
            mongoc_counter_streams_ingress_add(ret);
            RETURN (ret);
         }

         iov_pos += read_ret;
      }
   }

   if (ret >= 0) {
      mongoc_counter_streams_ingress_add(ret);
   }

   RETURN (ret);
}


/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_stream_tls_openssl_setsockopt --
 *
 *       Perform a setsockopt on the underlying stream.
 *
 * Returns:
 *       -1 on failure, otherwise opt specific value.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static int
_mongoc_stream_tls_openssl_setsockopt (mongoc_stream_t *stream,
                                       int              level,
                                       int              optname,
                                       void            *optval,
                                       socklen_t        optlen)
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;

   BSON_ASSERT (tls);

   return mongoc_stream_setsockopt (tls->base_stream,
                                    level,
                                    optname,
                                    optval,
                                    optlen);
}


static mongoc_stream_t *
_mongoc_stream_tls_openssl_get_base_stream (mongoc_stream_t *stream)
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;
   return tls->base_stream;
}


static bool
_mongoc_stream_tls_openssl_check_closed (mongoc_stream_t *stream) /* IN */
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;
   BSON_ASSERT (stream);
   return mongoc_stream_check_closed (tls->base_stream);
}


/**
 * mongoc_stream_tls_openssl_do_handshake:
 *
 * force an ssl handshake
 *
 * This will happen on the first read or write otherwise
 */
bool
mongoc_stream_tls_openssl_do_handshake (mongoc_stream_t *stream,
                                        int32_t          timeout_msec)
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;
   mongoc_stream_tls_openssl_t *openssl = (mongoc_stream_tls_openssl_t *) tls->ctx;

   BSON_ASSERT (tls);
   ENTRY;

   tls->timeout_msec = timeout_msec;

   if (BIO_do_handshake (openssl->bio) == 1) {
      RETURN(true);
   }

   if (!timeout_msec) {
      RETURN(false);
   }

   if (!errno) {
#ifdef _WIN32
      errno = WSAETIMEDOUT;
#else
      errno = ETIMEDOUT;
#endif
   }

   RETURN(false);
}

/**
 * mongoc_stream_tls_openssl_should_retry:
 *
 * If the stream should be retried
 */
bool
mongoc_stream_tls_openssl_should_retry (mongoc_stream_t *stream)
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;
   mongoc_stream_tls_openssl_t *openssl = (mongoc_stream_tls_openssl_t *) tls->ctx;

   BSON_ASSERT (tls);
   ENTRY;

   RETURN(BIO_should_retry (openssl->bio));
}


/**
 * mongoc_stream_tls_openssl_should_read:
 *
 * If the stream should read
 */
bool
mongoc_stream_tls_openssl_should_read (mongoc_stream_t *stream)
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;
   mongoc_stream_tls_openssl_t *openssl = (mongoc_stream_tls_openssl_t *) tls->ctx;

   BSON_ASSERT (tls);
   ENTRY;

   RETURN(BIO_should_read (openssl->bio));
}


/**
 * mongoc_stream_tls_openssl_should_write:
 *
 * If the stream should write
 */
bool
mongoc_stream_tls_openssl_should_write (mongoc_stream_t *stream)
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;
   mongoc_stream_tls_openssl_t *openssl = (mongoc_stream_tls_openssl_t *) tls->ctx;

   BSON_ASSERT (tls);
   ENTRY;

   RETURN(BIO_should_write (openssl->bio));
}


/**
 * mongoc_stream_tls_openssl_check_cert:
 *
 * check the cert returned by the other party
 */
bool
mongoc_stream_tls_openssl_check_cert (mongoc_stream_t *stream,
                                      const char      *host)
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;
   mongoc_stream_tls_openssl_t *openssl = (mongoc_stream_tls_openssl_t *) tls->ctx;
   SSL *ssl;

   BSON_ASSERT (tls);
   BSON_ASSERT (host);
   ENTRY;

   BIO_get_ssl (openssl->bio, &ssl);

   RETURN(_mongoc_ssl_check_cert (ssl, host, tls->weak_cert_validation));
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_openssl_new --
 *
 *       Creates a new mongoc_stream_tls_openssl_t to communicate with a remote
 *       server using a TLS stream.
 *
 *       @base_stream should be a stream that will become owned by the
 *       resulting tls stream. It will be used for raw I/O.
 *
 *       @trust_store_dir should be a path to the SSL cert db to use for
 *       verifying trust of the remote server.
 *
 * Returns:
 *       NULL on failure, otherwise a mongoc_stream_t.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

mongoc_stream_t *
mongoc_stream_tls_openssl_new (mongoc_stream_t  *base_stream,
                               mongoc_ssl_opt_t *opt,
                               int               client)
{
   mongoc_stream_tls_t *tls;
   mongoc_stream_tls_openssl_t *openssl;
   SSL_CTX *ssl_ctx = NULL;
   BIO *bio_ssl = NULL;
   BIO *bio_mongoc_shim = NULL;

   BSON_ASSERT(base_stream);
   BSON_ASSERT(opt);
   ENTRY;

   ssl_ctx = _mongoc_ssl_ctx_new (opt);

   if (!ssl_ctx) {
      RETURN(NULL);
   }

   bio_ssl = BIO_new_ssl (ssl_ctx, client);
   if (!bio_ssl) {
      SSL_CTX_free (ssl_ctx);
      RETURN(NULL);
   }

   bio_mongoc_shim = BIO_new (&gMongocStreamTlsOpenSslRawMethods);
   if (!bio_mongoc_shim) {
      BIO_free_all (bio_ssl);
      RETURN(NULL);
   }


   BIO_push (bio_ssl, bio_mongoc_shim);

   openssl = (mongoc_stream_tls_openssl_t *)bson_malloc0 (sizeof *openssl);
   openssl->bio = bio_ssl;
   openssl->ctx = ssl_ctx;

   tls = (mongoc_stream_tls_t *)bson_malloc0 (sizeof *tls);
   tls->parent.type = MONGOC_STREAM_TLS;
   tls->parent.destroy = _mongoc_stream_tls_openssl_destroy;
   tls->parent.failed = _mongoc_stream_tls_openssl_failed;
   tls->parent.close = _mongoc_stream_tls_openssl_close;
   tls->parent.flush = _mongoc_stream_tls_openssl_flush;
   tls->parent.writev = _mongoc_stream_tls_openssl_writev;
   tls->parent.readv = _mongoc_stream_tls_openssl_readv;
   tls->parent.setsockopt = _mongoc_stream_tls_openssl_setsockopt;
   tls->parent.get_base_stream = _mongoc_stream_tls_openssl_get_base_stream;
   tls->parent.check_closed = _mongoc_stream_tls_openssl_check_closed;
   tls->weak_cert_validation = opt->weak_cert_validation;
   tls->should_read = mongoc_stream_tls_openssl_should_read;
   tls->should_write = mongoc_stream_tls_openssl_should_write;
   tls->should_retry = mongoc_stream_tls_openssl_should_retry;
   tls->do_handshake = mongoc_stream_tls_openssl_do_handshake;
   tls->check_cert = mongoc_stream_tls_openssl_check_cert;
   tls->ctx = (void *)openssl;
   tls->timeout_msec = -1;
   tls->base_stream = base_stream;
   bio_mongoc_shim->ptr = tls;

   mongoc_counter_streams_active_inc();

   RETURN((mongoc_stream_t *)tls);
}

#endif /* MONGOC_ENABLE_OPENSSL */