Library Compilation Guide                         {#compile}
============

### Library build process
Here is a compilation quickstart guide:
* First of all you need to have a build system and toolchain [installed](@ref build) 
* **Setup** the build directory
  ```bash
  meson setup --prefix=path/to/install --strip -Dbuildtype=debug builddir/

  # --prefix=path specifies the installation path
  # --strip indicates that final binaries should be stripped
  # -Dbuildtype= specifies the debug/release build type, please check the Meson docs about full list of the build types.
  ```
* **Build**
  ```bash
  meson compile -C builddir
  # Or
  ninja -C builddir
  ```
* **Install**
  ```bash
  meson install -C builddir
  # Or
  ninja -C builddir/ install
  ```
  As a result of the `install` step you should have the library compiled and installed into the prefix directory you specified during the `setup` step.
* **Clean**
  ```bash
  ninja -C builddir clean
  ```
  Or you can just delete the builddir, you will need to `setup` it again in this case.
  ```bash
  rm -rf builddir
  ```

### Cross-compilation
* By default [Meson](https://mesonbuild.com) builds project for the host platform, but it's also possible to cross-compile the library and your application using [Meson](https://mesonbuild.com).
* Full [Meson](https://mesonbuild.com) cross-compilation documentation can be found [here](https://mesonbuild.com/Cross-compilation.html#cross-compilation).
* The difference between the host compilation described above and the cross-compilation is the additional `--cross-file=path/to/cross-file.txt` flag for the Meson `Setup` step, the `Setup` command should look like below:
  ```bash
  meson setup --prefix=path/to/install --strip -Dbuildtype=debug --cross-file=path/to/cross-file.txt builddir/
  ```
  `cross-file.txt` is the target platform description which in terms of Meson called a `cross-file`.
* `cross-file` example below is for the Debian provided `arm-linux-gnueabihf` toolchain installable using the Ubuntu's package manager command
  ```bash
  sudo apt install g++-arm-linux-gnueabihf
  ```
* Example of the ARMv7 `cross-file` can be found in `/cross` directory:
 <!-- \snippet ../cross/arm-example.txt cross-file-example -->