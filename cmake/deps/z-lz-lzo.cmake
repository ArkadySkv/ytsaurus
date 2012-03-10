if (UNIX)
  set( LTYPE SHARED )
endif()

if (WIN32)
  set( LTYPE STATIC )
endif()

set( BASE ${CMAKE_SOURCE_DIR}/contrib/libs )

add_library( ytext-fastlz ${LTYPE}
  ${BASE}/fastlz/fastlz.c
)

add_library( ytext-lz4 ${LTYPE}
  ${BASE}/lz4/lz4.c
)

add_library( ytext-minilzo ${LTYPE}
  ${BASE}/minilzo/minilzo.c
)

add_library( ytext-quicklz ${LTYPE}
  ${BASE}/quicklz/quicklz.c
)

add_library( ytext-snappy ${LTYPE}
  ${BASE}/snappy/snappy.cc
  ${BASE}/snappy/snappy-sinksource.cc
  ${BASE}/snappy/snappy-stubs-internal.cc
  ${BASE}/snappy/snappy-c.cc
)

add_library( ytext-zlib ${LTYPE}
  ${BASE}/zlib/adler32.c
  ${BASE}/zlib/compress.c
  ${BASE}/zlib/crc32.c
  ${BASE}/zlib/deflate.c
  ${BASE}/zlib/gzio.c
  ${BASE}/zlib/infback.c
  ${BASE}/zlib/inffast.c
  ${BASE}/zlib/inflate.c
  ${BASE}/zlib/inftrees.c
  ${BASE}/zlib/trees.c
  ${BASE}/zlib/uncompr.c
  ${BASE}/zlib/zutil.c
)

include_directories(
  ${CMAKE_SOURCE_DIR}
)

if (YT_BUILD_WITH_STLPORT)
  target_link_libraries( ytext-snappy stlport )
  if (CMAKE_COMPILER_IS_GNUCXX)
    set_target_properties( ytext-snappy PROPERTIES LINK_FLAGS "-nodefaultlibs" )
  endif()
endif()

set_target_properties( ytext-fastlz PROPERTIES
  VERSION   0.1.0
  SOVERSION 0.1
)

set_target_properties( ytext-lz4 PROPERTIES
  VERSION   0.1.0
  SOVERSION 0.1
)

set_target_properties( ytext-minilzo PROPERTIES
  VERSION   2.0.6
  SOVERSION 2.0
)

set_target_properties( ytext-quicklz PROPERTIES
  VERSION   1.5.0
  SOVERSION 1.5
)

set_target_properties( ytext-snappy PROPERTIES
  VERSION   1.0.4
  SOVERSION 1.0
)

set_target_properties( ytext-zlib PROPERTIES
  VERSION   1.2.3
  SOVERSION 1.2.3
)

set_target_properties( ytext-zlib PROPERTIES
  COMPILE_DEFINITIONS BUILD_ZLIB
)

install(
  TARGETS
  ytext-fastlz
  ytext-lz4
  ytext-minilzo
  ytext-snappy
  ytext-zlib
  LIBRARY DESTINATION lib
)
