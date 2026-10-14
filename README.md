# ONVIF Uplink Camera Plugin for VXG Cloud

Learn more about <a href="https://www.videoexpertsgroup.com">Cloud Video Surveillance</a>\
Learn more about usage and development in our <a href="https://help.videoexpertsgroup.com/kb/camera-uplink-agent">Knowledge Base</a>

The `ONVIF Uplink Camera Plugin` is a `C` reference client for integrating IP cameras with VXG Cloud.

It requires libwebsockets 4.5.8. CMake looks up that library only under `VXG_DEPS_PREFIX/lws` (`lws/lib` and `lws/include`). The default prefix is `/install`. OpenSSL and zlib are found under `ssl/` and `zlib/` in that prefix, or from paths you pass at configure time.

## Build

### Ubuntu

1. Install build tools and SSL/zlib development packages.

```
sudo apt update && sudo apt install -y build-essential cmake git libssl-dev zlib1g-dev
```

2. Build libwebsockets 4.5.8 into a local prefix.

```
git clone --branch v4.5.8 --depth 1 https://github.com/warmcat/libwebsockets.git /tmp/lws-4.5.8
cmake -S /tmp/lws-4.5.8 -B /tmp/lws-4.5.8-build \
  -DCMAKE_INSTALL_PREFIX="$PWD/.deps/install/lws" \
  -DLWS_WITH_SSL=ON -DLWS_WITHOUT_TESTAPPS=ON -DLWS_WITHOUT_TEST_SERVER=ON
cmake --build /tmp/lws-4.5.8-build -j"$(nproc)"
cmake --install /tmp/lws-4.5.8-build
```

3. Configure and build the client. Point OpenSSL and zlib at the system packages.

```
cmake -S . -B build \
  -DVXG_DEPS_PREFIX="$PWD/.deps/install" \
  -DOPENSSL_ROOT_DIR=/usr \
  -DZLIB_ROOT=/usr
cmake --build build
```

The binary is `build/vxg-proxy-client`.

### macOS

1. Install dependencies and build libwebsockets 4.5.8.

```
brew install cmake openssl@3 zlib git
rm -rf /tmp/lws-4.5.8 /tmp/lws-4.5.8-build
git clone --branch v4.5.8 --depth 1 https://github.com/warmcat/libwebsockets.git /tmp/lws-4.5.8
cmake -S /tmp/lws-4.5.8 -B /tmp/lws-4.5.8-build -DLWS_WITH_SSL=ON -DLWS_WITHOUT_TESTAPPS=ON -DLWS_WITHOUT_TEST_SERVER=ON
cmake --build /tmp/lws-4.5.8-build -j4
cmake --install /tmp/lws-4.5.8-build
```

2. Link the installed library into the prefix this project expects, then build.

```
mkdir -p .deps/install/lws/lib .deps/install/lws/include .deps/install/ssl .deps/install/zlib
ln -sf /usr/local/lib/libwebsockets.dylib .deps/install/lws/lib/libwebsockets.dylib
ln -sf /usr/local/include/libwebsockets .deps/install/lws/include/libwebsockets
cmake -S . -B build \
  -DVXG_DEPS_PREFIX="$PWD/.deps/install" \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
  -DZLIB_ROOT="$(brew --prefix zlib)"
cmake --build build -j4
```

On Apple Silicon, Homebrew libraries are under `/opt/homebrew` instead of `/usr/local`. Point the `ln -sf` commands at `$(brew --prefix)/lib` and `$(brew --prefix)/include`.

### Cross-compiling for embedded Linux

The target sysroot must provide target-architecture builds of OpenSSL and zlib. You must also cross-compile libwebsockets 4.5.8; libraries installed on the build host cannot be linked into the target binary.

1. Create a CMake toolchain file for the target. For example, save the following as `toolchains/arm-linux.cmake` and replace the compiler and sysroot paths with values from your platform SDK:

```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER /opt/toolchain/bin/arm-linux-gnueabihf-gcc)
set(CMAKE_SYSROOT /opt/toolchain/arm-linux-gnueabihf/sysroot)
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")

# Build tools run on the host; headers, libraries, and packages come from the target.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
```

2. Prepare a dependency prefix containing target builds with this layout:

```text
.deps/target/
├── lws/
│   ├── include/
│   └── lib/libwebsockets.a
├── ssl/
│   ├── include/
│   └── lib/
└── zlib/
    ├── include/
    └── lib/
```

If your platform SDK does not already include compatible target builds, cross-compile each dependency with the same toolchain. For libwebsockets:

```
cmake -S /path/to/libwebsockets-4.5.8 -B build-lws-target \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchains/arm-linux.cmake" \
  -DCMAKE_INSTALL_PREFIX="$PWD/.deps/target/lws" \
  -DOPENSSL_ROOT_DIR="$PWD/.deps/target/ssl" \
  -DZLIB_ROOT="$PWD/.deps/target/zlib" \
  -DLWS_WITH_SSL=ON \
  -DLWS_WITH_SHARED=OFF \
  -DLWS_WITH_STATIC=ON \
  -DLWS_WITHOUT_TESTAPPS=ON \
  -DLWS_WITHOUT_TEST_SERVER=ON
cmake --build build-lws-target
cmake --install build-lws-target
```

3. Configure and build the client with the same toolchain and dependency prefix:

```
cmake -S . -B build-target \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchains/arm-linux.cmake" \
  -DVXG_DEPS_PREFIX="$PWD/.deps/target" \
  -DOPENSSL_ROOT_DIR="$PWD/.deps/target/ssl" \
  -DZLIB_ROOT="$PWD/.deps/target/zlib"
cmake --build build-target
```

The target binary is `build-target/vxg-proxy-client`. Verify its architecture and dynamic dependencies with the inspection tools supplied by your SDK (for example, `file` and the target-prefixed `readelf`) before deploying it. If the target does not provide the required shared libraries, build static dependencies or package the matching shared libraries with the application.

### Provisioning server address

By default the client requests an access token from `camera.vxg.io`. Pass `VXG_DEFAULT_API_HOST` at configure time to compile in a different provisioning hostname. This applies to the native and cross builds above. Add it to the client `cmake` command:

```
-DVXG_DEFAULT_API_HOST=camera.example.com
```

For example:

```
cmake -S . -B build \
  -DVXG_DEPS_PREFIX="$PWD/.deps/install" \
  -DOPENSSL_ROOT_DIR=/usr \
  -DZLIB_ROOT=/usr \
  -DVXG_DEFAULT_API_HOST=camera.example.com
```

Use the hostname only, without a scheme or path. The token request path stays `/v1/token?serial_id=%s` unless `VXG_API_PATH` is set at runtime. If `VXG_API_HOST` is set when the client runs, that value replaces the hostname compiled in with `VXG_DEFAULT_API_HOST`.

## Example application usage

### Command-line options

```
--token <AUTH_TOKEN> (Access token for the camera, generated by VMS)
--serial <DEVICE_SERIAL> (Serial number of the device)
--http (disable HTTPS)
--insecure (do not verify HTTPS certificates)
--debug (enable debug logs)
-f <FWD_NAME>:http|https|tcp:[<IPV4_ADDRESS|HOSTNAME|IPV6_ADDRESS>]:<TARGET_PORT>
```

### Environment variables

`VXG_API_HOST` — hostname of the VXG Camera API. When unset, the client uses the hostname compiled in with `VXG_DEFAULT_API_HOST` (default: `camera.vxg.io`)

`VXG_API_PATH` — template for the VXG Camera API token request path and query string (default: `/v1/token?serial_id=%s`; `%s` is replaced with the device serial number at runtime)

`VXG_API_PASSWORD` — API password or authorization (required when `--token` is not set)

`PROXY_API_PATH` — URI path for requesting the websocket endpoint from the proxy API (default: `/api/ws-endpoint`)

`AUTH_TOKEN` — access token, as an alternative to `--token`

`DEVICE_SERIAL` — device serial number, as an alternative to `--serial`

### Running the application

There are two ways to run the sample application.

#### Using the provisioning server to retrieve an access token

Create an uplink camera in VMS with the camera serial number and MAC address. The client then requests a token itself.

```
VXG_API_PASSWORD=<MAC_ADDRESS> ./build/vxg-proxy-client --serial <SERIAL_NUMBER> -f fwd1:http:127.0.0.1:80 -f fwd2:tcp:127.0.0.1:554
```

`fwd1:http:127.0.0.1:80` forwards local port 80, typically the camera web UI.
`fwd2:tcp:127.0.0.1:554` forwards local port 554, typically the camera RTSP stream.

#### Using an access token from VMS

This is mainly for testing. Create the token in VMS and pass it on the command line. The client does not call the provisioning server.

```
echo '{"camid": 123456, "uplink": "dev-api.proxy.cloud-vms.com"}' | base64
```

That example encodes to:

```
eyJjYW1pZCI6IDEyMzQ1NiwgInVwbGluayI6ICJkZXYtYXBpLnByb3h5LmNsb3VkLXZtcy5jb20ifQo=
```

```
./build/vxg-proxy-client --token eyJjYW1pZCI6IDEyMzQ1NiwgInVwbGluayI6ICJkZXYtYXBpLnByb3h5LmNsb3VkLXZtcy5jb20ifQo= -f fwd1:http:127.0.0.1:80 -f fwd2:tcp:127.0.0.1:554
```

### Checking the connection

Request connection info for a camera from the uplink server API. `DEVICE_ID` is the `camid` value from the access token. `AUTH_TOKEN` and `PROXY_API_TOKEN` are different credentials.

```
curl -H 'Authorization: Bearer <PROXY_API_TOKEN>' https://<PROXY_API_HOSTNAME>/api/device/<DEVICE_ID>
```

Example response:

```
{
     "camid": "123456",
     "proxy_id": "f2f8b6edd7ebbdbd4100",
     "forwards": [
         {
            "name": "fwd1",
            "protocol": "tcp",
            "host": "google.com",
            "port": 443,
            "proxy_port": 38103,
            "proxy_host": "beeeaacf8af994911700.proxy.cloud-vms.com"
         }
     ]
}
```

`proxy_host`:`proxy_port` is the address that reaches the forwarded host and port.
