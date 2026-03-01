cmake_minimum_required(VERSION 3.16)
project(immich-photoframe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Dependencies ----
find_package(OpenCV 4 REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(QRENCODE REQUIRED libqrencode)
pkg_check_modules(LIBCURL REQUIRED libcurl)   # kept for Step 2 (Immich API)

# X11
find_package(X11 REQUIRED)

# ---- Sources ----
set(SOURCES
    main.cpp
    DisplayImage.cpp
    ImmichClient.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${OpenCV_INCLUDE_DIRS}
    ${QRENCODE_INCLUDE_DIRS}
    ${LIBCURL_INCLUDE_DIRS}
    ${X11_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${OpenCV_LIBS}
    ${QRENCODE_LIBRARIES}
    ${LIBCURL_LIBRARIES}
    ${X11_LIBRARIES}
    ${X11_Xfixes_LIB}
    pthread
)

target_compile_options(${PROJECT_NAME} PRIVATE
    ${QRENCODE_CFLAGS_OTHER}
)

# ---- Install target (optional) ----
install(TARGETS ${PROJECT_NAME} DESTINATION bin)
install(FILES config.json DESTINATION bin)
