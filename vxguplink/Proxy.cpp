#include "Proxy.h"

Uplink::Proxy::Proxy()
{
    strncpy(vxg_api_host, "camera.vxg.io", sizeof(vxg_api_host)-1);
    strncpy(vxg_api_path, "/v1/token?serial_id=%s", sizeof(vxg_api_path)-1);
    strncpy(vxg_api_password, "", sizeof(vxg_api_password)-1);

    strncpy(proxy_api_host,"",sizeof(proxy_api_host)-1);
    strncpy(proxy_api_path,"/api/ws-endpoint",sizeof(proxy_api_path)-1);
    strncpy(proxy_ws_host,"",sizeof(proxy_ws_host)-1);
    strncpy(proxy_ws_path,"",sizeof(proxy_ws_path)-1);
    proxy_ws_port = 443;

    strncpy(device_serial, "", sizeof(device_serial)-1);
    strncpy(auth_token, "", sizeof(auth_token)-1);
    port = 443;
    ssl_connection = LCCSCF_USE_SSL;

    ws_retry = {
        .retry_ms_table			= backoff_ms,
        .retry_ms_table_count	= LWS_ARRAY_SIZE(backoff_ms),
        .conceal_count			= LWS_RETRY_CONCEAL_ALWAYS,

        .secs_since_valid_ping		= 30,  /* force PINGs after secs idle */
        .secs_since_valid_hangup	= 60, /* hangup after secs idle */

        .jitter_percent			= 20,
    };

    vxg_token_api_retry = {
        .retry_ms_table			= backoff_ms,
        .retry_ms_table_count	= LWS_ARRAY_SIZE(backoff_ms),
        .conceal_count			= LWS_RETRY_CONCEAL_ALWAYS,

        .secs_since_valid_ping		= 30,  /* force PINGs after secs idle */
        .secs_since_valid_hangup	= 60, /* hangup after secs idle */

        .jitter_percent			= 20,
    };

    proxy_api_retry = {
        .retry_ms_table			= backoff_ms,
        .retry_ms_table_count	= LWS_ARRAY_SIZE(backoff_ms),
        .conceal_count			= LWS_RETRY_CONCEAL_ALWAYS,

        .secs_since_valid_ping		= 30,  /* force PINGs after secs idle */
        .secs_since_valid_hangup	= 60, /* hangup after secs idle */

        .jitter_percent			= 20,
    };

    vxg_token_api_request_status = 0;
    proxy_api_request_status = 0;

    websocket_rcv_buffer=NULL;
    websocket_rcv_buffer_len=0;

    force_exit = 0;
    restart = 1;
}

// ----------------------------------------------------------------------------
// Private Function Declarations

void Uplink::Proxy::_destroy_message(void *_msg)
{
    struct msg *msg = (struct msg *)_msg;

    free(msg->payload);
    msg->payload = NULL;
    msg->len = 0;
}

char* Uplink::Proxy::_convert_to_upper(char *s)
{
    int i;
    for (i = 0; s[i]!='\0'; i++) {
        if(s[i] >= 'a' && s[i] <= 'z') {
            s[i] = s[i] - 32;
        }
    }
    return s;
}

void Uplink::Proxy::_websocket_connect(lws_sorted_usec_list_t *sul)
{

    struct my_conn *m = lws_container_of(sul, struct my_conn, sul);
    struct lws_client_connect_info i;
    char json_buffer[1024*10];

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

void Uplink::Proxy::websocket_connect(lws_sorted_usec_list_t *sul)
{
    struct my_conn *m = lws_container_of(sul, struct my_conn, sul);
    lwsl_debug("websocket_connect my_conn:%x obj:%x\n",m,m->obj);
    Uplink::Proxy *proxy = (Proxy*)m->obj;
    proxy->_websocket_connect(sul);
}

int Uplink::Proxy::_proxy_client_connection_closed_notification(struct proxy_conn *client)
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
            _destroy_message(&amsg);
            lwsl_err("LWS_CALLBACK_CLIENT_RECEIVE: lws_ring_insert() error\n");
            return -1;
        }
        client->close_notification_sent = 1;
        lws_callback_on_writable(ws_conn.wsi);
        return 0;
    }
    return -1;
}

void Uplink::Proxy::_destroy_proxy_client(struct proxy_conn *client)
{
    int n;
    if (client->ring) {
        n = (int)lws_ring_get_count_waiting_elements(client->ring, &client->tail);
        ws_conn.total_msgs_in_client_rings -= n;

        if (ws_conn.flow_controlled && ws_conn.total_msgs_in_client_rings <= RING_DEPTH_OK) {
            ws_conn.flow_controlled = 0;
            lwsl_user("websocket rx unblocked\n");
            lws_rx_flow_control(ws_conn.wsi, 1);
        }
        lws_ring_destroy(client->ring);
        client->ring = NULL;
    }
    if (client->wsi_raw) {
        client->wsi_raw = NULL;
    }
    _proxy_client_connection_closed_notification(client);
    if (client->prev_client)
        client->prev_client->next_client = client->next_client;
    if (client->next_client)
        client->next_client->prev_client = client->prev_client;
    if (client == ws_conn.first_client)
        ws_conn.first_client = client->next_client;
    free(client);
}

int Uplink::Proxy::_proxy_client_callback(struct lws *wsi, enum lws_callback_reasons reason,
        void *user, void *in, size_t len)
{
    struct proxy_conn *client = (struct proxy_conn *)user;
    struct msg amsg;
    const struct msg *pmsg;
    int bytes_written, n;

    switch (reason) {
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        {
            lwsl_warn("%s: onward raw connection failed (client_id=%d)\n", __func__, client->client_id);
            _destroy_proxy_client(client);
            return 0;
        }

        case LWS_CALLBACK_RAW_CONNECTED:
        {
            // lwsl_user("%s: onward raw connection established (client_id=%d)\n", __func__, client->client_id);
            client->wsi_raw = wsi;
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
            // lwsl_user("LWS_CALLBACK_RAW_CLOSE (client_id=%d)\n", client->client_id);
            _destroy_proxy_client(client);
            return 0;
        }

        case LWS_CALLBACK_RAW_RX:
        {
            lwsl_debug("New data from client connection (client_id=%d, len=%d)\n", client->client_id, (int)len);
            lwsl_hexdump_debug(in, len);
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
            if (!client->flow_controlled && RING_DEPTH-(n-1) >= RING_DEPTH_CRITICAL) {
                client->flow_controlled = 1;
                lwsl_user("client %d flow rx blocked\n", client->client_id);
                lws_rx_flow_control(wsi, 0);
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
                _destroy_message(&amsg);
                lwsl_err("LWS_CALLBACK_RAW_RX: lws_ring_insert() error\n");
                lws_set_timeout(client->wsi_raw, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, LWS_TO_KILL_ASYNC);
                return -1;
            }
            lws_callback_on_writable(ws_conn.wsi);
            break;
        }

        case LWS_CALLBACK_RAW_WRITEABLE:
        {
            if (!client->ring)
                break;

            // pmsg = lws_ring_get_element(client->ring, &client->tail);
            pmsg = (const msg*)lws_ring_get_element(client->ring, &client->tail);
            if (!pmsg) {
                // lwsl_user(" CLIENT (nothing in ring)\n");
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

            lws_ring_consume_single_tail(client->ring, &client->tail, 1);
            ws_conn.total_msgs_in_client_rings--;

            if (ws_conn.flow_controlled && ws_conn.total_msgs_in_client_rings <= RING_DEPTH_OK) {
                ws_conn.flow_controlled = 0;
                lwsl_user("websocket rx unblocked\n");
                lws_rx_flow_control(ws_conn.wsi, 1);
            }

            n = (int)lws_ring_get_count_waiting_elements(client->ring, &client->tail);
            if (n > 0)
                lws_callback_on_writable(wsi);
            break;
        }

        default:
        {
            break;
        }
    }

    return 0;
}

int Uplink::Proxy::_websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
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

	switch (reason)
    {
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        {
            lwsl_err("CLIENT_CONNECTION_ERROR (%d): %s\n",
                lws_get_socket_fd(wsi),
                in ? (char *)in : "(null)");
            if (m->ring) {
                lws_ring_destroy(m->ring);
                m->total_msgs_in_client_rings = 0;
                m->ring = NULL;
            }
            for (client=m->first_client; client; client = client->next_client) {
                lws_set_timeout(client->wsi_raw, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, LWS_TO_KILL_ASYNC);
            }
            goto do_retry;
            break;
        }

        case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
        {
            lwsl_debug("RECEIVE_PONG\n");
            break;
        }

        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
        {
            unsigned char **p = (unsigned char **)in, *end = (*p) + len;
            if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_AUTHORIZATION,
                    (unsigned char *)auth_token, strlen(auth_token), p, end))
                return -1;
            break;
        }

        case LWS_CALLBACK_CLIENT_RECEIVE:
        {
            lwsl_debug("websocket RCV (len=%d):\n", (int)len);
            lwsl_hexdump_debug(in, len);
            if (!lws_frame_is_binary(wsi) || len < 1)
            {
                // lwsl_user("websocket RCV (len=%d):\n", (int)len);
                // lwsl_hexdump_err(in, len);
                break;
            }
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
                client = (proxy_conn*)malloc(sizeof(struct proxy_conn));
                if (!client)
                {
                    lwsl_err("%s:%d: malloc() error\n", __func__, __LINE__);
                    return -1;
                }
                memset(client, 0, sizeof(struct proxy_conn));
                if (prev_client)
                {
                    prev_client->next_client = client;
                    client->prev_client = prev_client;
                }
                else
                    m->first_client = client;

                client->client_id = client_id;
                client->forward_index = forward_index;
                client->ring = lws_ring_create(sizeof(struct msg), RING_DEPTH, _destroy_message);
                if (!client->ring)
                {
                    lwsl_err("lws_ring_create() error\n");
                    return -1;
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

                // lwsl_user("New connection open request (client_id=%d) to %s:%d\n", client_id, forward_items[forward_index].host, forward_items[forward_index].port);

                if (!lws_client_connect_via_info(&client_conn_info)) {
                    lwsl_err("lws_client_connect_via_info() failed\n");
                    client->wsi_raw = NULL;
                    _destroy_proxy_client(client);
                    break;
                }
            }
            else if (cmd == 'C' && websocket_rcv_buffer_len == 1+sizeof(client_id_t))
            {
                client_id = *((client_id_t *)(websocket_rcv_buffer+1));
                // lwsl_user("Connection close request (client_id=%d)\n", client_id);
                // search for client to disconnect
                for (client=m->first_client; client; client = client->next_client) {
                    if (client->client_id == client_id) {
                        client->close_notification_sent = 1;
                        lws_set_timeout(client->wsi_raw, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, LWS_TO_KILL_SYNC);
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
                            _destroy_message(&amsg);
                            lwsl_err("LWS_CALLBACK_CLIENT_RECEIVE: lws_ring_insert() error\n");
                            lws_set_timeout(client->wsi_raw, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, LWS_TO_KILL_SYNC);
                        }
                        m->total_msgs_in_client_rings++;
                        lws_callback_on_writable(client->wsi_raw);

                        if (!m->flow_controlled && m->total_msgs_in_client_rings >= RING_DEPTH_CRITICAL) {
                            m->flow_controlled = 1;
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
            break;
        }

        case LWS_CALLBACK_CLIENT_ESTABLISHED:
        {
            lwsl_user("%s: established\n", __func__);
            m->ring = lws_ring_create(sizeof(struct msg), RING_DEPTH, _destroy_message);
            m->total_msgs_in_client_rings = 0;
            if (!m->ring)
                return 1;
            m->tail = 0;

            // send forwardings info
            cJSON *auth_json = cJSON_CreateObject();
            cJSON_AddNumberToObject(auth_json, "websocket_buffer_size", WEBSOCKET_BUFFER_SIZE);
            cJSON_AddStringToObject(auth_json, "agent_version", AGENT_VERSION);
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
            if (bytes_written < (int)amsg.len) {
                lwsl_err("ERROR %d writing to ws socket\n", bytes_written);
                return -1;
            }
            break;
        }

        case LWS_CALLBACK_CLIENT_WRITEABLE:
        {
            lwsl_debug("%s: writable\n", __func__);

            if (m->write_consume_pending) {
                n = (int)lws_ring_get_count_free_elements(m->ring);
                /* perform the deferred fifo consume */
                lws_ring_consume_single_tail(m->ring, &m->tail, 1);
                m->write_consume_pending = 0;

                if (n >= RING_DEPTH_CRITICAL && n-1 <= RING_DEPTH_OK)
                {
                    for (client=m->first_client; client; client = client->next_client) {
                        if (client->flow_controlled) {
                            lwsl_user("client %d flow rx unblocked\n", client->client_id);
                            lws_rx_flow_control(client->wsi_raw, 1);
                            client->flow_controlled = 0;
                            n++;
                        }
                    }
                }
            }
            pmsg = (const msg*)lws_ring_get_element(m->ring, &m->tail);
            if (!pmsg) {
    //			lwsl_user(" WEBSOCKET (nothing in ring)\n");
                break;
            }

            lwsl_debug("websocket SND (len=%d):\n", (int)pmsg->len);
            lwsl_hexdump_debug(((unsigned char *)pmsg->payload) +
                    LWS_PRE, pmsg->len);

            /* notice we allowed for LWS_PRE in the payload already */
            bytes_written = lws_write(wsi, ((unsigned char *)pmsg->payload) +
                    LWS_PRE, pmsg->len, (enum lws_write_protocol)LWS_WRITE_BINARY);
            if (bytes_written < (int)pmsg->len) {
                lwsl_err("ERROR %d writing to ws socket\n", bytes_written);
                return -1;
            }

            m->write_consume_pending = 1;
            lws_callback_on_writable(wsi);
            break;
        }

        case LWS_CALLBACK_CLIENT_CLOSED:
        {
            lwsl_user("%s: closed\n", __func__);
            if (m->ring) {
                lws_ring_destroy(m->ring);
                m->total_msgs_in_client_rings = 0;
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
    // Return to vxg_api_connect() to fetch new token
	if (!force_exit && lws_retry_sul_schedule_retry_wsi(wsi, &m->sul, vxg_token_api_connect, &m->retry_count)) {
		lwsl_err("%s: connection attempts exhausted\n", __func__);
		force_exit = 1;
	}
	return 0;
}

void Uplink::Proxy::_vxg_token_api_connect(lws_sorted_usec_list_t *sul)
{
    struct my_conn *m = lws_container_of(sul, struct my_conn, sul);
    struct lws_client_connect_info i;
    char path_query[1024*4];

    memset(&i, 0, sizeof(i));

    sprintf(path_query, vxg_api_path, device_serial);
    i.context = context;
    i.port = port;
    i.address = vxg_api_host;
    i.path = path_query;
    i.host = i.address;
    i.origin = i.address;
    i.ssl_connection = ssl_connection;
    i.method = "GET";
    i.protocol = "vxg-authtoken";
    i.pwsi = &m->wsi;
    i.retry_and_idle_policy = &vxg_token_api_retry;
    i.userdata = m;

    lwsl_debug("Requesting auth token from %s:%d\n", i.host, i.port);
    if (!lws_client_connect_via_info(&i))
	if (lws_retry_sul_schedule(context, 0, sul, &vxg_token_api_retry,
		vxg_token_api_connect, &m->retry_count)) {
            lwsl_err("%s: connection attempts exhausted\n", __func__);
            force_exit = 1;
        }
}

void Uplink::Proxy::vxg_token_api_connect(lws_sorted_usec_list_t *sul)
{
    struct my_conn *m = lws_container_of(sul, struct my_conn, sul);
    lwsl_debug("vxg_token_api_connect :%x :%x %x\n", sul, m, m->obj);    
    Uplink::Proxy *proxy = (Proxy*)m->obj;
    proxy->_vxg_token_api_connect(sul);
}

int Uplink::Proxy::_vxg_token_api_callback(struct lws *wsi, enum lws_callback_reasons reason,
            void *user, void *in, size_t len)
{
    struct my_conn *m = (struct my_conn *)user;

    switch (reason) {

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        {
            lwsl_err("%s: CLIENT_CONNECTION_ERROR: %s\n",
                    in ? (char *)in : "(null)", __func__);
            goto do_retry_vxg_token_api;
            break;
        }

        case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
        {
            vxg_token_api_request_status = lws_http_client_http_response(wsi);
            memset(auth_res_json_buffer, 0, sizeof(auth_res_json_buffer));
            lwsl_user("Got HTTP %d response from auth token service\n", vxg_token_api_request_status);
		    if (vxg_token_api_request_status >= 400 && vxg_token_api_request_status < 500)
            {
				if (vxg_token_api_request_status == 401)
				{
					lwsl_err("Access token not found on provisioning server");
				}
				restart = 1;
				force_exit = 1;
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
            {
                    char buffer[1024 + LWS_PRE];
                    char *px = buffer + LWS_PRE;
                    int lenx = sizeof(buffer) - LWS_PRE;

                    if (lws_http_client_read(wsi, &px, &lenx) < 0)
                            return -1;
            }
            return 0;
        }
        case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
        {
            if (vxg_token_api_request_status  == 200)
            {
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
                lwsl_user("access_token: %s\n", auth_token);
                lwsl_user("Successfully received auth token\n");

                cJSON_Delete(res_json);

                lws_sul_schedule(context, 0, &proxy_api_conn.sul, proxy_api_connect, 1);
            }
            break;
        }

        default:
        {
            break;
        }
    }

    return 0;

do_retry_vxg_token_api:
	if (!force_exit && lws_retry_sul_schedule_retry_wsi(wsi, &m->sul, vxg_token_api_connect, &m->retry_count)) {
		lwsl_err("%s: connection attempts exhausted\n", __func__);
		force_exit = 1;
	}
	return 0;
}


void Uplink::Proxy::_proxy_api_connect(lws_sorted_usec_list_t *sul)
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

    // // Check uplink field
	j = cJSON_GetObjectItem(token_json, "uplink");
	if (!j || j->type != cJSON_String)
	{
		cJSON_Delete(token_json);
		lwsl_err("Unable to find proxy address in access token: %s\n", json_buffer);
        restart = 1;
		force_exit = 1;
		return;
	}
	strcpy(proxy_api_host, j->valuestring);
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


int Uplink::Proxy::_proxy_api_callback(struct lws *wsi, enum lws_callback_reasons reason,
              void *user, void *in, size_t len)
{
    struct my_conn *m = (struct my_conn *)user;

    switch (reason) {

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        {
            lwsl_err("%s: CLIENT_CONNECTION_ERROR: %s\n",
                        in ? (char *)in : "(null)", __func__);
            goto do_retry_proxy_api;
            break;
        }

        case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
        {
            proxy_api_request_status = lws_http_client_http_response(wsi);
		    memset(proxy_api_res_json_buffer, 0, sizeof(proxy_api_res_json_buffer));
            lwsl_user("Got HTTP %d response from proxy api websocket endpoint\n", proxy_api_request_status);
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
            {
                unsigned char **p = (unsigned char **)in, *end = (*p) + len;
                if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_AUTHORIZATION,
                    (unsigned char *)auth_token, strlen(auth_token), p, end))
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
            {
                    char buffer[1024 + LWS_PRE];
                    char *px = buffer + LWS_PRE;
                    int lenx = sizeof(buffer) - LWS_PRE;

                    if (lws_http_client_read(wsi, &px, &lenx) < 0)
                            return -1;
            }
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
                lwsl_debug("proxy_ws_host: %s\n", proxy_ws_host);
                lwsl_debug("ws_path: %s\n", proxy_ws_path);
                lwsl_user("Successfully received ws endpoint\n");
                cJSON_Delete(res_json);

                lws_sul_schedule(context, 0, &ws_conn.sul, websocket_connect, 1);
            }
            break;
        }

        default:
        {
            break;
        }
    }

	return 0;

do_retry_proxy_api:
	if (!force_exit && lws_retry_sul_schedule_retry_wsi(wsi, &m->sul, proxy_api_connect, &m->retry_count)) {
		lwsl_err("%s: connection attempts exhausted\n", __func__);
		force_exit = 1;
	}
	return 0;
}

void Uplink::Proxy::_wait_for_exit()
{
    int n = 0;
	while (n >= 0 && !force_exit) {
		n = lws_service(context, 0);
	}
}

void Uplink::Proxy::wait_for_exit(void* user_data)
{
    Uplink::Proxy* proxy = (Uplink::Proxy*)user_data;  	
    proxy->_wait_for_exit();
}

// Static wrappers for callback functions

int Uplink::Proxy::proxy_client_callback(struct lws *wsi, enum lws_callback_reasons reason,
                    void *user, void *in, size_t len)
{
    Uplink::Proxy *proxy = (Proxy*)lws_context_user(lws_get_context(wsi));
    lwsl_debug("proxy_client_callback:%x wsi:%x reason:%x\n",proxy,wsi,reason);
    return proxy->_proxy_client_callback(wsi, reason, user, in, len);
}

int Uplink::Proxy::websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
        void *user, void *in, size_t len)
{    
    Uplink::Proxy *proxy = (Proxy*)lws_context_user(lws_get_context(wsi));
    lwsl_debug("websocket_callback proxy:%x wsi:%x reason:%x\n",proxy,wsi,reason);
    return proxy->_websocket_callback(wsi, reason, user, in, len);
}

int Uplink::Proxy::vxg_token_api_callback(struct lws *wsi, enum lws_callback_reasons reason,
            void *user, void *in, size_t len)
{
    Uplink::Proxy *proxy = (Proxy*)lws_context_user(lws_get_context(wsi));
    lwsl_debug("vxg_token_api_callback proxy:%x wsi:%x reason:%x\n",proxy,wsi,reason);
    return proxy->_vxg_token_api_callback(wsi, reason, user, in, len);
}

int Uplink::Proxy::proxy_api_callback(struct lws *wsi, enum lws_callback_reasons reason,
            void *user, void *in, size_t len)
{
    Uplink::Proxy *proxy = (Proxy*)lws_context_user(lws_get_context(wsi));
    lwsl_debug("proxy_api_callback proxy:%x wsi:%x reason:%x\n",proxy,wsi,reason);
    return proxy->_proxy_api_callback(wsi, reason, user, in, len);
}

void Uplink::Proxy::proxy_api_connect(lws_sorted_usec_list_t *sul)
{
    struct my_conn *m = lws_container_of(sul, struct my_conn, sul);
    lwsl_debug("proxy_api_connect my_conn:%x obj:%x\n",m,m->obj);
    Uplink::Proxy *proxy = (Proxy*)m->obj;
    return proxy->_proxy_api_connect(sul);
}


// ----------------------------------------------------------------------------
// Public Function Declarations

int Uplink::Proxy::get_serial_number(char* ser_number)
{
    fprintf(stderr, "get_serial_number not implemented\n");
    return -1;
}

int Uplink::Proxy::get_mac_address(char* mac_address)
{
    fprintf(stderr, "get_mac_address not implemented\n");
    return -1;
}

int Uplink::Proxy::get_camera_info()
{
    fprintf(stderr, "get_camera_info not implemented\n");
    return -1;
}

int Uplink::Proxy::start()
{
    // lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_DEBUG, NULL);
    lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN, NULL);
    lwsl_user("Starting Uplink Client\n");

    static const struct lws_protocols protocols[] = {
        { "vxg-websocket", websocket_callback, sizeof(struct my_conn), WEBSOCKET_BUFFER_SIZE, 0, NULL, 0 },
        { "vxg-proxy", proxy_client_callback, sizeof(struct proxy_conn), 4096, 0, NULL, 0 },
        { "vxg-authtoken", vxg_token_api_callback, sizeof(struct my_conn), 4096, 0, NULL, 0 },
	    { "vxg-proxy-api", proxy_api_callback, sizeof(struct my_conn), 4096, 0, NULL, 0 },
        { NULL, NULL, 0, 0, 0, NULL, 0 }
    };

    restart = 0;
    force_exit = 0;

    memset(&ws_conn, 0, sizeof(struct my_conn));
    memset(&vxg_token_api_conn, 0, sizeof(struct my_conn));
    memset(&proxy_api_conn, 0, sizeof(struct my_conn));

    lwsl_debug("ws_conn:%x sul:%x this:%x info:%x\n",&ws_conn,&ws_conn.sul,this,&info);

    ws_conn.obj = (void*)this;
    proxy_api_conn.obj = (void*)this;
    vxg_token_api_conn.obj = (void*)this;

    lwsl_debug("proxy_api_conn:%x sul:%x this:%x info:%x\n",&proxy_api_conn,&proxy_api_conn.sul,this,&info);


    memset(&info, 0, sizeof info);
#ifndef __NO_SSL__
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
#endif
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.user = (void*)this;

    context = lws_create_context(&info);
    if (!context) {
        lwsl_err("lws init failed\n");
        return -1;
    }
    else {
        // if need to request auth token - send auth token request first
        if (strlen(auth_token) == 0)
        {
            is_provisioning = 1;
            lws_sul_schedule(context, 0, &vxg_token_api_conn.sul, vxg_token_api_connect, 1);
        }
        else
        {
            is_provisioning = 0;
            lws_sul_schedule(context, 0, &proxy_api_conn.sul, proxy_api_connect, 1);
        }
    }

    // create thread to spin client
    spin = std::thread(&Proxy::wait_for_exit, this);

    return 0;
}

void Uplink::Proxy::stop()
{
    if (!is_provisioning)
    {
        restart = 0;
    }
    force_exit = 1;
    spin.join();
    lws_context_destroy(context);
}


void Uplink::Proxy::set_parameters(char *api_host,
                                        char *api_path,
                                        char *api_password,
                                        char *ws_host,
                                        char *ws_path,
                                        char *device_ser,
                                        char *token,
                                        int conn_port,
                                        int ssl_conn,
                                        std::vector<forward_item> *fwd_items
                                        )
{
    // Replace all with arguments/env variables
    strncpy(vxg_api_path, api_path, sizeof(vxg_api_path)-1);

    strncpy(vxg_api_password, _convert_to_upper(api_password), sizeof(vxg_api_password)-1);

    //strncpy(websocket_host, ws_host, sizeof(websocket_host)-1);
    //strncpy(websocket_path, ws_path, sizeof(websocket_path)-1);

    strncpy(device_serial, _convert_to_upper(device_ser), sizeof(device_serial)-1);
    strncpy(auth_token, token, sizeof(auth_token)-1);

    lwsl_debug("lws init token %ld %s:%s\n",sizeof(token) ,auth_token, token);

    // update port and ssl connection
    port = conn_port;
    ssl_connection = ssl_conn;

    // update forward items
    forward_items = *fwd_items;
} // end set_parameters

volatile int Uplink::Proxy::get_force_exit()
{
    return force_exit;
}

volatile int Uplink::Proxy::get_restart()
{
    return restart;
}