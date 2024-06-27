# the name of the target operating system
set(CMAKE_SYSTEM_NAME Linux)

# which compilers to use for C and C++
set(CMAKE_C_COMPILER   armv7-cortex_A7-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER armv7-cortex_A7-linux-gnueabihf-g++)

# where is the target environment located
set(CMAKE_FIND_ROOT_PATH  /opt/platfrom/platform-version/armv7-cortex_A7-linux-gnueabihf)

# adjust the default behavior of the FIND_XXX() commands:
# search programs in the host environment
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# search headers and libraries in the target environment
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_C_FLAGS "-O2 -g -Wall -fmessage-length=0")
set(CMAKE_CXX_FLAGS "-O2 -g -Wall -fmessage-length=0")