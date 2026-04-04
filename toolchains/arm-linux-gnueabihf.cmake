set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_PREFIX arm-linux-gnueabihf)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)

# Where the target sysroot lives (optional but recommended)
set(CMAKE_SYSROOT /)

#set(CMAKE_FIND_ROOT_PATH
#    ${CMAKE_SYSROOT}
#)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Add the specific armhf library path to the search list
list(APPEND CMAKE_PREFIX_PATH /usr/lib/arm-linux-gnueabihf)
# Point to the armhf pkgconfig directories
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig")

# Prevent pkg-config from searching your host (x86_64) paths
set(ENV{PKG_CONFIG_PATH} "")