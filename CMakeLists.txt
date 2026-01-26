cmake_minimum_required(VERSION 3.28...3.30)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
set(CMAKE_XCODE_ATTRIBUTE_CLANG_ENABLE_EXPLICIT_MODULES NO)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)
include(FetchContent)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(sctp_werror OFF CACHE BOOL "" FORCE)

if(OS_MACOS)
  foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${_prefix}/lib/libssl.a" AND EXISTS "${_prefix}/lib/libcrypto.a")
      set(OPENSSL_ROOT_DIR "${_prefix}" CACHE PATH "" FORCE)
      set(OPENSSL_INCLUDE_DIR "${_prefix}/include" CACHE PATH "" FORCE)
      set(OPENSSL_SSL_LIBRARY "${_prefix}/lib/libssl.a" CACHE FILEPATH "" FORCE)
      set(OPENSSL_CRYPTO_LIBRARY "${_prefix}/lib/libcrypto.a" CACHE FILEPATH "" FORCE)
      message(STATUS "Using OpenSSL from obs-deps: ${_prefix}")
      break()
    endif()
  endforeach()
endif()

find_package(OpenSSL REQUIRED)

FetchContent_Declare(
  libdatachannel
  GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
  GIT_TAG v0.22.3
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(libdatachannel)

foreach(_target usrsctp juice-static srtp2 datachannel-static)
  if(TARGET ${_target})
    get_target_property(_opts ${_target} COMPILE_OPTIONS)
    if(_opts)
      list(FILTER _opts EXCLUDE REGEX "-Werror|/WX")
      set_target_properties(${_target} PROPERTIES COMPILE_OPTIONS "${_opts}")
    endif()
    target_compile_options(
      ${_target}
      PRIVATE
        $<$<C_COMPILER_ID:GNU>:-Wno-error
        -Wno-unused-parameter>
        $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-error
        -Wno-shorten-64-to-32
        -Wno-unused-parameter
        -fno-modules>
        $<$<C_COMPILER_ID:MSVC>:/WX-
        /wd4319
        /wd4267>
        $<$<CXX_COMPILER_ID:GNU>:-Wno-error
        -Wno-unused-parameter>
        $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-error
        -Wno-shorten-64-to-32
        -Wno-unused-parameter
        -Wno-unreachable-code
        -fno-modules>
        $<$<CXX_COMPILER_ID:MSVC>:/WX-
        /wd4319
        /wd4267>
    )
    set_target_properties(${_target} PROPERTIES XCODE_ATTRIBUTE_CLANG_ENABLE_EXPLICIT_MODULES NO)
  endif()
endforeach()

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(CURL REQUIRED)
find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil swscale)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    OBS::libobs
    CURL::libcurl
    LibDataChannel::LibDataChannelStatic
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::swscale
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/daydream-filter.c
    src/daydream-api.c
    src/daydream-auth.c
    src/daydream-encoder.c
    src/daydream-decoder.c
    src/daydream-whip.cpp
    src/daydream-whep.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
