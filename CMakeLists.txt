cmake_minimum_required(VERSION 3.10.2)
project(xmSigma VERSION 0.1.0)

message(STATUS "------------------------------------------------")
if (PROJECT_NAME STREQUAL CMAKE_PROJECT_NAME)
  set(BUILD_AS_MODULE OFF)
  message(STATUS "Build xmSigma as a standalone project")
else ()
  set(BUILD_AS_MODULE ON)
  message(STATUS "Build xmSigma as a module")
endif ()
message(STATUS "------------------------------------------------")

find_program(CCACHE_PROGRAM ccache)
if (CCACHE_PROGRAM)
  message(STATUS "Found ccache")
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
endif ()

## Project Options
option(BUILD_TESTING "Build tests" OFF)
option(BUILD_EXAMPLES "Build examples" OFF)
option(STATIC_CHECK "Enable static check" OFF)
option(XMOTION_DEV_MODE "Development mode forces building tests" OFF)

option(ENABLE_LOGGING "Enable logging" ON)
option(USE_SYS_SPDLOG "Use system spdlog" ON)

if (DEFINED ENV{ROS_DISTRO} AND "$ENV{ROS_DISTRO}" STREQUAL "humble")
  message(STATUS "ROS_DISTRO is $ENV{ROS_DISTRO}, use system spdlog")
  set(USE_SYS_SPDLOG ON)
endif ()

## Generate symbols for IDE indexer
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (STATIC_CHECK)
  find_program(CPPCHECK cppcheck)
  if (CPPCHECK)
    message(STATUS "Found cppcheck")
    set(CMAKE_CXX_CPPCHECK cppcheck;--std=c++11;--enable=all)
  endif ()
endif ()

## Additional cmake module path
set(USER_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${USER_CMAKE_PATH}/modules")
list(APPEND CMAKE_PREFIX_PATH "/usr/lib/${CMAKE_SYSTEM_PROCESSOR}-linux-gnu/cmake")

## Set compiler to use c++ 17 features
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

## Use GNUInstallDirs to install libraries into correct locations on all platforms.
include(GNUInstallDirs)

## Put all binary files into /bin and libraries into /lib
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR})
if (XMOTION_DEV_MODE)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR})
else ()
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/${CMAKE_INSTALL_BINDIR})
endif ()

if (XMOTION_DEV_MODE)
  message(STATUS "xmSigma development mode enabled")
endif ()

## Chosse build type
set(default_build_type "Release")

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
  set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE
      STRING "Choose the type of build." FORCE)

  # Set the possible values of build type for cmake-gui
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
      "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif ()

if (ENABLE_LOGGING)
  message("-- Enable logging with spdlog -- Yes")
  message("-- Default log file storage location: ~/.xmotion/log")
else (ENABLE_LOGGING)
  message("-- Disable logging with spdlog")
endif (ENABLE_LOGGING)

## Enable test if requested
if (((NOT BUILD_AS_MODULE) AND BUILD_TESTING) OR XMOTION_DEV_MODE)
  enable_testing()
  include(GoogleTest)
  set(BUILD_TESTS ON)
  message(STATUS "xmSigma tests will be built")
else ()
  set(BUILD_TESTS OFF)
  message(STATUS "xmSigma tests will not be built")
endif ()

## Enable -fPIC for all targets
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

## Dependent libraries
add_subdirectory(third_party)

## Build the xmSigma library from the include/ + src/ layout
find_package(Eigen3 REQUIRED NO_MODULE)
find_package(Threads REQUIRED)
if (USE_SYS_SPDLOG)
  find_package(spdlog REQUIRED)
endif ()

add_library(xmSigma STATIC
    src/ctrl_logger.cpp
    src/default_logger.cpp
    src/logger_vendor_spdlog.cpp
    src/rt_logger.cpp)
target_include_directories(xmSigma PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(xmSigma PUBLIC Eigen3::Eigen Threads::Threads)
if (ENABLE_LOGGING)
  target_compile_definitions(xmSigma PUBLIC ENABLE_LOGGING)
endif ()
# When spdlog is bundled (USE_SYS_SPDLOG=OFF) the xmSigma target links the
# in-tree spdlog target, so it must travel in the same export set — otherwise
# install(EXPORT) errors that spdlog is referenced but not exported. With system
# spdlog the link is an imported target and needs no exporting.
if (USE_SYS_SPDLOG)
  target_link_libraries(xmSigma PUBLIC spdlog::spdlog)
else ()
  set(_xmsigma_install_targets xmSigma spdlog)
  target_link_libraries(xmSigma PUBLIC spdlog)
endif ()
if (NOT DEFINED _xmsigma_install_targets)
  set(_xmsigma_install_targets xmSigma)
endif ()
add_library(xmotion::xmSigma ALIAS xmSigma)

install(TARGETS ${_xmsigma_install_targets} EXPORT xmSigmaTargets
    LIBRARY DESTINATION lib ARCHIVE DESTINATION lib RUNTIME DESTINATION bin
    INCLUDES DESTINATION include)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if (BUILD_TESTS)
  add_subdirectory(test)
endif ()
if (BUILD_EXAMPLES OR XMOTION_DEV_MODE)
  add_subdirectory(examples)
endif ()

## Installation setup
# Offer the user the choice of overriding the installation directories
set(INSTALL_LIBDIR ${CMAKE_INSTALL_LIBDIR} CACHE PATH "Installation directory for libraries")
set(INSTALL_BINDIR ${CMAKE_INSTALL_BINDIR} CACHE PATH "Installation directory for executables")
set(INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR} CACHE PATH "Installation directory for header files")
if (WIN32 AND NOT CYGWIN)
  set(DEF_INSTALL_CMAKEDIR CMake)
else ()
  set(DEF_INSTALL_CMAKEDIR share/cmake/${PROJECT_NAME})
endif ()
set(INSTALL_CMAKEDIR ${DEF_INSTALL_CMAKEDIR} CACHE PATH "Installation directory for CMake files")

# print installation paths
message(STATUS "Project will be installed to ${CMAKE_INSTALL_PREFIX} with 'make install'")
foreach (p LIB BIN INCLUDE CMAKE)
  file(TO_NATIVE_PATH ${CMAKE_INSTALL_PREFIX}/${INSTALL_${p}DIR} _path)
  message(STATUS " - To install ${p} components to ${_path}")
  unset(_path)
endforeach ()

# export target configuration
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    xmSigmaConfigVersion.cmake
    VERSION "${${PROJECT_NAME}_VERSION_MAJOR}.${${PROJECT_NAME}_VERSION_MINOR}.${${PROJECT_NAME}_VERSION_PATCH}"
    COMPATIBILITY SameMajorVersion)

install(EXPORT xmSigmaTargets
    FILE xmSigmaTargets.cmake
    NAMESPACE xmotion::
    DESTINATION lib/cmake/xmSigma)

configure_file(cmake/xmSigmaConfig.cmake.in xmSigmaConfig.cmake @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/xmSigmaConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/xmSigmaConfigVersion.cmake"
    DESTINATION lib/cmake/xmSigma)

# Packaging support
set(CPACK_DEBIAN_PACKAGE_NAME "libxmotion-sigma")
set(CPACK_PACKAGE_VENDOR "Ruixiang Du")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "xmSigma - the xMotion foundation: driver/control interfaces, logging, and common types")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/xmotion")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
# set(CPACK_DEBIAN_PACKAGE_DEPENDS "abc (>= 0.1.2), xyz (<< 0.3.4)")
if (USE_SYS_SPDLOG)
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "libspdlog-dev")
endif ()
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Ruixiang Du (ruixiang.du@gmail.com)")
set(CPACK_SOURCE_IGNORE_FILES .git dist .*build.* /\\\\.DS_Store)
include(CPack)
