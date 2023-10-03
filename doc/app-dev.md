Application Development         {#app-dev}
==============

## Overview

An application that uses VXG Uplink Client Library should implement the Uplink::Proxy class derived from the base classes provided by the library:
- @ref Uplink::Proxy "Uplink::Proxy" - common implementation class, used for obtaining camera information such as serial number and MAC Address.

Any Proxy implementation should implement the get_serial_number, get_mac_address, and get_camera_info functions.

The library provides the stub implementation for most of the virtual methods of these classes, the stub implementation prints a log message about this method is not implemented and returns an error, the final application should implement all virtual methods on its own.

### Linking application against the VXG Uplink Client Library
There are 3 possible ways of how to build and link your application
1. Building the application inside the VXG Uplink Client library's `Meson project`, the app will be assembled during the library project compilation in this case.  
You need to add a new executable target into the main `meson.build` file, please refer to the example app build target declaration:
\snippet app/meson.build meson-example-target
User must declare own executable target with a list of sources and dependencies, user may need to declare own dependencies if application requires it.  
  
  **This method is not recommended as it makes updating of the VXG Uplink Client library mostly not possible or very difficult for application developer**
2. Building your app using your own build system and linking against the installed library.  
  Running the `install` step from the [compile](@ref build) section installs the binary libraries and headers into the directory you specified during the `setup` step, it also puts the `pkg-config`'s `.pc` files into the prefix directory which could be used by your own build system.
  
3. Preferred and recommended way of application development is to hold the app as a separate `Meson project` and use the VXG Uplink Client library as a `Meson subproject` of the application's `Meson project`.  
Using this approach gives the most flexible and convenient workflow for updating the VXG Uplink Library, all library dependencies will be promoted to the main project and will be also accessible by the application.  
 **How does it work**
 - Assuming you have a `Meson` build system [installed](@ref build)
 - Start a new `Meson project` with a following command:
 ```
 meson init -l cpp -n your-project-name
 ```
 - As a result of this command you should have the following files tree:
 ```
 |-- meson.build
 |-- your_project_name.cpp
 ```
 - Add VXG Uplink Client library as a `Meson subproject`  
 All subprojects should be located in the `subprojects` directory so you have to create it first
 ```
 mkdir subprojects
 ```
 Now you have 2 options depending on how you want to store the VXG Uplink Client library sources:
  1. If you want to store the VXG Uplink Client library as a files tree locally.
   + Create a symlink to the library path inside the subprojects dir:
   ```
   ln -s path/to/vxgproxyclient subprojects/vxgproxyclient
   ```
   Or you can just move vxgproxyclient directory inside the subprojects dir.
   + Create a library's `Meson` wrap file inside the subprojects dir, the name of the file should be the same as symlink you created in 1.1 and the content of the file should be:
   ```
   [wrap-file]
   directory = vxgproxyclient

   [provide]
   vxgproxyclient = vxgproxyclient_dep
   ```
  2. If you want to store the library in a git repository you just need to create a wrap file with the content like below:
   ```
   [wrap-git]
   url=https://your-git-repo-url.com/path/vxgproxyclient.git
   # You can specify tag, branch or commit hash as revision
   revision=master

   [provide]
   vxgproxyclient = vxgproxyclient_dep
   ```
 You can find the example app `Meson project` in the example/app directory of the VXG Uplink library sources package.
