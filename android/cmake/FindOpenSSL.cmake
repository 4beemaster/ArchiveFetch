set(OPENSSL_FOUND TRUE)
set(OpenSSL_FOUND TRUE)
set(OPENSSL_VERSION "1.1.1")
set(OPENSSL_INCLUDE_DIR "${boringssl_SOURCE_DIR}/include")
set(OPENSSL_SSL_LIBRARY ssl)
set(OPENSSL_CRYPTO_LIBRARY crypto)
set(OPENSSL_LIBRARIES ssl crypto)
set(OPENSSL_SSL_LIBRARIES ssl crypto)
set(OPENSSL_CRYPTO_LIBRARIES crypto)
if(NOT TARGET OpenSSL::Crypto)
    add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
    set_target_properties(OpenSSL::Crypto PROPERTIES
        IMPORTED_LOCATION "${boringssl_BINARY_DIR}/libcrypto.a"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}")
    add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
    set_target_properties(OpenSSL::SSL PROPERTIES
        IMPORTED_LOCATION "${boringssl_BINARY_DIR}/libssl.a"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES OpenSSL::Crypto)
endif()