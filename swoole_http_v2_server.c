/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include "php_swoole.h"
#include "swoole_http.h"

#ifdef SW_USE_HTTP2
#include "http2.h"
#include <nghttp2/nghttp2.h>
#include <main/php_variables.h>

static sw_inline void http2_add_header(nghttp2_nv *headers, char *k, int kl, char *v, int vl)
{
    headers->name = (uchar*) k;
    headers->namelen = kl;
    headers->value = (uchar*) v;
    headers->valuelen = vl;
}

static void http2_onRequest(http_context *ctx TSRMLS_DC)
{
    zval *retval;
    zval **args[2];

    zval *zrequest_object = ctx->request.zrequest_object;
    zval *zresponse_object = ctx->response.zresponse_object;

    args[0] = &zrequest_object;
    args[1] = &zresponse_object;

#ifdef __CYGWIN__
    //TODO: memory error on cygwin.
    zval_add_ref(&zrequest_object);
    zval_add_ref(&zresponse_object);
#endif

    if (sw_call_user_function_ex(EG(function_table), NULL, php_sw_http_server_callbacks[HTTP_CALLBACK_onRequest], &retval, 2, args, 0, NULL TSRMLS_CC) == FAILURE)
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "onRequest handler error");
    }
    if (EG(exception))
    {
        zend_exception_error(EG(exception), E_ERROR TSRMLS_CC);
    }
    if (retval)
    {
        sw_zval_ptr_dtor(&retval);
    }
}

static int http2_build_header(http_context *ctx, uchar *buffer, int body_length TSRMLS_DC)
{
    assert(ctx->send_header == 0);

    char buf[SW_HTTP_HEADER_MAX_SIZE];
    char *date_str;
    char intbuf[2][16];

    int ret;

    /**
     * http header
     */
    zval *zheader = ctx->response.zheader;
    int index = 0;

    nghttp2_nv nv[128];

    /**
     * http status code
     */
    if (ctx->response.status == 0)
    {
        ctx->response.status = 200;
    }

    ret = swoole_itoa(intbuf[0], ctx->response.status);
    http2_add_header(&nv[index++], ZEND_STRL(":status"), intbuf[0], ret);

    if (zheader)
    {
        int flag = 0x0;
        char *key_server = "server";
        char *key_content_length = "content-length";
        char *key_content_type = "content-type";
        char *key_date = "date";

        HashTable *ht = Z_ARRVAL_P(zheader);
        zval *value = NULL;
        char *key = NULL;
        uint32_t keylen = 0;
        int type;

        SW_HASHTABLE_FOREACH_START2(ht, key, keylen, type, value)
        {
            if (!key)
            {
                break;
            }
            if (strcmp(key, key_server) == 0)
            {
                flag |= HTTP_RESPONSE_SERVER;
            }
            else if (strcmp(key, key_content_length) == 0)
            {
                flag |= HTTP_RESPONSE_CONTENT_LENGTH;
            }
            else if (strcmp(key, key_date) == 0)
            {
                flag |= HTTP_RESPONSE_DATE;
            }
            else if (strcmp(key, key_content_type) == 0)
            {
                flag |= HTTP_RESPONSE_CONTENT_TYPE;
            }
            http2_add_header(&nv[index++], key, keylen - 1, Z_STRVAL_P(value), Z_STRLEN_P(value));
        }
        SW_HASHTABLE_FOREACH_END();

        if (!(flag & HTTP_RESPONSE_SERVER))
        {
            http2_add_header(&nv[index++], ZEND_STRL("server"), ZEND_STRL(SW_HTTP_SERVER_SOFTWARE));
        }
        if (ctx->request.method == PHP_HTTP_OPTIONS)
        {
            http2_add_header(&nv[index++], ZEND_STRL("allow"), ZEND_STRL("GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS"));
        }
        else
        {
            if (!(flag & HTTP_RESPONSE_CONTENT_LENGTH) && body_length >= 0)
            {
#ifdef SW_HAVE_ZLIB
                if (ctx->gzip_enable)
                {
                    body_length = swoole_zlib_buffer->length;
                }
#endif
                ret = swoole_itoa(intbuf[1], body_length);
                http2_add_header(&nv[index++], ZEND_STRL("content-length"), intbuf[1], ret);
            }
        }
        if (!(flag & HTTP_RESPONSE_DATE))
        {
            date_str = sw_php_format_date(ZEND_STRL(SW_HTTP_DATE_FORMAT), SwooleGS->now, 0 TSRMLS_CC);
            http2_add_header(&nv[index++], ZEND_STRL("date"), date_str, strlen(date_str));
        }
        if (!(flag & HTTP_RESPONSE_CONTENT_TYPE))
        {
            http2_add_header(&nv[index++], ZEND_STRL("content-type"), ZEND_STRL("text/html"));
        }
    }
    else
    {
        http2_add_header(&nv[index++], ZEND_STRL("server"), ZEND_STRL(SW_HTTP_SERVER_SOFTWARE));
        http2_add_header(&nv[index++], ZEND_STRL("content-type"), ZEND_STRL("text/html"));

        date_str = sw_php_format_date(ZEND_STRL(SW_HTTP_DATE_FORMAT), SwooleGS->now, 0 TSRMLS_CC);
        http2_add_header(&nv[index++], ZEND_STRL("date"), date_str, strlen(date_str));

        if (ctx->request.method == PHP_HTTP_OPTIONS)
        {
            http2_add_header(&nv[index++], ZEND_STRL("allow"), ZEND_STRL("GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS"));
        }
        else if (body_length >= 0)
        {
#ifdef SW_HAVE_ZLIB
            if (ctx->gzip_enable)
            {
                body_length = swoole_zlib_buffer->length;
            }
#endif
            ret = swoole_itoa(buf, body_length);
            http2_add_header(&nv[index++], ZEND_STRL("content-length"), buf, ret);
        }
    }
    //http cookies
    if (ctx->response.zcookie)
    {
        zval *value;
        SW_HASHTABLE_FOREACH_START(Z_ARRVAL_P(ctx->response.zcookie), value)
        {
            if (Z_TYPE_P(value) != IS_STRING)
            {
                continue;
            }
            http2_add_header(&nv[index++], ZEND_STRL("set-cookie"), Z_STRVAL_P(value), Z_STRLEN_P(value));
        }
        SW_HASHTABLE_FOREACH_END();
    }
    //http compress
    if (ctx->gzip_enable)
    {
#ifdef SW_HTTP_COMPRESS_GZIP
        http2_add_header(&nv[index++], ZEND_STRL("content-encoding"), ZEND_STRL("gzip"));
#else
        http2_add_header(&nv[index++], ZEND_STRL("content-encoding"), ZEND_STRL("deflate"));
#endif
    }
    ctx->send_header = 1;

    ssize_t rv;
    size_t buflen;
    size_t i;
    size_t sum = 0;

    nghttp2_hd_deflater *deflater;
    ret = nghttp2_hd_deflate_new(&deflater, 4096);
    if (ret != 0)
    {
        swoole_php_error(E_WARNING, "nghttp2_hd_deflate_init failed with error: %s\n", nghttp2_strerror(ret));
        return SW_ERR;
    }

    for (i = 0; i < index; ++i)
    {
        sum += nv[i].namelen + nv[i].valuelen;
    }

    buflen = nghttp2_hd_deflate_bound(deflater, nv, index);
    rv = nghttp2_hd_deflate_hd(deflater, (uchar *) buffer, buflen, nv, index);
    if (rv < 0)
    {
        swoole_php_error(E_WARNING, "nghttp2_hd_deflate_hd() failed with error: %s\n", nghttp2_strerror((int ) rv));
        return SW_ERR;
    }

    efree(date_str);
    nghttp2_hd_deflate_del(deflater);

    return rv;
}

int swoole_http2_do_response(http_context *ctx, swString *body)
{
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif

    char header_buffer[8192];

    int n = http2_build_header(ctx, (uchar *) header_buffer, body->length TSRMLS_CC);
    swString_clear(swoole_http_buffer);

    /**
     +---------------+
     |Pad Length? (8)|
     +-+-------------+-----------------------------------------------+
     |E|                 Stream Dependency? (31)                     |
     +-+-------------+-----------------------------------------------+
     |  Weight? (8)  |
     +-+-------------+-----------------------------------------------+
     |                   Header Block Fragment (*)                 ...
     +---------------------------------------------------------------+
     |                           Padding (*)                       ...
     +---------------------------------------------------------------+
     */
    char frame_header[9];
    swHttp2_set_frame_header(frame_header, SW_HTTP2_TYPE_HEADERS, n, SW_HTTP2_FLAG_END_HEADERS, ctx->stream_id);
    swString_append_ptr(swoole_http_buffer, frame_header, 9);
    swString_append_ptr(swoole_http_buffer, header_buffer, n);

    swHttp2_set_frame_header(frame_header, SW_HTTP2_TYPE_DATA, body->length, SW_HTTP2_FLAG_END_STREAM, ctx->stream_id);
    swString_append_ptr(swoole_http_buffer, frame_header, 9);
    swString_append(swoole_http_buffer, body);

    int ret = swServer_tcp_send(SwooleG.serv, ctx->fd, swoole_http_buffer->str, swoole_http_buffer->length);
    if (ret < 0)
    {
        ctx->send_header = 0;
        return SW_ERR;
    }
    swoole_http_context_free(ctx TSRMLS_CC);

    if (ctx->client->streams)
    {
        swHashMap_del_int(ctx->client->streams, ctx->stream_id);
    }
    efree(ctx);

    return SW_OK;
}

static int http2_parse_header(swoole_http_client *client, http_context *ctx, int flags, char *in, size_t inlen)
{
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif

    nghttp2_hd_inflater *inflater = client->inflater;

    if (!inflater)
    {
        int ret = nghttp2_hd_inflate_new(&inflater);
        if (ret != 0)
        {
            swoole_php_error(E_WARNING, "nghttp2_hd_inflate_init() failed, Error: %s[%zd].", nghttp2_strerror(ret), ret);
            return SW_ERR;
        }
        client->inflater = inflater;
    }

    if (flags & SW_HTTP2_FLAG_PRIORITY)
    {
        //int stream_deps = ntohl(*(int *) (in));
        //uint8_t weight = in[4];
        in += 5;
        inlen -= 5;
    }

    zval *zheader = ctx->request.zheader;
    zval *zserver = ctx->request.zserver;

    ssize_t rv;
    for (;;)
    {
        nghttp2_nv nv;
        int inflate_flags = 0;
        size_t proclen;

        rv = nghttp2_hd_inflate_hd(inflater, &nv, &inflate_flags, (uchar *) in, inlen, 1);
        if (rv < 0)
        {
            swoole_php_error(E_WARNING, "inflate failed, Error: %s[%zd].", nghttp2_strerror(rv), rv);
            return -1;
        }

        proclen = (size_t) rv;

        in += proclen;
        inlen -= proclen;

        //swTraceLog(SW_TRACE_HTTP2, "Header: %s[%d]: %s[%d]", nv.name, nv.namelen, nv.value, nv.valuelen);

        if (inflate_flags & NGHTTP2_HD_INFLATE_EMIT)
        {
            if (nv.name[0] == ':')
            {
                if (strncasecmp((char *) nv.name + 1, ZEND_STRL("method")) == 0)
                {
                    sw_add_assoc_stringl_ex(zserver, ZEND_STRS("request_method"), (char *) nv.value, nv.valuelen, 1);
                }
                else if (strncasecmp((char *) nv.name + 1, ZEND_STRL("path")) == 0)
                {
                    char pathbuf[SW_HTTP_HEADER_MAX_SIZE];
                    char *v_str = strchr((char *) nv.value, '?');
                    if (v_str)
                    {
                        v_str++;
                        int k_len = v_str - (char *) nv.value - 1;
                        int v_len = nv.valuelen - k_len - 1;
                        memcpy(pathbuf, nv.value, k_len);
                        pathbuf[k_len] = 0;
                        sw_add_assoc_stringl_ex(zserver, ZEND_STRS("query_string"), v_str, v_len, 1);
                        sw_add_assoc_stringl_ex(zserver, ZEND_STRS("request_uri"), pathbuf, k_len, 1);

                        zval *zget;
                        http_alloc_zval(ctx, request, zget);
                        array_init(zget);
                        zend_update_property(swoole_http_request_class_entry_ptr, ctx->request.zrequest_object, ZEND_STRL("get"), zget TSRMLS_CC);

                        //no need free, will free by treat_data
                        char *query = estrndup(v_str, v_len);
                        //parse url params
                        sapi_module.treat_data(PARSE_STRING, query, zget TSRMLS_CC);
                    }
                    else
                    {
                        sw_add_assoc_stringl_ex(zserver, ZEND_STRS("request_uri"), (char *) nv.value, nv.valuelen, 1);
                    }
                }
                else if (strncasecmp((char *) nv.name + 1, ZEND_STRL("authority")) == 0)
                {
                    sw_add_assoc_stringl_ex(zheader, ZEND_STRS("host"), (char * ) nv.value, nv.valuelen, 1);
                }
            }
            else
            {
                if (memcmp(nv.name, ZEND_STRL("content-type")) == 0)
                {
                    if (strncasecmp((char *) nv.value, ZEND_STRL("application/x-www-form-urlencoded")) == 0)
                    {
                        ctx->request.post_form_urlencoded = 1;
                    }
                    else if (strncasecmp((char *) nv.value, ZEND_STRL("multipart/form-data")) == 0)
                    {
                        int boundary_len = nv.valuelen - strlen("multipart/form-data; boundary=");
                        swoole_http_parse_form_data(ctx, (char*) nv.value + nv.valuelen - boundary_len, boundary_len TSRMLS_CC);
                        ctx->parser.data = ctx;
                    }
                }
                else if (memcmp(nv.name, ZEND_STRL("cookie")) == 0)
                {
                    zval *zcookie = ctx->request.zcookie;
                    if (!zcookie)
                    {
                        http_alloc_zval(ctx, request, zcookie);
                        array_init(zcookie);
                        zend_update_property(swoole_http_request_class_entry_ptr, ctx->request.zrequest_object, ZEND_STRL("cookie"), zcookie TSRMLS_CC);
                    }

                    char keybuf[SW_HTTP_COOKIE_KEYLEN];
                    char *v_str = strchr((char *) nv.value, '=') + 1;
                    int k_len = v_str - (char *) nv.value - 1;
                    int v_len = nv.valuelen - k_len - 1;
                    memcpy(keybuf, nv.value, k_len);
                    keybuf[k_len] = 0;
                    sw_add_assoc_stringl_ex(zcookie, keybuf, k_len + 1, v_str, v_len, 1);
                    continue;
                }
                sw_add_assoc_stringl_ex(zheader, (char *) nv.name, nv.namelen + 1, (char *) nv.value, nv.valuelen, 1);
            }
        }

        if (inflate_flags & NGHTTP2_HD_INFLATE_FINAL)
        {
            nghttp2_hd_inflate_end_headers(inflater);
            break;
        }

        if ((inflate_flags & NGHTTP2_HD_INFLATE_EMIT) == 0 && inlen == 0)
        {
            break;
        }
    }

    rv = nghttp2_hd_inflate_change_table_size(inflater, 4096);
    if (rv != 0)
    {
        return rv;
    }
    return SW_OK;
}

/**
 * Http2
 */
int swoole_http2_onFrame(swoole_http_client *client, swEventData *req)
{
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif

    int fd = req->info.fd;

    http_context *ctx;

    zval *zdata;
    SW_MAKE_STD_ZVAL(zdata);
    zdata = php_swoole_get_recv_data(zdata, req TSRMLS_CC);

    char *buf = Z_STRVAL_P(zdata);

    int type = buf[3];
    int flags = buf[4];
    int stream_id = ntohl((*(int *) (buf + 5))) & 0x7fffffff;
    uint32_t length = swHttp2_get_length(buf);

    swTraceLog(SW_TRACE_HTTP2, "[%s]\tflags=%d, stream_id=%d, length=%d", swHttp2_get_type(type), flags, stream_id, length);

    if (type == SW_HTTP2_TYPE_HEADERS)
    {
        ctx = swoole_http_context_new(client TSRMLS_CC);
        if (!ctx)
        {
            sw_zval_ptr_dtor(&zdata);
            swoole_error_log(SW_LOG_WARNING, SW_ERROR_HTTP2_STREAM_NO_HEADER, "http2 error stream.");
            return SW_ERR;
        }

        ctx->http2 = 1;
        ctx->stream_id = stream_id;

        http2_parse_header(client, ctx, flags, buf + SW_HTTP2_FRAME_HEADER_SIZE, length);

        swConnection *conn = swWorker_get_connection(SwooleG.serv, fd);
        if (!conn)
        {
            sw_zval_ptr_dtor(&zdata);
            swWarn("connection[%d] is closed.", fd);
            return SW_ERR;
        }

        zval *zserver = ctx->request.zserver;
        sw_add_assoc_long_ex(zserver, ZEND_STRS("request_time"), SwooleGS->now);
        add_assoc_long(zserver, "server_port", swConnection_get_port(&SwooleG.serv->connection_list[conn->from_fd]));
        add_assoc_long(zserver, "remote_port", swConnection_get_port(conn));
        sw_add_assoc_string(zserver, "remote_addr", swConnection_get_ip(conn), 1);
        sw_add_assoc_string(zserver, "server_protocol", "HTTP/2", 1);
        sw_add_assoc_string(zserver, "server_software", SW_HTTP_SERVER_SOFTWARE, 1);

        if (flags & SW_HTTP2_FLAG_END_STREAM)
        {
            http2_onRequest(ctx TSRMLS_CC);
        }
        else
        {
            if (!client->streams)
            {
                client->streams = swHashMap_new(SW_HTTP2_MAX_CONCURRENT_STREAMS, NULL);
            }
            swHashMap_add_int(client->streams, stream_id, ctx);
        }
    }
    else if (type == SW_HTTP2_TYPE_DATA)
    {
        ctx = swHashMap_find_int(client->streams, stream_id);
        if (!ctx)
        {
            sw_zval_ptr_dtor(&zdata);
            swoole_error_log(SW_LOG_WARNING, SW_ERROR_HTTP2_STREAM_NO_HEADER, "http2 error stream.");
            return SW_ERR;
        }

        swString *buffer = ctx->buffer;
        if (!buffer)
        {
            buffer = swString_new(SW_HTTP2_DATA_BUFFSER_SIZE);
            ctx->buffer = buffer;
        }
        swString_append_ptr(buffer, buf + SW_HTTP2_FRAME_HEADER_SIZE, length);

        if (flags & SW_HTTP2_FLAG_END_STREAM)
        {
            if (SwooleG.serv->http_parse_post && ctx->request.post_form_urlencoded)
            {
                zval *zpost;
                http_alloc_zval(ctx, request, zpost);
                array_init(zpost);
                ctx->request.post_content = estrndup(buffer->str, buffer->length);
                zend_update_property(swoole_http_request_class_entry_ptr, ctx->request.zrequest_object, ZEND_STRL("post"), zpost TSRMLS_CC);
                sapi_module.treat_data(PARSE_STRING, ctx->request.post_content, zpost TSRMLS_CC);
            }
            else if (ctx->mt_parser != NULL)
            {
                multipart_parser *multipart_parser = ctx->mt_parser;
                size_t n = multipart_parser_execute(multipart_parser, buffer->str, buffer->length);
                if (n != length)
                {
                    swoole_php_fatal_error(E_WARNING, "parse multipart body failed.");
                }
            }
            http2_onRequest(ctx TSRMLS_CC);
        }
    }
    else if (type == SW_HTTP2_TYPE_PING)
    {
        char ping_frame[SW_HTTP2_FRAME_HEADER_SIZE + SW_HTTP2_FRAME_PING_PAYLOAD_SIZE];
        swHttp2_set_frame_header(ping_frame, SW_HTTP2_TYPE_PING, SW_HTTP2_FRAME_PING_PAYLOAD_SIZE, SW_HTTP2_FLAG_ACK, stream_id);
        memcpy(ping_frame + SW_HTTP2_FRAME_HEADER_SIZE, buf + SW_HTTP2_FRAME_HEADER_SIZE, SW_HTTP2_FRAME_PING_PAYLOAD_SIZE);
        swServer_tcp_send(SwooleG.serv, fd, ping_frame, SW_HTTP2_FRAME_HEADER_SIZE + SW_HTTP2_FRAME_PING_PAYLOAD_SIZE);
    }
    else if (type == SW_HTTP2_TYPE_WINDOW_UPDATE)
    {
        client->window_size = *(int *) (buf + SW_HTTP2_FRAME_HEADER_SIZE);
    }
    sw_zval_ptr_dtor(&zdata);
    return SW_OK;
}
#endif
