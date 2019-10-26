/*
 * h2sim - HTTP2 Simple Application Framework using nghttp2
 *
 * Copyright (c) 2019 Lee Yongjae, Telcoware Co.,LTD.
 *
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/time.h>    /* for gettimeofday */
#include <sys/types.h>
#include <sys/socket.h>  /* for shutdown() */
#ifdef EPOLL_MODE
#include <sys/epoll.h>
#endif

#include <nghttp2/nghttp2.h>

#include "h2.h"
#include "h2_priv.h"


/*
 * NGHTTP2 Header Utilities ------------------------------------------------
 */

static int ng_hdr_append(nghttp2_nv *hdr_tbl, int *hdr_num, int hdr_max,
                      const char *name, const char *value)
{
  if (*hdr_num >= hdr_max) {
    warnx("hdr_tbl is full: hdr_max=%d", hdr_max);
    return -1;
  }

  nghttp2_nv *nv = &hdr_tbl[(*hdr_num)++];
  nv->name = (uint8_t *)name;
  nv->value = (uint8_t *)value;
  nv->namelen = strlen(name);
  nv->valuelen = strlen(value);
  nv->flags = NGHTTP2_NV_FLAG_NONE;
  // NOTE: PERF: NGHTTP@_NV_FLAG_NO_INDEX shows inferior performance

  return 0;
}

static void ng_print_header(FILE *f, const char *name, int namelen,
                            const char *value, int value_len,
                            const char *prefix, int stream_id) {
  fprintf(f, "%s[%d]     %-16.*s = %.*s\n",
          prefix, stream_id, namelen, name, value_len, value);
}

static void ng_print_headers(FILE *f, nghttp2_nv *nva, size_t nvlen,
                   const char *prefix, int stream_id) {
  size_t i;
  for (i = 0; i < nvlen; ++i) {
    ng_print_header(f, (char *)nva[i].name, nva[i].namelen,
                    (char *)nva[i].value, nva[i].valuelen,
                    prefix, stream_id);
  }
}


/*
 * HTTP/2 Message Send -----------------------------------------------------
 */

static ssize_t ng_send_msg_body_cb(nghttp2_session *ng_sess,
                    int32_t stream_id, uint8_t *buf, size_t length,
                    uint32_t *data_flags, nghttp2_data_source *source,
                    void *user_data) {
  h2_send_buf *sb = source->ptr;
  h2_sess *sess = user_data;
  (void)ng_sess;
  (void)stream_id;

  int n = sb->data_size - sb->data_used;
  if (n > (int)length) {
    n = (int)length;
  }
  if (n > 0) {
    memcpy(buf, &sb->data[sb->data_used], n);
  }

  /* dump out response body */
  if (sess->ctx->verbose) {
    if (n == sb->data_size)
      fprintf(stderr, "%s[%d] %s DATA(%d):\n",
              sess->log_prefix, stream_id, h2_msg_type_str(sb->msg_type), n);
    else
      fprintf(stderr, "%s[%d] %s DATA(%d+%d/%d):\n",
              sess->log_prefix, stream_id, h2_msg_type_str(sb->msg_type),
              sb->data_used, n, sb->data_size);
    fwrite(buf, 1, n, stdout);
    if (n >= 1 && buf[n - 1] != '\n' && buf[0] != '\0') {
      fwrite("\n", 1, 1, stdout);
    }
    fflush(stdout);
  }

  sb->data_used += n;
  if (sb->data_used >= sb->data_size) {
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
  } else {
    //fprintf(stderr, "DEBUG: %s[%d] REMAINS DATA: %d sess.send_data_remain=%d\n",
    //        sess->log_prefix, stream_id, sb->data_size - sb->data_used,
    //        sess->send_data_remain);
  }

  sess->send_data_remain -= n;
  return n;
}

static void h2_cpy_send_data_prd(nghttp2_data_provider *data_prd, 
                                 h2_strm *strm, void *data, int size) {
  /* ASSUME: data!=null, size>0 */
  h2_send_buf *send_buf = &strm->send_body_sb;
  send_buf->data = malloc(size + 1);
  memcpy(send_buf->data, data, size);
  send_buf->data[size] = '\0';
  send_buf->data_size = size;
  send_buf->data_used = 0;
  send_buf->to_be_freed = 1;
  send_buf->msg_type = strm->send_msg_type;
  if (data_prd) {
    data_prd->source.ptr = send_buf;
    data_prd->read_callback = ng_send_msg_body_cb;
  }
}

int h2_send_request_v2(h2_sess *sess, h2_msg *req,
                       h2_response_cb response_cb, void *strm_user_data) {
#define REQ_HDR_MAX  (5 + H2_MSG_HDR_MAX)
  nghttp2_nv ng_hdr[REQ_HDR_MAX];
  int ng_hdr_num = 0;
  int stream_id, i;
  char s[1][32];

  ng_hdr_append(ng_hdr, &ng_hdr_num, REQ_HDR_MAX, ":method", h2_method(req));
  ng_hdr_append(ng_hdr, &ng_hdr_num, REQ_HDR_MAX, ":scheme", h2_scheme(req));
  ng_hdr_append(ng_hdr, &ng_hdr_num, REQ_HDR_MAX, ":authority",
                                                             h2_authority(req));
  ng_hdr_append(ng_hdr, &ng_hdr_num, REQ_HDR_MAX, ":path", h2_path(req));
  /* TODO: content-length MUST NOT be sent if  transfer-encoding is set. */
  /*       (rfc7230 3.3.2. Content-Length) */
  if (req->body && req->body_len > 0) {
    sprintf(s[0], "%d", req->body_len);
    ng_hdr_append(ng_hdr, &ng_hdr_num, REQ_HDR_MAX, "content-length", s[0]);
  }
  for (i = 0; i < req->hdr_num; i++) {
    ng_hdr_append(ng_hdr, &ng_hdr_num, REQ_HDR_MAX,
                  h2_hdr_idx_name(req, i), h2_hdr_idx_value(req, i));
  }

  /* ASSUME: success */ /* TODO: handled error case */
  h2_strm *strm = h2_strm_init(sess, 0, H2_RESPONSE,
                               response_cb, strm_user_data);

  /* set send message body read handler */
  nghttp2_data_provider data_prd_buf, *data_prd = NULL;
  if (req->body && req->body_len > 0) {
    h2_cpy_send_data_prd(&data_prd_buf, strm, req->body, req->body_len);
    data_prd = &data_prd_buf;
  }

  if (sess->ng_sess == NULL) {
    warnx("%send request failed for invalid session", sess->log_prefix);
    return -1;
  }

  stream_id = nghttp2_submit_request(sess->ng_sess, NULL,
                                     ng_hdr, ng_hdr_num, data_prd, strm);
  if (stream_id < 0) {
    warnx("%sCannot not submit HTTP request: %s",
          sess->log_prefix, nghttp2_strerror(stream_id));
    h2_strm_free(strm);
    return -2;
  }
  strm->is_req = 1;
  strm->stream_id = stream_id;
  sess->send_data_remain += strm->send_body_sb.data_size;
  sess->req_cnt++;

  if (sess->ctx->verbose) {
    fprintf(stderr, "%s[%d] REQUEST HEADER:\n",
            sess->log_prefix, stream_id);
    ng_print_headers(stderr, ng_hdr, ng_hdr_num, sess->log_prefix, stream_id);
  }

  //h2_sess_mark_send_pending(sess);
  h2_sess_send(sess);
  return 0;
}

int h2_send_response_v2(h2_sess *sess, h2_strm *strm, h2_msg *rsp) {
#define RSP_HDR_MAX  (2 + H2_MSG_HDR_MAX)
  nghttp2_nv ng_hdr[RSP_HDR_MAX];
  int ng_hdr_num = 0;
  int i, r;
  char s[2][32];

  sprintf(s[0], "%d", rsp->status);
  ng_hdr_append(ng_hdr, &ng_hdr_num, RSP_HDR_MAX, ":status", s[0]);
  if (rsp->body && rsp->body_len > 0) {
    sprintf(s[1], "%d", rsp->body_len);
    ng_hdr_append(ng_hdr, &ng_hdr_num, RSP_HDR_MAX, "content-length", s[1]);
  }
  for (i = 0; i < rsp->hdr_num; i++) {
    ng_hdr_append(ng_hdr, &ng_hdr_num, RSP_HDR_MAX,
                  h2_hdr_idx_name(rsp, i), h2_hdr_idx_value(rsp, i));
  }

  /* set response body read handler */
  nghttp2_data_provider data_prd_buf, *data_prd = NULL;
  if (rsp->body && rsp->body_len > 0) {
    h2_cpy_send_data_prd(&data_prd_buf, strm, rsp->body, rsp->body_len);
    data_prd = &data_prd_buf;
  }

  /* mark response sent to prevent further push_promise */
  strm->is_rsp_set = 1;
  sess->send_data_remain += strm->send_body_sb.data_size;

  if (sess->ng_sess == NULL) {
    warnx("%send response failed for invalid session", sess->log_prefix);
    return -1;
  }

  r = nghttp2_submit_response(sess->ng_sess, strm->stream_id,
                               ng_hdr, ng_hdr_num, data_prd);
  if (r != 0) {
    warnx("%s[%d] Fatal error: %d:%s",
          sess->log_prefix, strm->stream_id, r, nghttp2_strerror(r));
    return -1;
  }

  if (sess->ctx->verbose) {
    fprintf(stderr, "%s[%d] %s HEADER:\n",
            sess->log_prefix, strm->stream_id,
            h2_msg_type_str(strm->send_msg_type));
    ng_print_headers(stderr, ng_hdr, ng_hdr_num,
                     sess->log_prefix, strm->stream_id);
  }

  h2_sess_mark_send_pending(sess);
  return 0;
}

int h2_send_push_promise_v2(h2_sess *sess, h2_strm *request_strm,
                            h2_msg *prm_req, h2_msg *prm_rsp) {
#define PRM_HDR_MAX  (5 + H2_MSG_HDR_MAX)
  nghttp2_nv ng_hdr[REQ_HDR_MAX];
  int ng_hdr_num = 0;
  int stream_id, i;

  /* send PUSH_PROMISE with request headers */
  ng_hdr_append(ng_hdr, &ng_hdr_num, PRM_HDR_MAX, ":method", h2_method(prm_req));
  ng_hdr_append(ng_hdr, &ng_hdr_num, PRM_HDR_MAX, ":scheme", h2_scheme(prm_req));
  ng_hdr_append(ng_hdr, &ng_hdr_num, PRM_HDR_MAX, ":authority", h2_authority(prm_req));
  ng_hdr_append(ng_hdr, &ng_hdr_num, PRM_HDR_MAX, ":path", h2_path(prm_req));
  for (i = 0; i < prm_req->hdr_num; i++) {
    ng_hdr_append(ng_hdr, &ng_hdr_num, PRM_HDR_MAX,
                  h2_hdr_idx_name(prm_req, i), h2_hdr_idx_value(prm_req, i));
  }
  /* ASSUME PUSH_PROMISE request has no body */

  if (sess->ng_sess == NULL) {
    warnx("%send push promise failed for invalid session", sess->log_prefix);
    return -1;
  }

  h2_strm *strm = h2_strm_init(sess, 0, H2_PUSH_PROMISE, NULL, NULL);

  stream_id = nghttp2_submit_push_promise(sess->ng_sess,
                               NGHTTP2_FLAG_NONE,
                               request_strm->stream_id,
                               ng_hdr, ng_hdr_num, strm);
  if (stream_id < 0) {
    warnx("%sCannot not submit HTTP push promise: %s",
          sess->log_prefix, nghttp2_strerror(stream_id));
    h2_strm_free(strm);
    return -1;
  }
  strm->stream_id = stream_id;

  if (sess->ctx->verbose) {
    fprintf(stderr, "%s[%d] PUSH_PROMISE(%d)\n",
            sess->log_prefix, request_strm->stream_id,
            strm->stream_id);
    ng_print_headers(stderr, ng_hdr, ng_hdr_num,
                     sess->log_prefix, request_strm->stream_id);
  }

  /* send push response HEADERS with reponse headers and body */
  strm->send_msg_type = H2_PUSH_RESPONSE;
  return h2_send_response(sess, strm, prm_rsp);
}


/*
 * HTTP/2 Special Message Event Handlers ------------------------------------
 */

int h2_send_rst_stream_v2(h2_sess *sess, h2_strm *strm) {
  if (sess->ng_sess == NULL) {
    warnx("%send rst stream failed for invalid session", sess->log_prefix);
    return -1;
  }
  nghttp2_submit_rst_stream(sess->ng_sess, NGHTTP2_FLAG_NONE,
                            strm->stream_id, NGHTTP2_REFUSED_STREAM);  
  return 0;
}


/*
 * NGHTTP2 Session Callbacks h2_sess ----------------------------------------
 */

static int ng_header_cb(nghttp2_session *ng_sess,
                       const nghttp2_frame *frame, const uint8_t *name,
                       size_t name_len, const uint8_t *_value,
                       size_t value_len, uint8_t flags, void *user_data) {
  h2_strm *strm;
  h2_sess *sess = (h2_sess *)user_data;
  const char *value = (const char *)_value;
  (void)flags;

  /* NOTE: cannot batch process in on_frame_recived_callback */
  /*       because frame.headers.nva is not valid on this case */

  int is_request = 0;
  if (frame->hd.type == NGHTTP2_HEADERS &&
      (strm = nghttp2_session_get_stream_user_data(ng_sess,
                         frame->hd.stream_id)) &&
      strm->stream_id == frame->hd.stream_id) {
    if (frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
      is_request = 1;
    }
  } else if (frame->hd.type == NGHTTP2_PUSH_PROMISE &&
      (strm = nghttp2_session_get_stream_user_data(ng_sess,
                         frame->push_promise.promised_stream_id)) &&
      strm->stream_id == frame->push_promise.promised_stream_id) {
    is_request = 1;
  } else {
    warnx("%s[%d] UNKNOWN HEADER FRAME; ignore: "
          "frame.hd.type=%d frame.headers.cat=%d",
          sess->log_prefix, frame->hd.stream_id,
          frame->hd.type, frame->headers.cat);
    return 0;
  }

  h2_msg *msg = strm->rmsg;
  if (name[0] == ':') {  /* psuedo heaers */
    if (is_request) {
      if (name_len == 7 && !memcmp(":method", name, name_len)) {
        msg->method = h2_sbuf_put_n(&msg->sbuf, value, value_len);
      } else if (name_len == 7 && !memcmp(":scheme", name, name_len)) {
        msg->scheme = h2_sbuf_put_n(&msg->sbuf, value, value_len);
      } else if (name_len == 10 && !memcmp(":authority", name, name_len)) {
        msg->authority = h2_sbuf_put_n(&msg->sbuf, value, value_len);
      } else if (name_len == 5 && !memcmp(":path", name, name_len)) {
        /* TODO: NEED TO DECODE URI ENCODING(PERCENT ENCOOED) */
        /*
        char *req_path = percent_decode(value, value_len);
        h2_phdr_put(msg, &msg->path, req_path, strlen(req_path));
        free(req_path);
        */
        msg->path = h2_sbuf_put_n(&msg->sbuf, value, value_len);
      } else {
        warnx("%s[%d] Unknown psuedo header for request; ignore: %.*s=%.*s",
              sess->log_prefix, strm->stream_id,
              (int)name_len, name, (int)value_len, value);
      }
    } else {  /* response case */
      if (name_len == 7 && !memcmp(":status", name, name_len) &&
          value_len == 3 &&
          isdigit(value[0]) && isdigit(value[1]) && isdigit(value[2])) {
        msg->status = ((int)(value[0] - '0') * 100 +
                       (int)(value[1] - '0') * 10 +
                       (int)(value[2] - '0'));
      } else {
        warnx("%s[%d] Invalid psuedo header for response; ignore: %.*s=%.*s",
              sess->log_prefix, strm->stream_id,
              (int)name_len, name, (int)value_len, value);
      }
    }
  } else {
    /* normal headers */
    h2_add_hdr_n(msg, (char *)name, name_len, value, value_len);

    /* TODO: NEED TO HANDLE content-lenght header for body buffer pre-alloc */
  }

  if (sess->ctx->verbose) {
    ng_print_header(stderr, (char *)name, name_len, value, value_len,
                    sess->log_prefix, frame->hd.stream_id);
  }
  return 0;
}

static int ng_begin_hdr_cb(nghttp2_session *ng_sess,
                           const nghttp2_frame *frame, void *user_data) {
  /* NOTE: on_header_recv_callback and on_frame_recv_callback are called */
  /*       after this callback */

  h2_sess *sess = (h2_sess *)user_data;
  h2_strm *strm, *promise_strm;

  if (frame->hd.type == NGHTTP2_HEADERS) {
    if (frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
      /* server side */
      strm = h2_strm_init(sess, frame->hd.stream_id, H2_REQUEST, NULL, NULL);
      nghttp2_session_set_stream_user_data(ng_sess, frame->hd.stream_id,
                                           strm);
    } else if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
      /* client side */
      strm = nghttp2_session_get_stream_user_data(ng_sess, frame->hd.stream_id);
    } else if (frame->headers.cat == NGHTTP2_HCAT_PUSH_RESPONSE) {
      /* client side; after PUSH_PROMISE */
      strm = nghttp2_session_get_stream_user_data(ng_sess, frame->hd.stream_id);
      /* reinit for push_response */
      h2_msg_free(strm->rmsg);
      strm->recv_msg_type = H2_PUSH_RESPONSE;
      strm->rmsg = h2_msg_init();
    } else {
      warnx("%s[%d] UNKNOWN BEGIN HEADER; ignore: "
            "frame.hd.type=%d frame.headers.cat=%d",
            sess->log_prefix, frame->hd.stream_id,
            frame->hd.type, frame->headers.cat);
      return 0;
    }
    if (sess->ctx->verbose) {
      fprintf(stderr, "%s[%d] %s HEADER:\n",
              sess->log_prefix, frame->hd.stream_id,
              h2_msg_type_str(strm->recv_msg_type));
    }
  } else if (frame->hd.type == NGHTTP2_PUSH_PROMISE) {
    /* client side; prepare push promise session data */
    strm = nghttp2_session_get_stream_user_data(ng_sess, frame->hd.stream_id);
    promise_strm = h2_strm_init(sess, frame->push_promise.promised_stream_id,
                                H2_PUSH_PROMISE, NULL, NULL);
    nghttp2_session_set_stream_user_data(ng_sess,
                                frame->push_promise.promised_stream_id,
                                promise_strm);
    if (sess->ctx->verbose) {
      fprintf(stderr, "%s[%d] %s (%d):\n",
              sess->log_prefix, frame->hd.stream_id,
              h2_msg_type_str(promise_strm->recv_msg_type),
              frame->push_promise.promised_stream_id);
    }
  } else {
    warnx("%s[%d] UNKNOWN BEGIN HEADER; ignore: "
          "frame.hd.type=%d frame.headers.cat=%d",
          sess->log_prefix, frame->hd.stream_id,
          frame->hd.type, frame->headers.cat);
  }

  return 0;
}

static int ng_frame_recv_cb(nghttp2_session *ng_sess,
                            const nghttp2_frame *frame, void *user_data) {
  /* NOTE: NGHTTP2 handles CONTINUATION FRAME internally; do not consider */
  /* NOTE: cannot batch process in on_frame_recived_callback */
  /*       because frame.headers.nva is not valid on this case */

  h2_sess *sess = (h2_sess *)user_data;
  h2_strm *strm = NULL;
  h2_strm *request_strm, *promised_strm;
  int r;

  switch (frame->hd.type) {
  case NGHTTP2_DATA:     /* called after on_data_chunk_recevied */
  case NGHTTP2_HEADERS:
    if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) &&
        (strm ||
         (strm = nghttp2_session_get_stream_user_data(ng_sess,
                                                      frame->hd.stream_id))) &&
        strm->stream_id == frame->hd.stream_id) {
      switch (strm->recv_msg_type) {
      case H2_REQUEST:       r = h2_on_request_recv(sess, strm);
                             return (r < 0)? NGHTTP2_ERR_CALLBACK_FAILURE : 0;
      case H2_RESPONSE:      return h2_on_response_recv(sess, strm);
      case H2_PUSH_RESPONSE: return h2_on_push_response_recv(sess, strm);
      }
    }
    break;

  case NGHTTP2_PUSH_PROMISE:
    /* note promised_stream_id alread created */
    if ((frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) &&
        (request_strm = nghttp2_session_get_stream_user_data(ng_sess,
                                    frame->hd.stream_id)) &&
        ((promised_strm = strm) ||
          (promised_strm = nghttp2_session_get_stream_user_data(ng_sess,
                                    frame->push_promise.promised_stream_id))) &&
        request_strm->stream_id == frame->hd.stream_id &&
        promised_strm->stream_id == frame->push_promise.promised_stream_id) {
      h2_on_push_promise_recv(sess, request_strm, promised_strm);
    }
    break;

  case NGHTTP2_RST_STREAM:
    warnx("%s[%d] RST_STREAM RECEIVED", sess->log_prefix, frame->hd.stream_id);
    if ((strm ||
         (strm = nghttp2_session_get_stream_user_data(ng_sess,
                                                      frame->hd.stream_id))) &&
        strm->stream_id == frame->hd.stream_id) {
      h2_on_rst_stream_recv(sess, strm);
    }
    break;
  }

  return 0;
}

static int ng_data_cb(nghttp2_session *ng_sess, uint8_t flags,
                      int32_t stream_id, const uint8_t *data,
                      size_t len, void *user_data) {
  h2_sess *sess = (h2_sess *)user_data;
  h2_strm *strm;
  (void)flags;

  strm = nghttp2_session_get_stream_user_data(ng_sess, stream_id);
  if (strm == NULL || strm->stream_id != stream_id || len <= 0) {
    return 0;
  }

  /* TODO: pre-alloc body buffer if content-length header detected */
  /*       add h2_msg.body_alloced_size */

  h2_msg *msg = strm->rmsg;
  if (msg->body) {
    msg->body = (uint8_t *)realloc(msg->body, msg->body_len + len + 1);
  } else {
    msg->body = (uint8_t *)malloc(len + 1);
  }
  if (len > 0) {
    memcpy(&msg->body[msg->body_len], data, len);
    msg->body_len += len;
  }
  msg->body[msg->body_len] = '\0';  /* mark zero at the end of body buf */

  /* TODO: do total data chunk size counting per session */

  if (sess->ctx->verbose) {
    fprintf(stderr, "%s[%d] %s DATA(%d):\n",
            sess->log_prefix, stream_id,
            h2_msg_type_str(strm->recv_msg_type), (int)len);
    fwrite(data, 1, len, stdout);
    if (len >= 1 && data[len - 1] != '\n' && data[0] != '\0') {
      fwrite("\n", 1, 1, stdout);
    }
    fflush(stdout);
  }

  // HERE: TODO: here comes the application logic: data chunk received 
  
  return 0;
}

static int ng_strm_close_cb(nghttp2_session *ng_sess, int32_t stream_id,
                            uint32_t error_code, void *user_data) {
  h2_sess *sess = (h2_sess *)user_data;
  h2_strm *strm;

#if 0
  fprintf(stderr, "DEBUG: %s[%d] END OF STREAM: "
          "strm_close_cnt=%d req_cnt=%d rsp_cnt=%d rsp_rst_cnt=%d "
          "err=%u, send_data_remain=%d\n",
          sess->log_prefix, stream_id,
          sess->strm_close_cnt+1, 
          sess->req_cnt, sess->rsp_cnt, sess->rsp_rst_cnt,
          error_code, sess->send_data_remain);
#endif 
  
  if (sess->ctx->verbose) {
    if (error_code) {
      fprintf(stderr, "%s[%d] END OF STREAM (error=%u)\n",
              sess->log_prefix, stream_id, error_code);
    } else {
      fprintf(stderr, "%s[%d] END OF STREAM\n",
              sess->log_prefix, stream_id);
    }
  }

  strm = nghttp2_session_get_stream_user_data(ng_sess, stream_id);
  if (strm == NULL || strm->stream_id != stream_id) {
    return 0;
  }

  sess->strm_close_cnt++;
  sess->send_data_remain -=
    (strm->send_body_sb.data_size - strm->send_body_sb.data_used);
  h2_strm_free(strm);
  nghttp2_session_set_stream_user_data(ng_sess, stream_id, NULL);

  return 0;
}

static int ng_error2_cb(nghttp2_session *ng_sess, int lib_error_code,
                        const char *msg, size_t len, void *user_data) {
  h2_sess *sess = (h2_sess *)user_data;
  (void)ng_sess;

  warnx("%s### NGHTTP2_ERROR: error=%d:%s msg[%d]=%s",
        sess->log_prefix, 
        lib_error_code, nghttp2_strerror(lib_error_code), (int)len, msg);

  return 0;
}


/*
 * HTTP/2 Session Send -----------------------------------------------------
 */

/*
 * Send merge buf size consideration:
 * - min: too small packet causes perf damage including all network components
 * - max: cocurrent streams x req_hdr+data or rsp_hdr+data size
 * - tcp send buf range min value: /proc/sys/net/ipv4/tcp_wmem
 * - TLS record size
 * - tcp MTU: 1360 or less; cf. some public CPs site has MTU 1360
 */

int h2_sess_send_once_v2(h2_sess *sess) {
  h2_wr_buf *wb = &sess->wr_buf;
  int sent, total_sent = 0;
#ifdef TLS_MODE
  SSL *ssl = sess->ssl;
  int r;
#endif
#ifdef EPOLL_MODE
  int mem_send_zero = 0;
#endif

  /* NOTE: send is always blocking */
  /* TODO: save and retry to send on last to_send data */

  if (sess->ctx->verbose &&
      (wb->merge_size > 0 || wb->mem_send_size > 0)) {
    warnx("### DEBUG: REENTRY WITH REMAINING WRITE: "
          "merge_size=%d mem_send_size=%d", wb->merge_size, wb->mem_send_size);
  }

  while (wb->mem_send_size <= 0 && wb->merge_size < H2_WR_BUF_SIZE) {
    const uint8_t *mem_send_data;
    ssize_t mem_send_size;
    
    mem_send_size = nghttp2_session_mem_send(sess->ng_sess, &mem_send_data);
    /* DEBUG: to check mem_send size */
    /* fprintf(stderr, "%d ", (int)mem_send_size); */

    if (mem_send_size < 0) {
      /* probablly NGHTTP2_ERR_NOMEM; abort immediately */
      warnx("nghttp2_session_mem_send() error: %s",
            nghttp2_strerror(mem_send_size));
      sess->close_reason = CLOSE_BY_NGHTTP2_ERR;
      return -1;
    } else if (mem_send_size == 0) {
      /* no more data to send */
#ifdef EPOLL_MODE
      mem_send_zero = 1; 
#endif
      break;
    } else if (wb->merge_size + mem_send_size <= (int)sizeof(wb->merge_data)) {
      /* merge to buf */
      memcpy(&wb->merge_data[wb->merge_size], mem_send_data, mem_send_size);
      wb->merge_size += mem_send_size;
    } else {
      /* cannot merge to buf */
      wb->mem_send_data = (void *)mem_send_data;
      wb->mem_send_size = mem_send_size;
      break;
    }
  }

  /* DEBUG: to check merge_size and mem_send size */
  //fprintf(stderr, "%d+%d ", wb->merge_size, wb->mem_send_size);

  /* try to send merge_data once */
  if (wb->merge_size > 0) {
#ifdef TLS_MODE
    if (ssl) {
      r = SSL_write(ssl, wb->merge_data, wb->merge_size);
      if (r > 0) {
        sent = wb->merge_size;
      } else {  /* r <= 0 */
        if (SSL_get_error(ssl, r) == SSL_ERROR_WANT_WRITE) {
          //fprintf(stderr, "DEBUG: TLS SEND merge_data WOULD BLOCK: "
          //        "to_send=%d\n", (int)wb->merge_size);
          /* NOTE: should be repeated with same buf, and size */
          h2_sess_mark_send_pending(sess);
          return total_sent;  /* retry later */
        }
        warnx("SSL_write(merge_data) error: %d", SSL_get_error(ssl, r));
        sess->close_reason = CLOSE_BY_SSL_ERR;
        return -2;
      }
    } else
#endif
    {
      sent = send(sess->fd, wb->merge_data, wb->merge_size, 0);
      if (sent <= 0) {
        // note: in linux EAGAIN=EWHOULDBLOCK but some oldes are not */
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
          //fprintf(stderr, "DEBUG: TCP SEND merge_data WOULD BLOCK: "
          //        "to_send=%d\n", (int)wb->merge_size);
          h2_sess_mark_send_pending(sess);
          return total_sent;
        }
        if (sess->is_terminated && errno == EPIPE) {
          sess->close_reason = CLOSE_BY_SOCK_EOF;
        } else {
          warnx("send() error with to_send=%d: %s",
                wb->merge_size, strerror(errno));
          sess->close_reason = CLOSE_BY_SOCK_ERR;
        }
        return -3;
      }
    }

    //warnx("### DEBUG: DATA SENT: merge_buf sent=%d", sent);
    total_sent += sent;

    if (sent < wb->merge_size) {
      /* DEBUG: to check partial send for tcp socket buffer overflow */
      if (sess->ctx->verbose) {
        warnx("### DEBUG: MERGE_BUF PARTIAL SEND: %d/%d ",
              sent, wb->merge_size);
      }


      memmove(wb->merge_data, &wb->merge_data[sent], wb->merge_size - sent);
      wb->merge_size -= sent;
      h2_sess_mark_send_pending(sess);
      return total_sent;  /* possible block at send */
    } else {
      wb->merge_size = 0;
    }
  }

  /* try to send mem_send_data once */
  if (wb->mem_send_size) {
#ifdef TLS_MODE
    if (ssl) {
      r = SSL_write(ssl, wb->mem_send_data, wb->mem_send_size);
      if (r > 0) {
        sent = wb->mem_send_size;
      } else {  /* r <= 0 */
        if (SSL_get_error(ssl, r) == SSL_ERROR_WANT_WRITE) {
          //fprintf(stderr, "DEBUG: TLS SEND mem_send_data WOULD BLOCK: "
          //        "to_send=%d\n", (int)wb->mem_send_size);
          /* NOTE: should be repeated with same buf, and size */
          h2_sess_mark_send_pending(sess);
          return total_sent;  /* retry later */
        }
        warnx("SSL_write(mem_send_data) error: %d", SSL_get_error(ssl, r));
        sess->close_reason = CLOSE_BY_SSL_ERR;
        return -2;
      }
    } else
#endif
    {
      sent = send(sess->fd, wb->mem_send_data, wb->mem_send_size, 0);
      if (sent <= 0) {
        // note: in linux EAGAIN=EWHOULDBLOCK but some oldes are not */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          //fprintf(stderr, "DEBUG: TCP SEND mem_send_data WOULD BLOCK: "
          //        "to_send=%d\n", (int)wb->mem_send_size);
          h2_sess_mark_send_pending(sess);
          return total_sent;
        }
        if (sess->is_terminated && errno == EPIPE) {
          sess->close_reason = CLOSE_BY_SOCK_EOF;
        } else {
          warnx("send() error with to_send=%d: %s",
                wb->mem_send_size, strerror(errno));
          sess->close_reason = CLOSE_BY_SOCK_ERR;
        }
        return -5;
      }
    }

    //warnx("### DEBUG: DATA SENT: mem_send sent=%d", sent);
    total_sent += sent;

    if (sent < wb->mem_send_size) {
      /* indication for possible block at next */
      /* DEBUG: to check partial send for tcp socket buffer overflow */
      if (sess->ctx->verbose) {
        fprintf(stderr, "### DEBUG: MEM_SEND_BUF PARTIAL SEND: %d/%d ",
                   sent, wb->mem_send_size);
      }
      wb->mem_send_data += sent;
      wb->mem_send_size -= sent;
      h2_sess_mark_send_pending(sess);
      return total_sent;  /* possible block at send */
    } else {
      wb->mem_send_data = NULL;
      wb->mem_send_size = 0;
    }
  }

  if (total_sent == 0) {
    h2_sess_clear_send_pending(sess);
    if (sess->is_no_more_req && !sess->is_shutdown_send_called) {
      h2_sess_shutdown_send_v2(sess);
      sess->is_shutdown_send_called = 1;
    }
    /*
    static int c = 0;
    c++;
    warnx("### DEBUG: [%d] EXIT WITHOUT SENT DATA: merge_size=%d "
          "mem_send_size=%d", c, wb->merge_size, wb->mem_send_size);
    */
  }

#ifdef EPOLL_MODE
  if (mem_send_zero && !nghttp2_session_want_read(sess->ng_sess)) {
    sess->close_reason = CLOSE_BY_NGHTTP2_END;
    return -6;
  }
#endif

  return total_sent;
}

/*
 * Http2 Settings Handling -------------------------------------------------
 */

int h2_sess_send_settings_v2(h2_sess *sess) {
  nghttp2_settings_entry iv[16];
  int r, iv_num = 0;

  if (sess->http_ver != H2_HTTP_V2) {
    return 0;  /* simply ignored on HTTP/1.1 session */
  }

#define ADD_SETTINGS(_field,_id)  \
  if (sess->settings._field >= 0) {  \
    iv[iv_num].settings_id = _id;  \
    iv[iv_num++].value = sess->settings._field;  \
  }

  ADD_SETTINGS(header_table_size, NGHTTP2_SETTINGS_HEADER_TABLE_SIZE);
  ADD_SETTINGS(enable_push, NGHTTP2_SETTINGS_ENABLE_PUSH);
  ADD_SETTINGS(max_concurrent_streams, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
  ADD_SETTINGS(initial_window_size, NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE);
  ADD_SETTINGS(max_frame_size, NGHTTP2_SETTINGS_MAX_FRAME_SIZE);
  ADD_SETTINGS(max_header_list_size, NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE);
  ADD_SETTINGS(enable_connect_protocol, NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL);

  r = nghttp2_submit_settings(sess->ng_sess, NGHTTP2_FLAG_NONE, iv, iv_num);
  if (r != 0) {
    warnx("submit setting failed: %s", nghttp2_strerror(r));
    return -1;
  }
  return h2_sess_send(sess);
}

int h2_sess_recv_v2(h2_sess *sess, const void *data, int size) {
  int r = nghttp2_session_mem_recv(sess->ng_sess, data, size);
  if (r < 0) {
     warnx("HTTP2 read error; nghttp2_session_mem_recv failed: %s",
           nghttp2_strerror(r)); 
  }
  return r;
}

void h2_sess_init_v2(h2_sess *sess) {
  nghttp2_session_callbacks *cbs;

  nghttp2_session_callbacks_new(&cbs);
  nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, ng_begin_hdr_cb);
  nghttp2_session_callbacks_set_on_header_callback(cbs, ng_header_cb);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, ng_data_cb);
  nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, ng_frame_recv_cb);
  nghttp2_session_callbacks_set_on_stream_close_callback(cbs, ng_strm_close_cb);
  nghttp2_session_callbacks_set_error_callback2(cbs, ng_error2_cb);
  if (sess->is_server) {
    nghttp2_session_server_new(&sess->ng_sess, cbs, sess);
  } else {
    nghttp2_session_client_new(&sess->ng_sess, cbs, sess);
  }
  nghttp2_session_callbacks_del(cbs);
}

void h2_sess_free_v2(h2_sess *sess) {
  if (sess->ng_sess) {
    nghttp2_session_del(sess->ng_sess);
    sess->ng_sess = NULL;
  }
}

void h2_sess_terminate_v2(h2_sess *sess) {
  int r = nghttp2_session_terminate_session(sess->ng_sess, NGHTTP2_NO_ERROR);
  if (r < 0) {
    warnx("%snghttp2_session_terminate_session() failed; ignored: %s",
          sess->log_prefix, nghttp2_strerror(r));
  }
}

void h2_sess_shutdown_send_v2(h2_sess *sess) {
  int n = nghttp2_session_get_next_stream_id(sess->ng_sess) - 1;
  int r = nghttp2_submit_goaway(sess->ng_sess, NGHTTP2_FLAG_NONE,
                                n, NGHTTP2_NO_ERROR, NULL, 0);
  if (r < 0) {
    warnx("%snghttp2_submit_goaway() failed: last_stream_id=%d ret=%d",
          sess->log_prefix, n, r);
    return;
  } 
  h2_sess_mark_send_pending(sess);
}


/*
 * HTTP/1.1 to HTTP/2 Upgrade Handlers -------------------------------------
 */

#if 0
static int base64_encode(void *_org, int org_len, char *buf)
{
  static char encode_map[64] = {
    /* A - Z */
    65, 66, 67, 68, 69, 70, 71, 72,
    73, 74, 75, 76, 77, 78, 79, 80,
    81, 82, 83, 84, 85, 86, 87, 88, 89, 90,
    /* a - z */
    97, 98, 99, 100, 101, 102, 103, 104,
    105, 106, 107, 108, 109, 110, 111, 112,
    113, 114, 115, 116, 117, 118, 119, 120, 121, 122,
    /* 0 - 9 */
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
    /* + / */
    43, 47
  };

  unsigned char *org = (unsigned char *)_org;
  char *e = buf;
  int i, x;

  for (i = 0; i < org_len; i += 3) {
    if (org_len - i >= 3) {
      x = ((int)org[i] << 16) + ((int)org[i+1] << 8) + ((int)org[i+2]);
      e[0] = encode_map[(x >> 18) & 0x3f];
      e[1] = encode_map[(x >> 12) & 0x3f];
      e[2] = encode_map[(x >>  6) & 0x3f];
      e[3] = encode_map[(x) & 0x3f];
    }
    else if (org_len - i == 2) {  /* only two characters remain */
      x = ((int)org[i] << 16) + ((int)org[i+1] << 8);
      e[0] = encode_map[(x >> 18) & 0x3f];
      e[1] = encode_map[(x >> 12) & 0x3f];
      e[2] = encode_map[(x >>  6) & 0x3f];
      e[3] = '=';
    }
    else {  // n - i == 1  /* only one character remains */
      x = ((int)org[i] << 16);
      e[0] = encode_map[(x >> 18) & 0x3f];
      e[1] = encode_map[(x >> 12) & 0x3f];
      e[2] = '=';
      e[3] = '=';
    }
    e += 4;
  }

  /* returns encoded size */
  return e - buf;
}

static int h2_sess_send_upgrade_req_tcp(h2_sess *sess, const char *authority,
                                        h2_settings *settings) {
  char sb[256], sb64[1024];
  int sb64_len = 0;

  if (settings) {
#define ADD_SETTINGS(_id, _field)  \
    if (settings->_field) {  \
      uint16_t id = htons(_id);  \
      uint32_t value = htonl(settings->_field);  \
      memcpy(p + 0, &id, 2);  \
      memcpy(p + 2, &value, 4);  \
      p += 6;  \
    }

    void *p = sb;
    ADD_SETTINGS(NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, header_table_size);
    ADD_SETTINGS(NGHTTP2_SETTINGS_ENABLE_PUSH, enable_push);
    ADD_SETTINGS(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, max_concurrent_streams);
    ADD_SETTINGS(NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, initial_window_size);
    ADD_SETTINGS(NGHTTP2_SETTINGS_MAX_FRAME_SIZE, max_frame_size);
    ADD_SETTINGS(NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, max_header_list_size);
    ADD_SETTINGS(NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL, enable_connect_protocol);
    sb64_len = base64_encode(sb, p - (void *)sb, sb64);
  }
  sb64[sb64_len] = '\0';

  h2_msg_free(req);
  return h2_sess_send(sess);
}
#endif


