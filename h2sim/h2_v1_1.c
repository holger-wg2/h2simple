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

#include "h2.h"
#include "h2_priv.h"


/* HTTP/1.1 reason values per status */
static char *http_status_reason[5][20] = {
  { /*100*/"Continue",
    /*101*/"Switching Protocols"
  },
  { /*200*/"OK",
    /*201*/ "Created",
    /*202*/"Accepted",
    /*203*/"Non-Authoritative Information",
    /*204*/"No Content",
    /*205*/"Reset Content",
    /*206*/"Partial Content",
  },
  { /*300*/"Multiple Choices",
    /*301*/"Moved Permanently",
    /*302*/"Found",
    /*303*/"See Other",
    /*304*/"Not Modified",
    /*305*/"Use Proxy",
    /*307*/"Temporary Redirect"
  },
  {
    /*400*/"Bad Request",
    /*401*/"Unauthorized",
    /*402*/"Payment Required",
    /*403*/"Forbidden",
    /*404*/"Not Found",
    /*405*/"Method Not Allowed",
    /*406*/"Not Acceptable",
    /*407*/"Proxy Authentication Required",
    /*408*/"Request Time-out",
    /*409*/"Conflict",
    /*410*/"Gone",
    /*411*/"Length Required",
    /*412*/"Precondition Failed",
    /*413*/"Request Entity Too Large",
    /*414*/"Request-URI Too Large",
    /*415*/"Unsupported Media Type",
    /*416*/"Requested range not satisfiable",
    /*417*/"Expectation Failed"
  },
  {
    /*500*/"Internal Server Error",
    /*501*/"Not Implemented",
    /*502*/"Bad Gateway",
    /*503*/"Service Unavailable",
    /*504*/"Gateway Time-out",
    /*505*/"HTTP Version not supported"
  }
};


/*
 * HTTP/1.1 Message Send ---------------------------------------------------
 */

static void h2_set_send_data(h2_strm *strm, void *data, int size) {
  /* ASSUME: data!=null, size>0 */
  /* NOTE: the caller should not free data */
  h2_send_buf *send_buf = &strm->send_body_sb;
  send_buf->data = data;
  send_buf->data_size = size;
  send_buf->data_used = 0;
  send_buf->to_be_freed = 1;
  send_buf->msg_type = strm->send_msg_type;
}

int h2_send_request_v1_1(h2_sess *sess, h2_msg *req,
                         h2_response_cb response_cb, void *strm_user_data) {
  char *p, *buf;
  int i, buf_len;

  /* NOTE: buf should contain extra headers */
  buf_len = 64 + h2_sbuf_used(&req->sbuf) + h2_hdr_num(req) * 4 + 
            req->body_len + 1;
  buf = malloc(buf_len);

  /* set header */
  p = buf;
  p += sprintf(p, "%s %s HTTP/1.1\r\n", h2_method(req), h2_path(req));
  p += sprintf(p, "host: %s\r\n", h2_authority(req));
  if (req->body && req->body_len > 0) {
    p += sprintf(p, "content-length: %d\r\n", req->body_len);
  }
  if (sess->settings.single_req) {
    /* HERE: TODO: reimplement single_req */
    p += sprintf(p, "connection: close\r\n");
  }
  for (i = 0; i < req->hdr_num; i++) {
    p += sprintf(p, "%s: %s\r\n",
                 h2_hdr_idx_name(req, i), h2_hdr_idx_value(req, i));
  }
  p += sprintf(p, "\r\n");

  /* set body */
  if (req->body && req->body_len > 0) {
    memcpy(p, req->body, req->body_len);
    p += req->body_len;
  }
  *p = '\0';  /* mark NULL at the end of message */

  /* ASSUME: success */ /* TODO: handled error case */
  h2_strm *strm = h2_strm_init(sess, 2 * sess->req_cnt + 1, H2_RESPONSE,
                               response_cb, strm_user_data);
  sess->req_cnt++;
  strm->is_req = 1;

  /* HERE: TODO: reimplement single_req */
  if (sess->settings.single_req) {
    strm->close_sess = 1;
  }

  /* set send message as read data buf */
  h2_set_send_data(strm, buf, p - buf);
  sess->send_data_remain += strm->send_body_sb.data_size;

  return h2_sess_send(sess);
}

int h2_send_response_v1_1(h2_sess *sess, h2_strm *strm, h2_msg *rsp) {
  char *p, *buf, *reason = NULL;
  int s, i, j, buf_len;

  s = h2_status(rsp); 
  i = s / 100;
  j = s % 100;
  if (i >= 1 && i <= 5 && j >= 0 && j <= 19) {
    reason = http_status_reason[i - 1][j];
  }
  if (reason == NULL) {
    reason = "Unknown";
  }

  buf_len = 64 + h2_sbuf_used(&rsp->sbuf) + h2_hdr_num(rsp) * 4 + rsp->body_len;
  buf = malloc(buf_len);

  /* set header */
  p = buf;
  p += sprintf(p, "%d %s\r\n", s, reason);
  if (rsp->body && rsp->body_len > 0) {
    p += sprintf(p, "content-length: %d\r\n", rsp->body_len);
  }
  if (sess->settings.single_req) {
    /* HERE: TODO: reimplement single_req */
    p += sprintf(p, "connection: close\r\n");
    strm->close_sess = 1;
  }
  for (i = 0; i < rsp->hdr_num; i++) {
    p += sprintf(p, "%s: %s\r\n",
                 h2_hdr_idx_name(rsp, i), h2_hdr_idx_value(rsp, i));
  }
  p += sprintf(p, "\r\n");

  /* set body */
  if (rsp->body && rsp->body_len > 0) {
    memcpy(p, rsp->body, rsp->body_len);
    p += rsp->body_len;
  }

  /* set send message as read data buf */
  h2_set_send_data(strm, buf, p - buf);

  /* mark response to send */
  strm->is_rsp_set = 1;
  sess->send_data_remain += strm->send_body_sb.data_size;
  sess->rsp_cnt++;
  h2_sess_mark_send_pending(sess);

  /* HERE: TODO: check for terminate case by strm->close_sess */

  return 0;
}


/*
 * HTTP/1.1 I/O Handlers -----------------------------------------------------
 */

static int h2_sess_recv_hdl_once_v1_1(h2_sess *sess) {
  /* returns 1(message parse completed), 0(parse not completed), <0(error) */
  h2_msg *rmsg;
  if (sess->strm_recving) {
    rmsg = sess->strm_recving->rmsg;
  } else {
    /* check for starting new strm and rmsg */
    if (sess->is_server) {
      sess->strm_recving = h2_strm_init(sess, sess->req_cnt * 2 + 1,
                                        H2_REQUEST, NULL, NULL);
    } else {  /* client */
      if (sess->strm_list_head.next == NULL) {
        warnx("%sHTTP/1.1 response received for no request at %d: "
              "rdata_used=%d rdata_size=%d",
              sess->log_prefix, sess->rdata_offset + sess->rdata_used,
              sess->rdata_used, sess->rdata_size);
        return -1;
      }
      sess->strm_recving = sess->strm_list_head.next;
    }
    sess->rmsg_header_done = 0;
    sess->rmsg_header_line = 0;
    sess->rmsg_content_length = 0;
    rmsg = sess->strm_recving->rmsg;
  }

  /* check and parse http header */
  if (!sess->rmsg_header_done) {
    char *base = sess->rdata + sess->rdata_used;
    char *limit = sess->rdata + sess->rdata_size;
    char *p, *q;

    p = base;
    while (p < limit && (p = memchr(p, '\n', limit - p))) {
      char *end = p;
      /* remove '\r\n' from end pointer */
      if (end > base && *(end -1) == '\r') {
        end--;
      }
      /* check emplty line for header end mark */
      if (end == base) { 
        sess->rdata_used = p + 1 - sess->rdata;
        sess->rmsg_header_done = 1;
        break;
      }
      if (sess->rmsg_header_line == 0) {
        /* first line special handling */
        if (sess->is_server) {
          /* parse Request-Line */
          /* trim off version and spaces */
          if (end >= base + 1/*method*/ + 1 + 1/*path*/ + 1 + 8/*version*/ &&
              !memcmp(end - 8, "HTTP/1.1", 8)) {
            end -= 8; 
          } else {
            warnx("%sHTTP/1.1 request line parse failed at %ld",
                  sess->log_prefix, sess->rdata_offset + base - sess->rdata);
            return -1;
          }
          while (end >= base + 1/*method*/ + 1 + 1/*path*/ &&
                 (*(end - 1) == ' ' || *(end - 1) == '\t')) {
            end--;
          }
          /* find method */
          for (q = base; q < end && *q != ' ' && *q != '\t'; q++) {
            /* to count non-space chars */
          }
          h2_set_method_n(rmsg, base, q - base);
          h2_set_scheme(rmsg, (sess->ssl)? "https" : "http");
          h2_set_authority(rmsg,  "http");
          /* find path */
          for (; q < end && (*q == ' ' || *q == '\t'); q++) {
            /* to skip spaces */
          }
          h2_set_path_n(rmsg, q, end - q);
        } else {
          /* parse Status-Line */
          q = base;
          if (end - base >= 3 &&
              q[0] >= '1' && q[0] <= '5' &&
              q[1] >= '0' && q[1] <= '9' &&
              q[2] >= '0' && q[2] <= '9' &&
              (end - base <= 3 || (q[3] == ' ' || q[3] == '\t'))) {
            /* NOTE: ignore reason part */
            h2_set_status(rmsg,
                          ((int)(q[0] - '0')) * 100 +
                          ((int)(q[1] - '0')) * 10 +
                          ((int)(q[2] - '0')) * 1);
          } else {
            warnx("%sHTTP/1.1 status line parse failed at %ld",
                  sess->log_prefix, sess->rdata_offset + base - sess->rdata);
            return -2;
          }
        }
      } else {
        /* parse message-header */
        /* TODO: NEED TO HANDLE MULTI LINE VALUE */
        if ((q = memchr(base, ':', end - base))) {
          char *name = base;
          int nlen = q - base;
          /* trim off space heading and tail from value */
          char *value = q + 1;
          while (value < end && (*value == ' ' || *value == '\t')) {
            value++;
          }
          while (end > value && (*(end - 1) == ' ' || *(end - 1) == '\t')) {
            end--;
          }
          /* now append header */
          if (nlen == 4 && !strncasecmp(name, "host", 4) &&
              sess->is_server) {
            h2_set_authority_n(rmsg, value, end - value);
          } else if (nlen == 14 && !strncasecmp(name, "content-length", 14)) {
            /* TODO: NEED MORE STRICT CHECKING */
            sess->rmsg_content_length = atoi(value); 
          } else if (nlen == 10 && !strncasecmp(name, "connection", 10)) {
            if (!strcasecmp(value, "close")) {
               sess->strm_recving->close_sess = 1;
            }
            /* TODO: ELSE MAY NEED TO HANDLE <header fields> to remove */
          } else if (nlen == 10 && !strncasecmp(name, "keep-alive", 10)) {
            /* just ignore */
          } else {
            h2_add_hdr_n(rmsg, name, nlen, value, end - value);
          }
        } else {
          warnx("%sHTTP/1.1 message header parse failed at %ld",
                sess->log_prefix, sess->rdata_offset + base - sess->rdata);
          return -3;
        }
      }
      p ++;
      base = p;
      sess->rdata_used = base - sess->rdata;
      sess->rmsg_header_line++;
    }
    /* now, create meassage and parse header */
  }

  /* check and parse http body */
  if (sess->rmsg_header_done) {
    if (sess->rmsg_content_length && h2_body_len(rmsg) == 0) {
      /* check for data avaiable for content_length */
      /* TODO: NEED TO HANDLE Chunked Body case */
      if (sess->rdata_size - sess->rdata_used >= sess->rmsg_content_length) {
        h2_cpy_body(rmsg, sess->rdata + sess->rdata_used,
                    sess->rmsg_content_length);
        sess->rdata_used += sess->rmsg_content_length; 
      }
    }
    if (sess->rmsg_content_length == h2_body_len(rmsg)) {
      /* now, message compeleted, call app callbacks */
      int r;
      if (sess->is_server) {
        sess->req_cnt++;
        r = h2_on_request_recv(sess, sess->strm_recving);
        sess->strm_recving = NULL;
        /* NOTE: on sess.singe_req, sess is closed after send reponse */
      } else {  /* client */
        r = h2_on_response_recv(sess, sess->strm_recving);
        h2_strm_free(sess->strm_recving);
        sess->strm_close_cnt++;
        sess->strm_recving = NULL;
      }
      return (r >= 0)? 1 : r;
    }
  }
  return 0;
}

int h2_sess_recv_v1_1(h2_sess *sess, const void *data, int size) {
  /* append to rdata */
  if (sess->rdata == NULL) {
#define RDATA_ALLOC_DEF  (16 * 1024)
    sess->rdata_alloced = (size >= RDATA_ALLOC_DEF)? size : RDATA_ALLOC_DEF;
    sess->rdata = malloc(sess->rdata_alloced);
    memcpy(sess->rdata, data, size);
    sess->rdata_size = size; 
    sess->rdata_used = 0;
  } else if (sess->rdata_alloced >= sess->rdata_size + size) {
    /* just append new data */ 
    memcpy(sess->rdata + sess->rdata_size, data, size);
    sess->rdata_size += size;
  } else {
    /* realloc buffer and remove rdata_used */
    if (sess->rdata_alloced < sess->rdata_size - sess->rdata_used + size) {
      sess->rdata_alloced = sess->rdata_size - sess->rdata_used + size;
      sess->rdata = realloc(sess->rdata, sess->rdata_alloced); 
    }
    sess->rdata_size -= sess->rdata_used;
    sess->rdata_offset += sess->rdata_used;
    if (sess->rdata_size) {
      memmove(sess->rdata, sess->rdata + sess->rdata_used, sess->rdata_size);
    } 
    sess->rdata_used = 0;
    /* append new data */ 
    memcpy(sess->rdata + sess->rdata_size, data, size);
    sess->rdata_size += size;
  }

  /* try to parse and handle message */
  int r;
  while ((r = h2_sess_recv_hdl_once_v1_1(sess)) == 1) {
    if (sess->rdata_used == sess->rdata_size ||
        sess->is_terminated ||
        (sess->is_no_more_req && sess->req_cnt == sess->rsp_cnt)) {
      break;
    } /* else repeat for reamaing data */
  }
  if (r < 0) {
    warnx("%sHTTP/1.1 read error: h2_sess_recv_hdl_once_v1_1() failed: ret=%d",
          sess->log_prefix, r);
    return -1;
  }

  /* on all rdata handled, deallocated extended buffer */
  if (sess->rdata_used == sess->rdata_size &&
      sess->rdata_alloced > RDATA_ALLOC_DEF) {
    sess->rdata_offset += sess->rdata_used;
    free(sess->rdata);
    sess->rdata = NULL;
    sess->rdata_alloced = 0;
    sess->rdata_size = 0;
    sess->rdata_used = 0;
  }
  return size;
}

int h2_sess_send_once_v1_1(h2_sess *sess) {
  h2_wr_buf *wb = &sess->wr_buf;
  int sent, total_sent = 0;
#ifdef TLS_MODE
  SSL *ssl = sess->ssl;
  int r;
#endif

  /* NOTE: send is always blocking */

  if (wb->merge_size > 0 || wb->mem_send_size > 0) {
    warnx("### DEBUG: REENTRY WITH REMAINING WRITE: "
          "merge_size=%d mem_send_size=%d", wb->merge_size, wb->mem_send_size);
  }

  /* try to use stream's read buffer for send data */
  while (wb->mem_send_size <= 0 && wb->merge_size < H2_WR_BUF_SIZE) {
    const uint8_t *strm_send_data = NULL;
    ssize_t strm_send_size = 0;
    h2_strm *strm, *strm_next;
    h2_send_buf *sb;
   
    /* TODO: O BE REIMPLEMNTED */
    if (sess->is_server) {
      /* check and flush last sent strm */
      for (strm = sess->strm_list_head.next; strm && strm->is_rsp_set;
           strm = strm_next) {
        strm_next = strm->next;
        sb = &strm->send_body_sb;
        if (sb->data_used >= sb->data_size) {
          h2_strm_free(strm);  /* strm.data are all sent; free stream */
          sess->strm_close_cnt++;
          //{
          //  static int n = 0;
          //  printf("DEBUG[%d]: req_cnt=%d rsp_cnt=%d strm_close_cnt=%d\n",
          //         n, sess->req_cnt, sess->rsp_cnt, sess->strm_close_cnt);
          //}
          continue;

        } else {  /* found data to send */
          strm_send_data = sb->data + sb->data_used;
          strm_send_size = sb->data_size - sb->data_used;
          sb->data_used = sb->data_size;
          break;
        }
      }
    } else {  /* client */
      if (sess->strm_sending == NULL) {
        sess->strm_sending = sess->strm_list_head.next;
      } 
      while ((strm = sess->strm_sending)) {
        sb = &strm->send_body_sb;
        if (sb->data_used >= sb->data_size) {
          sess->strm_sending = strm->next;
        } else {
          strm_send_data = sb->data + sb->data_used;
          strm_send_size = sb->data_size - sb->data_used;
          sb->data_used = sb->data_size;
          break;
        }
      }
    }

    /* DEBUG: to check mem_send size */
    /* fprintf(stderr, "%d ", (int)strm_send_size); */

    // HERE: TODO: MARK CURRENT STREAM FOR MEM_SEND_DATA
    if (strm_send_size == 0) {
      /* no more data to send */
      break;
    } else if (wb->merge_size + strm_send_size <= (int)sizeof(wb->merge_data)) {
      /* merge to buf */
      memcpy(&wb->merge_data[wb->merge_size], strm_send_data, strm_send_size);
      wb->merge_size += strm_send_size;
    } else {
      /* cannot merge to buf */
      wb->mem_send_data = (void *)strm_send_data;
      wb->mem_send_size = strm_send_size;
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
          fprintf(stderr, "DEBUG: TLS SEND merge_data WOULD BLOCK: "
                  "to_send=%d\n", (int)wb->merge_size);
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
          fprintf(stderr, "DEBUG: TCP SEND merge_data WOULD BLOCK: "
                  "to_send=%d\n", (int)wb->merge_size);
          h2_sess_mark_send_pending(sess);
          return total_sent;
        }
        warnx("send() error on merge_date with merget_size=%d: %s",
              wb->merge_size, strerror(errno));
        sess->close_reason = CLOSE_BY_SOCK_ERR;
        return -3;
      }
    }

    //warnx("### DEBUG: DATA SENT: merge_buf sent=%d", sent);
    total_sent += sent;
    sess->send_data_remain -= sent;

    if (sent < wb->merge_size) {
      /* DEBUG: to check partial send for tcp socket buffer overflow */
      warnx("### DEBUG: MERGE_BUF PARTIAL!!! %d/%d ", sent, wb->merge_size);

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
          fprintf(stderr, "DEBUG: TLS SEND mem_send_data WOULD BLOCK: "
                  "to_send=%d\n", (int)wb->mem_send_size);
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
          fprintf(stderr, "DEBUG: TCP SEND mem_send_data WOULD BLOCK: "
                  "to_send=%d\n", (int)wb->mem_send_size);
          h2_sess_mark_send_pending(sess);
          return total_sent;
        }
        warnx("send() error on mem_send_data with mem_send_size=%d: %s",
              wb->mem_send_size, strerror(errno));
        sess->close_reason = CLOSE_BY_SOCK_ERR;
        return -5;
      }
    }

    //warnx("### DEBUG: DATA SENT: mem_send sent=%d", sent);
    total_sent += sent;
    sess->send_data_remain -= sent;

    if (sent < wb->mem_send_size) { 
      /* indication for possible block at next */
      /* DEBUG: to check partial send for tcp socket buffer overflow */
      fprintf(stderr, "### DEBUG: MEM_SEND PARTIAL!!!%d/%d ",
              sent, wb->mem_send_size);

      wb->mem_send_data += sent;
      wb->mem_send_size -= sent;
      h2_sess_mark_send_pending(sess);
      return total_sent;  /* possible block at send */
    } else {
      wb->mem_send_data = NULL;
      wb->mem_send_size = 0;
    }
  }

  /* NOTE: check total_send=0 to test if no more data to send */
  /*       causes 1 redundent write event instead of to send rechecking here */
  if (total_sent == 0) {
    h2_sess_clear_send_pending(sess);
    /* close session on singgle_req mode */ 
    if (sess->is_no_more_req && !sess->is_shutdown_send_called) {
      h2_sess_shutdown_send_v1_1(sess);
      sess->is_shutdown_send_called = 1;
    }
    /*
    static int c = 0;
    c++;
    warnx("### DEBUG: [%d] EXIT WITHOUT SENT DATA: merge_size=%d "
          "mem_send_size=%d", c, wb->merge_size, wb->mem_send_size);
    */
  }

  return total_sent;
}


/*
 * HTTP/1.1 Session Management ---------------------------------------------
 */

void h2_sess_terminate_v1_1(h2_sess *sess) {
  if (sess->ssl) {
    SSL_set_shutdown(sess->ssl, SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);
    SSL_shutdown(sess->ssl);
  } else {
    shutdown(sess->fd, SHUT_WR | SHUT_RD);
  } 
}

void h2_sess_shutdown_send_v1_1(h2_sess *sess) {
  /* HERE: TODO: MAY NEED TO SET LINGER OPTION */
  if (sess->ssl) {
    SSL_set_shutdown(sess->ssl, SSL_SENT_SHUTDOWN);
  } else {
    /* HERE: TODO: socket half not working now */
    //shutdown(sess->fd, SHUT_WR);
  }
}

