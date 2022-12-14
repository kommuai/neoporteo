cmake_minimum_required(VERSION 3.21)
project(neoporteo)

set(CMAKE_BUILD_TYPE debug)
set(CMAKE_EXPORT_COMPILE_COMMANDS true)
set(CMAKE_CXX_STANDARD 20)

include(FindPkgConfig)

pkg_check_modules(SDL REQUIRED sdl2)

pkg_check_modules(BZ2 REQUIRED bzip2)
pkg_check_modules(CAP REQUIRED capnp)
pkg_check_modules(CPC REQUIRED capnpc)

pkg_check_modules(GLW REQUIRED glew)

if(MINGW)
  set(EXTERNAL_MPV TRUE)
endif()

if(NOT EXTERNAL_MPV)
  pkg_check_modules(MPV REQUIRED mpv)
else()
  set(MPV_LIBRARIES ${PROJECT_SOURCE_DIR}/libmpv.dll.a)
endif()

find_package(OpenGL REQUIRED)

include_directories(${SDL_INCLUDE_DIRS})
include_directories(include)

add_executable(neoporteo src/neoporteo.cpp)
target_link_libraries(neoporteo ${SDL_LIBRARIES} ${OPENGL_LIBRARIES} ${GLW_LIBRARIES})
target_link_libraries(neoporteo ${BZ2_LIBRARIES} ${CAP_LIBRARIES} ${CPC_LIBRARIES})
target_link_libraries(neoporteo ${MPV_LIBRARIES})

set(IMGUI_DIR imgui)
include_directories(${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_sources(neoporteo
        PRIVATE
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
        )

include_directories(implot)
target_sources(neoporteo
        PRIVATE
        implot/implot.cpp
        implot/implot_items.cpp
        )

include_directories(imnodes)
target_sources(neoporteo
        PRIVATE
        imnodes/imnodes.cpp
        )

