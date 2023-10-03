#ifndef _PROXY_H
#define _PROXY_H
#include <libwebsockets.h>
#include <string>
#include <vector>
#include <thread>
#include "cjson/cJSON.h"

#ifndef WEBSOCKET_BUFFER_SIZE
#define WEBSOCKET_BUFFER_SIZE   	4096
#endif

#ifndef AGENT_VERSION
#define AGENT_VERSION   			"0.1.2"
#endif

#define RING_DEPTH 					1024
#define RING_DEPTH_CRITICAL			1010
#define RING_DEPTH_OK				1000			
#define MAX_FORWARD_ITEM_NAME_LEN 	50
#define MAX_FORWARD_ITEM_HOST_LEN 	255
#define MAX_FORWARD_ITEMS 			10
#define MAX_CLIENT_CONNECTIONS		256

enum Proto {
	PROTO_NONE,
	PROTO_HTTP,
	PROTO_HTTPS,
	PROTO_TCP
};

struct forward_item {
	char name[MAX_FORWARD_ITEM_NAME_LEN+1];
	char host[MAX_FORWARD_ITEM_HOST_LEN+1];
	Proto proto;
	uint16_t port;
};

struct msg {
	void *payload;
	size_t len;
};

typedef uint16_t client_id_t;

struct proxy_conn {
	struct lws		*wsi_raw; // wsi for the outbound raw conn
	struct lws_ring *ring;
	uint8_t forward_index;
	client_id_t client_id;
	uint32_t tail;
	char flow_controlled;
	char close_notification_sent;
	uint8_t write_consume_pending:1;
	struct proxy_conn *next_client;
	struct proxy_conn *prev_client;
    void* obj;
};

struct my_conn{
	lws_sorted_usec_list_t	sul;	     // schedule connection retry
	struct lws		*wsi;
	uint16_t		retry_count;
	struct lws_ring *ring;
	uint32_t tail;
	char flow_controlled;
	uint8_t write_consume_pending:1;
	struct proxy_conn *first_client;
	uint32_t  total_msgs_in_client_rings;
    void* obj;
} ;

namespace Uplink
{
    class Proxy
    {
        // Member Variables
        private:
            char vxg_api_host[128];
            char vxg_api_path[128];
            char vxg_api_password[128];

            char proxy_api_host[256];
            char proxy_api_path[256];
            char proxy_ws_host[256];
            char proxy_ws_path[256];
            unsigned short proxy_ws_port;

            char device_serial[256];
            char auth_token[1024*8];

            int port;
            int ssl_connection;

            const char *pro;
            const uint32_t backoff_ms[5] = { 1000, 2000, 3000, 4000, 5000 };
            lws_retry_bo_t ws_retry;
            lws_retry_bo_t vxg_token_api_retry;
            lws_retry_bo_t proxy_api_retry;

            int vxg_token_api_request_status;
            char auth_res_json_buffer[1024*10];

            int proxy_api_request_status;
            char proxy_api_res_json_buffer[1024*10];

            int websocket_rcv_buffer_len;
            char *websocket_rcv_buffer;

            volatile int force_exit;
            volatile int restart;
            volatile int is_provisioning;

            std::thread spin;

            struct lws_context *context;
            std::vector<forward_item> forward_items;
            struct lws *client_wsi;
            struct lws_context_creation_info info;
            struct my_conn ws_conn, authtoken_conn, vxg_token_api_conn, proxy_api_conn;

        public:
            Proxy();

            // Abstract Functions
            virtual ~Proxy() {}

            virtual int get_serial_number(char* ser_number);

            virtual int get_mac_address(char* mac_address);

            virtual int get_camera_info();

        private:
            static void _destroy_message(void *_msg);

            void _websocket_connect(lws_sorted_usec_list_t *sul);

            void _authtoken_connect(lws_sorted_usec_list_t *sul);

            void _proxy_api_connect(lws_sorted_usec_list_t *sul);

            void _vxg_token_api_connect(lws_sorted_usec_list_t *sul);

            int _proxy_client_connection_closed_notification(struct proxy_conn *client);

            void _destroy_proxy_client(struct proxy_conn *client);

            int _proxy_client_callback(struct lws *wsi, enum lws_callback_reasons reason,
                    void *user, void *in, size_t len);

                    int _proxy_api_callback(struct lws *wsi, enum lws_callback_reasons reason,
                    void *user, void *in, size_t len);

            int _websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
                    void *user, void *in, size_t len);

            int _authtoken_callback(struct lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len);

            int _vxg_token_api_callback(struct lws *wsi, enum lws_callback_reasons reason,
                    void *user, void *in, size_t len);

            void _wait_for_exit();

            // Static wrappers for callback functions
            static void websocket_connect(lws_sorted_usec_list_t *sul);

            static void proxy_api_connect(lws_sorted_usec_list_t *sul);

            static void vxg_token_api_connect(lws_sorted_usec_list_t *sul);

            static int proxy_client_callback(struct lws *wsi, enum lws_callback_reasons reason,
                    void *user, void *in, size_t len);

            static int proxy_api_callback(struct lws *wsi, enum lws_callback_reasons reason,
                    void *user, void *in, size_t len);

            static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
                    void *user, void *in, size_t len);

            static int vxg_token_api_callback(struct lws *wsi, enum lws_callback_reasons reason,
                    void *user, void *in, size_t len);

            static void wait_for_exit(void* user_data);


// ----------------------------------------------------------------------------

        public:

            //! @brief Start internal workflow, this is the main function which starts
            //!        all internal connections
            //!
            int start();

            //! @brief Stop internal workflow, this is the main function which stops
            //!        lws connection
            //!
            void stop();

            void set_parameters(char *api_host,
                                    char *api_path,
                                    char *api_password,
                                    char *ws_host,
                                    char *ws_path,
                                    char *device_ser,
                                    char *token,
                                    int conn_port,
                                    int ssl_conn,
                                    std::vector<forward_item> *fwd_items
                                    ); 

            volatile int get_force_exit();

            volatile int get_restart();
    }; // class Proxy
} // namespace uplink

#endif