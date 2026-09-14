cmake_minimum_required(VERSION 3.16)
project(rainmeter_linux VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo cairo-xlib)
pkg_check_modules(PANGO REQUIRED IMPORTED_TARGET pangocairo)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XRENDER REQUIRED IMPORTED_TARGET xrender)
pkg_check_modules(XRANDR REQUIRED IMPORTED_TARGET xrandr)
pkg_check_modules(XEXT REQUIRED IMPORTED_TARGET xext)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-8)
pkg_check_modules(GDK_PIXBUF REQUIRED IMPORTED_TARGET gdk-pixbuf-2.0)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

add_library(lua51 STATIC
  third_party/lua51/lapi.c third_party/lua51/lcode.c third_party/lua51/ldebug.c
  third_party/lua51/ldo.c third_party/lua51/ldump.c third_party/lua51/lfunc.c
  third_party/lua51/lgc.c third_party/lua51/llex.c third_party/lua51/lmem.c
  third_party/lua51/lobject.c third_party/lua51/lopcodes.c third_party/lua51/lparser.c
  third_party/lua51/lstate.c third_party/lua51/lstring.c third_party/lua51/ltable.c
  third_party/lua51/ltm.c third_party/lua51/lundump.c third_party/lua51/lvm.c
  third_party/lua51/lzio.c third_party/lua51/lauxlib.c third_party/lua51/lbaselib.c
  third_party/lua51/ldblib.c third_party/lua51/liolib.c third_party/lua51/lmathlib.c
  third_party/lua51/loslib.c third_party/lua51/ltablib.c third_party/lua51/lstrlib.c
  third_party/lua51/loadlib.c third_party/lua51/linit.c)
target_include_directories(lua51 PUBLIC third_party/lua51)
target_compile_definitions(lua51 PRIVATE LUA_USE_LINUX)
target_link_libraries(lua51 PUBLIC m dl)

add_library(rainmeter_core
  src/math_parser.cpp
  src/runtime.cpp
)
target_include_directories(rainmeter_core PUBLIC src)
target_link_libraries(rainmeter_core PUBLIC
  PkgConfig::CAIRO PkgConfig::PANGO PkgConfig::FONTCONFIG PkgConfig::GIO PkgConfig::PCRE2
  PkgConfig::GDK_PIXBUF lua51)
target_compile_options(rainmeter_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(xrainmeter src/main.cpp)
target_link_libraries(xrainmeter PRIVATE rainmeter_core PkgConfig::GTK3 PkgConfig::X11 PkgConfig::XRENDER PkgConfig::XRANDR PkgConfig::XEXT)
target_compile_options(xrainmeter PRIVATE -Wall -Wextra -Wpedantic)

add_executable(rainmeter-manager src/manager.cpp)
set_target_properties(rainmeter-manager PROPERTIES OUTPUT_NAME rainmeter)
target_link_libraries(rainmeter-manager PRIVATE PkgConfig::GTK3 PkgConfig::X11 PkgConfig::XRANDR)
target_compile_options(rainmeter-manager PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS xrainmeter rainmeter-manager RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS tools/extract_rmskin.py DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS tools/rainmeter-rmskin.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME rainmeter-rmskin)
install(FILES packaging/xrainmeter-rmskin.desktop
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES packaging/xrainmeter-rmskin.xml
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/mime/packages)
