# Project-local FatFs overlay; the installed ESP-IDF is never modified.
set(_geek_fatfs_src "$ENV{IDF_PATH}/components/fatfs")
set(_geek_fatfs_dst "${CMAKE_BINARY_DIR}/geek_components/fatfs")
if(NOT EXISTS "${_geek_fatfs_src}/src/ffconf.h")
    message(FATAL_ERROR "GEEK exFAT: ESP-IDF FatFs source is missing")
endif()
file(READ "${_geek_fatfs_src}/src/ffconf.h" _geek_ffconf)
if(NOT _geek_ffconf MATCHES "#define FF_FS_EXFAT[ \t]+0")
    message(FATAL_ERROR "GEEK exFAT: unexpected SDK configuration; review overlay before building")
endif()
file(COPY "${_geek_fatfs_src}/" DESTINATION "${_geek_fatfs_dst}"
    PATTERN "test_apps" EXCLUDE PATTERN "host_test" EXCLUDE
    PATTERN "test_fatfsgen" EXCLUDE PATTERN "__pycache__" EXCLUDE)
string(REGEX REPLACE "#define FF_FS_EXFAT[ \t]+0" "#define FF_FS_EXFAT 1" _geek_ffconf "${_geek_ffconf}")
file(WRITE "${_geek_fatfs_dst}/src/ffconf.h" "${_geek_ffconf}")
list(APPEND EXTRA_COMPONENT_DIRS "${_geek_fatfs_dst}")
message(STATUS "GEEK exFAT: using project-local FatFs overlay: ${_geek_fatfs_dst}")
