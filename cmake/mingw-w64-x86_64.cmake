# CMake toolchain file for MinGW-w64 cross-compilation from Linux
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc-posix)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++-posix)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX} /opt/openssl-mingw64)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Help find_package(OpenSSL) locate the cross-compiled OpenSSL
set(OPENSSL_ROOT_DIR /opt/openssl-mingw64)
set(OPENSSL_INCLUDE_DIR /opt/openssl-mingw64/include)
set(OPENSSL_SSL_LIBRARY /opt/openssl-mingw64/lib64/libssl.a)
set(OPENSSL_CRYPTO_LIBRARY /opt/openssl-mingw64/lib64/libcrypto.a)
