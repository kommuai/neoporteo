cmake_minimum_required(VERSION 3.21)
project(neoporteo)

set(CMAKE_BUILD_TYPE debug)
set(CMAKE_EXPORT_COMPILE_COMMANDS true)
set(CMAKE_CXX_STANDARD 20)

include(FindPkgConfig)

pkg_check_modules(SDL REQUIRED sdl2)

pkg_check_modules(AVC REQUIRED libavcodec)
pkg_check_modules(AVF REQUIRED libavformat)
pkg_check_modules(AVU REQUIRED libavutil)
pkg_check_modules(SWS REQUIRED libswscale)

pkg_check_modules(BZ2 REQUIRED bzip2)
pkg_check_modules(CAP REQUIRED capnp)
pkg_check_modules(CPC REQUIRED capnpc)

pkg_check_modules(MPV REQUIRED mpv)
pkg_check_modules(GLW REQUIRED glew)

find_package(OpenGL REQUIRED)

include_directories(${SDL_INCLUDE_DIRS})

add_executable(neoporteo src/neoporteo.cpp)
target_link_libraries(neoporteo ${SDL_LIBRARIES} ${OPENGL_LIBRARIES})
target_link_libraries(neoporteo ${AVC_LIBRARIES} ${AVF_LIBRARIES} ${AVU_LIBRARIES} ${SWS_LIBRARIES})
target_link_libraries(neoporteo ${BZ2_LIBRARIES} ${CAP_LIBRARIES} ${CPC_LIBRARIES})

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

add_executable(spool src/spool.cpp)
target_link_libraries(spool ${SDL_LIBRARIES} ${OPENGL_LIBRARIES})
target_link_libraries(spool ${GLW_LIBRARIES} ${MPV_LIBRARIES})
target_sources(spool
        PRIVATE
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
        )
