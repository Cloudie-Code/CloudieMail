cmake_minimum_required(VERSION 3.25)
project(CloudieMail VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Keep the runtime consistent with statically linked MSVC dependencies.
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC CRT (/MT in Release, /MTd in Debug)")
endif()
option(CLOUDIEMAIL_GUI "Build desktop application" ON)
option(CLOUDIEMAIL_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(CLOUDIEMAIL_SODIUM_STATIC "Link a static libsodium library" OFF)
option(CLOUDIEMAIL_STATIC_GNU_RUNTIME "Embed libstdc++ and libgcc (other libraries remain shared)" OFF)
if(CLOUDIEMAIL_STATIC_GNU_RUNTIME)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "CLOUDIEMAIL_STATIC_GNU_RUNTIME requires GCC on Linux")
  endif()
  if(CLOUDIEMAIL_SANITIZERS)
    message(FATAL_ERROR "Use a separate dynamic-runtime build for sanitizers")
  endif()
  message(STATUS "Static GNU runtime enabled; GTK4, Firebird, libc and other dependencies remain shared")
endif()
include(cmake/Dependencies.cmake)

add_library(cloudiemail_core
    src/core/crypto.cpp
    src/core/config.cpp
    src/core/database.cpp
    src/net/dns.cpp
    src/net/server.cpp
    src/mail/protocols.cpp
    src/mail/imap.cpp
    src/mail/antispam.cpp
    src/mail/mail_message.cpp
    src/net/http.cpp
    src/app/setup.cpp
    src/platform/process.cpp
    src/app/setup_console.cpp
    src/app/wizard.cpp
    src/mail/outbound.cpp
    src/platform/linux/apache.cpp
    src/app/i18n.cpp
    src/mail/mime.cpp
)
file(READ "${CMAKE_SOURCE_DIR}/web/i18n.json" CM_LOCALES_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," CM_LOCALES_BYTES "${CM_LOCALES_HEX}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/web/i18n.json")
configure_file(resources/translations.hpp.in generated/translations.hpp @ONLY)
target_include_directories(cloudiemail_core PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_include_directories(cloudiemail_core PUBLIC include ${SODIUM_INCLUDE_DIR} PRIVATE ${FIREBIRD_INCLUDE_DIR})
target_link_libraries(cloudiemail_core PUBLIC OpenSSL::SSL OpenSSL::Crypto ${SODIUM_LIBRARY} nlohmann_json::nlohmann_json Threads::Threads PRIVATE CURL::libcurl ${FIREBIRD_LIBRARY})
target_compile_definitions(cloudiemail_core PUBLIC CLOUDIEMAIL_VERSION="${PROJECT_VERSION}")
if(CLOUDIEMAIL_STATIC_GNU_RUNTIME)
  target_link_options(cloudiemail_core PUBLIC -static-libstdc++ -static-libgcc)
endif()
if(CLOUDIEMAIL_SODIUM_STATIC)
  target_compile_definitions(cloudiemail_core PUBLIC SODIUM_STATIC)
endif()
if(MSVC)
  target_sources(cloudiemail_core PRIVATE src/platform/windows/webmail_windows.cpp)
  target_compile_options(cloudiemail_core PRIVATE /W4 /permissive- /utf-8)
  add_compile_options(/utf-8)
  target_compile_definitions(cloudiemail_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
  target_link_libraries(cloudiemail_core PUBLIC ws2_32 advapi32 httpapi crypt32 dnsapi)
else()
  target_link_libraries(cloudiemail_core PRIVATE resolv)
  target_compile_options(cloudiemail_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  if(CLOUDIEMAIL_SANITIZERS)
    target_compile_options(cloudiemail_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(cloudiemail_core PUBLIC -fsanitize=address,undefined)
  endif()
endif()
add_executable(cloudiemail src/app/main.cpp)
target_link_libraries(cloudiemail PRIVATE cloudiemail_core)
if(CLOUDIEMAIL_GUI)
  if(WIN32)
    add_executable(cloudiemail-gui WIN32 src/platform/windows/gui_windows.cpp)
    target_link_libraries(cloudiemail-gui PRIVATE cloudiemail_core shell32 comctl32 comdlg32 uxtheme)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.10)
    add_executable(cloudiemail-gui src/platform/linux/gui_gtk.cpp)
    target_link_libraries(cloudiemail-gui PRIVATE cloudiemail_core PkgConfig::GTK4)
  endif()
endif()
include(CTest)
include(cmake/Tests.cmake)
install(TARGETS cloudiemail RUNTIME DESTINATION bin)
if(CLOUDIEMAIL_GUI)
  install(TARGETS cloudiemail-gui RUNTIME DESTINATION bin)
endif()
install(DIRECTORY web/ DESTINATION share/cloudiemail/web)

if(NOT WIN32)
  configure_file(scripts/runtime/configure-apache.py scripts/configure-apache.py COPYONLY)
  install(PROGRAMS scripts/runtime/configure-apache.py DESTINATION scripts)
endif()
