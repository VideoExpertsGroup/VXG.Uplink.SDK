Build System                         {#build}
============

### Overview
VXG Uplink Client library uses [Meson](https://mesonbuild.com) build system as a modern, fast and flexible build system that supports easy to set up and maintain a cross-compilation process.

It's recommended to refer to the [Meson](https://mesonbuild.com) guide.

### Build system installation
**IMPORTANT: This projects requires Meson version >= 0.56.0**

It's recommended to use [Ubuntu 20.04 LTS](https://releases.ubuntu.com/20.04/) distribution in development process but other distributions or operation systems are also supported by [Meson](https://mesonbuild.com).

Please refer to [Meson installation guide](https://mesonbuild.com/Getting-meson.html) to get and install `Meson`, preferable way to install `Meson` is `pip` method.

Quick install guide for Ubuntu 20.04.
If you have an old version of meson already installed please remove it first.
```bash
sudo apt-get update
sudo apt-get install -y python3-pip git ninja-build curl tzdata python3-tz
pip3 install git+https://github.com/mesonbuild/meson@0.56.0
# pip3 puts meson main script into the $HOME/.local/bin/ directory, you need to
# add $HOME/.local/bin/ into your PATH environment variable, for bash shell you
# can run the following command and restart the shell session.
echo 'export PATH=$HOME/.local/bin:$PATH' >> $HOME/.bashrc
# Check currently installed meson version
meson -v
```
