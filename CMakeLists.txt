cmake_minimum_required(VERSION 3.16)
project(leafturn C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(MUPDF REQUIRED mupdf)

add_executable(leafturn
  src/main.c
)

target_compile_options(leafturn PRIVATE
  ${GTK3_CFLAGS_OTHER}
  ${MUPDF_CFLAGS_OTHER}
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
  -Wno-unused-parameter
)

target_include_directories(leafturn PRIVATE ${GTK3_INCLUDE_DIRS} ${MUPDF_INCLUDE_DIRS})
target_link_directories(leafturn PRIVATE ${GTK3_LIBRARY_DIRS} ${MUPDF_LIBRARY_DIRS})
target_link_libraries(leafturn PRIVATE ${GTK3_LIBRARIES} ${MUPDF_LIBRARIES} m)

install(TARGETS leafturn RUNTIME DESTINATION bin)
install(FILES data/com.leafturn.Leafturn.desktop DESTINATION share/applications)
install(FILES data/com.leafturn.Leafturn.svg DESTINATION share/icons/hicolor/scalable/apps)
install(FILES data/com.leafturn.Leafturn.appdata.xml DESTINATION share/metainfo)
