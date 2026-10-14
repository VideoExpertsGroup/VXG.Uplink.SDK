#include <libwebsockets.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <ctype.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include "cjson/cJSON.h"
#include "vxg_proxy_client.h"

/* Do not use lws_log_emit_function / lws_log_emit_t here — names differ by libwebsockets version. */
static void (*vxg_lws_emit_fn)(int level, const char *line);
static int vxg_lws_level_mask = LLL_USER | LLL_ERR | LLL_WARN;

#ifndef VXG_ENABLE_BW_PROBE
#define VXG_ENABLE_BW_PROBE 1
#endif

#ifdef _WIN32
	#include <windows.h>
#endif

// Input validation function for device serial
static int is_valid_device_serial(const char* serial) {
    if (serial == NULL) return 0;
    
    size_t len = strlen(serial);
    if (len == 0 || len > 64) return 0;  // Must be 1-64 characters
    
    // Check each character: only alphanumeric, hyphens, and colons allowed
    for (size_t i = 0; i < len; i++) {
        if (!isalnum(serial[i]) && serial[i] != '-' && serial[i] != ':' && serial[i] != '_') {
            return 0;
        }
    }
    
    return 1;  // Valid
}

#ifndef VXG_DEFAULT_API_HOST
#define VXG_DEFAULT_API_HOST "camera.vxg.io"
#endif
char vxg_api_host[128] = VXG_DEFAULT_API_HOST;
char vxg_api_path[128] = "/v1/token?serial_id=%s";
char vxg_api_password[128] = "";

char proxy_api_host[256] = "";
char proxy_api_path[256] = "/api/ws-endpoint";
char proxy_ws_host[256] = "";
char proxy_ws_path[256] = "";
unsigned short proxy_ws_port = 443;

char device_serial[256] = "";
char auth_token[1024*8] = "";

// status file name
char status_file[256] = "";


static struct lws_context *context;
/** Set after first successful lws_create_context with SSL; do not repeat LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT. */
static int vxg_lws_ssl_global_inited;
/** Count consecutive HTTP 401s from token API; reset on success. In-context retry avoids lws_context_destroy. */
static unsigned int vxg_token_401_attempts;
/** Consecutive TLS setup failures (SSL_new/bio_create); reset on successful TLS progress. */
static unsigned int vxg_tls_setup_fail_count;
/*
 * Provisioning 401 (no token yet): two-phase in-process retries.
 *   Phase 1 — first LINEAR_ATTEMPTS failures: wait LINEAR_DELAY_SEC (default 60s) each
 *             (~one try per minute for the first 15 minutes).
 *   Phase 2 — then exponential: min(BACKOFF_BASE * 2^(k-1), BACKOFF_CAP); k starts at 1
 *             after the linear phase (default first gap 2 min, cap 1 h).
 * Tunable: -DVXG_TOKEN_401_*.
 */
#ifndef VXG_TOKEN_401_MAX_ATTEMPTS
#define VXG_TOKEN_401_MAX_ATTEMPTS 43
#endif
#ifndef VXG_TOKEN_401_LINEAR_ATTEMPTS
#define VXG_TOKEN_401_LINEAR_ATTEMPTS 15
#endif
#ifndef VXG_TOKEN_401_LINEAR_DELAY_SEC
#define VXG_TOKEN_401_LINEAR_DELAY_SEC 60
#endif
#ifndef VXG_TOKEN_401_BACKOFF_BASE_SEC
#define VXG_TOKEN_401_BACKOFF_BASE_SEC 120
#endif
#ifndef VXG_TOKEN_401_BACKOFF_CAP_SEC
#define VXG_TOKEN_401_BACKOFF_CAP_SEC 3600
#endif
/* After this many consecutive SSL_new/bio_create failures, recreate the lws context. */
#ifndef VXG_TLS_SETUP_FAIL_MAX
#define VXG_TLS_SETUP_FAIL_MAX 3
#endif
volatile int force_exit = 0;
volatile int restart = 1;

static vxg_proxy_client_idle_fn vxg_idle_cb;
static void *vxg_idle_userdata;

/** True when LWS CLIENT_CONNECTION_ERROR indicates unusable TLS (not a normal network blip). */
static int vxg_conn_error_is_tls_fatal(const char *in)
{
	if (!in || !in[0])
		return 0;
	if (strstr(in, "SSL_new") || strstr(in, "ssl_new"))
		return 1;
	if (strstr(in, "bio_create") || strstr(in, "BIO_create"))
		return 1;
	if (strstr(in, "null ssl") || strstr(in, "null SSL") || strstr(in, "NULL SSL"))
		return 1;
	return 0;
}

/**
 * Handle TLS-fatal connection errors. Returns 1 if escalated (caller must not conceal-retry).
 * Under the threshold, returns 0 so the caller may still schedule a short retry.
 */
static int vxg_handle_tls_fatal_conn_error(const char *in)
{
	if (!vxg_conn_error_is_tls_fatal(in))
		return 0;

	vxg_tls_setup_fail_count++;
	lwsl_err("TLS setup failed (%s); consecutive=%u/%u\n",
		 in ? in : "(null)",
		 vxg_tls_setup_fail_count,
		 (unsigned)VXG_TLS_SETUP_FAIL_MAX);

	if (vxg_tls_setup_fail_count < (unsigned)VXG_TLS_SETUP_FAIL_MAX)
		return 0;

	lwsl_err("TLS setup failures exceeded; recreating lws context\n");
	restart = 1;
	force_exit = 1;
	if (context)
		lws_cancel_service(context);
	return 1;
}

static void vxg_tls_setup_fail_reset(void)
{
	vxg_tls_setup_fail_count = 0;
}

#define PERF_LOG

#ifndef WEBSOCKET_BUFFER_SIZE
#define WEBSOCKET_BUFFER_SIZE   	131072
#endif

#ifndef RAW_RX_BUFFER_SIZE
#define RAW_RX_BUFFER_SIZE      	131072
#endif

#ifndef AGENT_VERSION
#define AGENT_VERSION   			"2.4.6"
#endif

#define RING_DEPTH 					8192
#define RING_DEPTH_CRITICAL			7800
#define RING_DEPTH_OK				6000

#ifndef WS_RING_BYTES_MAX
#define WS_RING_BYTES_MAX			(16 * 1024 * 1024)   /* 16 MB */
#endif
#ifndef WS_RING_BYTES_OK
#define WS_RING_BYTES_OK			(8 * 1024 * 1024)   /* 8 MB */
#endif

/* Priority tiers: real-time (TCP/RTSP) vs bulk (HTTP/HTTPS).
 * Bulk clients get throttled earlier and resumed later to
 * preserve WS ring capacity for latency-sensitive streams. */
#define PRIORITY_REALTIME  0   /* TCP — RTSP, SSH, etc. */
#define PRIORITY_BULK      1   /* HTTP / HTTPS — file downloads, HLS */

#ifndef RING_DEPTH_CRITICAL_BULK
#define RING_DEPTH_CRITICAL_BULK	6600   /* vs 7800 for realtime */
#endif
#ifndef WS_RING_BYTES_MAX_BULK
#define WS_RING_BYTES_MAX_BULK		(12 * 1024 * 1024)  /* 12 MB vs 16 MB */
#endif
#ifndef RING_DEPTH_OK_BULK
#define RING_DEPTH_OK_BULK			4500   /* vs 6000 for realtime */
#endif
#ifndef WS_RING_BYTES_OK_BULK
#define WS_RING_BYTES_OK_BULK		(6 * 1024 * 1024)   /* 6 MB vs 8 MB */
#endif

#define MAX_FORWARD_ITEM_NAME_LEN 	50
#define MAX_FORWARD_ITEM_HOST_LEN 	255
#define MAX_FORWARD_ITEMS 			10
#define MAX_CLIENT_CONNECTIONS		256

struct forward_item {
	char name[MAX_FORWARD_ITEM_NAME_LEN+1];
	char host[MAX_FORWARD_ITEM_HOST_LEN+1];
	enum {
		PROTO_NONE,
		PROTO_HTTP,
		PROTO_HTTPS,
		PROTO_TCP
	} proto;
	uint16_t port;
	uint8_t realtime;  /* 1 = realtime priority, 0 = bulk; set only by --priority / -p CLI flag */
};

struct forward_item forward_items[MAX_FORWARD_ITEMS+1];

struct msg {
	void *payload;
	size_t len;
};

static void destroy_message(void *_msg)
{
	struct msg *msg = _msg;

	free(msg->payload);
	msg->payload = NULL;
	msg->len = 0;
}

#ifdef PERF_LOG
#define PERF_INTERVAL_SEC 5
static uint64_t perf_raw_rx_bytes = 0;   /* backend → WS */
static uint64_t perf_ws_rx_bytes = 0;    /* WS → backend */
static uint32_t perf_raw_rx_msgs = 0;
static uint32_t perf_ws_rx_msgs = 0;
static uint64_t perf_up_bytes_rt = 0;    /* UP bytes from realtime clients */
static uint64_t perf_up_bytes_bulk = 0;  /* UP bytes from bulk clients */
static uint64_t perf_down_bytes_rt = 0;  /* DOWN bytes to realtime clients */
static uint64_t perf_down_bytes_bulk = 0;/* DOWN bytes to bulk clients */
static lws_sorted_usec_list_t perf_sul;
static uint32_t perf_block_events = 0;
static uint32_t perf_unblock_events = 0;
static uint32_t perf_block_rt = 0;      /* blocks on realtime (TCP) clients */
static uint32_t perf_block_bulk = 0;    /* blocks on bulk (HTTP) clients */
static uint32_t perf_unblock_rt = 0;    /* unblocks of realtime clients */
static uint32_t perf_unblock_bulk = 0;  /* unblocks of bulk clients */
static uint32_t perf_client_count = 0;
static int perf_rt_clients = 0;         /* snapshot of realtime_client_count */
static int perf_bulk_clients = 0;       /* snapshot of bulk_client_count */
static uint64_t perf_ring_bytes = 0;
static struct timeval perf_prev_utime;
static struct timeval perf_prev_stime;

static void perf_timer_cb(lws_sorted_usec_list_t *sul)
{
	struct rusage ru;
	double cpu_pct = 0.0;
	long rss_kb = 0;

	if (getrusage(RUSAGE_SELF, &ru) == 0) {
		double u = (double)(ru.ru_utime.tv_sec - perf_prev_utime.tv_sec)
		         + (double)(ru.ru_utime.tv_usec - perf_prev_utime.tv_usec) / 1e6;
		double s = (double)(ru.ru_stime.tv_sec - perf_prev_stime.tv_sec)
		         + (double)(ru.ru_stime.tv_usec - perf_prev_stime.tv_usec) / 1e6;
		cpu_pct = (u + s) / PERF_INTERVAL_SEC * 100.0;
		perf_prev_utime = ru.ru_utime;
		perf_prev_stime = ru.ru_stime;
	}

#ifdef __APPLE__
	{
		struct mach_task_basic_info info;
		mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
		if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
			      (task_info_t)&info, &count) == KERN_SUCCESS)
			rss_kb = (long)(info.resident_size / 1024);
	}
#else
	{
		FILE *f = fopen("/proc/self/status", "r");
		if (f) {
			char line[256];
			while (fgets(line, sizeof(line), f)) {
				if (strncmp(line, "VmRSS:", 6) == 0) {
					unsigned long kb = 0;
					if (sscanf(line + 6, "%lu", &kb) == 1 &&
					    kb <= (unsigned long)LONG_MAX)
						rss_kb = (long)kb;
					break;
				}
			}
			fclose(f);
		}
	}
#endif

	lwsl_user("PERF ↑%.1f ↓%.1f (RT %.1f/%.1f BLK %.1f/%.1f) Mbps"
		" | ↑%u/↓%u Msgs"
		" | cli %u (RT %d BLK %d)"
		" | Bcli %u (RT %u BLK %u)"
		" | Ocli %u (RT %u BLK %u)"
		" | %.1f%%/%.2f CPU/MB, %.1f MB RING\n",
		(double)perf_raw_rx_bytes * 8.0 / PERF_INTERVAL_SEC / 1e6,
		(double)perf_ws_rx_bytes * 8.0 / PERF_INTERVAL_SEC / 1e6,
		(double)perf_up_bytes_rt * 8.0 / PERF_INTERVAL_SEC / 1e6,
		(double)perf_down_bytes_rt * 8.0 / PERF_INTERVAL_SEC / 1e6,
		(double)perf_up_bytes_bulk * 8.0 / PERF_INTERVAL_SEC / 1e6,
		(double)perf_down_bytes_bulk * 8.0 / PERF_INTERVAL_SEC / 1e6,
		perf_raw_rx_msgs,
		perf_ws_rx_msgs,
		perf_client_count,
		perf_rt_clients,
		perf_bulk_clients,
		perf_block_events, perf_block_rt, perf_block_bulk,
		perf_unblock_events, perf_unblock_rt, perf_unblock_bulk,
		cpu_pct,
		(double)rss_kb / 1024.0,
		(double)perf_ring_bytes / (1024.0 * 1024.0));

	perf_raw_rx_bytes = perf_ws_rx_bytes = 0;
	perf_raw_rx_msgs = perf_ws_rx_msgs = 0;
	perf_up_bytes_rt = perf_up_bytes_bulk = 0;
	perf_down_bytes_rt = perf_down_bytes_bulk = 0;
	perf_block_events = perf_unblock_events = 0;
	perf_block_rt = perf_block_bulk = 0;
	perf_unblock_rt = perf_unblock_bulk = 0;
	lws_sul_schedule(context, 0, &perf_sul, perf_timer_cb, PERF_INTERVAL_SEC * LWS_USEC_PER_SEC);
}
#endif

char* convert_to_upper(char *s)
{
    int i;
    for (i = 0; s[i]!='\0'; i++) {
        if(s[i] >= 'a' && s[i] <= 'z') {
            s[i] = s[i] - 32;
        }
    }
    return s;
}

static void copy_bounded(char *dst, size_t dstsz, const char *src)
{
	if (!dst || dstsz == 0)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	strncpy(dst, src, dstsz - 1);
	dst[dstsz - 1] = '\0';
}

static int copy_env(char *dst, size_t dstsz, const char *name)
{
	const char *v;

	if (!dst || dstsz == 0 || !name)
		return 0;
	v = getenv(name);
	if (!v)
		return 0;
	strncpy(dst, v, dstsz - 1);
	dst[dstsz - 1] = '\0';
	return 1;
}


typedef uint16_t client_id_t;

struct proxy_conn {
	struct lws		*wsi_raw; // wsi for the outbound raw conn
	struct lws_ring *ring;
	uint8_t forward_index;
	client_id_t client_id;
	uint32_t tail;
	char flow_controlled;
	char proxy_paused;  /* set by proxy P command, cleared by R */
	char close_notification_sent;
	uint8_t write_consume_pending:1;
	uint8_t priority;   /* PRIORITY_REALTIME or PRIORITY_BULK */
	struct proxy_conn *next_client;
	struct proxy_conn *prev_client;
};

static struct my_conn {
	lws_sorted_usec_list_t	sul;	     // schedule connection retry
	struct lws		*wsi;
	uint16_t		retry_count;
	struct lws_ring *ring;
	uint32_t tail;
	char flow_controlled;
	struct proxy_conn *first_client;
	uint32_t  total_msgs_in_client_rings;
	uint64_t  ring_bytes;
	int realtime_client_count;  /* number of active PRIORITY_REALTIME clients */
	int bulk_client_count;      /* number of active PRIORITY_BULK clients;
	                             * differential throttling only when BOTH > 0 */
} ws_conn, vxg_token_api_conn, proxy_api_conn;


#ifdef __NO_SSL__
static int port = 80, ssl_connection = LCCSCF_ALLOW_INSECURE;
#elif __SKIP_SSL_CHECK__
static int port = 443, ssl_connection = LCCSCF_ALLOW_INSECURE | LCCSCF_ALLOW_EXPIRED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK | LCCSCF_ALLOW_SELFSIGNED;
#else
static int port = 443, ssl_connection = LCCSCF_USE_SSL;
#endif

static const uint32_t backoff_ms[] = { 1000, 2000, 3000, 4000, 5000 };

static const lws_retry_bo_t ws_retry = {
	.retry_ms_table			= backoff_ms,
	.retry_ms_table_count	= LWS_ARRAY_SIZE(backoff_ms),
	.conceal_count			= LWS_RETRY_CONCEAL_ALWAYS,

	.secs_since_valid_ping		= 20,  /* force PINGs after secs idle */
	.secs_since_valid_hangup	= 120, /* hangup after secs idle, gives 5 PING attempts */

	.jitter_percent			= 20,
};

static const lws_retry_bo_t vxg_token_api_retry = {
	.retry_ms_table			= backoff_ms,
	.retry_ms_table_count	= LWS_ARRAY_SIZE(backoff_ms),
	.conceal_count			= LWS_RETRY_CONCEAL_ALWAYS,

	.secs_since_valid_ping		= 30,  /* force PINGs after secs idle */
	.secs_since_valid_hangup	= 60, /* hangup after secs idle */

	.jitter_percent			= 20,
};

static const lws_retry_bo_t proxy_api_retry = {
	.retry_ms_table			= backoff_ms,
	.retry_ms_table_count	= LWS_ARRAY_SIZE(backoff_ms),
	.conceal_count			= LWS_RETRY_CONCEAL_ALWAYS,

	.secs_since_valid_ping		= 30,  /* force PINGs after secs idle */
	.secs_since_valid_hangup	= 60, /* hangup after secs idle */

	.jitter_percent			= 20,
};


static void proxy_api_connect(lws_sorted_usec_list_t *sul);

static void encode_be_double(double value, unsigned char out[8])
{
	uint64_t raw_bits;
	memcpy(&raw_bits, &value, sizeof(raw_bits));
	for (int i = 0; i < 8; ++i)
		out[i] = (unsigned char)((raw_bits >> ((7 - i) * 8)) & 0xffu);
}

#ifndef VXG_BW_UPLOAD_PROBE_MAX_BYTES
#define VXG_BW_UPLOAD_PROBE_MAX_BYTES (10 * 1024 * 1024)  /* hard cap regardless of what proxy asks for */
#endif

static uint32_t decode_be_u32(const unsigned char *in)
{
	return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
	       ((uint32_t)in[2] << 8)  | (uint32_t)in[3];
}

// ----------------------------------------------------------------------------
static void websocket_connect(lws_sorted_usec_list_t *sul)
{
	struct my_conn *m = lws_container_of(sul, struct my_conn, sul);
	struct lws_client_connect_info i;

	memset(&i, 0, sizeof(i));

	i.context = context;
	i.port = port;
	i.address = proxy_ws_host;
	i.path = proxy_ws_path;
	i.host = i.address;
	i.origin = i.address;
	i.ssl_connection = ssl_connection;
	i.protocol = "ra-proxy";
	i.local_protocol_name = "vxg-websocket";
	i.pwsi = &m->wsi;
	i.retry_and_idle_policy = &ws_retry;
	i.userdata = m;

    lwsl_user("Trying to connect to %s:%d%s\n", i.host, i.port, i.path);

	if (!lws_client_connect_via_info(&i))
		if (lws_retry_sul_schedule(context, 0, sul, &ws_retry, websocket_connect, &m->retry_count)) {
			lwsl_err("%s: connection attempts exhausted\n", __func__);
			force_exit = 1;
		}
}

// ----------------------------------------------------------------------------
static int proxy_client_connection_closed_notification(struct proxy_conn *client)
{
	struct msg amsg;

	if (ws_conn.ring && !client->close_notification_sent) {
		amsg.len = 1+sizeof(client_id_t);
		amsg.payload = malloc(LWS_PRE + amsg.len);
		if (!amsg.payload)
		{
			lwsl_err("%s: malloc() error\n", __func__);
			return -1;
		}
		*((char *)(amsg.payload+LWS_PRE)) = 'C';
		*((client_id_t *)(amsg.payload+LWS_PRE+1)) = client->client_id;
		if (!lws_ring_insert(ws_conn.ring, &amsg, 1)) {
			destroy_message(&amsg);
			lwsl_err("LWS_CALLBACK_CLIENT_RECEIVE: lws_ring_insert() error\n");
			return -1;
		}
		client->close_notification_sent = 1;
		lws_callback_on_writable(ws_conn.wsi);
		return 0;
	}
	return -1;
}


// ----------------------------------------------------------------------------
static int destroy_proxy_client(struct proxy_conn *client)
{
	int n;
	if (client->ring) {
		n = (int)lws_ring_get_count_waiting_elements(client->ring, &client->tail);
		ws_conn.total_msgs_in_client_rings -= n;

		if (ws_conn.flow_controlled && ws_conn.total_msgs_in_client_rings <= RING_DEPTH_OK) {
			ws_conn.flow_controlled = 0;
#ifdef PERF_LOG
			perf_unblock_events++;
#endif
			lwsl_user("websocket rx unblocked\n");
			lws_rx_flow_control(ws_conn.wsi, 1);
		}
		lws_ring_destroy(client->ring);
		client->ring = NULL;
	}
	if (client->wsi_raw) {
		client->wsi_raw = NULL;
	}
	proxy_client_connection_closed_notification(client);
	if (client->prev_client)
		client->prev_client->next_client = client->next_client;
	if (client->next_client)
		client->next_client->prev_client = client->prev_client;
	if (client == ws_conn.first_client)
		ws_conn.first_client = client->next_client;
	if (client->priority == PRIORITY_REALTIME && ws_conn.realtime_client_count > 0)
		ws_conn.realtime_client_count--;
	if (client->priority == PRIORITY_BULK && ws_conn.bulk_client_count > 0)
		ws_conn.bulk_client_count--;
#ifdef PERF_LOG
	if (perf_client_count > 0) perf_client_count--;
	perf_rt_clients = ws_conn.realtime_client_count;
	perf_bulk_clients = ws_conn.bulk_client_count;
#endif
	free(client);
	return 0;
}


// ----------------------------------------------------------------------------
static int proxy_client_callback(struct lws *wsi, enum lws_callback_reasons reason,
		 void *user, void *in, size_t len)
{
	struct proxy_conn *client = (struct proxy_conn *)user;
	struct msg amsg;
	const struct msg *pmsg;
	int bytes_written, n;

	switch (reason) {
	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
	{
		lwsl_debug("%s: onward raw connection failed (client_id=%d)\n", __func__, client->client_id);
		destroy_proxy_client(client);
		return 0;
	}

	case LWS_CALLBACK_RAW_CONNECTED:
	{
		lwsl_debug("%s: onward raw connection established (client_id=%d)\n", __func__, client->client_id);
		client->wsi_raw = wsi;
		/* Disable Nagle only for realtime (TCP/RTSP) clients where
		 * per-packet latency matters.  Bulk (HTTP) clients benefit from
		 * Nagle’s coalescing — fewer packets, better throughput. */
		if (client->priority == PRIORITY_REALTIME) {
			int fd = lws_get_socket_fd(wsi);
			if (fd >= 0) {
				int flag = 1;
				setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
			}
		}
		lws_callback_on_writable(wsi);
		return 0;
	}

	case LWS_CALLBACK_RAW_ADOPT:
	{
		lwsl_user("LWS_CALLBACK_RAW_ADOPT\n");
		client->wsi_raw = wsi;
		break;
	}

	case LWS_CALLBACK_RAW_CLOSE:
	{
		lwsl_debug("LWS_CALLBACK_RAW_CLOSE (client_id=%d)\n", client->client_id);
		destroy_proxy_client(client);
		return 0;
	}

	case LWS_CALLBACK_RAW_RX:
	{
		lwsl_debug("New data from client connection (client_id=%d, len=%d)\n", client->client_id, (int)len);
		lwsl_hexdump_debug(in, len);
#ifdef PERF_LOG
		perf_raw_rx_bytes += len;
		perf_raw_rx_msgs++;
		if (client->priority == PRIORITY_REALTIME)
			perf_up_bytes_rt += len;
		else
			perf_up_bytes_bulk += len;
#endif
		if (!ws_conn.ring) {
			lws_set_timeout(client->wsi_raw, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, LWS_TO_KILL_ASYNC);
			return -1;
		}
		n = (int)lws_ring_get_count_free_elements(ws_conn.ring);
		if (!n) {
			lwsl_err("LWS_CALLBACK_RAW_RX: lws_ring_insert() - ring is full!\n");
			lws_set_timeout(client->wsi_raw, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, LWS_TO_KILL_ASYNC);
			break;
		}
		amsg.len = 1+sizeof(client_id_t)+len;
		amsg.payload = malloc(LWS_PRE + amsg.len);
		if (!amsg.payload)
		{
			lwsl_err("%s: malloc() error\n", __func__);
			return -1;
		}
		*((char *)(amsg.payload+LWS_PRE)) = 'D';
		*((client_id_t *)(amsg.payload+LWS_PRE+1)) = client->client_id;
		memcpy(amsg.payload + LWS_PRE + 1+sizeof(client_id_t), in, len);
		if (!lws_ring_insert(ws_conn.ring, &amsg, 1)) {
			destroy_message(&amsg);
			lwsl_err("LWS_CALLBACK_RAW_RX: lws_ring_insert() error\n");
			lws_set_timeout(client->wsi_raw, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, LWS_TO_KILL_ASYNC);
			return -1;
		}
		ws_conn.ring_bytes += amsg.len;
#ifdef PERF_LOG
		perf_ring_bytes = ws_conn.ring_bytes;
#endif
		{
			/* Priority-aware flow control: bulk (HTTP) clients get
			 * throttled earlier to preserve ring capacity for
			 * real-time (TCP/RTSP) traffic — but only when at least
			 * one realtime client is active.  When no realtime
			 * clients exist, everyone uses normal thresholds. */
			int used = RING_DEPTH - (n - 1);
			int mixed = (ws_conn.realtime_client_count > 0 &&
			             ws_conn.bulk_client_count > 0);
			int depth_limit = (client->priority == PRIORITY_BULK && mixed)
			                ? RING_DEPTH_CRITICAL_BULK : RING_DEPTH_CRITICAL;
			uint64_t bytes_limit = (client->priority == PRIORITY_BULK && mixed)
			                     ? WS_RING_BYTES_MAX_BULK : WS_RING_BYTES_MAX;

			if (!client->flow_controlled &&
			    (used >= depth_limit || ws_conn.ring_bytes >= bytes_limit)) {
				client->flow_controlled = 1;
#ifdef PERF_LOG
				perf_block_events++;
				if (client->priority == PRIORITY_REALTIME)
					perf_block_rt++;
				else
					perf_block_bulk++;
#endif
				lws_rx_flow_control(wsi, 0);
			}
		}
		lws_callback_on_writable(ws_conn.wsi);
		break;
	}

	case LWS_CALLBACK_RAW_WRITEABLE:
	{
		if (!client->ring)
			break;

		pmsg = lws_ring_get_element(client->ring, &client->tail);
		if (!pmsg) {
//			lwsl_user(" CLIENT (nothing in ring)\n");
			break;
		}

		/* notice we allowed for LWS_PRE in the payload already */
		bytes_written = lws_write(wsi, ((unsigned char *)pmsg->payload) +
			      LWS_PRE, pmsg->len, (enum lws_write_protocol)LWS_WRITE_BINARY);
		if (bytes_written < (int)pmsg->len) {
			lwsl_err("ERROR %d writing to ws socket\n", bytes_written);
			lws_set_timeout(client->wsi_raw, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, LWS_TO_KILL_ASYNC);
			return -1;
		}

#ifdef PERF_LOG
		if (client->priority == PRIORITY_REALTIME)
			perf_down_bytes_rt += bytes_written;
		else
			perf_down_bytes_bulk += bytes_written;
#endif

		lws_ring_consume_single_tail(client->ring, &client->tail, 1);
		ws_conn.total_msgs_in_client_rings--;

		if (ws_conn.flow_controlled && ws_conn.total_msgs_in_client_rings <= RING_DEPTH_OK) {
			ws_conn.flow_controlled = 0;
#ifdef PERF_LOG
			perf_unblock_events++;
#endif
			// lwsl_user("websocket rx unblocked\n");
			lws_rx_flow_control(ws_conn.wsi, 1);
		}

		n = (int)lws_ring_get_count_waiting_elements(client->ring, &client->tail);
		if (n > 0)
			lws_callback_on_writable(wsi);
		break;
	}
	default:
		break;
	}

	return 0;
}

char *websocket_rcv_buffer=NULL;
int websocket_rcv_buffer_len=0;

#if VXG_ENABLE_BW_PROBE
/*
 * Downlink bandwidth probe receive state.
 * A large W frame may be delivered across multiple receive callbacks even
 * without WebSocket fragmentation.  Keep only the fixed 25-byte probe header;
 * the padding itself does not need to be reassembled.
 */
static int bw_down_probe_rx_in_progress = 0;
static unsigned char bw_down_probe_header[25];
#endif

/** Wait time before the next token request after the attempt-th 401 (attempt is 1-based). */
static unsigned int vxg_token_401_backoff_delay_sec(unsigned int attempt_1based)
{
	unsigned int o;
	unsigned int sh;
	unsigned long long d;

	if (attempt_1based < 1u)
		attempt_1based = 1u;

	if (attempt_1based <= VXG_TOKEN_401_LINEAR_ATTEMPTS)
		return VXG_TOKEN_401_LINEAR_DELAY_SEC;

	o = attempt_1based - VXG_TOKEN_401_LINEAR_ATTEMPTS; /* 1 = first gap after linear phase */
	if (o > 20u)
		o = 20u;
	sh = o - 1u;
	d = (unsigned long long)VXG_TOKEN_401_BACKOFF_BASE_SEC << sh;
	if (d > (unsigned long long)VXG_TOKEN_401_BACKOFF_CAP_SEC)
		d = (unsigned long long)VXG_TOKEN_401_BACKOFF_CAP_SEC;
	return (unsigned int)d;
}

// ----------------------------------------------------------------------------
static void vxg_token_api_connect(lws_sorted_usec_list_t *sul)
{
	struct my_conn *m = lws_container_of(sul, struct my_conn, sul);
	struct lws_client_connect_info i;
	char path_query[1024*4];

	memset(&i, 0, sizeof(i));

	i.context = context;
	i.port = port;
	i.address = vxg_api_host;
	
	// Validate device serial to prevent injection attacks
	if (!is_valid_device_serial(device_serial)) {
		lwsl_err("Invalid device serial format: %s\n", device_serial);
		force_exit = 1;
		return;
	}
	
	// Safe replacement for format string vulnerability: replace %s with device_serial
	char *placeholder = strstr(vxg_api_path, "%s");
	if (placeholder != NULL) {
		// Calculate lengths
		size_t prefix_len = placeholder - vxg_api_path;
		size_t suffix_len = strlen(placeholder + 2); // +2 to skip "%s"
		
		// Validate that the resulting path won't exceed buffer size
		if (prefix_len + strlen(device_serial) + suffix_len >= sizeof(path_query)) {
			lwsl_err("Path would exceed buffer size\n");
			force_exit = 1;
			return;
		}
		
		// Build safe path using validated input
		snprintf(path_query, sizeof(path_query), "%.*s%s%s", 
			(int)prefix_len, vxg_api_path, device_serial, placeholder + 2);
	} else {
		// No %s found, just copy the path as-is
		strncpy(path_query, vxg_api_path, sizeof(path_query) - 1);
		path_query[sizeof(path_query) - 1] = '\0';
	}
	i.path = path_query;
	i.host = i.address;
	i.origin = i.address;
	i.ssl_connection = ssl_connection;
    i.method = "GET";
	i.protocol = "vxg-authtoken";
	i.pwsi = &m->wsi;
	i.retry_and_idle_policy = &vxg_token_api_retry;
	i.userdata = m;

	lwsl_user("Requesting auth token from %s:%d\n", i.host, i.port);
	if (!lws_client_connect_via_info(&i))
		if (lws_retry_sul_schedule(context, 0, sul, &vxg_token_api_retry,
					   vxg_token_api_connect, &m->retry_count)) {
			lwsl_err("%s: connection attempts exhausted\n", __func__);
			force_exit = 1;
		}
}

int vxg_token_api_request_status = 0;
char auth_res_json_buffer[1024*10];


/* Sticky cookie stored from ws-endpoint response; sent on both /api/ws-endpoint
 * and the /device-ws WebSocket upgrade to keep this camera on the same proxy. */
static char proxy_sticky_cookie[256] = "";

// ----------------------------------------------------------------------------
static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
		 void *user, void *in, size_t len)
{
	struct my_conn *m = (struct my_conn *)user;
	const struct msg *pmsg;
	struct msg amsg;
	int n, bytes_written;
	char cmd;
	uint8_t forward_index;
	client_id_t client_id;
	struct lws_client_connect_info client_conn_info;
	struct proxy_conn *client, *prev_client;

	switch (reason) {

	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
	{
#if VXG_ENABLE_BW_PROBE
		bw_down_probe_rx_in_progress = 0;
#endif
		lwsl_err("CLIENT_CONNECTION_ERROR (%d): %s\n",
			lws_get_socket_fd(wsi),
			in ? (char *)in : "(null)");
		if (m->ring) {
			lws_ring_destroy(m->ring);
			m->total_msgs_in_client_rings = 0;
			m->ring_bytes = 0;
			m->ring = NULL;
		}
		for (client=m->first_client; client; client = client->next_client) {
			lws_set_timeout(client->wsi_raw, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, LWS_TO_KILL_ASYNC);
		}
		if (vxg_handle_tls_fatal_conn_error(in ? (char *)in : NULL))
			break;
		goto do_retry;
		break;
	}

	case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
	{
		static time_t last_pong_time = 0;
		time_t now = time(NULL);
		lwsl_user("RECEIVE_PONG (gap=%lds)\n",
			last_pong_time ? (long)(now - last_pong_time) : 0L);
		last_pong_time = now;
		lws_validity_confirmed(wsi);
		break;
	}

	case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
	{
		unsigned char **p = (unsigned char **)in, *end = (*p) + len;
		if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_AUTHORIZATION,
				(unsigned char *)auth_token, strlen(auth_token), p, end))
			return -1;
		/* Send VXG_PROXY_STICKY cookie on the WebSocket upgrade so the LB can
		 * route /device-ws to the same proxy that issued the reservation. */
		if (proxy_sticky_cookie[0]) {
			char cookie_hdr[300];
			int cookie_len = snprintf(cookie_hdr, sizeof(cookie_hdr),
			                         "VXG_PROXY_STICKY=%s", proxy_sticky_cookie);
			if (lws_add_http_header_by_name(wsi,
					(const unsigned char *)"Cookie:",
					(unsigned char *)cookie_hdr, cookie_len, p, end))
				return -1;
		}
		break;
	}

	case LWS_CALLBACK_CLIENT_RECEIVE:
	{
		/* Any frame from server is proof of validity */
		lws_validity_confirmed(wsi);

		lwsl_debug("websocket RCV (len=%d):\n", (int)len);
		lwsl_hexdump_debug(in, len);
#ifdef PERF_LOG
		perf_ws_rx_bytes += len;
		perf_ws_rx_msgs++;
#endif
		if (!lws_frame_is_binary(wsi) || len < 1)
		{
			// lwsl_user("websocket RCV (len=%d):\n", (int)len);
			// lwsl_hexdump_err(in, len);
			break;
		}
#if VXG_ENABLE_BW_PROBE
		/*
		 * Probe-only receive path for large downlink W messages.
		 *
		 * libwebsockets can split one large WebSocket frame across multiple
		 * LWS_CALLBACK_CLIENT_RECEIVE callbacks according to its rx buffer.
		 * Do not feed W padding into the normal command reassembly path:
		 * remember only the fixed header, consume subsequent callbacks, and
		 * ACK only when the complete frame payload has arrived.
		 */
		if (bw_down_probe_rx_in_progress ||
		    (((unsigned char *)in)[0] == 'W' && len >= 25))
		{
			if (!bw_down_probe_rx_in_progress)
			{
				memcpy(bw_down_probe_header, in, sizeof(bw_down_probe_header));
				bw_down_probe_rx_in_progress = 1;
			}

			if (lws_is_final_fragment(wsi) &&
			    lws_remaining_packet_payload(wsi) == 0)
			{
				unsigned char ack_frame[LWS_PRE + 25];
				double probe_ts;

				memset(ack_frame, 0, sizeof(ack_frame));
				ack_frame[LWS_PRE] = 'X';
				memcpy(ack_frame + LWS_PRE + 1,
				       bw_down_probe_header + 1, 16);
				memcpy(&probe_ts, bw_down_probe_header + 17,
				       sizeof(probe_ts));
				(void)probe_ts;
				encode_be_double((double)time(NULL),
				                 ack_frame + LWS_PRE + 17);

				bytes_written = lws_write(
					wsi, ack_frame + LWS_PRE, 25, LWS_WRITE_BINARY);
				if (bytes_written < 25)
					lwsl_warn("bandwidth probe ack write incomplete: %d\n",
						  bytes_written);
				else
					lwsl_debug("sent bw_probe ack after full W payload\n");

				bw_down_probe_rx_in_progress = 0;
			}

			/* W is a control/probe message; never pass its padding through
			 * normal O/C/P/R/D reassembly or forwarding for accurte bandwidth test. */
			break;
		}
#endif
		if (lws_is_first_fragment(wsi) && lws_is_final_fragment(wsi))
		{
			if (websocket_rcv_buffer)
				free(websocket_rcv_buffer);
			websocket_rcv_buffer = (char *)in;
			websocket_rcv_buffer_len = len;
		}
		else if (lws_is_first_fragment(wsi))
		{
			if (websocket_rcv_buffer)
				free(websocket_rcv_buffer);
			websocket_rcv_buffer = (char *)malloc(len);
			memcpy(websocket_rcv_buffer, in, len);
			websocket_rcv_buffer_len = len;
			break;
		}
		else
		{
			char *tmp_buf = (char *)realloc(websocket_rcv_buffer, websocket_rcv_buffer_len+len);
			if (!tmp_buf)
			{
				tmp_buf = (char *)malloc(websocket_rcv_buffer_len+len);
				if (!tmp_buf) {
					free(websocket_rcv_buffer);
					websocket_rcv_buffer = NULL;
					websocket_rcv_buffer_len = 0;
					lwsl_err("%s: OOM during fragment reassembly\n", __func__);
					break;
				}
				memcpy(tmp_buf, websocket_rcv_buffer, websocket_rcv_buffer_len);
				free(websocket_rcv_buffer);
				websocket_rcv_buffer = tmp_buf;
			}
			else
				websocket_rcv_buffer = tmp_buf;
			memcpy(websocket_rcv_buffer+websocket_rcv_buffer_len, in, len);
			websocket_rcv_buffer_len += len;
			if (!lws_is_final_fragment(wsi))
				break;
		}
		cmd = *websocket_rcv_buffer;
#if VXG_ENABLE_BW_PROBE
		if (cmd == 'U' && websocket_rcv_buffer_len >= 29)
		{
			/* Upload-probe request: proxy asks us to send back `requested_size`
			 * padding bytes so it can measure agent->proxy (upload) capacity.
			 * Clamp to VXG_BW_UPLOAD_PROBE_MAX_BYTES regardless of what was
			 * requested, so a bad/compromised proxy can't force a huge alloc. */
			uint32_t requested_size = decode_be_u32(
				(unsigned char *)websocket_rcv_buffer + 25);
			uint32_t reply_size = requested_size > VXG_BW_UPLOAD_PROBE_MAX_BYTES
				? VXG_BW_UPLOAD_PROBE_MAX_BYTES : requested_size;
			unsigned char *reply_frame = (unsigned char *)malloc(LWS_PRE + 25 + reply_size);
			if (!reply_frame)
			{
				lwsl_err("%s: OOM building upload-probe reply (%u bytes)\n",
					 __func__, reply_size);
			}
			else
			{
				memset(reply_frame + LWS_PRE, 0, 25 + reply_size);
				reply_frame[LWS_PRE] = 'V';
				memcpy(reply_frame + LWS_PRE + 1, websocket_rcv_buffer + 1, 16);
				encode_be_double((double)time(NULL), reply_frame + LWS_PRE + 17);
				bytes_written = lws_write(wsi, reply_frame + LWS_PRE, 25 + reply_size,
							   LWS_WRITE_BINARY);
				if (bytes_written < (int)(25 + reply_size))
					lwsl_warn("upload-probe reply write incomplete: %d/%u\n",
						  bytes_written, 25 + reply_size);
				else
					lwsl_debug("sent upload-probe reply size=%u\n", reply_size);
				free(reply_frame);
			}
		}
		else
#endif
		if (cmd == 'O' && websocket_rcv_buffer_len == 1+sizeof(client_id_t)+1)
		{
			client_id = *((client_id_t *)(websocket_rcv_buffer+1));
			forward_index = *((uint8_t *)(websocket_rcv_buffer+1+sizeof(client_id_t)));
			if (forward_index > MAX_FORWARD_ITEMS-1 || forward_items[forward_index].proto == PROTO_NONE)
			{
				lwsl_err("Received invalid forward_index=%d\n", forward_index);
				lwsl_hexdump_notice(websocket_rcv_buffer, websocket_rcv_buffer_len);
			}
			lwsl_debug("forward_index=%d, client_id=%d\n", forward_index, client_id);

			// search for client slot available
			for (prev_client=m->first_client; prev_client && prev_client->next_client; prev_client = prev_client->next_client);
			// create new client
			client = malloc(sizeof(struct proxy_conn));
			if (!client)
			{
				lwsl_err("%s:%d: malloc() error\n", __func__, __LINE__);
				return -1;
			}
			memset(client, 0, sizeof(struct proxy_conn));
#ifdef PERF_LOG
			perf_client_count++;
#endif
			if (prev_client)
			{
				prev_client->next_client = client;
				client->prev_client = prev_client;
			}
			else
				m->first_client = client;

			client->client_id = client_id;
			client->forward_index = forward_index;
			client->priority = forward_items[forward_index].realtime
			                 ? PRIORITY_REALTIME : PRIORITY_BULK;
			if (client->priority == PRIORITY_REALTIME)
				m->realtime_client_count++;
			else
				m->bulk_client_count++;
#ifdef PERF_LOG
			perf_rt_clients = m->realtime_client_count;
			perf_bulk_clients = m->bulk_client_count;
#endif
			client->ring = lws_ring_create(sizeof(struct msg), RING_DEPTH, destroy_message);
			if (!client->ring)
			{
				lwsl_err("lws_ring_create() error\n");
				destroy_proxy_client(client);
				break;
			}
			client->tail = 0;

			memset(&client_conn_info, 0, sizeof(client_conn_info));
			client_conn_info.method = "RAW";
			client_conn_info.context = context;
			client_conn_info.port = forward_items[forward_index].port;
			client_conn_info.address = forward_items[forward_index].host;
			client_conn_info.ssl_connection = 0;
			client_conn_info.local_protocol_name = "vxg-proxy";
			client_conn_info.pwsi = &client->wsi_raw;
			client_conn_info.userdata = client;

			lwsl_debug("New connection open request (client_id=%d) to %s:%d\n", client_id, forward_items[forward_index].host, forward_items[forward_index].port);

			if (!lws_client_connect_via_info(&client_conn_info)) {
				lwsl_err("lws_client_connect_via_info() failed\n");
				destroy_proxy_client(client);
				break;
			}
		}
		else if (cmd == 'C' && websocket_rcv_buffer_len == 1+sizeof(client_id_t))
		{
			client_id = *((client_id_t *)(websocket_rcv_buffer+1));
			lwsl_debug("Connection close request (client_id=%d)\n", client_id);
			// search for client to disconnect
			for (client=m->first_client; client; client = client->next_client) {
				if (client->client_id == client_id) {
					client->close_notification_sent = 1;
					lws_set_timeout(client->wsi_raw, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, LWS_TO_KILL_SYNC);
					break;
				}
			}
		}
		else if (cmd == 'P' && websocket_rcv_buffer_len == 1+sizeof(client_id_t))
		{
			/* Per-client PAUSE: proxy says this client's fwd_queue is too large,
			 * stop reading from the backend for this client_id. */
			client_id = *((client_id_t *)(websocket_rcv_buffer+1));
			lwsl_user("PAUSE rx for client_id=%d (proxy backpressure)\n", client_id);
			for (client=m->first_client; client; client = client->next_client) {
				if (client->client_id == client_id && client->wsi_raw) {
					client->proxy_paused = 1;
					if (!client->flow_controlled) {
						client->flow_controlled = 1;
						lws_rx_flow_control(client->wsi_raw, 0);
					}
					break;
				}
			}
		}
		else if (cmd == 'R' && websocket_rcv_buffer_len == 1+sizeof(client_id_t))
		{
			/* Per-client RESUME: proxy says this client's fwd_queue has drained
			 * below the low watermark, resume reading. */
			client_id = *((client_id_t *)(websocket_rcv_buffer+1));
			lwsl_user("RESUME rx for client_id=%d (proxy backpressure lifted)\n", client_id);
			for (client=m->first_client; client; client = client->next_client) {
				if (client->client_id == client_id && client->wsi_raw) {
					client->proxy_paused = 0;
					if (client->flow_controlled) {
						/* Only resume if ring conditions allow it.
						 * Use priority-aware thresholds only when
						 * realtime clients are active; otherwise
						 * everyone uses normal thresholds. */
						int used = RING_DEPTH - (int)lws_ring_get_count_free_elements(m->ring);
						int mixed = (m->realtime_client_count > 0 &&
						             m->bulk_client_count > 0);
						int ok_depth = (client->priority == PRIORITY_BULK && mixed)
						             ? RING_DEPTH_OK_BULK : RING_DEPTH_OK;
						uint64_t ok_bytes = (client->priority == PRIORITY_BULK && mixed)
						                  ? WS_RING_BYTES_OK_BULK : WS_RING_BYTES_OK;
						if (used <= ok_depth && m->ring_bytes <= ok_bytes) {
							client->flow_controlled = 0;
							lws_rx_flow_control(client->wsi_raw, 1);
						}
					}
					break;
				}
			}
		}
		else if (cmd == 'D' && websocket_rcv_buffer_len > 1+sizeof(client_id_t))
		{
			client_id = *((client_id_t *)(websocket_rcv_buffer+1));
			lwsl_debug("Data forwarding request (client_id=%d, len=%d)\n", client_id, (int)websocket_rcv_buffer_len);
			// search for client
			for (client=m->first_client; client; client = client->next_client) {
				if (client->client_id == client_id && client->ring && client->wsi_raw) {
					n = (int)lws_ring_get_count_free_elements(client->ring);
					if (!n) {
						lwsl_err("LWS_CALLBACK_CLIENT_RECEIVE: lws_ring_insert() - ring is full!\n");
						lws_set_timeout(client->wsi_raw, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, LWS_TO_KILL_SYNC);
						break;
					}

					amsg.len = websocket_rcv_buffer_len-(1+sizeof(client_id_t));
					amsg.payload = malloc(LWS_PRE + amsg.len);
					if (!amsg.payload)
					{
						lwsl_err("%s:%d: malloc() error\n", __func__, __LINE__);
						return -1;
					}
					memcpy(amsg.payload+LWS_PRE, websocket_rcv_buffer+(1+sizeof(client_id_t)), amsg.len);
					if (!lws_ring_insert(client->ring, &amsg, 1)) {
						destroy_message(&amsg);
						lwsl_err("LWS_CALLBACK_CLIENT_RECEIVE: lws_ring_insert() error\n");
						lws_set_timeout(client->wsi_raw, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, LWS_TO_KILL_SYNC);
					}
					m->total_msgs_in_client_rings++;
					lws_callback_on_writable(client->wsi_raw);

					if (!m->flow_controlled && m->total_msgs_in_client_rings >= RING_DEPTH_CRITICAL) {
						m->flow_controlled = 1;
#ifdef PERF_LOG
						perf_block_events++;
#endif
						lwsl_user("websocket rx blocked\n");
						lws_rx_flow_control(wsi, 0);
					}
					break;
				}
			}
		}
		if (websocket_rcv_buffer == (char *)in)
		{
			websocket_rcv_buffer = NULL;
			websocket_rcv_buffer_len = 0;
		}
		if (websocket_rcv_buffer && lws_is_final_fragment(wsi))
		{
			free(websocket_rcv_buffer);
			websocket_rcv_buffer = NULL;
			websocket_rcv_buffer_len = 0;
		}
		break;
	}

	case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
	{
		int http_status = lws_http_client_http_response(wsi);
		lwsl_user("Got HTTP %d response from websocket endpoint\n", http_status);

		if (http_status == 403)
		{
			lwsl_err("Access token is invalid.");
			restart = 1;
			force_exit = 1;
		}
		else if (http_status == 508)
		{
			/* 508 = Loop Detected = Stale session on proxy server
			 * This occurs when the device tries to reconnect but a previous session
			 * entry still exists in the proxy's device_forwards dictionary.
			 * Force full reconnection with fresh Route53 lookup to get a clean proxy.
			 */
			lwsl_warn("HTTP 508 Loop Detected - stale proxy session detected. "
					  "Clearing connection state and reconnecting...\n");
			
			/* Reset websocket ring to discard any pending messages */
			if (m->ring) {
				lws_ring_destroy(m->ring);
				m->ring = NULL;
				m->total_msgs_in_client_rings = 0;
				m->ring_bytes = 0;
				lwsl_info("Cleared websocket ring due to 508 error\n");
			}
			
			/* Kill all outbound client connections to clean state */
			for (client = m->first_client; client; client = client->next_client) {
				if (client->wsi_raw) {
					lwsl_debug("Closing stale client connection (client_id=%d) due to 508\n",
							   client->client_id);
					lws_set_timeout(client->wsi_raw, 
								  PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, 
								  LWS_TO_KILL_ASYNC);
				}
			}
			
			/* Trigger reconnection with exponential backoff via retry mechanism */
			restart = 1;  /* Signal to user code that reconnection occurred */
			goto do_retry;  /* Uses the ws_retry backoff policy for exponential delays */
		}
		else if (http_status == 503)
		{
			/* 503 = No reservation found on proxy (proxy restart / RA sync window).
			 * This is a transient condition — retry with backoff until the proxy
			 * re-populates the device reservation from RA.
			 */
			lwsl_warn("HTTP 503 Service Unavailable - no reservation on proxy yet. "
					  "Retrying with backoff...\n");
			restart = 1;
			goto do_retry;
		}
		else if (http_status >= 500 && http_status < 600)
		{
			/* Other 5xx errors - retry with backoff */
			lwsl_warn("Got HTTP %d error from proxy. Retrying with backoff...\n", http_status);
			goto do_retry;
		}
		break;
	}

	case LWS_CALLBACK_CLIENT_ESTABLISHED:
	{
		lwsl_user("%s: established\n", __func__);
		vxg_tls_setup_fail_reset();

		/* Reset validity as soon as we’re up */
		lws_validity_confirmed(wsi);

		/* Disable Nagle on the WS transport for low-latency forwarding */
		{
			int fd = lws_get_socket_fd(wsi);
			if (fd >= 0) {
				int flag = 1;
				setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
			}
		}

		if (strlen(status_file) > 0) {
			lwsl_user("Writing to status file: %s\n", status_file);
			FILE *sf = fopen(status_file, "w");
			if (sf) {
				fprintf(sf, "connected\n");
				fclose(sf);
			}
		}


#ifdef PERF_LOG
		perf_raw_rx_bytes = perf_ws_rx_bytes = 0;
		perf_raw_rx_msgs = perf_ws_rx_msgs = 0;
		perf_ring_bytes = 0;
		lws_sul_schedule(context, 0, &perf_sul, perf_timer_cb, PERF_INTERVAL_SEC * LWS_USEC_PER_SEC);
#endif

		m->ring = lws_ring_create(sizeof(struct msg), RING_DEPTH, destroy_message);
		m->total_msgs_in_client_rings = 0;
		m->ring_bytes = 0;
		if (!m->ring)
			return 1;
		m->tail = 0;

		// send forwardings info
		cJSON *auth_json = cJSON_CreateObject();
		cJSON_AddNumberToObject(auth_json, "websocket_buffer_size", WEBSOCKET_BUFFER_SIZE);
		cJSON_AddStringToObject(auth_json, "agent_version", AGENT_VERSION);
		cJSON *capabilities = cJSON_AddObjectToObject(auth_json, "capabilities");
#if VXG_ENABLE_BW_PROBE
		cJSON_AddTrueToObject(capabilities, "bw_probe");
#else
		cJSON_AddFalseToObject(capabilities, "bw_probe");
#endif
		cJSON *j_forwards = cJSON_AddArrayToObject(auth_json, "forwards");
		
		for (n=0; n < MAX_FORWARD_ITEMS; n++)
		{
			if (forward_items[n].proto == PROTO_NONE)
				break;
			cJSON *j_forward = cJSON_CreateObject();
			cJSON_AddItemToArray(j_forwards, j_forward);
			cJSON_AddStringToObject(j_forward, "name", forward_items[n].name);
			cJSON_AddStringToObject(j_forward, "protocol", forward_items[n].proto == PROTO_HTTP ? "http" : (forward_items[n].proto == PROTO_HTTPS ? "https" : "tcp"));
			cJSON_AddStringToObject(j_forward, "host", forward_items[n].host);
			cJSON_AddNumberToObject(j_forward, "port", forward_items[n].port);
			cJSON_AddBoolToObject(j_forward, "realtime", forward_items[n].realtime);
		}
		char *auth_buffer = cJSON_PrintUnformatted(auth_json);
		cJSON_Delete(auth_json);
		amsg.len = strlen(auth_buffer);
		amsg.payload = malloc(LWS_PRE + amsg.len);
		if (!amsg.payload) {
			cJSON_free(auth_buffer);
			lwsl_user("OOM: dropping\n");
			break;
		}
		memcpy((char *)amsg.payload + LWS_PRE, auth_buffer, amsg.len);
		cJSON_free(auth_buffer);
		bytes_written = lws_write(wsi, ((unsigned char *)amsg.payload) +
			      LWS_PRE, amsg.len, (enum lws_write_protocol)LWS_WRITE_TEXT);
		free(amsg.payload);
		if (bytes_written < (int)amsg.len) {
			lwsl_err("ERROR %d writing to ws socket\n", bytes_written);
			return -1;
		}
		/* Successfully sent auth JSON -> connection is definitely alive */
		lws_validity_confirmed(wsi);
		break;
	}

	case LWS_CALLBACK_CLIENT_WRITEABLE:
	{
		lwsl_debug("%s: writable\n", __func__);

		/* Batch drain loop: write as many messages as the socket can accept */
		while (1) {
			pmsg = lws_ring_get_element(m->ring, &m->tail);
			if (!pmsg)
				break;

			lwsl_debug("websocket SND (len=%d):\n", (int)pmsg->len);
			lwsl_hexdump_debug(((unsigned char *)pmsg->payload) + LWS_PRE, pmsg->len);

			bytes_written = lws_write(wsi, ((unsigned char *)pmsg->payload) +
				      LWS_PRE, pmsg->len, (enum lws_write_protocol)LWS_WRITE_BINARY);
			if (bytes_written < (int)pmsg->len) {
				lwsl_err("ERROR %d writing to ws socket\n", bytes_written);
				return -1;
			}

			/* Immediate consume: subtract bytes then free the ring slot */
			if (m->ring_bytes >= pmsg->len)
				m->ring_bytes -= pmsg->len;
			lws_ring_consume_single_tail(m->ring, &m->tail, 1);

#ifdef PERF_LOG
			perf_ring_bytes = m->ring_bytes;
#endif

			/* Check if we should unblock flow-controlled clients.
			 * Skip clients with proxy_paused — only the proxy R command
			 * can lift that; ring drain must not override it.
			 *
			 * Priority-aware resume (only under mixed traffic):
			 *   When BOTH realtime AND bulk clients are connected:
			 *     1. At RING_DEPTH_OK / WS_RING_BYTES_OK  → resume realtime (TCP) only
			 *     2. At RING_DEPTH_OK_BULK / WS_RING_BYTES_OK_BULK → also resume bulk (HTTP)
			 *   When all clients are the same type (all RTSP or all HTTP),
			 *   everyone resumes at the normal threshold — no penalty. */
			{
				int free_elems = (int)lws_ring_get_count_free_elements(m->ring);
				int used_elems = RING_DEPTH - free_elems;

				if (used_elems <= RING_DEPTH_OK && m->ring_bytes <= WS_RING_BYTES_OK) {
					struct proxy_conn *client;
					int mixed = (m->realtime_client_count > 0 &&
					             m->bulk_client_count > 0);
					if (mixed) {
						/* Two-pass: realtime first, bulk only after deeper drain */
						for (client = m->first_client; client; client = client->next_client) {
							if (client->flow_controlled && !client->proxy_paused &&
							    client->priority == PRIORITY_REALTIME) {
							#ifdef PERF_LOG
								perf_unblock_events++;
								perf_unblock_rt++;
							#endif
								lws_rx_flow_control(client->wsi_raw, 1);
								client->flow_controlled = 0;
							}
						}
						if (used_elems <= RING_DEPTH_OK_BULK &&
						    m->ring_bytes <= WS_RING_BYTES_OK_BULK) {
							for (client = m->first_client; client; client = client->next_client) {
								if (client->flow_controlled && !client->proxy_paused &&
								    client->priority == PRIORITY_BULK) {
								#ifdef PERF_LOG
									perf_unblock_events++;
									perf_unblock_bulk++;
								#endif
									lws_rx_flow_control(client->wsi_raw, 1);
									client->flow_controlled = 0;
								}
							}
						}
					} else {
						/* All same type: resume everyone at normal threshold */
						for (client = m->first_client; client; client = client->next_client) {
							if (client->flow_controlled && !client->proxy_paused) {
							#ifdef PERF_LOG
								perf_unblock_events++;
								if (client->priority == PRIORITY_REALTIME)
									perf_unblock_rt++;
								else
									perf_unblock_bulk++;
							#endif
								lws_rx_flow_control(client->wsi_raw, 1);
								client->flow_controlled = 0;
							}
						}
					}
				}
			}

			/* Stop if the socket write buffer is full */
			if (lws_send_pipe_choked(wsi))
				break;
		}

		// The call at vxg_proxy_client.c:1190 is architecturally wrong. 
		// Sending data proves your side is alive, not the peer's. 
		// Validity should only be confirmed by receiving data from the peer.
		// lws_validity_confirmed(wsi);

		/* If more data remains, request another writable callback */
		if (lws_ring_get_element(m->ring, &m->tail))
			lws_callback_on_writable(wsi);
		break;
	}

	case LWS_CALLBACK_CLIENT_CLOSED:
	{
#if VXG_ENABLE_BW_PROBE
		bw_down_probe_rx_in_progress = 0;
#endif
		lwsl_user("%s: closed\n", __func__);
		// update status file to "disconnected"
		if (strlen(status_file) > 0) {
			lwsl_user("Writing to status file: %s\n", status_file);
			FILE *sf = fopen(status_file, "w");
			if (sf) {
				fprintf(sf, "disconnected\n");
				fclose(sf);
			}
		}

#ifdef PERF_LOG
		lws_sul_cancel(&perf_sul);
		perf_ring_bytes = 0;
#endif
		if (m->ring) {
			lws_ring_destroy(m->ring);
			m->total_msgs_in_client_rings = 0;
			m->ring_bytes = 0;
			m->ring = NULL;
		}
		for (client=m->first_client; client; client = client->next_client) {
			lws_set_timeout(client->wsi_raw, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, LWS_TO_KILL_ASYNC);
		}
		goto do_retry;
	}

	default:
		break;
	}

	return 0;
	// return lws_callback_http_dummy(wsi, reason, user, in, len);

do_retry:
	if (strlen(device_serial) == 0 && strlen(vxg_api_password) == 0) {
		if (!force_exit && lws_retry_sul_schedule_retry_wsi(wsi, &m->sul, proxy_api_connect, &m->retry_count)) {
			lwsl_err("%s: connection attempts exhausted\n", __func__);
			force_exit = 1;
		}
	} else {
		if (!force_exit && lws_retry_sul_schedule_retry_wsi(wsi, &m->sul, vxg_token_api_connect, &m->retry_count)) {
			lwsl_err("%s: connection attempts exhausted\n", __func__);
			force_exit = 1;
		}
	}
	return 0;
}


// ----------------------------------------------------------------------------
static int vxg_token_api_callback(struct lws *wsi, enum lws_callback_reasons reason,
              void *user, void *in, size_t len)
{
	struct my_conn *m = (struct my_conn *)user;

    switch (reason) {

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		{
			lwsl_err("%s: CLIENT_CONNECTION_ERROR: %s\n",
						in ? (char *)in : "(null)", __func__);
			if (vxg_handle_tls_fatal_conn_error(in ? (char *)in : NULL))
				break;
			goto do_retry_vxg_token_api;
			break;
		}

        case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
		{
			vxg_token_api_request_status = lws_http_client_http_response(wsi);
			memset(auth_res_json_buffer, 0, sizeof(auth_res_json_buffer));
			lwsl_user("Got HTTP %d response from auth token service\n", vxg_token_api_request_status);
			if (vxg_token_api_request_status == 200)
				vxg_tls_setup_fail_reset();
			if (vxg_token_api_request_status >= 400 && vxg_token_api_request_status < 500)
			{
				if (vxg_token_api_request_status == 401)
				{
					lwsl_err("Access token not found on provisioning server\n");
				} else {
					/* Non-401 4xx: keep outer destroy + sleep path (restart set on close). */
					restart = 1;
				}
			}
			else if (vxg_token_api_request_status != 200)
				goto do_retry_vxg_token_api;
			break;
		}

		case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
		{
			unsigned char **p = (unsigned char **)in, *end = (*p) + len;
			if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_AUTHORIZATION,
					(unsigned char *)vxg_api_password, strlen(vxg_api_password), p, end))
				return -1;
			break;
		}

        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ:
		{
			if (vxg_token_api_request_status == 200 && len+strlen(auth_res_json_buffer) < sizeof(auth_res_json_buffer)-1)
				memcpy(auth_res_json_buffer+strlen(auth_res_json_buffer), in, len);
			return 0;
		}
        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP:
		{
			char buffer[1024 + LWS_PRE];
			char *px = buffer + LWS_PRE;
			int lenx = sizeof(buffer) - LWS_PRE;

			if (lws_http_client_read(wsi, &px, &lenx) < 0)
					return -1;
			return 0;
		}

        case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
		{
			/* HTTP 401: retry in-process after delay. Destroying the context and recreating
			 * it was unreliable on Axis (SEGV on second lws_create_context). */
			if (vxg_token_api_request_status == 401)
			{
				vxg_token_401_attempts++;
				if (vxg_token_401_attempts > VXG_TOKEN_401_MAX_ATTEMPTS)
				{
					lwsl_err("Maximum auth token provisioning retries (%u) exceeded.\n",
						 (unsigned)VXG_TOKEN_401_MAX_ATTEMPTS);
					force_exit = 1;
					if (context)
						lws_cancel_service(context);
					break;
				}
				vxg_token_api_conn.retry_count = 0;
				{
					unsigned int delay_sec =
						vxg_token_401_backoff_delay_sec(vxg_token_401_attempts);

					lwsl_user("Scheduling auth token retry in %u s (attempt %u/%u; "
						  "first %u @ %us, then exp from %us cap %us)\n",
						  delay_sec,
						  vxg_token_401_attempts,
						  (unsigned)VXG_TOKEN_401_MAX_ATTEMPTS,
						  (unsigned)VXG_TOKEN_401_LINEAR_ATTEMPTS,
						  (unsigned)VXG_TOKEN_401_LINEAR_DELAY_SEC,
						  (unsigned)VXG_TOKEN_401_BACKOFF_BASE_SEC,
						  (unsigned)VXG_TOKEN_401_BACKOFF_CAP_SEC);
					lws_sul_schedule(context, 0, &vxg_token_api_conn.sul,
							 vxg_token_api_connect,
							 (lws_usec_t)((unsigned long long)delay_sec *
								       (unsigned long long)LWS_USEC_PER_SEC));
				}
				break;
			}
			if (vxg_token_api_request_status >= 400 && vxg_token_api_request_status < 500)
			{
				force_exit = 1;
				if (context)
					lws_cancel_service(context);
				break;
			}
			if (vxg_token_api_request_status == 200)
			{
				vxg_token_401_attempts = 0;
				cJSON *res_json = cJSON_Parse(auth_res_json_buffer);
				if (!res_json)
				{
					lwsl_err("Unable to parse JSON response: %s\n", auth_res_json_buffer);
					goto do_retry_vxg_token_api;
					break;
				}
				cJSON *j = cJSON_GetObjectItem(res_json, "access_token");
				if (!j || j->type != cJSON_String)
				{
					cJSON_Delete(res_json);
					lwsl_err("Unable to find access_token in JSON response: %s\n", auth_res_json_buffer);
					goto do_retry_vxg_token_api;
					break;
				}
				strncpy(auth_token, j->valuestring, sizeof(auth_token)-1);
				lwsl_debug("access_token: %s\n", auth_token);
				lwsl_user("Successfully received auth token\n");
				cJSON_Delete(res_json);

				lws_sul_schedule(context, 0, &proxy_api_conn.sul, proxy_api_connect, 1);
			}
			break;
		}

        default:
                break;
    }

	return 0;

do_retry_vxg_token_api:
	if (!force_exit && lws_retry_sul_schedule_retry_wsi(wsi, &m->sul, vxg_token_api_connect, &m->retry_count)) {
		lwsl_err("%s: connection attempts exhausted\n", __func__);
		force_exit = 1;
	}
	return 0;
}


// ----------------------------------------------------------------------------
static void proxy_api_connect(lws_sorted_usec_list_t *sul)
{
	struct my_conn *m = lws_container_of(sul, struct my_conn, sul);
	struct lws_client_connect_info i;
	char json_buffer[1024*10];

	memset(&i, 0, sizeof(i));

	lws_b64_decode_string(auth_token, json_buffer, sizeof(json_buffer));
//	printf("Decoded token: %s\n", auth_res_json_buffer);
	cJSON *token_json = cJSON_Parse(json_buffer);
	if (!token_json)
	{
		lwsl_err("Invalid access token construction, unable to parse access token JSON: %s\n", json_buffer);
        restart = 1;
		force_exit = 1;
		return;
	}
    
    // Check camid field
    cJSON *j = cJSON_GetObjectItem(token_json, "camid");
	if (!j || j->type != cJSON_Number)
	{
		cJSON_Delete(token_json);
		lwsl_err("Unable to find camid in access token: %s\n", json_buffer);
        restart = 1;
		force_exit = 1;
		return;
	}

    // Check cmngrid field
    j = cJSON_GetObjectItem(token_json, "cmngrid");
	if (!j || j->type != cJSON_Number)
	{
		cJSON_Delete(token_json);
		lwsl_err("Unable to find cmngrid in access token: %s\n", json_buffer);
        restart = 1;
		force_exit = 1;
		return;
	}

    // Check uplink field
	j = cJSON_GetObjectItem(token_json, "uplink");
	if (!j || j->type != cJSON_String)
	{
		cJSON_Delete(token_json);
		lwsl_err("Unable to find proxy address in access token: %s\n", json_buffer);
        restart = 1;
		force_exit = 1;
		return;
	}
	if (strlen(j->valuestring) >= sizeof(proxy_api_host))
	{
		cJSON_Delete(token_json);
		lwsl_err("Proxy address in access token is too long\n");
		restart = 1;
		force_exit = 1;
		return;
	}
	copy_bounded(proxy_api_host, sizeof(proxy_api_host), j->valuestring);
	cJSON_Delete(token_json);

	i.context = context;
	i.port = port;
	i.address = proxy_api_host;
	i.path = proxy_api_path;
	i.host = i.address;
	i.origin = i.address;
	i.ssl_connection = ssl_connection;
    i.method = "GET";
	i.protocol = "vxg-proxy-api";
	i.pwsi = &m->wsi;
	i.retry_and_idle_policy = &proxy_api_retry;
	i.userdata = m;

	lwsl_user("Requesting websocket endpoint from %s:%d\n", i.host, i.port);
	if (!lws_client_connect_via_info(&i))
		if (lws_retry_sul_schedule(context, 0, sul, &proxy_api_retry,
					   proxy_api_connect, &m->retry_count)) {
			lwsl_err("%s: connection attempts exhausted\n", __func__);
			force_exit = 1;
		}
}

int proxy_api_request_status = 0;
char proxy_api_res_json_buffer[1024*10];

// ----------------------------------------------------------------------------
static int proxy_api_callback(struct lws *wsi, enum lws_callback_reasons reason,
              void *user, void *in, size_t len)
{
	struct my_conn *m = (struct my_conn *)user;

    switch (reason) {

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		{
			lwsl_err("%s: CLIENT_CONNECTION_ERROR: %s\n",
						in ? (char *)in : "(null)", __func__);
			if (vxg_handle_tls_fatal_conn_error(in ? (char *)in : NULL))
				break;
			goto do_retry_proxy_api;
			break;
		}

        case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
		{
			proxy_api_request_status = lws_http_client_http_response(wsi);
			memset(proxy_api_res_json_buffer, 0, sizeof(proxy_api_res_json_buffer));
			lwsl_user("Got HTTP %d response from proxy api websocket endpoint\n", proxy_api_request_status);

			if (proxy_api_request_status == 200)
				vxg_tls_setup_fail_reset();
			if (proxy_api_request_status >= 400 && proxy_api_request_status < 500)
			{
				if (proxy_api_request_status == 403)
				{
					lwsl_err("Access token is invalid.");
					restart = 1;
				}
					force_exit = 1;
			}
			else if (proxy_api_request_status != 200)
				goto do_retry_proxy_api;
			break;
		}

		case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
		{
			unsigned char **p = (unsigned char **)in, *end = (*p) + len;
			if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_AUTHORIZATION,
					(unsigned char *)auth_token, strlen(auth_token), p, end))
				return -1;
			/* Send the stored VXG_PROXY_STICKY cookie to pin this camera to the
			 * proxy that owns its session. Use lws_add_http_header_by_name
			 * because WSI_TOKEN_HTTP_COOKIE is not used for client-side headers. */
			if (proxy_sticky_cookie[0]) {
				char cookie_hdr[300];
				int cookie_len = snprintf(cookie_hdr, sizeof(cookie_hdr),
				                         "VXG_PROXY_STICKY=%s", proxy_sticky_cookie);
				if (lws_add_http_header_by_name(wsi,
						(const unsigned char *)"Cookie:",
						(unsigned char *)cookie_hdr, cookie_len, p, end))
					return -1;
			}
			break;
		}

        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ:
		{
			if (proxy_api_request_status == 200 && len+strlen(proxy_api_res_json_buffer) < sizeof(proxy_api_res_json_buffer)-1)
				memcpy(proxy_api_res_json_buffer+strlen(proxy_api_res_json_buffer), in, len);
			return 0;
		}
        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP:
		{
			char buffer[1024 + LWS_PRE];
			char *px = buffer + LWS_PRE;
			int lenx = sizeof(buffer) - LWS_PRE;

			if (lws_http_client_read(wsi, &px, &lenx) < 0)
					return -1;
			return 0;
		}

        case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
		{
			if (proxy_api_request_status == 200)
			{
				cJSON *res_json = cJSON_Parse(proxy_api_res_json_buffer);
				if (!res_json)
				{
					lwsl_err("Unable to parse JSON response: %s\n", proxy_api_res_json_buffer);
					goto do_retry_proxy_api;
					break;
				}
				cJSON *j = cJSON_GetObjectItem(res_json, "domain");
				if (!j || j->type != cJSON_String)
				{
					cJSON_Delete(res_json);
					lwsl_err("Unable to find domain in JSON response: %s\n", proxy_api_res_json_buffer);
					goto do_retry_proxy_api;
					break;
				}
				strncpy(proxy_ws_host, j->valuestring, sizeof(proxy_ws_host)-1);
				j = cJSON_GetObjectItem(res_json, "path");
				if (!j || j->type != cJSON_String)
				{
					cJSON_Delete(res_json);
					lwsl_err("Unable to find path in JSON response: %s\n", proxy_api_res_json_buffer);
					goto do_retry_proxy_api;
					break;
				}
				strncpy(proxy_ws_path, j->valuestring, sizeof(proxy_ws_path)-1);
				/* Read proxy_id from the JSON body and reuse it as the sticky cookie
				 * on later requests so the ALB can keep this camera on the same proxy. */
				j = cJSON_GetObjectItem(res_json, "proxy_id");
				if (j && j->type == cJSON_String && j->valuestring[0]) {
					strncpy(proxy_sticky_cookie, j->valuestring,
					        sizeof(proxy_sticky_cookie) - 1);
					proxy_sticky_cookie[sizeof(proxy_sticky_cookie) - 1] = '\0';
					lwsl_user("Stored sticky cookie VXG_PROXY_STICKY=%s\n",
					          proxy_sticky_cookie);
				}
				lwsl_debug("proxy_ws_host: %s\n", proxy_ws_host);
				lwsl_debug("ws_path: %s\n", proxy_ws_path);
				lwsl_user("Successfully received ws endpoint\n");
				cJSON_Delete(res_json);

				lws_sul_schedule(context, 0, &ws_conn.sul, websocket_connect, 1);
			}
			break;
		}

        default:
                break;
    }

	return 0;

do_retry_proxy_api:
	if (!force_exit && lws_retry_sul_schedule_retry_wsi(wsi, &m->sul, proxy_api_connect, &m->retry_count)) {
		lwsl_err("%s: connection attempts exhausted\n", __func__);
		force_exit = 1;
	}
	return 0;
}

static const struct lws_protocols protocols[] = {
	{ "vxg-websocket", websocket_callback, sizeof(struct my_conn), WEBSOCKET_BUFFER_SIZE, 0, NULL, 0 },
	{ "vxg-proxy", proxy_client_callback, sizeof(struct proxy_conn), RAW_RX_BUFFER_SIZE, 0, NULL, 0 },
	{ "vxg-authtoken", vxg_token_api_callback, sizeof(struct my_conn), 4096, 0, NULL, 0 },
	{ "vxg-proxy-api", proxy_api_callback, sizeof(struct my_conn), 4096, 0, NULL, 0 },
	{ NULL, NULL, 0, 0, 0, NULL, 0 }
};

// ----------------------------------------------------------------------------
void sigint_handler(int sig)
{
	(void)sig;
	restart = 0;
	force_exit = 1;
	if (context)
		lws_cancel_service(context);
}

void vxg_proxy_client_set_lws_log_emit(int level_mask, vxg_proxy_client_log_emit_fn emit_fn)
{
	vxg_lws_level_mask = level_mask ? level_mask : (LLL_USER | LLL_ERR | LLL_WARN);
	vxg_lws_emit_fn = emit_fn;
}

void vxg_proxy_client_set_idle_callback(vxg_proxy_client_idle_fn fn, void *userdata)
{
	vxg_idle_cb = fn;
	vxg_idle_userdata = userdata;
}

void vxg_proxy_client_request_stop(void)
{
	restart = 0;
	force_exit = 1;
	if (context)
		lws_cancel_service(context);
}

int vxg_proxy_client_get_force_exit(void)
{
	return (int)force_exit;
}

int vxg_proxy_client_get_restart(void)
{
	return (int)restart;
}

// ----------------------------------------------------------------------------
int vxg_proxy_client_run(int argc, char *const *argv)
{
	static struct lws_context_creation_info info;
	int n, i;
	int forward_item_index;
	char *p, *p_end, *p_host_end = NULL;
	char buf[MAX_FORWARD_ITEM_NAME_LEN+MAX_FORWARD_ITEM_HOST_LEN+21];
	/* --priority / -p names: stored during parsing, applied after all args */
	char priority_names[MAX_FORWARD_ITEMS][MAX_FORWARD_ITEM_NAME_LEN+1];
	int priority_name_count = 0;

	// vars for reboot delay
	#define MAX_REBOOT_ATTEMPTS 15
	#define REBOOT_DURATION 60
	int reboot_cnt = 0;

#ifndef VXG_PROXY_CLIENT_EMBEDDED
	signal(SIGINT, sigint_handler);
#else
	/* Each in-process invocation must enter the outer reconnect loop. */
	restart = 1;
	force_exit = 0;
#endif

	while (restart)
	{
		restart = 0;

		/* Clear state from the previous context (401 reboot, etc.): sul nodes, HTTP status,
		 * and websocket reassembly must not survive lws_context_destroy. */
		vxg_token_401_attempts = 0;
		vxg_tls_setup_fail_reset();
		vxg_token_api_request_status = 0;
		proxy_api_request_status = 0;
		memset(auth_res_json_buffer, 0, sizeof(auth_res_json_buffer));
		memset(proxy_api_res_json_buffer, 0, sizeof(proxy_api_res_json_buffer));
		if (websocket_rcv_buffer) {
			free(websocket_rcv_buffer);
			websocket_rcv_buffer = NULL;
		}
		websocket_rcv_buffer_len = 0;
#ifdef PERF_LOG
		memset(&perf_sul, 0, sizeof(perf_sul));
#endif

		lws_set_log_level(vxg_lws_level_mask, vxg_lws_emit_fn);
		lwsl_user("Starting Uplink Client\n");

		memset(auth_token, 0, sizeof(auth_token));

		copy_env(vxg_api_host, sizeof(vxg_api_host), "VXG_API_HOST");
		copy_env(vxg_api_path, sizeof(vxg_api_path), "VXG_API_PATH");
		if (copy_env(vxg_api_password, sizeof(vxg_api_password), "VXG_API_PASSWORD"))
			convert_to_upper(vxg_api_password);
		copy_env(proxy_api_path, sizeof(proxy_api_path), "PROXY_API_PATH");
		copy_env(auth_token, sizeof(auth_token), "AUTH_TOKEN");
		if (copy_env(device_serial, sizeof(device_serial), "DEVICE_SERIAL"))
			convert_to_upper(device_serial);
		copy_env(status_file, sizeof(status_file), "STATUS_FILE");

		memset(&ws_conn, 0, sizeof(struct my_conn));
		memset(&vxg_token_api_conn, 0, sizeof(struct my_conn));
		memset(&proxy_api_conn, 0, sizeof(struct my_conn));

		for (forward_item_index=0; forward_item_index < MAX_FORWARD_ITEMS; forward_item_index++)
			forward_items[forward_item_index].proto = PROTO_NONE;
		forward_item_index = 0;
		/* Re-parsing argv on each in-process reboot must not accumulate -p names. */
		priority_name_count = 0;
		for (n=1; n < argc; n++)
		{
			if (strcmp(argv[n], "--token") == 0)
			{
				if (n+1 >= argc)
				{
					fprintf(stderr, "auth token format: --token token (e.g. --token BASE64_ENCODED_TOKEN)\n");
					return 1;
				}
				if (strlen(argv[n+1]) > sizeof(auth_token)-1)
				{
					fprintf(stderr, "auth token is too long\n");
					return 1;
				}
				copy_bounded(auth_token, sizeof(auth_token), argv[++n]);
			}
			else if (strcmp(argv[n], "--serial") == 0)
			{
				if (n+1 >= argc)
				{
					fprintf(stderr, "device serial format: --serial DEVICE_SERIAL\n");
					return 1;
				}
				if (strlen(argv[n+1]) > sizeof(device_serial)-1)
				{
					fprintf(stderr, "device serial is too long\n");
					return 1;
				}

				copy_bounded(device_serial, sizeof(device_serial), argv[++n]);

				convert_to_upper(device_serial);
			}
			else if (strcmp(argv[n], "--debug") == 0)
			{
				fprintf(stdout, "--debug argument: adding more log verbosity\n");
				lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_DEBUG, vxg_lws_emit_fn);
			}
			else if (strcmp(argv[n], "--http") == 0)
			{
				fprintf(stdout, "--http argument: switching to unencrypted HTTP\n");
				port = 80;
				ssl_connection = LCCSCF_ALLOW_INSECURE;
			}
			else if (strcmp(argv[n], "--insecure") == 0)
			{
				fprintf(stdout, "--insecure argument: skip all certificate checks\n");
				ssl_connection |= LCCSCF_ALLOW_INSECURE | LCCSCF_ALLOW_EXPIRED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK | LCCSCF_ALLOW_SELFSIGNED;
			}
			else if ((strcmp(argv[n], "--forward") == 0 || strcmp(argv[n], "-f") == 0) && n+1 < argc && forward_item_index < MAX_FORWARD_ITEMS)
			{
				// Parse such construction: -f name:proto:host:port
				// Retrive forward name
				p_host_end = NULL;
				if (strlen(argv[n+1]) >= sizeof(buf))
				{
					fprintf(stderr, "forward argument is too long\n");
					return 1;
				}
				memset(buf, 0, sizeof(buf));
				copy_bounded(buf, sizeof(buf), argv[++n]);
				p = buf;
				p_end = strchr(p, ':');
				if (!p_end || p_end == p)
				{
					fprintf(stderr, "forward argument format 1: -f name:proto:host:port (e.g. -f fwd1:http:127.0.0.1:80)\n");
					return 1;
				}
				*p_end = '\0';
				if (p_end-p > MAX_FORWARD_ITEM_NAME_LEN)
				{
					fprintf(stderr, "forward item name is too long\n");
					return 1;
				}
				for (i=0; i < forward_item_index; i++)
				{
					if (strcmp(forward_items[i].name, p) == 0)
					{
						fprintf(stderr, "forward item name must be unique\n");
						return 1;
					}
				}
				copy_bounded(forward_items[forward_item_index].name,
					     sizeof(forward_items[forward_item_index].name), p);
			
				// Retrive forward proto which can be http, https or tcp
				p = p_end + 1;
				p_end = strchr(p, ':');
				if (!p_end || p_end == p)
				{
					fprintf(stderr, "forward argument format 2: -f name:proto:host:port (e.g. -f fwd1:http:127.0.0.1:80)\n");
					return 1;
				}
				*p_end = '\0';
				if (strcasecmp(p, "http") == 0)
					forward_items[forward_item_index].proto = PROTO_HTTP;
				else if (strcasecmp(p, "https") == 0)
					forward_items[forward_item_index].proto = PROTO_HTTPS;
				else if (strcasecmp(p, "tcp") == 0)
					forward_items[forward_item_index].proto = PROTO_TCP;
				else
				{
					fprintf(stderr, "forward proto must be one of: http, https, tcp\n");
					return 1;
				}

				// Retrive forward host. It can be IPv4, IPv6 or domain name
				p = p_end + 1;
				if (*p == '[')
				{
					p = p + 1;
					p_end = strchr(p, ']');
					p_host_end = p_end + 1;
					*p_host_end = '\0';
				}
				else
				{
					p_end = strchr(p, ':');
				}
				if (!p_end || p_end == p)
				{
					return 1;
				}
				*p_end = '\0';

				if (p_end-p > MAX_FORWARD_ITEM_HOST_LEN)
				{
					fprintf(stderr, "forward host is too long\n");
					return 1;
				}
				copy_bounded(forward_items[forward_item_index].host,
					     sizeof(forward_items[forward_item_index].host), p);

				// Retrive forward port
				if (p_host_end != NULL)
				{
					p = p_host_end + 1;
				}
				else
				{
					p = p_end + 1;
				}

				if (*p == '\0')
				{
					fprintf(stderr, "forward argument format 4: -f name:proto:host:port (e.g. -f fwd1:http:127.0.0.1:80)\n");
					return 1;
				}
				forward_items[forward_item_index].port = atoi(p);
				if (forward_items[forward_item_index].port < 1)
				{
					fprintf(stderr, "forward port is invalid\n");
					return 1;
				}
				forward_item_index++;
			}
			else if (strcmp(argv[n], "--loglevel") == 0  || strcmp(argv[n], "-l") == 0)
			{
				char loglevel[10];
				if (n+1 >= argc)
				{
					fprintf(stderr, "log level argument format: -l option (e.g. -l debug, -l info, -l error)\n");
					return 1;
				}
				if (strlen(argv[n+1]) > sizeof(loglevel)-1)
				{
					fprintf(stderr, "log level parameter too long\n");
					return 1;
				}
				copy_bounded(loglevel, sizeof(loglevel), argv[++n]);
				
				if (strcmp(loglevel, "debug") == 0)
				{
					lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_DEBUG, vxg_lws_emit_fn);
					fprintf(stdout, "log level set to DEBUG\n");
				}
				else if (strcmp(loglevel, "info") == 0)
				{
					lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE, vxg_lws_emit_fn);
					fprintf(stdout, "log level set to INFO\n");
				}
				else if (strcmp(loglevel, "error") == 0)
				{
					lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE, vxg_lws_emit_fn);
					fprintf(stdout, "log level set to ERROR\n");
				}
				else
				{
					fprintf(stderr, "log level argument format: -l option (e.g. -l debug, -l info, -l error)\n");
					return 1;
				}
			}
			else if ((strcmp(argv[n], "--priority") == 0 || strcmp(argv[n], "-p") == 0) && n+1 < argc)
			{
				if (priority_name_count >= MAX_FORWARD_ITEMS)
				{
					fprintf(stderr, "too many --priority arguments (max %d)\n", MAX_FORWARD_ITEMS);
					return 1;
				}
				if (strlen(argv[n+1]) > MAX_FORWARD_ITEM_NAME_LEN)
				{
					fprintf(stderr, "priority forward name is too long\n");
					return 1;
				}
				copy_bounded(priority_names[priority_name_count++],
					     sizeof(priority_names[0]), argv[++n]);
			}
			else if (strcmp(argv[n], "--status-file") == 0)
			{
				if (n+1 >= argc)
				{
					fprintf(stderr, "Status file format: --status-file /tmp/camera.log\n");
					return 1;
				}
				if (strlen(argv[n+1]) > sizeof(status_file)-1)
				{
					fprintf(stderr, "Status file name is too long\n");
					return 1;
				}
				copy_bounded(status_file, sizeof(status_file), argv[++n]);
			}
		}

		if (strlen(device_serial) == 0 && strlen(auth_token) == 0)
		{
			fprintf(stderr, "No auth token or device serial specified. Format: --serial DEVICE_SERIAL or --token AUTH_TOKEN_BASE64_ENCODED\n");
			return 1;
		}
		if (strlen(vxg_api_password) == 0 && strlen(auth_token) == 0)
		{
			fprintf(stderr, "No VXG API password set. Use env variable VXG_API_PASSWORD\n");
			return 1;
		}
		if (forward_item_index == 0)
		{
			fprintf(stderr, "Need at least one forward specified. Format: -f name:proto:host:port (e.g. -f fwd1:http:127.0.0.1:80)\n");
			return 1;
		}

		/* Apply priority: only forwards explicitly named via --priority / -p
		 * are marked realtime.  If no -p given, all forwards stay bulk (no
		 * differential scheduling). */
		for (i = 0; i < forward_item_index; i++)
			forward_items[i].realtime = 0;
		for (n = 0; n < priority_name_count; n++)
		{
			int found = 0;
			for (i = 0; i < forward_item_index; i++)
			{
				if (strcmp(forward_items[i].name, priority_names[n]) == 0)
				{
					forward_items[i].realtime = 1;
					found = 1;
					break;
				}
			}
			if (!found)
			{
				fprintf(stderr, "--priority name '%s' does not match any -f forward name\n", priority_names[n]);
				return 1;
			}
		}

		for (i = 0; i < forward_item_index; i++)
			fprintf(stdout, "Forward [%s] proto=%s priority=%s\n",
				forward_items[i].name,
				forward_items[i].proto == PROTO_HTTP ? "http" : (forward_items[i].proto == PROTO_HTTPS ? "https" : "tcp"),
				forward_items[i].realtime ? "realtime" : "bulk");

		memset(&info, 0, sizeof info);
	#ifndef __NO_SSL__
		/* Re-running OpenSSL global init on every lws_create_context can crash the second
		 * context (Axis: SEGV right after reboot when token HTTP returned 401). */
		if (!vxg_lws_ssl_global_inited)
			info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	#endif
		info.port = CONTEXT_PORT_NO_LISTEN;
		info.protocols = protocols;
		info.pt_serv_buf_size = RAW_RX_BUFFER_SIZE;

		context = lws_create_context(&info);
		if (!context) {
			lwsl_err("lws init failed\n");
			return 1;
		}
	#ifndef __NO_SSL__
		vxg_lws_ssl_global_inited = 1;
	#endif
		// if need to request auth token - send auth token request first
		if (strlen(auth_token) == 0)
		{
			lwsl_user("Device MAC: %s\n", vxg_api_password);
			lwsl_user("Device serial: %s\n", device_serial);
			lws_sul_schedule(context, 0, &vxg_token_api_conn.sul, vxg_token_api_connect, 1);
		}
		else
			lws_sul_schedule(context, 0, &proxy_api_conn.sul, proxy_api_connect, 1);

		n = 0;
		while (n >= 0 && !force_exit) {
			n = lws_service(context, 0);
			if (vxg_idle_cb)
				vxg_idle_cb(vxg_idle_userdata);
		}

		lwsl_user("Stopping Uplink Client\n");

		lws_context_destroy(context);
		context = NULL;

		if (restart)
		{
			if (reboot_cnt > MAX_REBOOT_ATTEMPTS)
			{
				lwsl_err("Maximum reboot attempts exceeded.\n");
				return 0;
			}
			else
			{
				force_exit = 0;
				lwsl_user("Rebooting Uplink Client, reboot delay: %d seconds\n", REBOOT_DURATION);
				#ifdef _WIN32
					Sleep(REBOOT_DURATION * 1000);
				#else
					sleep(REBOOT_DURATION);
				#endif

				reboot_cnt++;
			}
		}
	}
	return 0;
}

#ifndef VXG_PROXY_CLIENT_EMBEDDED
int main(int argc, const char **argv)
{
	return vxg_proxy_client_run(argc, (char *const *)argv);
}
#endif
