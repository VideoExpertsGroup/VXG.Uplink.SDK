#ifndef VXG_PROXY_CLIENT_H
#define VXG_PROXY_CLIENT_H

/**
 * Log line sink compatible with libwebsockets (int level, const char *line).
 * Do not include libwebsockets.h here — OpenSDK sysroots may ship headers
 * without lws_log_emit_function / LLL_* for C++ consumers.
 */
typedef void (*vxg_proxy_client_log_emit_fn)(int level, const char *line);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Install libwebsockets log routing before vxg_proxy_client_run().
 * @param level_mask LLL_* bitmask (e.g. LLL_USER | LLL_ERR | LLL_WARN).
 * @param emit_fn    Callback, or NULL for libwebsockets default.
 */
void vxg_proxy_client_set_lws_log_emit(int level_mask, vxg_proxy_client_log_emit_fn emit_fn);

/**
 * Optional tick after each lws_service() poll (for example a keep-alive).
 */
typedef void (*vxg_proxy_client_idle_fn)(void *userdata);
void vxg_proxy_client_set_idle_callback(vxg_proxy_client_idle_fn fn, void *userdata);

/**
 * Run the uplink / proxy client until shutdown (blocks the calling thread).
 * Same argv/env contract as the standalone binary (see vxg_proxy_client.c main).
 */
int vxg_proxy_client_run(int argc, char *const *argv);

/** Ask the client to exit the lws loop (e.g. app shutdown or signal). */
void vxg_proxy_client_request_stop(void);

int vxg_proxy_client_get_force_exit(void);
int vxg_proxy_client_get_restart(void);

#ifdef __cplusplus
}
#endif

#endif /* VXG_PROXY_CLIENT_H */
