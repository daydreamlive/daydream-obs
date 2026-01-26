# FindFFmpeg.cmake
# Find FFmpeg libraries (avcodec, avutil, swscale)

include(FindPackageHandleStandardArgs)

# Build search paths based on platform
set(_FFMPEG_SEARCH_PATHS "")

# Common paths from CMAKE_PREFIX_PATH
foreach(_path ${CMAKE_PREFIX_PATH})
  list(APPEND _FFMPEG_SEARCH_PATHS "${_path}")
endforeach()

if(WIN32)
  # Windows: Look in obs-deps
  file(GLOB _OBS_DEPS_DIRS "${CMAKE_SOURCE_DIR}/.deps/obs-deps-*-x64")
  list(APPEND _FFMPEG_SEARCH_PATHS ${_OBS_DEPS_DIRS})
  
  find_path(AVCODEC_INCLUDE_DIR
    NAMES libavcodec/avcodec.h
    PATHS ${_FFMPEG_SEARCH_PATHS}
    PATH_SUFFIXES include
  )
  
  find_library(AVCODEC_LIBRARY
    NAMES avcodec avcodec-61 avcodec-60 avcodec-59 avcodec-58
    PATHS ${_FFMPEG_SEARCH_PATHS}
    PATH_SUFFIXES lib bin
  )
  
  find_library(AVUTIL_LIBRARY
    NAMES avutil avutil-59 avutil-58 avutil-57 avutil-56
    PATHS ${_FFMPEG_SEARCH_PATHS}
    PATH_SUFFIXES lib bin
  )
  
  find_library(SWSCALE_LIBRARY
    NAMES swscale swscale-8 swscale-7 swscale-6 swscale-5
    PATHS ${_FFMPEG_SEARCH_PATHS}
    PATH_SUFFIXES lib bin
  )
  
elseif(APPLE)
  # macOS: Look in obs-deps or homebrew
  file(GLOB _OBS_DEPS_DIRS "${CMAKE_SOURCE_DIR}/.deps/obs-deps-*-universal" "${CMAKE_SOURCE_DIR}/.deps/obs-deps-*-arm64" "${CMAKE_SOURCE_DIR}/.deps/obs-deps-*-x86_64")
  list(APPEND _FFMPEG_SEARCH_PATHS ${_OBS_DEPS_DIRS})
  list(APPEND _FFMPEG_SEARCH_PATHS "/opt/homebrew" "/usr/local")
  
  find_path(AVCODEC_INCLUDE_DIR
    NAMES libavcodec/avcodec.h
    PATHS ${_FFMPEG_SEARCH_PATHS}
    PATH_SUFFIXES include
  )
  
  find_library(AVCODEC_LIBRARY
    NAMES avcodec
    PATHS ${_FFMPEG_SEARCH_PATHS}
    PATH_SUFFIXES lib
  )
  
  find_library(AVUTIL_LIBRARY
    NAMES avutil
    PATHS ${_FFMPEG_SEARCH_PATHS}
    PATH_SUFFIXES lib
  )
  
  find_library(SWSCALE_LIBRARY
    NAMES swscale
    PATHS ${_FFMPEG_SEARCH_PATHS}
    PATH_SUFFIXES lib
  )
  
else()
  # Linux: Use pkg-config
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(PC_AVCODEC QUIET libavcodec)
    pkg_check_modules(PC_AVUTIL QUIET libavutil)
    pkg_check_modules(PC_SWSCALE QUIET libswscale)
  endif()
  
  find_path(AVCODEC_INCLUDE_DIR
    NAMES libavcodec/avcodec.h
    HINTS ${PC_AVCODEC_INCLUDEDIR} ${PC_AVCODEC_INCLUDE_DIRS}
    PATHS /usr/include /usr/local/include
  )
  
  find_library(AVCODEC_LIBRARY
    NAMES avcodec
    HINTS ${PC_AVCODEC_LIBDIR} ${PC_AVCODEC_LIBRARY_DIRS}
    PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib
  )
  
  find_library(AVUTIL_LIBRARY
    NAMES avutil
    HINTS ${PC_AVUTIL_LIBDIR} ${PC_AVUTIL_LIBRARY_DIRS}
    PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib
  )
  
  find_library(SWSCALE_LIBRARY
    NAMES swscale
    HINTS ${PC_SWSCALE_LIBDIR} ${PC_SWSCALE_LIBRARY_DIRS}
    PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib
  )
endif()

# Debug output
message(STATUS "FFmpeg search paths: ${_FFMPEG_SEARCH_PATHS}")
message(STATUS "AVCODEC_INCLUDE_DIR: ${AVCODEC_INCLUDE_DIR}")
message(STATUS "AVCODEC_LIBRARY: ${AVCODEC_LIBRARY}")
message(STATUS "AVUTIL_LIBRARY: ${AVUTIL_LIBRARY}")
message(STATUS "SWSCALE_LIBRARY: ${SWSCALE_LIBRARY}")

find_package_handle_standard_args(FFmpeg
  REQUIRED_VARS AVCODEC_LIBRARY AVUTIL_LIBRARY SWSCALE_LIBRARY AVCODEC_INCLUDE_DIR
)

if(FFmpeg_FOUND)
  set(FFmpeg_INCLUDE_DIRS ${AVCODEC_INCLUDE_DIR})
  set(FFmpeg_LIBRARIES ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY})
  
  if(NOT TARGET FFmpeg::avcodec)
    add_library(FFmpeg::avcodec UNKNOWN IMPORTED)
    set_target_properties(FFmpeg::avcodec PROPERTIES
      IMPORTED_LOCATION "${AVCODEC_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${AVCODEC_INCLUDE_DIR}"
    )
  endif()
  
  if(NOT TARGET FFmpeg::avutil)
    add_library(FFmpeg::avutil UNKNOWN IMPORTED)
    set_target_properties(FFmpeg::avutil PROPERTIES
      IMPORTED_LOCATION "${AVUTIL_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${AVCODEC_INCLUDE_DIR}"
    )
  endif()
  
  if(NOT TARGET FFmpeg::swscale)
    add_library(FFmpeg::swscale UNKNOWN IMPORTED)
    set_target_properties(FFmpeg::swscale PROPERTIES
      IMPORTED_LOCATION "${SWSCALE_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${AVCODEC_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(AVCODEC_INCLUDE_DIR AVCODEC_LIBRARY AVUTIL_LIBRARY SWSCALE_LIBRARY)
