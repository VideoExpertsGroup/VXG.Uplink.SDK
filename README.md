# ONVIF Uplink Camera Plugin for VXG Cloud

Learn more about <a href="https://www.videoexpertsgroup.com">Cloud Video Surveillance</a>

The `ONVIF Uplink Camera Plugin` is a simple `C++` reference code for integration of `IP cameras` with `VXG Cloud`.
<br>
<br>
This library requires requires libwebsockets version 4.0 (4.0.20-2) as a minimum.
<br>
<br>
A complete documentation can be found in the `/doc-${version}.pdf` file.
<br>

## How to build locally for x86

### Setup build system
This projects requires Meson version >= 0.56.0
```
sudo apt-get update
```
Install Dependencies
```
sudo apt-get install -y python3-pip git ninja-build curl tzdata python3-tz
```
Install Meson
```
pip3 install git+https://github.com/mesonbuild/meson@0.56.0
```
Add Meson to path
```
echo ’export PATH=$HOME/.local/bin:$PATH’ » $HOME/.bashrc
```
Check currently installed meson version
```
meson -v
```

### Setup the build directory
```bash
meson setup --prefix=path/to/install --strip -Dbuildtype=debug builddir/
--prefix=path specifies the installation path
--strip indicates that final binaries should be stripped
-Dbuildtype= specifies the debug/release build type, please check the Meson docs about full list of the build types
```
### Build
```
meson compile -C builddir
```

### Install
```
meson install -C builddir
```
As a result of the install step you should have the application and library compiled and installed into the prefix directory
you specified during the setup step.
### Clean
```
ninja -C builddir clean
```
or you can just delete the builddir, you will need to setup it again in this case.
```
rm -rf builddir
```

## Cross compilation

if toolchain is in /opt/:

```bash
export PATH=/opt/toolchain-path/bin:$PATH
meson setup --cross-file cross/cross-file.txt builddir-cross
meson compile -C builddir-cross
```

## How to get ACCESS TOKEN
Create camera on Cloud VMS or using Cloud VMS API.
![Adding-uplink-camera-preview](https://github.com/VideoExpertsGroup/VXG.Uplink.SDK-/assets/43914253/fc2b6978-097e-47ba-9f00-48cc0eaad026)
Once camera created, an `ACCESS TOKEN` should be associated and created along with camera.

## How to execute application
### Starting application using access token
```
./vxg-proxy-client --token $ACCESS_TOKEN -f "$HTTP_FORWARD_NAME":http:$IP:80 -f "$RTSP_FORWARD_NAME":tcp:$IP:554
```
where `$ACCESS_TOKEN` is your camera's access token,
<br>
      `$HTTP_FORWARD_NAME` is the name you want for your HTTP proxy. Ex: camera-1-http,
<br>
      `$RTSP_FORWARD_NAME` is the name you want for your RTSP proxy. Ex: camera-1-rtsp,
<br>
      `$IP` is the local IP of the device you want to proxy. Ex: 127.0.0.1
<br>
Port numbers can be modified as well depending on how device is configured.

### Starting application using device serial number and MAC Address
#### Configure camera
Add camera to VMS Platform using the provisioning server option and enter the device's Serial Number and MAC Address where specified.
#### Start application
```
VXG_API_PASSWORD=$DEVICE_MAC ./vxg-proxy-client --serial $DEVICE_SERIAL -f "$HTTP_FORWARD_NAME":http:$IP:80 -f "$RTSP_FORWARD_NAME":tcp:$IP:554
```
where `$DEVICE_MAC` is the device's MAC Address
<br>
      `$DEVICE_SERIAL` is the device's serial number
<br>
Ideally the application will retireve the device info and adding the `VXG_API_PASSWORD` and `--serial` options won't be neccessary.

## How to check if application is working with webplayer
Open [VXG WebPlayer SDK](https://github.com/VideoExpertsGroup/VXG.WebPlayer.SDK "VXG WebPlayer SDK") and enter the camera's access token in the access token field.
