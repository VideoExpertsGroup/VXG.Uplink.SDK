#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <vector>
#include <agent/Proxy.h>

volatile int reboot = 1;
int n, i;
int forward_item_index;
char *p, *p_end;
char buf[MAX_FORWARD_ITEM_NAME_LEN+MAX_FORWARD_ITEM_HOST_LEN+21];
std::vector<forward_item> app_forward_items(MAX_FORWARD_ITEMS+1);

char api_host[128] = "camera.vxg.io";
char api_path[128] = "/v1/token?serial_id=%s";
char api_password[128] = "";
char ws_host[256] = "";
char proxy_api_path[256] = "";
char ws_path[256] = "/device-ws";
char device_sn[256] = "";
char token[1024*8] = "";

#ifdef __NO_SSL__
int conn_port = 80;
int ssl_conn = LCCSCF_ALLOW_INSECURE;
#elif __SKIP_SSL_CHECK__
int conn_port = 443;
int ssl_conn = LCCSCF_ALLOW_INSECURE | LCCSCF_ALLOW_EXPIRED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK | LCCSCF_ALLOW_SELFSIGNED;
#else
int conn_port = 443;
int ssl_conn = LCCSCF_USE_SSL;
#endif

static bool quit = 0;

static void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        fprintf(stderr, "\nSIGTERM received\n\n");
		reboot = 0;
        quit = true;
    }
}


/*
===========================================================================================================================
*/

class Derived_Proxy : public Uplink::Proxy {
	public:
		//! [Get serial number implementation]
		int get_serial_number(char* ser_number) override
		{
			fprintf(stderr, "get_serial_number not implemented\n");
			return -1;
		}

		//! [Get mac address implementation]
		int get_mac_address(char* mac_address) override
		{
			fprintf(stderr, "get_mac_address not implemented\n");
			return -1;
		}

		//! @brief Get camera info function, responsible for retrieving
		//! camera S/N and MAC address
		//!
		//! @return 0 if successful
		//!
		int get_camera_info() override // input list of parameters
		{
			// get and set MAC Address
			if (get_mac_address(api_password) == -1)
			{
				return -1;
			}

			// get and set device serial number
			if (get_serial_number(device_sn) == -1)
			{
				return -1;
			}
			return 0;
		}
};

/*
===========================================================================================================================
*/

// TODO: Change return value
//! @brief Parse supplied command line arguments.
//!
//! @return true if successful,
//! @return false if failure
//!
bool parse_args(int argc, char** argv)
{
	// Update variables with environment variables
	if (getenv("VXG_API_HOST"))
		strncpy(api_host, getenv("VXG_API_HOST"), sizeof(api_host)-1);
	if (getenv("VXG_API_PATH"))
		strncpy(api_path, getenv("VXG_API_PATH"), sizeof(api_path)-1);
	if (getenv("VXG_API_PASSWORD"))
		strncpy(api_password, getenv("VXG_API_PASSWORD"), sizeof(api_password)-1);
	if (getenv("PROXY_API_PATH"))
		strncpy(proxy_api_path, getenv("PROXY_API_PATH"), sizeof(proxy_api_path)-1);
	if (getenv("AUTH_TOKEN"))
		strncpy(token, getenv("AUTH_TOKEN"), sizeof(token)-1);
	if (getenv("DEVICE_SERIAL"))
		strncpy(device_sn, getenv("DEVICE_SERIAL"), sizeof(device_sn)-1);

	for (forward_item_index=0; forward_item_index < MAX_FORWARD_ITEMS; forward_item_index++)
		app_forward_items[forward_item_index].proto = PROTO_NONE;
	forward_item_index = 0;

	// Update variables from arguments
	for (n=1; n < argc; n++)
	{
		if (strcmp(argv[n], "--token") == 0)
		{
			if (n+1 >= argc)
			{
				fprintf(stderr, "auth token format: -t token (e.g. -t BASE64_ENCODED_TOKEN)\n");
				return false;
			}
			if (strlen(argv[n+1]) > sizeof(token)-1)
			{
				fprintf(stderr, "auth token is too long\n");
				return false;
			}
			strcpy(token, argv[++n]);
		}
		else if (strcmp(argv[n], "--serial") == 0)
		{
			if (n+1 >= argc)
			{
				fprintf(stderr, "device serial format: --serial DEVICE_SERIAL\n");
				return false;
			}
			if (strlen(argv[n+1]) > sizeof(device_sn))
			{
				fprintf(stderr, "device serial is too long\n");
				return false;
			}
			strcpy(device_sn, argv[++n]);
		}
		else if (strcmp(argv[n], "--debug") == 0)
		{
			fprintf(stdout, "--debug argument: adding more log verbosity\n");
			// TODO Konst
			// lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_DEBUG, NULL);
		}
		else if (strcmp(argv[n], "--http") == 0)
		{
			fprintf(stdout, "--http argument: switching to unencrypted HTTP\n");
			conn_port = 80;
			ssl_conn = LCCSCF_ALLOW_INSECURE;
		}
		else if (strcmp(argv[n], "--insecure") == 0)
		{
			fprintf(stdout, "--insecure argument: skip all certificate checks\n");
			ssl_conn |= LCCSCF_ALLOW_INSECURE | LCCSCF_ALLOW_EXPIRED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK | LCCSCF_ALLOW_SELFSIGNED;
		}
		else if ((strcmp(argv[n], "--forward") == 0 || strcmp(argv[n], "-f") == 0) && n+1 < argc && forward_item_index < MAX_FORWARD_ITEMS)
		{
			strcpy(buf, argv[++n]);
			p = buf;
			p_end = strchr(p, ':');
			if (!p_end || p_end == p)
			{
				fprintf(stderr, "forward argument format1: -f name:proto:host:port (e.g. -f fwd1:http:127.0.0.1:80)\n");
				return false;
			}
			*p_end = '\0';
			if (p_end-p > MAX_FORWARD_ITEM_NAME_LEN)
			{
				fprintf(stderr, "forward item name is too long\n");
				return false;
			}

			// forward name validation for uniqueness
			for (i=0; i < forward_item_index; i++)
			{
				if (strcmp(app_forward_items[i].name, p) == 0)
				{
					fprintf(stderr, "forward item name must be unique\n");
					return 1;
				}
			}

			strcpy(app_forward_items[forward_item_index].name, p);
			p = p_end+1;
			p_end = strchr(p, ':');
			if (!p_end || p_end == p)
			{
				fprintf(stderr, "forward argument format2: -f name:proto:host:port (e.g. -f fwd1:http:127.0.0.1:80)\n");
				return false;
			}
			*p_end = '\0';
			if (strcasecmp(p, "http") == 0)
				app_forward_items[forward_item_index].proto = PROTO_HTTP;
			else if (strcasecmp(p, "https") == 0)
				app_forward_items[forward_item_index].proto = PROTO_HTTPS;
			else if (strcasecmp(p, "tcp") == 0)
				app_forward_items[forward_item_index].proto = PROTO_TCP;
			else
			{
				fprintf(stderr, "forward proto must be one of: http, https, tcp\n");
				return false;
			}
			p = p_end+1;
			p_end = strchr(p, ':');
			if (!p_end || p_end == p)
			{
				fprintf(stderr, "forward argument format3: -f name:proto:host:port (e.g. -f fwd1:http:127.0.0.1:80)\n");
				return false;
			}
			*p_end = '\0';
			if (p_end-p > MAX_FORWARD_ITEM_HOST_LEN)
			{
				fprintf(stderr, "forward host is too long\n");
				return false;
			}
			strcpy(app_forward_items[forward_item_index].host, p);
			p = p_end+1;
			if (*p == '\0')
			{
				fprintf(stderr, "forward argument format4: -f name:proto:host:port (e.g. -f fwd1:http:127.0.0.1:80)\n");
				return false;
			}
			app_forward_items[forward_item_index].port = atoi(p);
			if (app_forward_items[forward_item_index].port < 1)
			{
				fprintf(stderr, "forward port is invalid\n");
				return false;
			}
			forward_item_index++;
		}
	}

	if (forward_item_index == 0)
	{
		fprintf(stderr, "Need at least one forward specified. Format: -f name:proto:host:port (e.g. -f fwd1:http:127.0.0.1:80)\n");
		return false;
	}
	return true;
}

/*
===========================================================================================================================
*/

int main(int argc, char** argv)
{
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGPIPE, SIG_IGN);

	// Parse agruments
	if (!parse_args(argc, argv))
	{
		fprintf(stderr, "Failed to parse arguments.\n");
		return -1;
	}

	// Create Uplink::Proxy object
	Derived_Proxy agent;

	// Get information from Camera
	#if !defined(__x86_64__) // ignore if x86
	if (agent.get_camera_info() != 0)
	{
		fprintf(stderr, "Failed to set parameters.\n");
		return -1;
	}
	#endif

	// Reboot loop
	while (reboot && !quit)
	{
		reboot = 0;

		fprintf(stdout, "Setting parameters.\n");

		// Set parameters
		agent.set_parameters(api_host, api_path, api_password, ws_host, 
							ws_path, device_sn, token, conn_port, 
							ssl_conn, &app_forward_items
							);

		fprintf(stdout, "Starting Uplink Client.\n");

		agent.start();

		// Spin main thread until stopped
		while (!quit && !agent.get_force_exit()) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}

		agent.stop();

		reboot = agent.get_restart();

		fprintf(stdout, "Stopped Uplink Client.\n");

		if (reboot && !quit) {
			fprintf(stdout, "Rebooting Uplink Client...\n");
		}
	}
} // end main