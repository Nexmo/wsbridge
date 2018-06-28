/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * 
 * Anthony Minessale II <anthm@freeswitch.org>
 * Dorian Peake
 * Tiago Lam
 * Neil Stratford (neil.stratford@vonage.com)
 * Dragos Oancea  (dragos.oancea@vonage.com)
 *
 *
 * mod_wsbridge.c -- WSBRIDGE Endpoint Module for Websockets
 *
 */

#include <switch.h>
#include <switch_json.h>
#include <libwebsockets.h>

/*
 * Design Notes
 * ------------
 *
 * Both input and output need to be buffered so that we are async from the websocket.
 * Output: Queue of 20ms FS FRAMES
 *  FS callback: Take the packet from FS core and add it to an output queue.
 *  WS callback: Wrap in RTP and send all the frames that are currently buffered. If empty, do nothing. If full -> skip frames.
 *
 * Input: Queue of RTP framed data (bytes)
 *  WS callback: Append the data to the byte buffer, wrapping when needed.
 *  FS callback: Build a 20ms audio frame from the input buffer. If empty send comfort noise. If full skip frames.
 */

#define LWS_DEBUG // -DCMAKE_BUILD_TYPE=DEBUG when we build the libwebsockets library 

// #define LWS_RX_FLOW_CONTROL /*enable flow WS control*/  - default 

#define WSBRIDGE_FRAME_SIZE_16000 640 /* which means each 20ms frame as 640 bytes at 16 khz (1 channel only) */
#define WSBRIDGE_FRAME_SIZE_8000  320 /*which means each 20ms frame as 320 bytes at 8 khz (1 channel only)*/

#define WSBRIDGE_FRAME_SIZE WSBRIDGE_FRAME_SIZE_16000

#define PTIME_RTP_MS  20 /*ms*/
#define WS_TIMEOUT_MS 20  /* same as ptime on the RTP side */
#define FRAMES_NR 5 /*buffer this many frames on the RTP side*/

#define WSBRIDGE_OUTPUT_BUFFER_SIZE (WSBRIDGE_FRAME_SIZE*FRAMES_NR) /* 20 * 5 = 100 ms */
#define WSBRIDGE_INPUT_BUFFER_SIZE (WSBRIDGE_FRAME_SIZE*1) /* flow control without circular buffer*/

#define RX_BUFFER_SIZE WSBRIDGE_INPUT_BUFFER_SIZE

#define WSBRIDGE_INTERFACE_NAME "wsbridge" // dialplan: <action application="bridge" data="wsbridge"/>
#define WSBRIDGE_SIP_HEADER_TOKEN WSBRIDGE_INTERFACE_NAME 

#define L16_16000   "audio/l16;rate=16000"
#define L16_8000    "audio/l16;rate=8000"

#define HEADER_WS_URI          "P-"WSBRIDGE_SIP_HEADER_TOKEN"-websocket-uri"
#define HEADER_WS_HEADERS      "P-"WSBRIDGE_SIP_HEADER_TOKEN"-websocket-headers"
#define HEADER_WS_CONT_TYPE    "P-"WSBRIDGE_SIP_HEADER_TOKEN"-websocket-content-type"
#define WS_URI_MAX_SIZE         2048
#define WS_HEADERS_MAX_SIZE     512
#define WS_CONT_TYPE_MAX_SIZE   50

#define WSBRIDGE_STATE_STARTED 0
#define WSBRIDGE_STATE_DESTROY 1

SWITCH_MODULE_LOAD_FUNCTION(mod_wsbridge_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_wsbridge_shutdown);
SWITCH_MODULE_DEFINITION(mod_wsbridge, mod_wsbridge_load, mod_wsbridge_shutdown, NULL);

switch_endpoint_interface_t *wsbridge_endpoint_interface;
static switch_memory_pool_t *module_pool = NULL;
static int running = 1;

typedef enum {
	GFLAG_MY_CODEC_PREFS = (1 << 0)
} GFLAGS;

typedef enum {
	TFLAG_IO = (1 << 0),
	TFLAG_INBOUND = (1 << 1),
	TFLAG_OUTBOUND = (1 << 2),
	TFLAG_DTMF = (1 << 3),
	TFLAG_VOICE = (1 << 4),
	TFLAG_HANGUP = (1 << 5),
	TFLAG_LINEAR = (1 << 6),
	TFLAG_CODEC = (1 << 7),
	TFLAG_BREAK = (1 << 8)
} TFLAGS;

static struct {
	int debug;
	char *ip;
	int port;
	char *dialplan;
	char *codec_string;
	char *codec_order[SWITCH_MAX_CODECS];
	int codec_order_last;
	char *codec_rates_string;
	char *codec_rates[SWITCH_MAX_CODECS];
	int codec_rates_last;
	unsigned int flags;
	int calls;
	switch_mutex_t *mutex;
} globals;

struct private_object {
	unsigned int flags;
	switch_codec_t read_codec;
	switch_codec_t write_codec;
	switch_frame_t read_frame;
	switch_core_session_t *session;
	switch_caller_profile_t *caller_profile;
	switch_mutex_t *read_mutex;
	switch_mutex_t *write_mutex;
	switch_mutex_t *flag_mutex;
	switch_timer_t timer;
	cJSON* message;
	char content_type[WS_CONT_TYPE_MAX_SIZE];
	struct lws *wsi_wsbridge;
	struct lws_context_creation_info info;
	struct lws_client_connect_info i;
	struct lws_context *context;
	char path[2048];
	int state;
	/* This is the circular buffer of data from the websocket */
	char *read_data; // [WSBRIDGE_INPUT_BUFFER_SIZE];
	unsigned int read_start;
	unsigned int read_end;
	unsigned int read_count;
	/* This is the frame that we send on the RTP side*/
	unsigned char *databuf; // [WSBRIDGE_FRAME_SIZE]; 
	char *write_data; // [WSBRIDGE_OUTPUT_BUFFER_SIZE];
	unsigned int write_start;
	unsigned int write_end;
	unsigned int write_count;
	int started;
	switch_bool_t ws_backpressure;
	int ws_counter_read; /*stats*/
	int rtp_counter_write; /*stats*/
	int ws_counter_write; /*stats*/
	int rtp_counter_read; /*stats*/
	size_t frame_sz;  /*in samples*/
	size_t output_buffer_sz;  /*in bytes*/
	switch_bool_t have_compressed_audio; /*not used yet*/
};

typedef struct private_object private_t;

enum WSBRIDGE_protocols {
	PROTOCOL_WSBRIDGE,
	/* always last */
	PROTOCOL_COUNT
};

static int wsbridge_callback_ws(struct lws *wsi, enum lws_callback_reasons reason,
								void *user, void *in, size_t len);

static struct lws_protocols WSBRIDGE_protocols[] = {
	{
		"WSBRIDGE",
		wsbridge_callback_ws,
		0,
	/* rx_buffer_size Docs:
	 *
	 * If you want atomic frames delivered to the callback, you should set this to the size of the biggest legal frame that you support. 
	 * If the frame size is exceeded, there is no error, but the buffer will spill to the user callback when full, which you can detect by using lws_remaining_packet_payload. 
	 *
	 * * */
		RX_BUFFER_SIZE,
	},
	{ NULL, NULL, 0, 0 } /* end */
};

static const struct lws_extension exts[] = {
	{ NULL, NULL, NULL /* terminator */ }
};

SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_dialplan, globals.dialplan);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_codec_string, globals.codec_string);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_codec_rates_string, globals.codec_rates_string);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_ip, globals.ip);

static switch_status_t channel_on_init(switch_core_session_t *session);
static switch_status_t channel_on_hangup(switch_core_session_t *session);
static switch_status_t channel_on_destroy(switch_core_session_t *session);
static switch_status_t channel_on_routing(switch_core_session_t *session);
static switch_status_t channel_on_exchange_media(switch_core_session_t *session);
static switch_status_t channel_on_consume_media(switch_core_session_t *session);
static switch_status_t channel_on_soft_execute(switch_core_session_t *session);
static switch_call_cause_t channel_outgoing_channel(switch_core_session_t *session, switch_event_t *var_event,
													switch_caller_profile_t *outbound_profile,
													switch_core_session_t **new_session, switch_memory_pool_t **pool, switch_originate_flag_t flags,
													switch_call_cause_t *cancel_cause);
static switch_status_t channel_read_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id);
static switch_status_t channel_write_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id);
static switch_status_t channel_kill_channel(switch_core_session_t *session, int sig);

static void wsbridge_strncpy_null_term(char *, char *, int);
static void wsbridge_str_remove_quotes(char *);
static void wsbridge_str_remove_empty_spaces(char *);
static int wsbridge_codecs_init(private_t*, switch_core_session_t*);

#ifdef LWS_DEBUG
void ws_debug(int level, const char *line) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%d %s\n", level, line);
}
#endif 

static void *SWITCH_THREAD_FUNC wsbridge_thread_run(switch_thread_t *thread, void *obj)
{
	private_t *tech_pvt = (private_t *) obj;
	struct lws_context *context = tech_pvt->context;
	int n = 0;

	if (!context) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "wsbridge_thread_run(): No context\n");
		return NULL;
	}
	while (tech_pvt->started == 0) {
		n = lws_service(context, WS_TIMEOUT_MS);
		if (n < 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "wsbridge_thread_run(): negative ret from lws_service()\n");
			return NULL;
		}
	}
	/*
	* Destroy the context sequentially, in order to avoid multithreading
	* issues (segfaults) coming from OpenSSL
	*/
	switch_mutex_lock(globals.mutex);
	lws_context_destroy(tech_pvt->context);
	switch_mutex_unlock(globals.mutex);
	return NULL;
}

static void wsbridge_thread_launch(private_t *tech_pvt)
{
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;

	switch_threadattr_create(&thd_attr, module_pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	tech_pvt->started = 0;
	switch_thread_create(&thread, thd_attr, wsbridge_thread_run, tech_pvt, module_pool);
}

static int
websocket_write_back(struct lws *wsi_in, enum lws_write_protocol type, char *str, size_t str_size_in)
{
	int n = 0;
	int len;
	unsigned char *out = NULL;

	if (str == NULL || wsi_in == NULL) {
		return -1;
	}
	if (str_size_in < 1) {
		len = strlen(str);
	} else {
		len = str_size_in;
	}
	switch_zmalloc(out, sizeof(char) * LWS_SEND_BUFFER_PRE_PADDING + len + LWS_SEND_BUFFER_POST_PADDING);
	if (!out) {
		return 0;
	}
	/* setup the buffer*/
	memcpy(out + LWS_SEND_BUFFER_PRE_PADDING, str, len);
	/* write out*/
	n = lws_write(wsi_in, out + LWS_SEND_BUFFER_PRE_PADDING, len, type);
	/* free the buffer*/
	switch_safe_free(out);

	/* Return actually number of bytes written */
	return n;
}

static int
wsbridge_callback_ws(struct lws *wsi, enum lws_callback_reasons reason,
					 void *user, void *in, size_t len)
{
	int bytes_sent = 0;
	static char *message;
	char *bugfree_message;
	switch_core_session_t *session;
	switch_channel_t *channel;
	private_t *tech_pvt;
	size_t n;
	int size;

	globals.debug = 1;

	switch (reason) {
	case LWS_CALLBACK_CLIENT_ESTABLISHED:
		if (globals.debug) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "WebSockets client established\n");
		}

		session = (switch_core_session_t*) lws_wsi_user(wsi);
		assert(session != NULL);

		tech_pvt = switch_core_session_get_private(session);
		message = cJSON_PrintUnformatted(tech_pvt->message);
		/* XXX EASY FIX FOR A STUPID BUG, look into this properly:
			When the JSON structure is sent with no spaces, the audio we
			get is garbage. So, we append a space as the first character.
			And not, cJSON_Print (which prints pretty JSON), does not work
			either */
		/* 2 extra bytes, 1 for the terminator '\0' and another for the empty space */
		size = strlen(message);
		bugfree_message = (char*) calloc(size + 2, sizeof(char));
		bugfree_message[0] = ' ';
		strncpy(bugfree_message + 1, message, size);

		switch_log_printf(
			SWITCH_CHANNEL_LOG,
			SWITCH_LOG_INFO,
			"WebSockets sending TEXT message: %s\n",
			bugfree_message);
		websocket_write_back(wsi, LWS_WRITE_TEXT, bugfree_message, strlen(bugfree_message));

		channel = switch_core_session_get_channel(session);
		assert(channel != NULL);

		switch_channel_mark_answered(channel);

		free(bugfree_message);

		/* Start the poll... */
		lws_callback_on_writable(wsi);

		break;

	case LWS_CALLBACK_CLOSED:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "WebSockets callback closed\n");
		session = (switch_core_session_t *)lws_wsi_user(wsi);
		tech_pvt = switch_core_session_get_private(session);
		tech_pvt->started = 1;
		tech_pvt->state = WSBRIDGE_STATE_DESTROY;
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		break;

	case LWS_CALLBACK_CLIENT_RECEIVE:
		/* Copy the data into the read_data circular buffer and update the
		start and end pointers */
		if (globals.debug) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "WebSockets client received frame of size %zu\n", len);
		}
		session = (switch_core_session_t *)lws_wsi_user(wsi);
		assert(session != NULL);
		tech_pvt = switch_core_session_get_private(session);

		if (len < (tech_pvt->frame_sz * sizeof(int16_t))) {
			switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_WARNING,
				"WebSockets received frame len: [%u] < %d Bytes\n",
				(unsigned int) len, (int)(tech_pvt->frame_sz * sizeof(int16_t)));
		}

		if (tech_pvt->state == WSBRIDGE_STATE_DESTROY) {
			return 0;
		}

		switch_mutex_lock(tech_pvt->read_mutex);
		if (len > (tech_pvt->frame_sz * sizeof(int16_t))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, 
					"WebSockets RX: truncating payload to  - %d bytes (frame size) original payload len was: %u\n", (int)(tech_pvt->frame_sz * sizeof(int16_t)), (unsigned int) len);
			len = tech_pvt->frame_sz * sizeof(int16_t);
		}
		if (lws_frame_is_binary(tech_pvt->wsi_wsbridge)) {
			memcpy(tech_pvt->read_data, in, len);
			tech_pvt->read_count = len;
		} else {
			if (globals.debug) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "WebSockets RX: Frame not received in binary mode: %s \n", (char *)in);
			}
			tech_pvt->read_count = 0;
		}
			
		n = lws_remaining_packet_payload(tech_pvt->wsi_wsbridge);

		/* Check if we are spilling anything */
		if (n > 0) {
			switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_WARNING,
				"WebSockets `remaining_packet_payload` is above zero: %zu\n",
				n);
		}
		if (!(tech_pvt->ws_backpressure)) {
			if (globals.debug) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "RX FLOW CONTROL: [throttling]\n");
			}
			lws_rx_flow_control(tech_pvt->wsi_wsbridge, 0);
			tech_pvt->ws_backpressure = 1;
		}

		tech_pvt->ws_counter_read++;
		switch_mutex_unlock(tech_pvt->read_mutex);
		/* We dont care about the end of the frame, its all about the data */
		break;

	/* because we are protocols[0] ... */

	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		/* If `in` is not null, then there's info about the error */
		if (in != NULL) {
			switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_ERROR,
				"WebSockets connection error: %s\n",
				(char*) in);
		} else {
			switch_log_printf(
				SWITCH_CHANNEL_LOG,
				SWITCH_LOG_ERROR,
				"WebSockets connection error.\n");
		}

		session = (switch_core_session_t *)lws_wsi_user(wsi);
		tech_pvt = switch_core_session_get_private(session);
		tech_pvt->started = 1;
		tech_pvt->state = WSBRIDGE_STATE_DESTROY;
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
		break;

	case LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED:
		/* XXX No extensions defined yet */
		break;

	case LWS_CALLBACK_RECEIVE_CLIENT_HTTP:
		{
			char buffer[1024 + LWS_PRE];
			char *px = buffer + LWS_PRE;
			int lenx = sizeof(buffer) - LWS_PRE;
			if (globals.debug) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "WebSockets client HTTP\n");
			}

			/*
			 * Often you need to flow control this by something
			 * else being writable.  In that case call the api
			 * to get a callback when writable here, and do the
			 * pending client read in the writeable callback of
			 * the output.
			 */
			if (lws_http_client_read(wsi, &px, &lenx) < 0) {
				return -1;
			}
			while (lenx--) {
				putchar(*px++);
			}
		}
		break;

	case LWS_CALLBACK_COMPLETED_CLIENT_HTTP:
		if (globals.debug) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "WebSockets completed HTTP client\n");
		}
		break;

	case LWS_CALLBACK_CLIENT_WRITEABLE:
		/* Read buffer data write_data circular buffer and write it as a frame
		of size tech_pvt->frame_sz * sizeof(int16_t) */
		session = (switch_core_session_t *) lws_wsi_user(wsi);
		assert(session != NULL);
		tech_pvt = switch_core_session_get_private(session);

		switch_mutex_lock(tech_pvt->write_mutex);
		/* Check if what we have in buffer is enough to compose a frame, and we're skewing */
		if ((tech_pvt->write_count >= (tech_pvt->frame_sz * sizeof(int16_t))) && (tech_pvt->write_count <= tech_pvt->output_buffer_sz)) {
			if (tech_pvt->write_start + (tech_pvt->frame_sz * sizeof(int16_t)) >= tech_pvt->output_buffer_sz) {
				uint32_t amount = tech_pvt->output_buffer_sz - tech_pvt->write_start;
				char *tmp_frame;  
				switch_zmalloc(tmp_frame,  tech_pvt->frame_sz * sizeof(int16_t));
				memcpy(tmp_frame, tech_pvt->write_data + tech_pvt->write_start, amount);
				memcpy(tmp_frame + amount, tech_pvt->write_data + ((tech_pvt->frame_sz * sizeof(int16_t)) - amount), (tech_pvt->frame_sz * sizeof(int16_t) - amount));
				bytes_sent = websocket_write_back(wsi, LWS_WRITE_BINARY, tmp_frame, tech_pvt->frame_sz * sizeof(int16_t));
				switch_safe_free(tmp_frame);
			} else {
				bytes_sent = websocket_write_back(wsi, LWS_WRITE_BINARY, tech_pvt->write_data + tech_pvt->write_start, tech_pvt->frame_sz * sizeof(int16_t));
			}

			tech_pvt->ws_counter_write++;

			tech_pvt->write_start = (tech_pvt->write_start + bytes_sent) % tech_pvt->output_buffer_sz;
			if (tech_pvt->write_start >= tech_pvt->output_buffer_sz) {
				tech_pvt->write_start = 0;
			}
			tech_pvt->write_count -= bytes_sent;
		} else {
			/* We either don't have enough data or we've hold TIMEOUT_SIZE  (20 ms)
			worth of data; Skip hold frames */
			if (tech_pvt->write_count) {
				tech_pvt->write_count = 0;
				tech_pvt->write_start = 0;
				tech_pvt->write_end = 0;
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Websockets: LWS Service timeout.\n");
			}
		}
		switch_mutex_unlock(tech_pvt->write_mutex);

		break;

	case LWS_CALLBACK_GET_THREAD_ID:
		return switch_thread_self();

	case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
	case LWS_CALLBACK_LOCK_POLL:
	case LWS_CALLBACK_UNLOCK_POLL:
		/*avoid logging these, too much logging*/
		break;
	default:
		if (globals.debug) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Unknown reason (%d) for WebSockets callback\n", reason);
		}
		break;
	}

	return 0;
}

switch_status_t wsbridge_tech_init(private_t *tech_pvt, switch_core_session_t *session)
{
	switch_mutex_init(&tech_pvt->read_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));
	switch_mutex_init(&tech_pvt->write_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));
	switch_mutex_init(&tech_pvt->flag_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));
	switch_core_session_set_private(session, tech_pvt);
	tech_pvt->session = session;

	memset(&tech_pvt->i, 0, sizeof(tech_pvt->i));

	return SWITCH_STATUS_SUCCESS;
}

/*
State methods they get called when the state changes to the specific state
returning SWITCH_STATUS_SUCCESS tells the core to execute the standard state method next
so if you fully implement the state you can return SWITCH_STATUS_FALSE to skip it. */
static switch_status_t channel_on_init(switch_core_session_t *session)
{
	switch_channel_t *channel;
	private_t *tech_pvt = NULL;

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);
	switch_set_flag_locked(tech_pvt, TFLAG_IO);

	switch_mutex_lock(globals.mutex);
	globals.calls++;
	switch_mutex_unlock(globals.mutex);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_routing(switch_core_session_t *session)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);
	if (globals.debug) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s CHANNEL ROUTING\n", switch_channel_get_name(channel));
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_execute(switch_core_session_t *session)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	if (globals.debug) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s CHANNEL EXECUTE\n", switch_channel_get_name(channel));
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_destroy(switch_core_session_t *session)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);

	if (tech_pvt) {
		switch_core_timer_destroy(&tech_pvt->timer);

		cJSON_Delete(tech_pvt->message);

		if (switch_core_codec_ready(&tech_pvt->read_codec)) {
			switch_core_codec_destroy(&tech_pvt->read_codec);
		}

		if (switch_core_codec_ready(&tech_pvt->write_codec)) {
			switch_core_codec_destroy(&tech_pvt->write_codec);
		}

		switch_safe_free(tech_pvt->databuf);
		switch_safe_free(tech_pvt->read_data);
		switch_safe_free(tech_pvt->write_data);

	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_hangup(switch_core_session_t *session)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);
	switch_clear_flag_locked(tech_pvt, TFLAG_IO);
	switch_clear_flag_locked(tech_pvt, TFLAG_VOICE);

	/* Kill the service thread */
	tech_pvt->started = 1;
	/* Cancel service so we don't reference a null context */
	lws_cancel_service(tech_pvt->context);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s CHANNEL HANGUP\n", switch_channel_get_name(channel));
	if (globals.debug) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
				"RX FLOW CONTROL DEBUG STATS read WS: [%d]  write WS: [%d] read RTP: [%d] write RTP: [%d]\n", 
				tech_pvt->ws_counter_read, tech_pvt->ws_counter_write, tech_pvt->rtp_counter_read, tech_pvt->rtp_counter_write);
	}
	switch_mutex_lock(globals.mutex);
	globals.calls--;
	if (globals.calls < 0) {
		globals.calls = 0;
	}
	switch_mutex_unlock(globals.mutex);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_kill_channel(switch_core_session_t *session, int sig)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	switch (sig) {
	case SWITCH_SIG_KILL:
		switch_clear_flag_locked(tech_pvt, TFLAG_IO);
		switch_clear_flag_locked(tech_pvt, TFLAG_VOICE);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		break;
	case SWITCH_SIG_BREAK:
		switch_set_flag_locked(tech_pvt, TFLAG_BREAK);
		break;
	default:
		break;
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_exchange_media(switch_core_session_t *session)
{
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "CHANNEL LOOPBACK\n");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_soft_execute(switch_core_session_t *session)
{
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "CHANNEL TRANSMIT\n");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_send_dtmf(switch_core_session_t *session, const switch_dtmf_t *dtmf)
{
	private_t *tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt != NULL);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_read_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;
	switch_byte_t *data;

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);
	tech_pvt->read_frame.flags = SFF_PLC; /*let write audio codec on the RTP side always do PLC, unless we have a valid read from WS */
	*frame = NULL;

	switch_core_timer_next(&tech_pvt->timer);

	data = (switch_byte_t *) tech_pvt->read_frame.data;
	if (data) {
		memset(data, 255, tech_pvt->frame_sz * sizeof(int16_t));
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "pad silence toward RTP side, frame sz: %d (samples)\n", (int)tech_pvt->frame_sz);
	}
	tech_pvt->read_frame.datalen = tech_pvt->frame_sz * sizeof(int16_t);
	tech_pvt->read_frame.codec = &tech_pvt->read_codec;

	*frame = &tech_pvt->read_frame;

	if (tech_pvt->state == WSBRIDGE_STATE_DESTROY) {
		return SWITCH_STATUS_FALSE;
	}

	switch_mutex_lock(tech_pvt->read_mutex);

	if (tech_pvt->read_count) {
		memcpy(tech_pvt->databuf, tech_pvt->read_data, tech_pvt->frame_sz * sizeof(int16_t));
		tech_pvt->read_frame.flags = SFF_NONE;
		tech_pvt->read_count = 0;
	} else {
		tech_pvt->read_frame.flags = SFF_PLC;
	}
	
	tech_pvt->read_frame.datalen = tech_pvt->frame_sz * sizeof(int16_t);
	tech_pvt->read_frame.codec = &tech_pvt->read_codec;

	if (tech_pvt->ws_backpressure) {
		if (globals.debug) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "RX FLOW CONTROL: [enable receiving]\n");
		}
		lws_rx_flow_control(tech_pvt->wsi_wsbridge, 1);
		tech_pvt->ws_backpressure = 0;
	}

	/* Set out output frame */
	*frame = &tech_pvt->read_frame;

	tech_pvt->rtp_counter_write++;

	switch_mutex_unlock(tech_pvt->read_mutex);

	return SWITCH_STATUS_SUCCESS;

}

static switch_status_t channel_write_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	if (globals.debug) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Writing frame of size %d\n", frame->datalen);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Output circular buffer - write_end: %d, write_count:%d\n", tech_pvt->write_end, tech_pvt->write_count);
	}

	if (tech_pvt->state == WSBRIDGE_STATE_DESTROY) {
		return SWITCH_STATUS_FALSE;
	}

	if (!switch_test_flag(tech_pvt, TFLAG_IO)) {
		return SWITCH_STATUS_FALSE;
	}
#if SWITCH_BYTE_ORDER == __BIG_ENDIAN
	if (switch_test_flag(tech_pvt, TFLAG_LINEAR)) {
		switch_swap_linear(frame->data, (int) frame->datalen / 2);
	}
#endif

	switch_mutex_lock(tech_pvt->write_mutex);

	/* Copy the frame on the output circular buffer, wrapping around if needed */
	if (tech_pvt->write_end + frame->datalen < tech_pvt->output_buffer_sz) {
		memcpy(tech_pvt->write_data + tech_pvt->write_end, frame->data, frame->datalen);
	} else {
		int amount = tech_pvt->output_buffer_sz - tech_pvt->write_end;
		memcpy(tech_pvt->write_data + tech_pvt->write_end, frame->data, amount);
		memcpy(tech_pvt->write_data, frame->data, frame->datalen - amount);
	}

	tech_pvt->write_end = (tech_pvt->write_end + frame->datalen) % tech_pvt->output_buffer_sz;
	tech_pvt->write_count += frame->datalen;

	switch_mutex_unlock(tech_pvt->write_mutex);

	/* Ask for writing on the next service tick */
	lws_callback_on_writable(tech_pvt->wsi_wsbridge);

	tech_pvt->rtp_counter_read++;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_answer_channel(switch_core_session_t *session)
{
	private_t *tech_pvt;
	switch_channel_t *channel = NULL;

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_receive_message(switch_core_session_t *session, switch_core_session_message_t *msg)
{
	switch_channel_t *channel;
	private_t *tech_pvt;

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = (private_t *) switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	switch (msg->message_id) {
	case SWITCH_MESSAGE_INDICATE_ANSWER:
		{
			channel_answer_channel(session);
		}
		break;
	default:
		if (globals.debug) {
			switch_log_printf(SWITCH_CHANNEL_LOG,SWITCH_LOG_DEBUG,"received event: %d", (int)msg->message_id);
		}
		break;
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_consume_media(switch_core_session_t *session) {
	switch_channel_t *channel;
	private_t *tech_pvt;
	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = (private_t *) switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	if (globals.debug) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "channel_on_consume_media() called\n");
	}
	return SWITCH_STATUS_SUCCESS;

}
/* Make sure when you have 2 sessions in the same scope that you pass the appropriate one to the routines
   that allocate memory or you will have 1 channel with memory allocated from another channel's pool! */
static switch_call_cause_t channel_outgoing_channel(switch_core_session_t *session, switch_event_t *var_event,
													switch_caller_profile_t *outbound_profile,
													switch_core_session_t **new_session, switch_memory_pool_t **pool, switch_originate_flag_t flags,
													switch_call_cause_t *cancel_cause)
{
	if ((*new_session = switch_core_session_request(wsbridge_endpoint_interface, SWITCH_CALL_DIRECTION_OUTBOUND, flags, pool)) != 0) {
		private_t *tech_pvt;
		switch_channel_t *channel, *new_channel;
		switch_caller_profile_t *caller_profile;
		const char *prot, *p;
		uint32_t use_ssl;
		char *ws_uri, *ws_content_type, *ws_headers;
		/* Maximum lenght for the headers field. */
		char parsed_ws_headers[WS_HEADERS_MAX_SIZE];
		struct lws_context *context;
		cJSON* json_req = NULL;

		switch_core_session_add_stream(*new_session, NULL);
		if ((tech_pvt = (private_t *) switch_core_session_alloc(*new_session, sizeof(private_t))) != 0) {
			new_channel = switch_core_session_get_channel(*new_session);
			wsbridge_tech_init(tech_pvt, *new_session);
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(*new_session), SWITCH_LOG_CRIT, "Hey where is my memory pool?\n");
			switch_core_session_destroy(new_session);
			return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
		}

		channel = switch_core_session_get_channel(session);
		assert(channel != NULL);

		ws_uri =  (char*) switch_channel_get_variable(channel, HEADER_WS_URI);
		ws_headers = (char*) switch_channel_get_variable(channel, HEADER_WS_HEADERS);
		ws_content_type = (char*) switch_channel_get_variable(channel, HEADER_WS_CONT_TYPE);

		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_INFO,
			"SIP headers, URI [%s], HEADERS [%s], CONTENT-TYPE [%s]",
			ws_uri,
			ws_headers,
			ws_content_type);

		 if (zstr(ws_uri)) {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_ERROR,
				"Invalid Websocket destination (empty URI)\n");
				return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
		 }
		/* Handle the URI (it's mandatory) */
		if (outbound_profile) {
			char name[WS_URI_MAX_SIZE];

			switch_url_decode(ws_uri);
			wsbridge_str_remove_quotes(ws_uri);
			wsbridge_str_remove_empty_spaces(ws_uri);

			outbound_profile->destination_number = switch_core_strdup(outbound_profile->pool, ws_uri);
			snprintf(name, sizeof(name), "wsbridge/%s", outbound_profile->destination_number);
			switch_channel_set_name(new_channel, name);

			caller_profile = switch_caller_profile_clone(*new_session, outbound_profile);
			switch_channel_set_caller_profile(new_channel, caller_profile);
			tech_pvt->caller_profile = caller_profile;
		} else {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_ERROR,
				"Doh! Invalid caller profile / WS destination\n");

			switch_core_session_destroy(new_session);
			return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
		}

		/* Handle the custom headers (they are optional) */
		if (!zstr(ws_headers)) {
			switch_url_decode((char *)ws_headers);
			wsbridge_str_remove_quotes(ws_headers);

			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_INFO,
				"Decoded "HEADER_WS_HEADERS": [%s]",
				ws_headers);

			/* Too long JSON headers */
			if (strlen(ws_headers) >= WS_HEADERS_MAX_SIZE) {
				switch_log_printf(
					SWITCH_CHANNEL_SESSION_LOG(session),
					SWITCH_LOG_NOTICE,
					"JSON blob [%s] in \"HEADER_WS_HEADERS\" header too long [%d]. Dropping.\n",
					ws_headers,
					WS_HEADERS_MAX_SIZE);

				return SWITCH_CAUSE_MANDATORY_IE_LENGTH_ERROR;
			}

			wsbridge_strncpy_null_term(parsed_ws_headers, ws_headers, WS_HEADERS_MAX_SIZE);
			json_req = cJSON_Parse(parsed_ws_headers);

			/* Failed to parse JSON headers */
			if (json_req == NULL) {
				switch_log_printf(
					SWITCH_CHANNEL_SESSION_LOG(session),
					SWITCH_LOG_NOTICE,
					"Invalid JSON blob [%s] in \"HEADER_WS_HEADERS\" header. Dropping.\n",
					parsed_ws_headers);

				return SWITCH_CAUSE_INVALID_IE_CONTENTS;
			}
		} else {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_NOTICE,
				"Missing optional \"HEADER_WS_HEADERS\" header. Ignoring.\n");
		}

		/* Handle the content-type header */
		if (zstr(ws_content_type)) {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_NOTICE,
				"Invalid content-type; Using default [%s]\n",
				L16_16000);

			wsbridge_strncpy_null_term(tech_pvt->content_type, L16_16000, WS_CONT_TYPE_MAX_SIZE);
		} else {
			switch_url_decode((char *)ws_content_type);
			wsbridge_str_remove_quotes(ws_content_type);
			wsbridge_str_remove_empty_spaces(ws_content_type);
			/*
			  XXXNOTE: This could be much more more granular, trying to parse
			  the format below. However there's not much gain as of now, since
			  only two content types are supported.
			  (Content-Type := type "/" subtype *[";" parameter] )
			*/
			wsbridge_strncpy_null_term(tech_pvt->content_type, ws_content_type, WS_CONT_TYPE_MAX_SIZE);
		}

		if (wsbridge_codecs_init(tech_pvt, *new_session)) {
			/* The content type provided is not supported */
			return SWITCH_CAUSE_SERVICE_NOT_IMPLEMENTED;
		}

		if (tech_pvt->frame_sz) {
			switch_core_timer_init(&tech_pvt->timer, "soft", PTIME_RTP_MS, tech_pvt->frame_sz, switch_core_session_get_pool(session));

			switch_zmalloc(tech_pvt->read_data, tech_pvt->frame_sz * 2);  // bytes WSBRIDGE_INPUT_BUFFER_SIZE  
			switch_zmalloc(tech_pvt->write_data, tech_pvt->frame_sz * 2 * FRAMES_NR);  // bytes  WSBRIDGE_OUTPUT_BUFFER_SIZE
			switch_zmalloc(tech_pvt->databuf, tech_pvt->frame_sz * 2);  // bytes tech_pvt->frame_sz * sizeof(int16_t)
			tech_pvt->read_frame.data = tech_pvt->databuf;
			tech_pvt->read_frame.buflen = tech_pvt->frame_sz * 2;

		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Frame size not set!\n");
			return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
		}

		/* Add content-type to JSON message sent on handshake */
		if (json_req == NULL) {
			json_req = cJSON_CreateObject();
		}
		cJSON_AddItemToObject(json_req, "content-type", cJSON_CreateString(tech_pvt->content_type));

		tech_pvt->message = json_req;

		/* Set the actual thing up here */
		if (lws_parse_uri((char*) ws_uri, &prot, &tech_pvt->i.address, &tech_pvt->i.port, &p)) {
			/* XXX Error */
			return SWITCH_CAUSE_INVALID_URL;
		}

		/* add back the leading / on path */
		tech_pvt->path[0] = '/';
		strncpy(tech_pvt->path + 1, p, sizeof(tech_pvt->path) - 2);
		tech_pvt->path[sizeof(tech_pvt->path) - 1] = '\0';
		tech_pvt->i.path = tech_pvt->path;

		if (!strcmp(prot, "http") || !strcmp(prot, "ws")) {
			/* Do not establish a secure connection */
			use_ssl = 0;
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_DEBUG,
				"Found http/ws protocol. Establishing an unsecure connection\n");
		} else if (!strcmp(prot, "https") || !strcmp(prot, "wss")) {
			/*
			 * Note: A new version of libwebsockets has an enum defining names
			 * for the `use_ssl` member, but for now:
			 * 0 = ws://, 1 = wss:// encrypted, 2 = wss:// allow self
			 */

			/* Accept self-signed certificates */
			use_ssl = 2;
			/* Initialise the SSL library - otherwise the call does nothing */
			tech_pvt->info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_DEBUG,
				"Found https/wss protocol. Establishing a secure connection, accepting self-signed certificates\n");
		} else {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_WARNING,
				"Unknown protocol found in URI [%s]. Dropping the connection\n",
				prot);

			return SWITCH_CAUSE_INVALID_URL;
		}

		tech_pvt->info.port = CONTEXT_PORT_NO_LISTEN;
		tech_pvt->info.protocols = WSBRIDGE_protocols;
		tech_pvt->info.gid = -1;
		tech_pvt->info.uid = -1;
#ifdef LWS_DEBUG
		lws_set_log_level(0xF, ws_debug);
#endif 
		/*
		 * Create the context sequentially, as indicated in the docs - but the
		 * reason is the same as when calling lws_context_destroy, to avoid
		 * multithreading issues coming from OpenSSL
		 */
		switch_mutex_lock(globals.mutex);
		context = lws_create_context(&tech_pvt->info);
		switch_mutex_unlock(globals.mutex);
		if (context == NULL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Creating libwebsocket context failed\n");
			return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
		}

		tech_pvt->context = context;

		tech_pvt->i.context = context;
		tech_pvt->i.ssl_connection = use_ssl;
		tech_pvt->i.host = tech_pvt->i.address;
		tech_pvt->i.origin = tech_pvt->i.address;
		tech_pvt->i.ietf_version_or_minus_one = -1;
		tech_pvt->i.client_exts = exts;

		/* Hook the session onto the user object for the websocket */
		assert(*new_session != NULL);
		tech_pvt->i.userdata = (void*)*new_session;

		tech_pvt->i.protocol = WSBRIDGE_protocols[PROTOCOL_WSBRIDGE].name;
		tech_pvt->wsi_wsbridge = lws_client_connect_via_info(&tech_pvt->i);

		tech_pvt->write_count = 0;
		tech_pvt->write_start = 0;
		tech_pvt->write_end = 0;
		tech_pvt->read_start = 0;
		tech_pvt->read_end = 0;
		tech_pvt->read_count = 0;

		tech_pvt->state = WSBRIDGE_STATE_STARTED;

		wsbridge_thread_launch(tech_pvt);

		switch_set_flag_locked(tech_pvt, TFLAG_OUTBOUND);
		switch_channel_set_state(new_channel, CS_INIT);

		return SWITCH_CAUSE_SUCCESS;
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Cannot allocate memory\n");
	return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;

}

static int wsbridge_codecs_init(private_t *tech_pvt, switch_core_session_t *session) {

	/* Initialize read & write codecs */
	const char *content_type = tech_pvt->content_type;
	if (!strcasecmp(content_type, L16_16000)) {
		tech_pvt->frame_sz = WSBRIDGE_FRAME_SIZE_16000 / sizeof(int16_t); /*samples*/
		tech_pvt->output_buffer_sz =  WSBRIDGE_FRAME_SIZE_16000 * FRAMES_NR; /*bytes*/

		/* L16 signed linear 16 khz */
		if (switch_core_codec_init(&tech_pvt->read_codec, /* name */ "L16", /* modname */ NULL,
			/* fmtp */ NULL,  /* rate */ 16000, /* ms */ PTIME_RTP_MS, /* channels */ 1,
			/* flags */ SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
			/* codec settings */ NULL, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't initialize read codec\n");

			return -1;
		}

		if (switch_core_codec_init(&tech_pvt->write_codec, /* name */ "L16", /* modname */ NULL,
			/* fmtp */ NULL,  /* rate */ 16000, /* ms */ PTIME_RTP_MS, /* channels */ 1,
			/* flags */ SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
			/* codec settings */ NULL, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't initialize write codec\n");

			return -1;
		}
	} else if (!strcasecmp(content_type, L16_8000)) {
		tech_pvt->frame_sz = WSBRIDGE_FRAME_SIZE_8000 / sizeof(int16_t); /*samples*/
		tech_pvt->output_buffer_sz =  WSBRIDGE_FRAME_SIZE_8000 * FRAMES_NR; /*bytes*/

		/* L16 signed linear 8 khz */
		if (switch_core_codec_init(&tech_pvt->read_codec, /* name */ "L16", /* modname */ NULL,
			/* fmtp */ NULL,  /* rate */ 8000, /* ms */ PTIME_RTP_MS, /* channels */ 1,
			/* flags */ SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
			/* codec settings */ NULL, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't initialize read codec\n");

			return -1;
		}

		if (switch_core_codec_init(&tech_pvt->write_codec, /* name */ "L16", /* modname */ NULL,
			/* fmtp */ NULL,  /* rate */ 8000, /* ms */ PTIME_RTP_MS, /* channels */ 1,
			/* flags */ SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
			/* codec settings */ NULL, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't initialize write codec\n");
			return -1;
		}

	} else {
		switch_log_printf(
			SWITCH_CHANNEL_LOG,
			SWITCH_LOG_DEBUG,
			"Content-type specified [%s] is not supported\n",
			content_type);

			return -1;
	}

	switch_core_session_set_read_codec(session, &tech_pvt->read_codec);
	switch_core_session_set_write_codec(session, &tech_pvt->write_codec);

	return 0;
}

/*
 * Copy string from src to dst, using strncpy, forcing the null terminator at
 * the end of dst.
 *
 * The len argument is the total size of destination buffer, dst.
 */
static void wsbridge_strncpy_null_term(char *dst, char *src, int len)
{
	if ((dst == NULL) || (src == NULL)) {
		return; 
	}
	strncpy(dst, src, len);
	dst[len - 1] = '\0';
}

/*
 * Groom the content type to consider both cases where content-type is passed
 * under quotes or no quotes
 */
static void wsbridge_str_remove_quotes(char *str)
{
	char *i = str;

	if (str == NULL) {
		return;
	}
	/* There's an initial quote, remove both initial and final quotes */
	if (*i == '\"') {
		*i = ' ';
		*(i + (strlen(str)-1)) = ' ';
	}
}

/*
 * Remove any empty spaces that might be contained before, after or in the
 * middle of a string
 */
static void wsbridge_str_remove_empty_spaces(char *str)
{
	char *i = str;
	const char *j = str;

	if (str == NULL) {
		return;
	}
	/* Remove any empty spaces */
	while (*j != '\0') {
		*i = *j++;
		if (*i != ' ') {
			i++;
		}
	}
	*i = '\0';
}

static switch_status_t channel_receive_event(switch_core_session_t *session, switch_event_t *event)
{
	struct private_object *tech_pvt = switch_core_session_get_private(session);
	char *body = switch_event_get_body(event);
	switch_assert(tech_pvt != NULL);

	if (!body) {
		body = "";
	}

	return SWITCH_STATUS_SUCCESS;
}

switch_state_handler_table_t wsbridge_state_handlers = {
	/*.on_init */ channel_on_init,
	/*.on_routing */ channel_on_routing,
	/*.on_execute */ channel_on_execute,
	/*.on_hangup */ channel_on_hangup,
	/*.on_exchange_media */ channel_on_exchange_media,
	/*.on_soft_execute */ channel_on_soft_execute,
	/*.on_consume_media */ channel_on_consume_media,
	/*.on_hibernate */ NULL,
	/*.on_reset */ NULL,
	/*.on_park */ NULL,
	/*.on_reporting */ NULL,
	/*.on_destroy */ channel_on_destroy
};

switch_io_routines_t wsbridge_io_routines = {
	/*.outgoing_channel */ channel_outgoing_channel,
	/*.read_frame */ channel_read_frame,
	/*.write_frame */ channel_write_frame,
	/*.kill_channel */ channel_kill_channel,
	/*.send_dtmf */ channel_send_dtmf,
	/*.receive_message */ channel_receive_message,
	/*.receive_event */ channel_receive_event
};

static switch_status_t load_config(void)
{
	char *cf = "wsbridge.conf";
	switch_xml_t cfg, xml = NULL, settings, param;

	memset(&globals, 0, sizeof(globals));
	switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, module_pool);
	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", cf);
		/*don't stop loading if we don't find the config file*/
		return SWITCH_STATUS_SUCCESS;
	}

	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			char *var = (char *) switch_xml_attr_soft(param, "name");
			char *val = (char *) switch_xml_attr_soft(param, "value");

			if (!strcmp(var, "debug")) {
				globals.debug = atoi(val);
			} else if (!strcmp(var, "ip")) {
				set_global_ip(val);
			} else if (!strcmp(var, "codec-master")) {
				if (!strcasecmp(val, "us")) {
					switch_set_flag(&globals, GFLAG_MY_CODEC_PREFS);
				}
			} else if (!strcmp(var, "dialplan")) {
				set_global_dialplan(val);
			} else if (!strcmp(var, "codec-prefs")) {
				set_global_codec_string(val);
				globals.codec_order_last = switch_separate_string(globals.codec_string, ',', globals.codec_order, SWITCH_MAX_CODECS);
			} else if (!strcmp(var, "codec-rates")) {
				set_global_codec_rates_string(val);
				globals.codec_rates_last = switch_separate_string(globals.codec_rates_string, ',', globals.codec_rates, SWITCH_MAX_CODECS);
			}
		}
	}

	if (!globals.dialplan) {
		set_global_dialplan("default");
	}

	switch_xml_free(xml);

	return SWITCH_STATUS_SUCCESS;
}

#define WSBRIDGE_DEBUG_SYNTAX "<on|off>"
SWITCH_STANDARD_API(mod_wsbridge_debug)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-USAGE: %s\n", WSBRIDGE_DEBUG_SYNTAX);
	} else {
		if (!strcasecmp(cmd, "on")) {
			globals.debug = 1;
			stream->write_function(stream, "WSBridge Debug: on\n");
			stream->write_function(stream, "Library version: %s\n", lws_get_library_version());
		} else if (!strcasecmp(cmd, "off")) {
			globals.debug = 0;
			stream->write_function(stream, "WSBridge Debug: off\n");
		} else {
			stream->write_function(stream, "-USAGE: %s\n", WSBRIDGE_DEBUG_SYNTAX);
		}
	}

	return SWITCH_STATUS_SUCCESS;
} 

SWITCH_MODULE_LOAD_FUNCTION(mod_wsbridge_load)
{

	switch_api_interface_t *commands_api_interface;
	module_pool = pool;

	load_config();

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	wsbridge_endpoint_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_ENDPOINT_INTERFACE);
	wsbridge_endpoint_interface->interface_name = WSBRIDGE_INTERFACE_NAME;
	wsbridge_endpoint_interface->io_routines = &wsbridge_io_routines;
	wsbridge_endpoint_interface->state_handler = &wsbridge_state_handlers;

	SWITCH_ADD_API(commands_api_interface, "wsbridge_debug", "Enable WSBridge Debug", mod_wsbridge_debug, WSBRIDGE_DEBUG_SYNTAX);
	switch_console_set_complete("add wsbridge_debug on");
	switch_console_set_complete("add wsbridge_debug off");

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_wsbridge_shutdown)
{
	int x = 0;

	running = -1;

	while (running) {
		if (x++ > 100) {
			break;
		}
		switch_yield(20000);
	}

	/* Free dynamically allocated strings */
	switch_safe_free(globals.dialplan);
	switch_safe_free(globals.codec_string);
	switch_safe_free(globals.codec_rates_string);
	switch_safe_free(globals.ip);

	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
