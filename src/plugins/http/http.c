/*
 * Copyright (c) 2022 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <http/http.h>
#include <vnet/session/session.h>
#include <http/http_timer.h>
#include <http/http_status_codes.h>

static http_main_t http_main;

#define HTTP_FIFO_THRESH (16 << 10)

/* HTTP state machine result */
typedef enum http_sm_result_t_
{
  HTTP_SM_STOP = 0,
  HTTP_SM_CONTINUE = 1,
  HTTP_SM_ERROR = -1,
} http_sm_result_t;

const http_buffer_type_t msg_to_buf_type[] = {
  [HTTP_MSG_DATA_INLINE] = HTTP_BUFFER_FIFO,
  [HTTP_MSG_DATA_PTR] = HTTP_BUFFER_PTR,
};

static u8 *
format_http_state (u8 *s, va_list *va)
{
  http_state_t state = va_arg (*va, http_state_t);

  switch (state)
    {
    case HTTP_STATE_IDLE:
      return format (s, "idle");
    case HTTP_STATE_WAIT_APP_METHOD:
      return format (s, "wait app method");
    case HTTP_STATE_WAIT_SERVER_REPLY:
      return format (s, "wait server reply");
    case HTTP_STATE_CLIENT_IO_MORE_DATA:
      return format (s, "client io more data");
    case HTTP_STATE_WAIT_CLIENT_METHOD:
      return format (s, "wait client method");
    case HTTP_STATE_WAIT_APP_REPLY:
      return format (s, "wait app reply");
    case HTTP_STATE_APP_IO_MORE_DATA:
      return format (s, "app io more data");
    default:
      break;
    }
  return format (s, "unknown");
}

#define http_state_change(_hc, _state)                                        \
  do                                                                          \
    {                                                                         \
      HTTP_DBG (1, "changing http state %U -> %U", format_http_state,         \
		(_hc)->http_state, format_http_state, _state);                \
      (_hc)->http_state = _state;                                             \
    }                                                                         \
  while (0)

static inline int
http_state_is_tx_valid (http_conn_t *hc)
{
  http_state_t state = hc->http_state;
  return (state == HTTP_STATE_APP_IO_MORE_DATA ||
	  state == HTTP_STATE_WAIT_APP_REPLY ||
	  state == HTTP_STATE_WAIT_APP_METHOD);
}

static inline int
http_state_is_rx_valid (http_conn_t *hc)
{
  http_state_t state = hc->http_state;
  return (state == HTTP_STATE_WAIT_SERVER_REPLY ||
	  state == HTTP_STATE_CLIENT_IO_MORE_DATA ||
	  state == HTTP_STATE_WAIT_CLIENT_METHOD);
}

static inline http_worker_t *
http_worker_get (u32 thread_index)
{
  return &http_main.wrk[thread_index];
}

static inline u32
http_conn_alloc_w_thread (u32 thread_index)
{
  http_worker_t *wrk = http_worker_get (thread_index);
  http_conn_t *hc;

  pool_get_aligned_safe (wrk->conn_pool, hc, CLIB_CACHE_LINE_BYTES);
  clib_memset (hc, 0, sizeof (*hc));
  hc->c_thread_index = thread_index;
  hc->h_hc_index = hc - wrk->conn_pool;
  hc->h_pa_session_handle = SESSION_INVALID_HANDLE;
  hc->h_tc_session_handle = SESSION_INVALID_HANDLE;
  return hc->h_hc_index;
}

static inline http_conn_t *
http_conn_get_w_thread (u32 hc_index, u32 thread_index)
{
  http_worker_t *wrk = http_worker_get (thread_index);
  return pool_elt_at_index (wrk->conn_pool, hc_index);
}

static inline http_conn_t *
http_conn_get_w_thread_if_valid (u32 hc_index, u32 thread_index)
{
  http_worker_t *wrk = http_worker_get (thread_index);
  if (pool_is_free_index (wrk->conn_pool, hc_index))
    return 0;
  return pool_elt_at_index (wrk->conn_pool, hc_index);
}

void
http_conn_free (http_conn_t *hc)
{
  http_worker_t *wrk = http_worker_get (hc->c_thread_index);
  pool_put (wrk->conn_pool, hc);
}

static inline http_conn_t *
http_ho_conn_get (u32 ho_hc_index)
{
  http_main_t *hm = &http_main;
  return pool_elt_at_index (hm->ho_conn_pool, ho_hc_index);
}

void
http_ho_conn_free (http_conn_t *ho_hc)
{
  http_main_t *hm = &http_main;
  pool_put (hm->ho_conn_pool, ho_hc);
}

static inline u32
http_ho_conn_alloc (void)
{
  http_main_t *hm = &http_main;
  http_conn_t *hc;

  pool_get_aligned_safe (hm->ho_conn_pool, hc, CLIB_CACHE_LINE_BYTES);
  clib_memset (hc, 0, sizeof (*hc));
  hc->h_hc_index = hc - hm->ho_conn_pool;
  hc->h_pa_session_handle = SESSION_INVALID_HANDLE;
  hc->h_tc_session_handle = SESSION_INVALID_HANDLE;
  hc->timeout = HTTP_CONN_TIMEOUT;
  return hc->h_hc_index;
}

static u32
http_listener_alloc (void)
{
  http_main_t *hm = &http_main;
  http_conn_t *lhc;

  pool_get_zero (hm->listener_pool, lhc);
  lhc->c_c_index = lhc - hm->listener_pool;
  lhc->timeout = HTTP_CONN_TIMEOUT;
  return lhc->c_c_index;
}

http_conn_t *
http_listener_get (u32 lhc_index)
{
  return pool_elt_at_index (http_main.listener_pool, lhc_index);
}

void
http_listener_free (http_conn_t *lhc)
{
  http_main_t *hm = &http_main;

  vec_free (lhc->app_name);
  if (CLIB_DEBUG)
    memset (lhc, 0xfc, sizeof (*lhc));
  pool_put (hm->listener_pool, lhc);
}

void
http_disconnect_transport (http_conn_t *hc)
{
  vnet_disconnect_args_t a = {
    .handle = hc->h_tc_session_handle,
    .app_index = http_main.app_index,
  };

  hc->state = HTTP_CONN_STATE_CLOSED;

  if (vnet_disconnect_session (&a))
    clib_warning ("disconnect returned");
}

static void
http_conn_invalidate_timer_cb (u32 hs_handle)
{
  http_conn_t *hc;

  hc =
    http_conn_get_w_thread_if_valid (hs_handle & 0x00FFFFFF, hs_handle >> 24);

  HTTP_DBG (1, "hc [%u]%x", hs_handle >> 24, hs_handle & 0x00FFFFFF);
  if (!hc)
    {
      HTTP_DBG (1, "already deleted");
      return;
    }

  hc->timer_handle = HTTP_TIMER_HANDLE_INVALID;
  hc->pending_timer = 1;
}

static void
http_conn_timeout_cb (void *hc_handlep)
{
  http_conn_t *hc;
  uword hs_handle;

  hs_handle = pointer_to_uword (hc_handlep);
  hc =
    http_conn_get_w_thread_if_valid (hs_handle & 0x00FFFFFF, hs_handle >> 24);

  HTTP_DBG (1, "hc [%u]%x", hs_handle >> 24, hs_handle & 0x00FFFFFF);
  if (!hc)
    {
      HTTP_DBG (1, "already deleted");
      return;
    }

  if (!hc->pending_timer)
    {
      HTTP_DBG (1, "timer not pending");
      return;
    }

  session_transport_closing_notify (&hc->connection);
  http_disconnect_transport (hc);
}

int
http_ts_accept_callback (session_t *ts)
{
  session_t *ts_listener, *as, *asl;
  app_worker_t *app_wrk;
  http_conn_t *lhc, *hc;
  u32 hc_index, thresh;
  int rv;

  ts_listener = listen_session_get_from_handle (ts->listener_handle);
  lhc = http_listener_get (ts_listener->opaque);

  hc_index = http_conn_alloc_w_thread (ts->thread_index);
  hc = http_conn_get_w_thread (hc_index, ts->thread_index);
  clib_memcpy_fast (hc, lhc, sizeof (*lhc));
  hc->timer_handle = HTTP_TIMER_HANDLE_INVALID;
  hc->c_thread_index = ts->thread_index;
  hc->h_hc_index = hc_index;

  hc->h_tc_session_handle = session_handle (ts);
  hc->c_flags |= TRANSPORT_CONNECTION_F_NO_LOOKUP;

  hc->state = HTTP_CONN_STATE_ESTABLISHED;
  http_state_change (hc, HTTP_STATE_WAIT_CLIENT_METHOD);

  ts->session_state = SESSION_STATE_READY;
  ts->opaque = hc_index;

  /*
   * Alloc session and initialize
   */
  as = session_alloc (hc->c_thread_index);
  hc->c_s_index = as->session_index;

  as->app_wrk_index = hc->h_pa_wrk_index;
  as->connection_index = hc->c_c_index;
  as->session_state = SESSION_STATE_ACCEPTING;

  asl = listen_session_get_from_handle (lhc->h_pa_session_handle);
  as->session_type = asl->session_type;
  as->listener_handle = lhc->h_pa_session_handle;

  /*
   * Init session fifos and notify app
   */
  if ((rv = app_worker_init_accepted (as)))
    {
      HTTP_DBG (1, "failed to allocate fifos");
      hc->h_pa_session_handle = SESSION_INVALID_HANDLE;
      session_free (as);
      return rv;
    }

  hc->h_pa_session_handle = session_handle (as);
  hc->h_pa_wrk_index = as->app_wrk_index;
  app_wrk = app_worker_get (as->app_wrk_index);

  HTTP_DBG (1, "Accepted on listener %u new connection [%u]%x",
	    ts_listener->opaque, vlib_get_thread_index (), hc_index);

  if ((rv = app_worker_accept_notify (app_wrk, as)))
    {
      HTTP_DBG (0, "app accept returned");
      session_free (as);
      return rv;
    }

  /* Avoid enqueuing small chunks of data on transport tx notifications. If
   * the fifo is small (under 16K) we set the threshold to it's size, meaning
   * a notification will be given when the fifo empties.
   */
  ts = session_get_from_handle (hc->h_tc_session_handle);
  thresh = clib_min (svm_fifo_size (ts->tx_fifo), HTTP_FIFO_THRESH);
  svm_fifo_set_deq_thresh (ts->tx_fifo, thresh);

  http_conn_timer_start (hc);

  return 0;
}

static int
http_ts_connected_callback (u32 http_app_index, u32 ho_hc_index, session_t *ts,
			    session_error_t err)
{
  u32 new_hc_index;
  session_t *as;
  http_conn_t *hc, *ho_hc;
  app_worker_t *app_wrk;
  int rv;

  ho_hc = http_ho_conn_get (ho_hc_index);
  ASSERT (ho_hc->state == HTTP_CONN_STATE_CONNECTING);

  if (err)
    {
      clib_warning ("half-open hc index %d, error: %U", ho_hc_index,
		    format_session_error, err);
      app_wrk = app_worker_get_if_valid (ho_hc->h_pa_wrk_index);
      if (app_wrk)
	app_worker_connect_notify (app_wrk, 0, err, ho_hc->h_pa_app_api_ctx);
      return 0;
    }

  new_hc_index = http_conn_alloc_w_thread (ts->thread_index);
  hc = http_conn_get_w_thread (new_hc_index, ts->thread_index);

  clib_memcpy_fast (hc, ho_hc, sizeof (*hc));

  hc->timer_handle = HTTP_TIMER_HANDLE_INVALID;
  hc->c_thread_index = ts->thread_index;
  hc->h_tc_session_handle = session_handle (ts);
  hc->c_c_index = new_hc_index;
  hc->c_flags |= TRANSPORT_CONNECTION_F_NO_LOOKUP;
  hc->state = HTTP_CONN_STATE_ESTABLISHED;
  http_state_change (hc, HTTP_STATE_WAIT_APP_METHOD);

  ts->session_state = SESSION_STATE_READY;
  ts->opaque = new_hc_index;

  /* allocate app session and initialize */

  as = session_alloc (hc->c_thread_index);
  hc->c_s_index = as->session_index;
  as->connection_index = hc->c_c_index;
  as->app_wrk_index = hc->h_pa_wrk_index;
  as->session_state = SESSION_STATE_READY;
  as->opaque = hc->h_pa_app_api_ctx;
  as->session_type = session_type_from_proto_and_ip (
    TRANSPORT_PROTO_HTTP, session_type_is_ip4 (ts->session_type));

  HTTP_DBG (1, "half-open hc index %x,  hc [%u]%x", ho_hc_index,
	    ts->thread_index, new_hc_index);

  app_wrk = app_worker_get (hc->h_pa_wrk_index);
  if (!app_wrk)
    {
      clib_warning ("no app worker");
      return -1;
    }

  if ((rv = app_worker_init_connected (app_wrk, as)))
    {
      HTTP_DBG (1, "failed to allocate fifos");
      session_free (as);
      return rv;
    }
  app_worker_connect_notify (app_wrk, as, err, hc->h_pa_app_api_ctx);
  hc->h_pa_session_handle = session_handle (as);
  http_conn_timer_start (hc);

  return 0;
}

static void
http_ts_disconnect_callback (session_t *ts)
{
  http_conn_t *hc;

  hc = http_conn_get_w_thread (ts->opaque, ts->thread_index);

  if (hc->state < HTTP_CONN_STATE_TRANSPORT_CLOSED)
    hc->state = HTTP_CONN_STATE_TRANSPORT_CLOSED;

  /* Nothing more to rx, propagate to app */
  if (!svm_fifo_max_dequeue_cons (ts->rx_fifo))
    session_transport_closing_notify (&hc->connection);
}

static void
http_ts_reset_callback (session_t *ts)
{
  http_conn_t *hc;

  hc = http_conn_get_w_thread (ts->opaque, ts->thread_index);

  hc->state = HTTP_CONN_STATE_CLOSED;
  http_buffer_free (&hc->tx_buf);
  http_state_change (hc, HTTP_STATE_WAIT_CLIENT_METHOD);
  session_transport_reset_notify (&hc->connection);

  http_disconnect_transport (hc);
}

/**
 * http error boilerplate
 */
static const char *http_error_template = "HTTP/1.1 %s\r\n"
					 "Date: %U GMT\r\n"
					 "Connection: close\r\n"
					 "Content-Length: 0\r\n\r\n";

/**
 * http response boilerplate
 */
static const char *http_response_template = "HTTP/1.1 %s\r\n"
					    "Date: %U GMT\r\n"
					    "Server: %v\r\n"
					    "Content-Length: %llu\r\n"
					    "%s";

/**
 * http request boilerplate
 */
static const char *http_get_request_template = "GET %s HTTP/1.1\r\n"
					       "Host: %v\r\n"
					       "User-Agent: %v\r\n"
					       "%s";

static const char *http_post_request_template = "POST %s HTTP/1.1\r\n"
						"Host: %v\r\n"
						"User-Agent: %v\r\n"
						"Content-Length: %llu\r\n"
						"%s";

static u32
http_send_data (http_conn_t *hc, u8 *data, u32 length)
{
  const u32 max_burst = 64 << 10;
  session_t *ts;
  u32 to_send;
  int rv;

  ts = session_get_from_handle (hc->h_tc_session_handle);

  to_send = clib_min (length, max_burst);
  rv = svm_fifo_enqueue (ts->tx_fifo, to_send, data);
  if (rv <= 0)
    {
      clib_warning ("svm_fifo_enqueue failed, rv %d", rv);
      return 0;
    }

  if (svm_fifo_set_event (ts->tx_fifo))
    session_program_tx_io_evt (ts->handle, SESSION_IO_EVT_TX);

  return rv;
}

static void
http_send_error (http_conn_t *hc, http_status_code_t ec)
{
  http_main_t *hm = &http_main;
  u8 *data;
  f64 now;

  if (ec >= HTTP_N_STATUS)
    ec = HTTP_STATUS_INTERNAL_ERROR;

  now = clib_timebase_now (&hm->timebase);
  data = format (0, http_error_template, http_status_code_str[ec],
		 format_clib_timebase_time, now);
  HTTP_DBG (3, "%v", data);
  http_send_data (hc, data, vec_len (data));
  vec_free (data);
}

static int
http_read_message (http_conn_t *hc)
{
  u32 max_deq;
  session_t *ts;
  int n_read;

  ts = session_get_from_handle (hc->h_tc_session_handle);

  max_deq = svm_fifo_max_dequeue (ts->rx_fifo);
  if (PREDICT_FALSE (max_deq == 0))
    return -1;

  vec_validate (hc->rx_buf, max_deq - 1);
  n_read = svm_fifo_peek (ts->rx_fifo, 0, max_deq, hc->rx_buf);
  ASSERT (n_read == max_deq);
  HTTP_DBG (1, "read %u bytes from rx_fifo", n_read);

  return 0;
}

static void
http_read_message_drop (http_conn_t *hc, u32 len)
{
  session_t *ts;

  ts = session_get_from_handle (hc->h_tc_session_handle);
  svm_fifo_dequeue_drop (ts->rx_fifo, len);
  vec_reset_length (hc->rx_buf);

  if (svm_fifo_is_empty (ts->rx_fifo))
    svm_fifo_unset_event (ts->rx_fifo);
}

static void
http_read_message_drop_all (http_conn_t *hc)
{
  session_t *ts;

  ts = session_get_from_handle (hc->h_tc_session_handle);
  svm_fifo_dequeue_drop_all (ts->rx_fifo);
  vec_reset_length (hc->rx_buf);

  if (svm_fifo_is_empty (ts->rx_fifo))
    svm_fifo_unset_event (ts->rx_fifo);
}

/**
 * @brief Find the first occurrence of the string in the vector.
 *
 * @param vec The vector to be scanned.
 * @param offset Search offset in the vector.
 * @param num Maximum number of characters to be searched if non-zero.
 * @param str The string to be searched.
 *
 * @return @c -1 if the string is not found within the vector; index otherwise.
 */
static inline int
v_find_index (u8 *vec, u32 offset, u32 num, char *str)
{
  int start_index = offset;
  u32 slen = (u32) strnlen_s_inline (str, 16);
  u32 vlen = vec_len (vec);

  ASSERT (slen > 0);

  if (vlen <= slen)
    return -1;

  int end_index = vlen - slen;
  if (num)
    {
      if (num < slen)
	return -1;
      end_index = clib_min (end_index, offset + num - slen);
    }

  for (; start_index <= end_index; start_index++)
    {
      if (!memcmp (vec + start_index, str, slen))
	return start_index;
    }

  return -1;
}

static void
http_identify_optional_query (http_conn_t *hc)
{
  int i;
  for (i = hc->target_path_offset;
       i < (hc->target_path_offset + hc->target_path_len); i++)
    {
      if (hc->rx_buf[i] == '?')
	{
	  hc->target_query_offset = i + 1;
	  hc->target_query_len = hc->target_path_offset + hc->target_path_len -
				 hc->target_query_offset;
	  hc->target_path_len = hc->target_path_len - hc->target_query_len - 1;
	  break;
	}
    }
}

static int
http_get_target_form (http_conn_t *hc)
{
  int i;

  /* "*" */
  if ((hc->rx_buf[hc->target_path_offset] == '*') &&
      (hc->target_path_len == 1))
    {
      hc->target_form = HTTP_TARGET_ASTERISK_FORM;
      return 0;
    }

  /* 1*( "/" segment ) [ "?" query ] */
  if (hc->rx_buf[hc->target_path_offset] == '/')
    {
      /* drop leading slash */
      hc->target_path_len--;
      hc->target_path_offset++;
      hc->target_form = HTTP_TARGET_ORIGIN_FORM;
      http_identify_optional_query (hc);
      return 0;
    }

  /* scheme "://" host [ ":" port ] *( "/" segment ) [ "?" query ] */
  i = v_find_index (hc->rx_buf, hc->target_path_offset, hc->target_path_len,
		    "://");
  if (i > 0)
    {
      hc->target_form = HTTP_TARGET_ABSOLUTE_FORM;
      http_identify_optional_query (hc);
      return 0;
    }

  /* host ":" port */
  for (i = hc->target_path_offset;
       i < (hc->target_path_offset + hc->target_path_len); i++)
    {
      if ((hc->rx_buf[i] == ':') && (isdigit (hc->rx_buf[i + 1])))
	{
	  hc->target_form = HTTP_TARGET_AUTHORITY_FORM;
	  return 0;
	}
    }

  return -1;
}

static int
http_parse_request_line (http_conn_t *hc, http_status_code_t *ec)
{
  int i, target_len;
  u32 next_line_offset, method_offset;

  /* request-line = method SP request-target SP HTTP-version CRLF */
  i = v_find_index (hc->rx_buf, 8, 0, "\r\n");
  if (i < 0)
    {
      clib_warning ("request line incomplete");
      *ec = HTTP_STATUS_BAD_REQUEST;
      return -1;
    }
  HTTP_DBG (2, "request line length: %d", i);
  hc->control_data_len = i + 2;
  next_line_offset = hc->control_data_len;

  /* there should be at least one more CRLF */
  if (vec_len (hc->rx_buf) < (next_line_offset + 2))
    {
      clib_warning ("malformed message, too short");
      *ec = HTTP_STATUS_BAD_REQUEST;
      return -1;
    }

  /*
   * RFC9112 2.2:
   * In the interest of robustness, a server that is expecting to receive and
   * parse a request-line SHOULD ignore at least one empty line (CRLF)
   * received prior to the request-line.
   */
  method_offset = hc->rx_buf[0] == '\r' && hc->rx_buf[1] == '\n' ? 2 : 0;
  /* parse method */
  if (!memcmp (hc->rx_buf + method_offset, "GET ", 4))
    {
      HTTP_DBG (0, "GET method");
      hc->method = HTTP_REQ_GET;
      hc->target_path_offset = method_offset + 4;
    }
  else if (!memcmp (hc->rx_buf + method_offset, "POST ", 5))
    {
      HTTP_DBG (0, "POST method");
      hc->method = HTTP_REQ_POST;
      hc->target_path_offset = method_offset + 5;
    }
  else
    {
      if (hc->rx_buf[method_offset] - 'A' <= 'Z' - 'A')
	{
	  clib_warning ("method not implemented: %8v", hc->rx_buf);
	  *ec = HTTP_STATUS_NOT_IMPLEMENTED;
	  return -1;
	}
      else
	{
	  clib_warning ("not method name: %8v", hc->rx_buf);
	  *ec = HTTP_STATUS_BAD_REQUEST;
	  return -1;
	}
    }

  /* find version */
  i = v_find_index (hc->rx_buf, next_line_offset - 11, 11, " HTTP/");
  if (i < 0)
    {
      clib_warning ("HTTP version not present");
      *ec = HTTP_STATUS_BAD_REQUEST;
      return -1;
    }
  /* verify major version */
  if (isdigit (hc->rx_buf[i + 6]))
    {
      if (hc->rx_buf[i + 6] != '1')
	{
	  clib_warning ("HTTP major version '%c' not supported",
			hc->rx_buf[i + 6]);
	  *ec = HTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED;
	  return -1;
	}
    }
  else
    {
      clib_warning ("HTTP major version '%c' is not digit", hc->rx_buf[i + 6]);
      *ec = HTTP_STATUS_BAD_REQUEST;
      return -1;
    }

  /* parse request-target */
  HTTP_DBG (2, "http at %d", i);
  target_len = i - hc->target_path_offset;
  HTTP_DBG (2, "target_len %d", target_len);
  if (target_len < 1)
    {
      clib_warning ("request-target not present");
      *ec = HTTP_STATUS_BAD_REQUEST;
      return -1;
    }
  hc->target_path_len = target_len;
  hc->target_query_offset = 0;
  hc->target_query_len = 0;
  if (http_get_target_form (hc))
    {
      clib_warning ("invalid target");
      *ec = HTTP_STATUS_BAD_REQUEST;
      return -1;
    }
  HTTP_DBG (2, "request-target path length: %u", hc->target_path_len);
  HTTP_DBG (2, "request-target path offset: %u", hc->target_path_offset);
  HTTP_DBG (2, "request-target query length: %u", hc->target_query_len);
  HTTP_DBG (2, "request-target query offset: %u", hc->target_query_offset);

  /* set buffer offset to nex line start */
  hc->rx_buf_offset = next_line_offset;

  return 0;
}

#define expect_char(c)                                                        \
  if (*p++ != c)                                                              \
    {                                                                         \
      clib_warning ("unexpected character");                                  \
      return -1;                                                              \
    }

#define parse_int(val, mul)                                                   \
  do                                                                          \
    {                                                                         \
      if (!isdigit (*p))                                                      \
	{                                                                     \
	  clib_warning ("expected digit");                                    \
	  return -1;                                                          \
	}                                                                     \
      val += mul * (*p++ - '0');                                              \
    }                                                                         \
  while (0)

static int
http_parse_status_line (http_conn_t *hc)
{
  int i;
  u32 next_line_offset;
  u8 *p, *end;
  u16 status_code = 0;

  i = v_find_index (hc->rx_buf, 0, 0, "\r\n");
  /* status-line = HTTP-version SP status-code SP [ reason-phrase ] CRLF */
  if (i < 0)
    {
      clib_warning ("status line incomplete");
      return -1;
    }
  HTTP_DBG (2, "status line length: %d", i);
  if (i < 12)
    {
      clib_warning ("status line too short (%d)", i);
      return -1;
    }
  hc->control_data_len = i + 2;
  next_line_offset = hc->control_data_len;
  p = hc->rx_buf;
  end = hc->rx_buf + i;

  /* there should be at least one more CRLF */
  if (vec_len (hc->rx_buf) < (next_line_offset + 2))
    {
      clib_warning ("malformed message, too short");
      return -1;
    }

  /* parse version */
  expect_char ('H');
  expect_char ('T');
  expect_char ('T');
  expect_char ('P');
  expect_char ('/');
  expect_char ('1');
  expect_char ('.');
  if (!isdigit (*p++))
    {
      clib_warning ("invalid HTTP minor version");
      return -1;
    }

  /* skip space(s) */
  if (*p != ' ')
    {
      clib_warning ("no space after HTTP version");
      return -1;
    }
  do
    {
      p++;
      if (p == end)
	{
	  clib_warning ("no status code");
	  return -1;
	}
    }
  while (*p == ' ');

  /* parse status code */
  if ((end - p) < 3)
    {
      clib_warning ("not enough characters for status code");
      return -1;
    }
  parse_int (status_code, 100);
  parse_int (status_code, 10);
  parse_int (status_code, 1);
  if (status_code < 100 || status_code > 599)
    {
      clib_warning ("invalid status code %d", status_code);
      return -1;
    }
  hc->status_code = status_code;
  HTTP_DBG (0, "status code: %d", hc->status_code);

  /* set buffer offset to nex line start */
  hc->rx_buf_offset = next_line_offset;

  return 0;
}

static int
http_identify_headers (http_conn_t *hc, http_status_code_t *ec)
{
  int i;

  /* check if we have any header */
  if ((hc->rx_buf[hc->rx_buf_offset] == '\r') &&
      (hc->rx_buf[hc->rx_buf_offset + 1] == '\n'))
    {
      /* just another CRLF -> no headers */
      HTTP_DBG (2, "no headers");
      hc->headers_len = 0;
      hc->control_data_len += 2;
      return 0;
    }

  /* find empty line indicating end of header section */
  i = v_find_index (hc->rx_buf, hc->rx_buf_offset, 0, "\r\n\r\n");
  if (i < 0)
    {
      clib_warning ("cannot find header section end");
      *ec = HTTP_STATUS_BAD_REQUEST;
      return -1;
    }
  hc->headers_offset = hc->rx_buf_offset;
  hc->headers_len = i - hc->rx_buf_offset + 2;
  hc->control_data_len += (hc->headers_len + 2);
  HTTP_DBG (2, "headers length: %u", hc->headers_len);
  HTTP_DBG (2, "headers offset: %u", hc->headers_offset);

  return 0;
}

static int
http_identify_message_body (http_conn_t *hc, http_status_code_t *ec)
{
  int i, value_len;
  u8 *end, *p, *value_start;
  u64 body_len = 0, digit;

  hc->body_len = 0;

  if (hc->headers_len == 0)
    {
      HTTP_DBG (2, "no header, no message-body");
      return 0;
    }

  /* TODO check for chunked transfer coding */

  /* try to find Content-Length header */
  i = v_find_index (hc->rx_buf, hc->headers_offset, hc->headers_len,
		    "Content-Length:");
  if (i < 0)
    {
      HTTP_DBG (2, "Content-Length header not present, no message-body");
      return 0;
    }
  hc->rx_buf_offset = i + 15;

  i = v_find_index (hc->rx_buf, hc->rx_buf_offset, hc->headers_len, "\r\n");
  if (i < 0)
    {
      clib_warning ("end of line missing");
      *ec = HTTP_STATUS_BAD_REQUEST;
      return -1;
    }
  value_len = i - hc->rx_buf_offset;
  if (value_len < 1)
    {
      clib_warning ("invalid header, content length value missing");
      *ec = HTTP_STATUS_BAD_REQUEST;
      return -1;
    }

  end = hc->rx_buf + hc->rx_buf_offset + value_len;
  p = hc->rx_buf + hc->rx_buf_offset;
  /* skip leading whitespace */
  while (1)
    {
      if (p == end)
	{
	  clib_warning ("value not found");
	  *ec = HTTP_STATUS_BAD_REQUEST;
	  return -1;
	}
      else if (*p != ' ' && *p != '\t')
	{
	  break;
	}
      p++;
      value_len--;
    }
  value_start = p;
  /* skip trailing whitespace */
  p = value_start + value_len - 1;
  while (*p == ' ' || *p == '\t')
    {
      p--;
      value_len--;
    }

  if (value_len < 1)
    {
      clib_warning ("value not found");
      *ec = HTTP_STATUS_BAD_REQUEST;
      return -1;
    }

  p = value_start;
  for (i = 0; i < value_len; i++)
    {
      /* check for digit */
      if (!isdigit (*p))
	{
	  clib_warning ("expected digit");
	  *ec = HTTP_STATUS_BAD_REQUEST;
	  return -1;
	}
      digit = *p - '0';
      u64 new_body_len = body_len * 10 + digit;
      /* check for overflow */
      if (new_body_len < body_len)
	{
	  clib_warning ("too big number, overflow");
	  *ec = HTTP_STATUS_BAD_REQUEST;
	  return -1;
	}
      body_len = new_body_len;
      p++;
    }

  hc->body_len = body_len;

  hc->body_offset = hc->headers_offset + hc->headers_len + 2;
  HTTP_DBG (2, "body length: %llu", hc->body_len);
  HTTP_DBG (2, "body offset: %u", hc->body_offset);

  return 0;
}

static http_sm_result_t
http_state_wait_server_reply (http_conn_t *hc, transport_send_params_t *sp)
{
  int rv;
  http_msg_t msg = {};
  app_worker_t *app_wrk;
  session_t *as;
  u32 len, max_enq, body_sent;
  http_status_code_t ec;
  http_main_t *hm = &http_main;

  rv = http_read_message (hc);

  /* Nothing yet, wait for data or timer expire */
  if (rv)
    {
      HTTP_DBG (1, "no data to deq");
      return HTTP_SM_STOP;
    }

  HTTP_DBG (3, "%v", hc->rx_buf);

  if (vec_len (hc->rx_buf) < 8)
    {
      clib_warning ("response buffer too short");
      goto error;
    }

  rv = http_parse_status_line (hc);
  if (rv)
    goto error;

  rv = http_identify_headers (hc, &ec);
  if (rv)
    goto error;

  rv = http_identify_message_body (hc, &ec);
  if (rv)
    goto error;

  /* send at least "control data" which is necessary minimum,
   * if there is some space send also portion of body */
  as = session_get_from_handle (hc->h_pa_session_handle);
  max_enq = svm_fifo_max_enqueue (as->rx_fifo);
  max_enq -= sizeof (msg);
  if (max_enq < hc->control_data_len)
    {
      clib_warning ("not enough room for control data in app's rx fifo");
      goto error;
    }
  len = clib_min (max_enq, vec_len (hc->rx_buf));

  msg.type = HTTP_MSG_REPLY;
  msg.code = hm->sc_by_u16[hc->status_code];
  msg.data.headers_offset = hc->headers_offset;
  msg.data.headers_len = hc->headers_len;
  msg.data.body_offset = hc->body_offset;
  msg.data.body_len = hc->body_len;
  msg.data.type = HTTP_MSG_DATA_INLINE;
  msg.data.len = len;

  svm_fifo_seg_t segs[2] = { { (u8 *) &msg, sizeof (msg) },
			     { hc->rx_buf, len } };

  rv = svm_fifo_enqueue_segments (as->rx_fifo, segs, 2, 0 /* allow partial */);
  ASSERT (rv == (sizeof (msg) + len));

  http_read_message_drop (hc, len);

  body_sent = len - hc->control_data_len;
  hc->to_recv = hc->body_len - body_sent;
  if (hc->to_recv == 0)
    {
      /* all sent, we are done */
      http_state_change (hc, HTTP_STATE_WAIT_APP_METHOD);
    }
  else
    {
      /* stream rest of the response body */
      http_state_change (hc, HTTP_STATE_CLIENT_IO_MORE_DATA);
    }

  app_wrk = app_worker_get_if_valid (as->app_wrk_index);
  if (app_wrk)
    app_worker_rx_notify (app_wrk, as);
  return HTTP_SM_STOP;

error:
  http_read_message_drop_all (hc);
  session_transport_closing_notify (&hc->connection);
  session_transport_closed_notify (&hc->connection);
  http_disconnect_transport (hc);
  return HTTP_SM_ERROR;
}

static http_sm_result_t
http_state_wait_client_method (http_conn_t *hc, transport_send_params_t *sp)
{
  http_status_code_t ec;
  app_worker_t *app_wrk;
  http_msg_t msg;
  session_t *as;
  int rv;
  u32 len, max_enq, body_sent;
  u64 max_deq;

  rv = http_read_message (hc);

  /* Nothing yet, wait for data or timer expire */
  if (rv)
    return HTTP_SM_STOP;

  HTTP_DBG (3, "%v", hc->rx_buf);

  if (vec_len (hc->rx_buf) < 8)
    {
      ec = HTTP_STATUS_BAD_REQUEST;
      goto error;
    }

  rv = http_parse_request_line (hc, &ec);
  if (rv)
    goto error;

  rv = http_identify_headers (hc, &ec);
  if (rv)
    goto error;

  rv = http_identify_message_body (hc, &ec);
  if (rv)
    goto error;

  /* send at least "control data" which is necessary minimum,
   * if there is some space send also portion of body */
  as = session_get_from_handle (hc->h_pa_session_handle);
  max_enq = svm_fifo_max_enqueue (as->rx_fifo);
  if (max_enq < hc->control_data_len)
    {
      clib_warning ("not enough room for control data in app's rx fifo");
      ec = HTTP_STATUS_INTERNAL_ERROR;
      goto error;
    }
  /* do not dequeue more than one HTTP request, we do not support pipelining */
  max_deq =
    clib_min (hc->control_data_len + hc->body_len, vec_len (hc->rx_buf));
  len = clib_min (max_enq, max_deq);

  msg.type = HTTP_MSG_REQUEST;
  msg.method_type = hc->method;
  msg.data.type = HTTP_MSG_DATA_INLINE;
  msg.data.len = len;
  msg.data.target_form = hc->target_form;
  msg.data.target_path_offset = hc->target_path_offset;
  msg.data.target_path_len = hc->target_path_len;
  msg.data.target_query_offset = hc->target_query_offset;
  msg.data.target_query_len = hc->target_query_len;
  msg.data.headers_offset = hc->headers_offset;
  msg.data.headers_len = hc->headers_len;
  msg.data.body_offset = hc->body_offset;
  msg.data.body_len = hc->body_len;

  svm_fifo_seg_t segs[2] = { { (u8 *) &msg, sizeof (msg) },
			     { hc->rx_buf, len } };

  rv = svm_fifo_enqueue_segments (as->rx_fifo, segs, 2, 0 /* allow partial */);
  ASSERT (rv == (sizeof (msg) + len));

  body_sent = len - hc->control_data_len;
  hc->to_recv = hc->body_len - body_sent;
  if (hc->to_recv == 0)
    {
      /* drop everything, we do not support pipelining */
      http_read_message_drop_all (hc);
      /* all sent, we are done */
      http_state_change (hc, HTTP_STATE_WAIT_APP_REPLY);
    }
  else
    {
      http_read_message_drop (hc, len);
      /* stream rest of the response body */
      http_state_change (hc, HTTP_STATE_CLIENT_IO_MORE_DATA);
    }

  app_wrk = app_worker_get_if_valid (as->app_wrk_index);
  if (app_wrk)
    app_worker_rx_notify (app_wrk, as);

  return HTTP_SM_STOP;

error:
  http_read_message_drop_all (hc);
  http_send_error (hc, ec);
  session_transport_closing_notify (&hc->connection);
  http_disconnect_transport (hc);

  return HTTP_SM_ERROR;
}

static http_sm_result_t
http_state_wait_app_reply (http_conn_t *hc, transport_send_params_t *sp)
{
  http_main_t *hm = &http_main;
  u8 *response;
  u32 sent;
  f64 now;
  session_t *as;
  http_status_code_t sc;
  http_msg_t msg;
  int rv;
  http_sm_result_t sm_result = HTTP_SM_ERROR;

  as = session_get_from_handle (hc->h_pa_session_handle);

  rv = svm_fifo_dequeue (as->tx_fifo, sizeof (msg), (u8 *) &msg);
  ASSERT (rv == sizeof (msg));

  if (msg.data.type > HTTP_MSG_DATA_PTR)
    {
      clib_warning ("no data");
      sc = HTTP_STATUS_INTERNAL_ERROR;
      goto error;
    }

  if (msg.type != HTTP_MSG_REPLY)
    {
      clib_warning ("unexpected message type %d", msg.type);
      sc = HTTP_STATUS_INTERNAL_ERROR;
      goto error;
    }

  if (msg.code >= HTTP_N_STATUS)
    {
      clib_warning ("unsupported status code: %d", msg.code);
      return HTTP_SM_ERROR;
    }

  /*
   * Add "protocol layer" headers:
   * - current time
   * - server name
   * - data length
   */
  now = clib_timebase_now (&hm->timebase);
  response = format (0, http_response_template, http_status_code_str[msg.code],
		     /* Date */
		     format_clib_timebase_time, now,
		     /* Server */
		     hc->app_name,
		     /* Length */
		     msg.data.body_len,
		     /* Any headers from app? */
		     msg.data.headers_len ? "" : "\r\n");

  /* Add headers from app (if any) */
  if (msg.data.headers_len)
    {
      HTTP_DBG (0, "got headers from app, len %d", msg.data.headers_len);
      if (msg.data.type == HTTP_MSG_DATA_PTR)
	{
	  uword app_headers_ptr;
	  rv = svm_fifo_dequeue (as->tx_fifo, sizeof (app_headers_ptr),
				 (u8 *) &app_headers_ptr);
	  ASSERT (rv == sizeof (app_headers_ptr));
	  vec_append (response, uword_to_pointer (app_headers_ptr, u8 *));
	}
      else
	{
	  u32 orig_len = vec_len (response);
	  vec_resize (response, msg.data.headers_len);
	  u8 *p = response + orig_len;
	  rv = svm_fifo_dequeue (as->tx_fifo, msg.data.headers_len, p);
	  ASSERT (rv == msg.data.headers_len);
	}
    }
  HTTP_DBG (3, "%v", response);

  sent = http_send_data (hc, response, vec_len (response));
  if (sent != vec_len (response))
    {
      clib_warning ("sending status-line and headers failed!");
      sc = HTTP_STATUS_INTERNAL_ERROR;
      vec_free (response);
      goto error;
    }
  vec_free (response);

  if (msg.data.body_len)
    {
      /* Start sending the actual data */
      http_buffer_init (&hc->tx_buf, msg_to_buf_type[msg.data.type],
			as->tx_fifo, msg.data.body_len);
      http_state_change (hc, HTTP_STATE_APP_IO_MORE_DATA);
      sm_result = HTTP_SM_CONTINUE;
    }
  else
    {
      /* No response body, we are done */
      http_state_change (hc, HTTP_STATE_WAIT_CLIENT_METHOD);
      sm_result = HTTP_SM_STOP;
    }

  ASSERT (sp->max_burst_size >= sent);
  sp->max_burst_size -= sent;
  return sm_result;

error:
  http_send_error (hc, sc);
  http_state_change (hc, HTTP_STATE_WAIT_CLIENT_METHOD);
  session_transport_closing_notify (&hc->connection);
  http_disconnect_transport (hc);
  return HTTP_SM_STOP;
}

static http_sm_result_t
http_state_wait_app_method (http_conn_t *hc, transport_send_params_t *sp)
{
  http_msg_t msg;
  session_t *as;
  u8 *target_buff = 0, *request = 0, *target;
  u32 sent;
  int rv;
  http_sm_result_t sm_result = HTTP_SM_ERROR;
  http_state_t next_state;

  as = session_get_from_handle (hc->h_pa_session_handle);

  rv = svm_fifo_dequeue (as->tx_fifo, sizeof (msg), (u8 *) &msg);
  ASSERT (rv == sizeof (msg));

  if (msg.data.type > HTTP_MSG_DATA_PTR)
    {
      clib_warning ("no data");
      goto error;
    }

  if (msg.type != HTTP_MSG_REQUEST)
    {
      clib_warning ("unexpected message type %d", msg.type);
      goto error;
    }

  /* read request target */
  if (msg.data.type == HTTP_MSG_DATA_PTR)
    {
      uword target_ptr;
      rv = svm_fifo_dequeue (as->tx_fifo, sizeof (target_ptr),
			     (u8 *) &target_ptr);
      ASSERT (rv == sizeof (target_ptr));
      target = uword_to_pointer (target_ptr, u8 *);
    }
  else
    {
      vec_validate (target_buff, msg.data.target_path_len - 1);
      rv =
	svm_fifo_dequeue (as->tx_fifo, msg.data.target_path_len, target_buff);
      ASSERT (rv == msg.data.target_path_len);
      target = target_buff;
    }

  /* currently we support only GET and POST method */
  if (msg.method_type == HTTP_REQ_GET)
    {
      if (msg.data.body_len)
	{
	  clib_warning ("GET request shouldn't include data");
	  goto error;
	}
      /*
       * Add "protocol layer" headers:
       * - host
       * - user agent
       */
      request = format (0, http_get_request_template,
			/* target */
			target,
			/* Host */
			hc->host,
			/* User-Agent */
			hc->app_name,
			/* Any headers from app? */
			msg.data.headers_len ? "" : "\r\n");

      next_state = HTTP_STATE_WAIT_SERVER_REPLY;
      sm_result = HTTP_SM_STOP;
    }
  else if (msg.method_type == HTTP_REQ_POST)
    {
      if (!msg.data.body_len)
	{
	  clib_warning ("POST request should include data");
	  goto error;
	}
      /*
       * Add "protocol layer" headers:
       * - host
       * - user agent
       * - content length
       */
      request = format (0, http_post_request_template,
			/* target */
			target,
			/* Host */
			hc->host,
			/* User-Agent */
			hc->app_name,
			/* Content-Length */
			msg.data.body_len,
			/* Any headers from app? */
			msg.data.headers_len ? "" : "\r\n");

      http_buffer_init (&hc->tx_buf, msg_to_buf_type[msg.data.type],
			as->tx_fifo, msg.data.body_len);

      next_state = HTTP_STATE_APP_IO_MORE_DATA;
      sm_result = HTTP_SM_CONTINUE;
    }
  else
    {
      clib_warning ("unsupported method %d", msg.method_type);
      goto error;
    }

  /* Add headers from app (if any) */
  if (msg.data.headers_len)
    {
      HTTP_DBG (0, "got headers from app, len %d", msg.data.headers_len);
      if (msg.data.type == HTTP_MSG_DATA_PTR)
	{
	  uword app_headers_ptr;
	  rv = svm_fifo_dequeue (as->tx_fifo, sizeof (app_headers_ptr),
				 (u8 *) &app_headers_ptr);
	  ASSERT (rv == sizeof (app_headers_ptr));
	  vec_append (request, uword_to_pointer (app_headers_ptr, u8 *));
	}
      else
	{
	  u32 orig_len = vec_len (request);
	  vec_resize (request, msg.data.headers_len);
	  u8 *p = request + orig_len;
	  rv = svm_fifo_dequeue (as->tx_fifo, msg.data.headers_len, p);
	  ASSERT (rv == msg.data.headers_len);
	}
    }
  HTTP_DBG (3, "%v", request);

  sent = http_send_data (hc, request, vec_len (request));
  if (sent != vec_len (request))
    {
      clib_warning ("sending request-line and headers failed!");
      sm_result = HTTP_SM_ERROR;
      goto error;
    }

  http_state_change (hc, next_state);
  goto done;

error:
  svm_fifo_dequeue_drop_all (as->tx_fifo);
  session_transport_closing_notify (&hc->connection);
  session_transport_closed_notify (&hc->connection);
  http_disconnect_transport (hc);

done:
  vec_free (target_buff);
  vec_free (request);
  return sm_result;
}

static http_sm_result_t
http_state_client_io_more_data (http_conn_t *hc, transport_send_params_t *sp)
{
  session_t *as, *ts;
  app_worker_t *app_wrk;
  svm_fifo_seg_t _seg, *seg = &_seg;
  u32 max_len, max_deq, max_enq, n_segs = 1;
  int rv, len;

  as = session_get_from_handle (hc->h_pa_session_handle);
  ts = session_get_from_handle (hc->h_tc_session_handle);

  max_deq = svm_fifo_max_dequeue (ts->rx_fifo);
  if (max_deq == 0)
    {
      HTTP_DBG (1, "no data to deq");
      return HTTP_SM_STOP;
    }

  max_enq = svm_fifo_max_enqueue (as->rx_fifo);
  if (max_enq == 0)
    {
      HTTP_DBG (1, "app's rx fifo full");
      svm_fifo_add_want_deq_ntf (as->rx_fifo, SVM_FIFO_WANT_DEQ_NOTIF);
      return HTTP_SM_STOP;
    }

  max_len = clib_min (max_enq, max_deq);
  len = svm_fifo_segments (ts->rx_fifo, 0, seg, &n_segs, max_len);
  if (len < 0)
    {
      HTTP_DBG (1, "svm_fifo_segments() len %d", len);
      return HTTP_SM_STOP;
    }

  rv = svm_fifo_enqueue_segments (as->rx_fifo, seg, 1, 0 /* allow partial */);
  if (rv < 0)
    {
      clib_warning ("data enqueue failed, rv: %d", rv);
      return HTTP_SM_ERROR;
    }

  svm_fifo_dequeue_drop (ts->rx_fifo, rv);
  if (rv > hc->to_recv)
    {
      clib_warning ("http protocol error: received more data than expected");
      session_transport_closing_notify (&hc->connection);
      http_disconnect_transport (hc);
      http_state_change (hc, HTTP_STATE_WAIT_APP_METHOD);
      return HTTP_SM_ERROR;
    }
  hc->to_recv -= rv;
  HTTP_DBG (1, "drained %d from ts; remains %lu", rv, hc->to_recv);

  /* Finished transaction:
   * server back to HTTP_STATE_WAIT_APP_REPLY
   * client to HTTP_STATE_WAIT_APP_METHOD */
  if (hc->to_recv == 0)
    http_state_change (hc, hc->is_server ? HTTP_STATE_WAIT_APP_REPLY :
					   HTTP_STATE_WAIT_APP_METHOD);

  app_wrk = app_worker_get_if_valid (as->app_wrk_index);
  if (app_wrk)
    app_worker_rx_notify (app_wrk, as);

  if (svm_fifo_max_dequeue_cons (ts->rx_fifo))
    session_enqueue_notify (ts);

  return HTTP_SM_STOP;
}

static http_sm_result_t
http_state_app_io_more_data (http_conn_t *hc, transport_send_params_t *sp)
{
  u32 max_send = 64 << 10, n_segs;
  http_buffer_t *hb = &hc->tx_buf;
  svm_fifo_seg_t *seg;
  session_t *ts;
  int sent = 0;

  max_send = clib_min (max_send, sp->max_burst_size);
  ts = session_get_from_handle (hc->h_tc_session_handle);
  if ((seg = http_buffer_get_segs (hb, max_send, &n_segs)))
    sent = svm_fifo_enqueue_segments (ts->tx_fifo, seg, n_segs,
				      1 /* allow partial */);

  if (sent > 0)
    {
      /* Ask scheduler to notify app of deq event if needed */
      sp->bytes_dequeued += http_buffer_drain (hb, sent);
      sp->max_burst_size -= sent;
    }

  /* Not finished sending all data */
  if (!http_buffer_is_drained (hb))
    {
      if (sent && svm_fifo_set_event (ts->tx_fifo))
	session_program_tx_io_evt (ts->handle, SESSION_IO_EVT_TX);

      if (svm_fifo_max_enqueue (ts->tx_fifo) < HTTP_FIFO_THRESH)
	{
	  /* Deschedule http session and wait for deq notification if
	   * underlying ts tx fifo almost full */
	  svm_fifo_add_want_deq_ntf (ts->tx_fifo, SVM_FIFO_WANT_DEQ_NOTIF);
	  transport_connection_deschedule (&hc->connection);
	  sp->flags |= TRANSPORT_SND_F_DESCHED;
	}
    }
  else
    {
      if (sent && svm_fifo_set_event (ts->tx_fifo))
	session_program_tx_io_evt (ts->handle, SESSION_IO_EVT_TX_FLUSH);

      /* Finished transaction:
       * server back to HTTP_STATE_WAIT_METHOD
       * client to HTTP_STATE_WAIT_SERVER_REPLY */
      http_state_change (hc, hc->is_server ? HTTP_STATE_WAIT_CLIENT_METHOD :
						   HTTP_STATE_WAIT_SERVER_REPLY);
      http_buffer_free (&hc->tx_buf);
    }

  return HTTP_SM_STOP;
}

typedef http_sm_result_t (*http_sm_handler) (http_conn_t *,
					     transport_send_params_t *sp);

static http_sm_handler state_funcs[HTTP_N_STATES] = {
  0, /* idle state */
  http_state_wait_app_method,
  http_state_wait_client_method,
  http_state_wait_server_reply,
  http_state_wait_app_reply,
  http_state_client_io_more_data,
  http_state_app_io_more_data,
};

static void
http_req_run_state_machine (http_conn_t *hc, transport_send_params_t *sp)
{
  http_sm_result_t res;

  do
    {
      res = state_funcs[hc->http_state](hc, sp);
      if (res == HTTP_SM_ERROR)
	{
	  HTTP_DBG (1, "error in state machine %d", res);
	  return;
	}
    }
  while (res == HTTP_SM_CONTINUE);

  /* Reset the session expiration timer */
  http_conn_timer_update (hc);
}

static int
http_ts_rx_callback (session_t *ts)
{
  http_conn_t *hc;

  HTTP_DBG (1, "hc [%u]%x", ts->thread_index, ts->opaque);

  hc = http_conn_get_w_thread (ts->opaque, ts->thread_index);

  if (hc->state == HTTP_CONN_STATE_CLOSED)
    {
      HTTP_DBG (1, "conn closed");
      svm_fifo_dequeue_drop_all (ts->tx_fifo);
      return 0;
    }

  if (!http_state_is_rx_valid (hc))
    {
      if (hc->state != HTTP_CONN_STATE_CLOSED)
	clib_warning ("app data req state '%U' session state %u",
		      format_http_state, hc->http_state, hc->state);
      svm_fifo_dequeue_drop_all (ts->tx_fifo);
      return 0;
    }

  HTTP_DBG (1, "run state machine");
  http_req_run_state_machine (hc, 0);

  if (hc->state == HTTP_CONN_STATE_TRANSPORT_CLOSED)
    {
      if (!svm_fifo_max_dequeue_cons (ts->rx_fifo))
	session_transport_closing_notify (&hc->connection);
    }
  return 0;
}

int
http_ts_builtin_tx_callback (session_t *ts)
{
  http_conn_t *hc;

  hc = http_conn_get_w_thread (ts->opaque, ts->thread_index);
  transport_connection_reschedule (&hc->connection);

  return 0;
}

static void
http_ts_cleanup_callback (session_t *ts, session_cleanup_ntf_t ntf)
{
  http_conn_t *hc;

  if (ntf == SESSION_CLEANUP_TRANSPORT)
    return;

  hc = http_conn_get_w_thread (ts->opaque, ts->thread_index);

  HTTP_DBG (1, "going to free hc [%u]%x", ts->thread_index, ts->opaque);

  vec_free (hc->rx_buf);

  http_buffer_free (&hc->tx_buf);

  if (hc->pending_timer == 0)
    http_conn_timer_stop (hc);

  session_transport_delete_notify (&hc->connection);

  if (!hc->is_server)
    {
      vec_free (hc->app_name);
      vec_free (hc->host);
    }
  http_conn_free (hc);
}

static void
http_ts_ho_cleanup_callback (session_t *ts)
{
  http_conn_t *ho_hc;
  HTTP_DBG (1, "half open: %x", ts->opaque);
  ho_hc = http_ho_conn_get (ts->opaque);
  session_half_open_delete_notify (&ho_hc->connection);
  http_ho_conn_free (ho_hc);
}

int
http_add_segment_callback (u32 client_index, u64 segment_handle)
{
  /* No-op for builtin */
  return 0;
}

int
http_del_segment_callback (u32 client_index, u64 segment_handle)
{
  return 0;
}

static session_cb_vft_t http_app_cb_vft = {
  .session_accept_callback = http_ts_accept_callback,
  .session_disconnect_callback = http_ts_disconnect_callback,
  .session_connected_callback = http_ts_connected_callback,
  .session_reset_callback = http_ts_reset_callback,
  .session_cleanup_callback = http_ts_cleanup_callback,
  .half_open_cleanup_callback = http_ts_ho_cleanup_callback,
  .add_segment_callback = http_add_segment_callback,
  .del_segment_callback = http_del_segment_callback,
  .builtin_app_rx_callback = http_ts_rx_callback,
  .builtin_app_tx_callback = http_ts_builtin_tx_callback,
};

static clib_error_t *
http_transport_enable (vlib_main_t *vm, u8 is_en)
{
  vnet_app_detach_args_t _da, *da = &_da;
  vnet_app_attach_args_t _a, *a = &_a;
  u64 options[APP_OPTIONS_N_OPTIONS];
  http_main_t *hm = &http_main;

  if (!is_en)
    {
      da->app_index = hm->app_index;
      da->api_client_index = APP_INVALID_INDEX;
      vnet_application_detach (da);
      return 0;
    }

  clib_memset (a, 0, sizeof (*a));
  clib_memset (options, 0, sizeof (options));

  a->session_cb_vft = &http_app_cb_vft;
  a->api_client_index = APP_INVALID_INDEX;
  a->options = options;
  a->name = format (0, "http");
  a->options[APP_OPTIONS_SEGMENT_SIZE] = hm->first_seg_size;
  a->options[APP_OPTIONS_ADD_SEGMENT_SIZE] = hm->add_seg_size;
  a->options[APP_OPTIONS_RX_FIFO_SIZE] = hm->fifo_size;
  a->options[APP_OPTIONS_TX_FIFO_SIZE] = hm->fifo_size;
  a->options[APP_OPTIONS_FLAGS] = APP_OPTIONS_FLAGS_IS_BUILTIN;
  a->options[APP_OPTIONS_FLAGS] |= APP_OPTIONS_FLAGS_USE_GLOBAL_SCOPE;
  a->options[APP_OPTIONS_FLAGS] |= APP_OPTIONS_FLAGS_IS_TRANSPORT_APP;

  if (vnet_application_attach (a))
    return clib_error_return (0, "failed to attach http app");

  hm->app_index = a->app_index;
  vec_free (a->name);

  if (hm->is_init)
    return 0;

  vec_validate (hm->wrk, vlib_num_workers ());

  clib_timebase_init (&hm->timebase, 0 /* GMT */, CLIB_TIMEBASE_DAYLIGHT_NONE,
		      &vm->clib_time /* share the system clock */);

  http_timers_init (vm, http_conn_timeout_cb, http_conn_invalidate_timer_cb);
  hm->is_init = 1;

  return 0;
}

static int
http_transport_connect (transport_endpoint_cfg_t *tep)
{
  vnet_connect_args_t _cargs, *cargs = &_cargs;
  http_main_t *hm = &http_main;
  session_endpoint_cfg_t *sep = (session_endpoint_cfg_t *) tep;
  application_t *app;
  http_conn_t *hc;
  int error;
  u32 hc_index;
  session_t *ho;
  transport_endpt_ext_cfg_t *ext_cfg;
  app_worker_t *app_wrk = app_worker_get (sep->app_wrk_index);

  clib_memset (cargs, 0, sizeof (*cargs));
  clib_memcpy (&cargs->sep_ext, sep, sizeof (session_endpoint_cfg_t));
  cargs->sep.transport_proto = TRANSPORT_PROTO_TCP;
  cargs->app_index = hm->app_index;
  app = application_get (app_wrk->app_index);
  cargs->sep_ext.ns_index = app->ns_index;

  hc_index = http_ho_conn_alloc ();
  hc = http_ho_conn_get (hc_index);
  hc->h_pa_wrk_index = sep->app_wrk_index;
  hc->h_pa_app_api_ctx = sep->opaque;
  hc->state = HTTP_CONN_STATE_CONNECTING;
  cargs->api_context = hc_index;

  ext_cfg = session_endpoint_get_ext_cfg (sep, TRANSPORT_ENDPT_EXT_CFG_HTTP);
  if (ext_cfg)
    {
      HTTP_DBG (1, "app set timeout %u", ext_cfg->opaque);
      hc->timeout = ext_cfg->opaque;
    }

  hc->is_server = 0;

  if (vec_len (app->name))
    hc->app_name = vec_dup (app->name);
  else
    hc->app_name = format (0, "VPP HTTP client");

  if (sep->is_ip4)
    hc->host = format (0, "%U:%d", format_ip4_address, &sep->ip.ip4,
		       clib_net_to_host_u16 (sep->port));
  else
    hc->host = format (0, "%U:%d", format_ip6_address, &sep->ip.ip6,
		       clib_net_to_host_u16 (sep->port));

  HTTP_DBG (1, "hc ho_index %x", hc_index);

  if ((error = vnet_connect (cargs)))
    return error;

  ho = session_alloc_for_half_open (&hc->connection);
  ho->app_wrk_index = app_wrk->wrk_index;
  ho->ho_index = app_worker_add_half_open (app_wrk, session_handle (ho));
  ho->opaque = sep->opaque;
  ho->session_type =
    session_type_from_proto_and_ip (TRANSPORT_PROTO_HTTP, sep->is_ip4);
  hc->h_tc_session_handle = cargs->sh;
  hc->c_s_index = ho->session_index;

  return 0;
}

static u32
http_start_listen (u32 app_listener_index, transport_endpoint_cfg_t *tep)
{
  vnet_listen_args_t _args = {}, *args = &_args;
  session_t *ts_listener, *app_listener;
  http_main_t *hm = &http_main;
  session_endpoint_cfg_t *sep;
  app_worker_t *app_wrk;
  transport_proto_t tp = TRANSPORT_PROTO_TCP;
  app_listener_t *al;
  application_t *app;
  http_conn_t *lhc;
  u32 lhc_index;
  transport_endpt_ext_cfg_t *ext_cfg;

  sep = (session_endpoint_cfg_t *) tep;

  app_wrk = app_worker_get (sep->app_wrk_index);
  app = application_get (app_wrk->app_index);

  args->app_index = hm->app_index;
  args->sep_ext = *sep;
  args->sep_ext.ns_index = app->ns_index;

  ext_cfg = session_endpoint_get_ext_cfg (sep, TRANSPORT_ENDPT_EXT_CFG_CRYPTO);
  if (ext_cfg)
    {
      HTTP_DBG (1, "app set tls");
      tp = TRANSPORT_PROTO_TLS;
    }
  args->sep_ext.transport_proto = tp;

  if (vnet_listen (args))
    return SESSION_INVALID_INDEX;

  lhc_index = http_listener_alloc ();
  lhc = http_listener_get (lhc_index);

  ext_cfg = session_endpoint_get_ext_cfg (sep, TRANSPORT_ENDPT_EXT_CFG_HTTP);
  if (ext_cfg)
    {
      HTTP_DBG (1, "app set timeout %u", ext_cfg->opaque);
      lhc->timeout = ext_cfg->opaque;
    }

  /* Grab transport connection listener and link to http listener */
  lhc->h_tc_session_handle = args->handle;
  al = app_listener_get_w_handle (lhc->h_tc_session_handle);
  ts_listener = app_listener_get_session (al);
  ts_listener->opaque = lhc_index;

  /* Grab application listener and link to http listener */
  app_listener = listen_session_get (app_listener_index);
  lhc->h_pa_wrk_index = sep->app_wrk_index;
  lhc->h_pa_session_handle = listen_session_get_handle (app_listener);
  lhc->c_s_index = app_listener_index;
  lhc->c_flags |= TRANSPORT_CONNECTION_F_NO_LOOKUP;

  lhc->is_server = 1;

  if (vec_len (app->name))
    lhc->app_name = vec_dup (app->name);
  else
    lhc->app_name = format (0, "VPP server app");

  return lhc_index;
}

static u32
http_stop_listen (u32 listener_index)
{
  http_conn_t *lhc;
  int rv;

  lhc = http_listener_get (listener_index);

  vnet_unlisten_args_t a = {
    .handle = lhc->h_tc_session_handle,
    .app_index = http_main.app_index,
    .wrk_map_index = 0 /* default wrk */
  };

  if ((rv = vnet_unlisten (&a)))
    clib_warning ("unlisten returned %d", rv);

  http_listener_free (lhc);

  return 0;
}

static void
http_transport_close (u32 hc_index, u32 thread_index)
{
  session_t *as;
  http_conn_t *hc;

  HTTP_DBG (1, "App disconnecting [%u]%x", thread_index, hc_index);

  hc = http_conn_get_w_thread (hc_index, thread_index);
  if (hc->state == HTTP_CONN_STATE_CONNECTING)
    {
      hc->state = HTTP_CONN_STATE_APP_CLOSED;
      http_disconnect_transport (hc);
      return;
    }
  else if (hc->state == HTTP_CONN_STATE_CLOSED)
    {
      HTTP_DBG (1, "nothing to do, already closed");
      return;
    }
  as = session_get_from_handle (hc->h_pa_session_handle);

  /* Nothing more to send, confirm close */
  if (!svm_fifo_max_dequeue_cons (as->tx_fifo))
    {
      session_transport_closed_notify (&hc->connection);
      http_disconnect_transport (hc);
    }
  else
    {
      /* Wait for all data to be written to ts */
      hc->state = HTTP_CONN_STATE_APP_CLOSED;
    }
}

static transport_connection_t *
http_transport_get_connection (u32 hc_index, u32 thread_index)
{
  http_conn_t *hc = http_conn_get_w_thread (hc_index, thread_index);
  return &hc->connection;
}

static transport_connection_t *
http_transport_get_listener (u32 listener_index)
{
  http_conn_t *lhc = http_listener_get (listener_index);
  return &lhc->connection;
}

static int
http_app_tx_callback (void *session, transport_send_params_t *sp)
{
  session_t *as = (session_t *) session;
  u32 max_burst_sz, sent;
  http_conn_t *hc;

  HTTP_DBG (1, "hc [%u]%x", as->thread_index, as->connection_index);

  hc = http_conn_get_w_thread (as->connection_index, as->thread_index);
  if (!http_state_is_tx_valid (hc))
    {
      if (hc->state != HTTP_CONN_STATE_CLOSED)
	{
	  clib_warning ("hc [%u]%x invalid tx state http state "
			"'%U', session state %u",
			as->thread_index, as->connection_index,
			format_http_state, hc->http_state, hc->state);
	}
      svm_fifo_dequeue_drop_all (as->tx_fifo);
      return 0;
    }

  max_burst_sz = sp->max_burst_size * TRANSPORT_PACER_MIN_MSS;
  sp->max_burst_size = max_burst_sz;

  HTTP_DBG (1, "run state machine");
  http_req_run_state_machine (hc, sp);

  if (hc->state == HTTP_CONN_STATE_APP_CLOSED)
    {
      if (!svm_fifo_max_dequeue_cons (as->tx_fifo))
	http_disconnect_transport (hc);
    }

  sent = max_burst_sz - sp->max_burst_size;

  return sent > 0 ? clib_max (sent / TRANSPORT_PACER_MIN_MSS, 1) : 0;
}

static void
http_transport_get_endpoint (u32 hc_index, u32 thread_index,
			     transport_endpoint_t *tep, u8 is_lcl)
{
  http_conn_t *hc = http_conn_get_w_thread (hc_index, thread_index);
  session_t *ts;

  ts = session_get_from_handle (hc->h_tc_session_handle);
  session_get_endpoint (ts, tep, is_lcl);
}

static u8 *
format_http_connection (u8 *s, va_list *args)
{
  http_conn_t *hc = va_arg (*args, http_conn_t *);
  session_t *ts;

  ts = session_get_from_handle (hc->h_tc_session_handle);
  s = format (s, "[%d:%d][H] app_wrk %u ts %d:%d", hc->c_thread_index,
	      hc->c_s_index, hc->h_pa_wrk_index, ts->thread_index,
	      ts->session_index);

  return s;
}

static u8 *
format_http_listener (u8 *s, va_list *args)
{
  http_conn_t *lhc = va_arg (*args, http_conn_t *);
  app_listener_t *al;
  session_t *lts;

  al = app_listener_get_w_handle (lhc->h_tc_session_handle);
  lts = app_listener_get_session (al);
  s = format (s, "[%d:%d][H] app_wrk %u ts %d:%d", lhc->c_thread_index,
	      lhc->c_s_index, lhc->h_pa_wrk_index, lts->thread_index,
	      lts->session_index);

  return s;
}

static u8 *
format_http_conn_state (u8 *s, va_list *args)
{
  http_conn_t *hc = va_arg (*args, http_conn_t *);

  switch (hc->state)
    {
    case HTTP_CONN_STATE_LISTEN:
      s = format (s, "LISTEN");
      break;
    case HTTP_CONN_STATE_CONNECTING:
      s = format (s, "CONNECTING");
      break;
    case HTTP_CONN_STATE_ESTABLISHED:
      s = format (s, "ESTABLISHED");
      break;
    case HTTP_CONN_STATE_TRANSPORT_CLOSED:
      s = format (s, "TRANSPORT_CLOSED");
      break;
    case HTTP_CONN_STATE_APP_CLOSED:
      s = format (s, "APP_CLOSED");
      break;
    case HTTP_CONN_STATE_CLOSED:
      s = format (s, "CLOSED");
      break;
    }

  return s;
}

static u8 *
format_http_transport_connection (u8 *s, va_list *args)
{
  u32 tc_index = va_arg (*args, u32);
  u32 thread_index = va_arg (*args, u32);
  u32 verbose = va_arg (*args, u32);
  http_conn_t *hc;

  hc = http_conn_get_w_thread (tc_index, thread_index);

  s = format (s, "%-" SESSION_CLI_ID_LEN "U", format_http_connection, hc);
  if (verbose)
    {
      s =
	format (s, "%-" SESSION_CLI_STATE_LEN "U", format_http_conn_state, hc);
      if (verbose > 1)
	s = format (s, "\n");
    }

  return s;
}

static u8 *
format_http_transport_listener (u8 *s, va_list *args)
{
  u32 tc_index = va_arg (*args, u32);
  u32 __clib_unused thread_index = va_arg (*args, u32);
  u32 __clib_unused verbose = va_arg (*args, u32);
  http_conn_t *lhc = http_listener_get (tc_index);

  s = format (s, "%-" SESSION_CLI_ID_LEN "U", format_http_listener, lhc);
  if (verbose)
    s =
      format (s, "%-" SESSION_CLI_STATE_LEN "U", format_http_conn_state, lhc);
  return s;
}

static u8 *
format_http_transport_half_open (u8 *s, va_list *args)
{
  u32 ho_index = va_arg (*args, u32);
  u32 __clib_unused thread_index = va_arg (*args, u32);
  u32 __clib_unused verbose = va_arg (*args, u32);
  http_conn_t *ho_hc;
  session_t *tcp_ho;

  ho_hc = http_ho_conn_get (ho_index);
  tcp_ho = session_get_from_handle (ho_hc->h_tc_session_handle);

  s = format (s, "[%d:%d][H] half-open app_wrk %u ts %d:%d",
	      ho_hc->c_thread_index, ho_hc->c_s_index, ho_hc->h_pa_wrk_index,
	      tcp_ho->thread_index, tcp_ho->session_index);
  return s;
}

static transport_connection_t *
http_transport_get_ho (u32 ho_hc_index)
{
  http_conn_t *ho_hc;

  HTTP_DBG (1, "half open: %x", ho_hc_index);
  ho_hc = http_ho_conn_get (ho_hc_index);
  return &ho_hc->connection;
}

static void
http_transport_cleanup_ho (u32 ho_hc_index)
{
  http_conn_t *ho_hc;

  HTTP_DBG (1, "half open: %x", ho_hc_index);
  ho_hc = http_ho_conn_get (ho_hc_index);
  session_cleanup_half_open (ho_hc->h_tc_session_handle);
  http_ho_conn_free (ho_hc);
}

static const transport_proto_vft_t http_proto = {
  .enable = http_transport_enable,
  .connect = http_transport_connect,
  .start_listen = http_start_listen,
  .stop_listen = http_stop_listen,
  .close = http_transport_close,
  .cleanup_ho = http_transport_cleanup_ho,
  .custom_tx = http_app_tx_callback,
  .get_connection = http_transport_get_connection,
  .get_listener = http_transport_get_listener,
  .get_half_open = http_transport_get_ho,
  .get_transport_endpoint = http_transport_get_endpoint,
  .format_connection = format_http_transport_connection,
  .format_listener = format_http_transport_listener,
  .format_half_open = format_http_transport_half_open,
  .transport_options = {
    .name = "http",
    .short_name = "H",
    .tx_type = TRANSPORT_TX_INTERNAL,
    .service_type = TRANSPORT_SERVICE_APP,
  },
};

static clib_error_t *
http_transport_init (vlib_main_t *vm)
{
  http_main_t *hm = &http_main;
  int i;

  transport_register_protocol (TRANSPORT_PROTO_HTTP, &http_proto,
			       FIB_PROTOCOL_IP4, ~0);
  transport_register_protocol (TRANSPORT_PROTO_HTTP, &http_proto,
			       FIB_PROTOCOL_IP6, ~0);

  /* Default values, configurable via startup conf */
  hm->add_seg_size = 256 << 20;
  hm->first_seg_size = 32 << 20;
  hm->fifo_size = 512 << 10;

  /* Setup u16 to http_status_code_t map */
  /* Unrecognized status code is equivalent to the x00 status */
  vec_validate (hm->sc_by_u16, 599);
  for (i = 100; i < 200; i++)
    hm->sc_by_u16[i] = HTTP_STATUS_CONTINUE;
  for (i = 200; i < 300; i++)
    hm->sc_by_u16[i] = HTTP_STATUS_OK;
  for (i = 300; i < 400; i++)
    hm->sc_by_u16[i] = HTTP_STATUS_MULTIPLE_CHOICES;
  for (i = 400; i < 500; i++)
    hm->sc_by_u16[i] = HTTP_STATUS_BAD_REQUEST;
  for (i = 500; i < 600; i++)
    hm->sc_by_u16[i] = HTTP_STATUS_INTERNAL_ERROR;

    /* Registered status codes */
#define _(c, s, str) hm->sc_by_u16[c] = HTTP_STATUS_##s;
  foreach_http_status_code
#undef _

    return 0;
}

VLIB_INIT_FUNCTION (http_transport_init);

static clib_error_t *
http_config_fn (vlib_main_t *vm, unformat_input_t *input)
{
  http_main_t *hm = &http_main;
  uword mem_sz;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "first-segment-size %U", unformat_memory_size,
		    &mem_sz))
	{
	  hm->first_seg_size = clib_max (mem_sz, 1 << 20);
	  if (hm->first_seg_size != mem_sz)
	    clib_warning ("first seg size too small %u", mem_sz);
	}
      else if (unformat (input, "add-segment-size %U", unformat_memory_size,
			 &mem_sz))
	{
	  hm->add_seg_size = clib_max (mem_sz, 1 << 20);
	  if (hm->add_seg_size != mem_sz)
	    clib_warning ("add seg size too small %u", mem_sz);
	}
      else if (unformat (input, "fifo-size %U", unformat_memory_size, &mem_sz))
	{
	  hm->fifo_size = clib_clamp (mem_sz, 4 << 10, 2 << 30);
	  if (hm->fifo_size != mem_sz)
	    clib_warning ("invalid fifo size %lu", mem_sz);
	}
      else
	return clib_error_return (0, "unknown input `%U'",
				  format_unformat_error, input);
    }
  return 0;
}

VLIB_CONFIG_FUNCTION (http_config_fn, "http");

VLIB_PLUGIN_REGISTER () = {
  .version = VPP_BUILD_VER,
  .description = "Hypertext Transfer Protocol (HTTP)",
  .default_disabled = 0,
};

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
