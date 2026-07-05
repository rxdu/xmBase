cmake_minimum_required(VERSION 3.10.2)
project(xmBase VERSION 0.3.0)

message(STATUS "------------------------------------------------")
if (PROJECT_NAME STREQUAL CMAKE_PROJECT_NAME)
  set(BUILD_AS_MODULE OFF)
  message(STATUS "Build xmBase as a standalone project")
else ()
  set(BUILD_AS_MODULE ON)
  message(STATUS "Build xmBase as a module")
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

option(ENABLE_LOGGING "Enable the built-in console logging binding" ON)
# CI hygiene: strict warnings on first-party code (third_party is added
# before this takes effect and stays untouched).
option(XMOTION_WERROR "Treat warnings as errors (-Wall -Wextra -Werror)" OFF)

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
  message(STATUS "xmBase development mode enabled")
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
  message(STATUS "Console logging (telemetry default binding): enabled")
else ()
  message(STATUS "Console logging (telemetry default binding): disabled")
endif ()

## Enable test if requested
if (((NOT BUILD_AS_MODULE) AND BUILD_TESTING) OR XMOTION_DEV_MODE)
  enable_testing()
  include(GoogleTest)
  set(BUILD_TESTS ON)
  message(STATUS "xmBase tests will be built")
else ()
  set(BUILD_TESTS OFF)
  message(STATUS "xmBase tests will not be built")
endif ()

## Enable -fPIC for all targets
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

## Dependent libraries
add_subdirectory(third_party)

if(XMOTION_WERROR)
  # -Wmaybe-uninitialized stays a visible warning but does not gate: GCC's
  # flow analysis has a documented false-positive record inside libstdc++
  # headers (std::function/std::regex internals under sanitizers + -O).
  add_compile_options(-Wall -Wextra -Werror -Wno-error=maybe-uninitialized)
endif()

## Build the xmBase library from the include/ + src/ layout
find_package(Eigen3 REQUIRED NO_MODULE)
find_package(Threads REQUIRED)

add_library(xmBase STATIC
    src/telemetry_unbound.cpp
    src/telemetry_console_binding.cpp
    src/event_dispatcher.cpp
    src/async_event_dispatcher.cpp)
target_include_directories(xmBase PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(xmBase PUBLIC Eigen3::Eigen Threads::Threads)
if (ENABLE_LOGGING)
  target_compile_definitions(xmBase PUBLIC ENABLE_LOGGING)
endif ()
add_library(xmotion::xmBase ALIAS xmBase)

install(TARGETS xmBase EXPORT xmBaseTargets
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
    xmBaseConfigVersion.cmake
    VERSION "${${PROJECT_NAME}_VERSION_MAJOR}.${${PROJECT_NAME}_VERSION_MINOR}.${${PROJECT_NAME}_VERSION_PATCH}"
    COMPATIBILITY SameMajorVersion)

install(EXPORT xmBaseTargets
    FILE xmBaseTargets.cmake
    NAMESPACE xmotion::
    DESTINATION lib/cmake/xmBase)

configure_file(cmake/xmBaseConfig.cmake.in xmBaseConfig.cmake @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/xmBaseConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/xmBaseConfigVersion.cmake"
    DESTINATION lib/cmake/xmBase)

# Packaging support
set(CPACK_DEBIAN_PACKAGE_NAME "libxmotion-base")
set(CPACK_PACKAGE_VENDOR "Ruixiang Du")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "xmBase - the XMotion foundation: driver/control interfaces, logging, and common types")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/xmotion")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
# set(CPACK_DEBIAN_PACKAGE_DEPENDS "abc (>= 0.1.2), xyz (<< 0.3.4)")
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Ruixiang Du (ruixiang.du@gmail.com)")
set(CPACK_SOURCE_IGNORE_FILES .git dist .*build.* /\\\\.DS_Store)
include(CPack)
