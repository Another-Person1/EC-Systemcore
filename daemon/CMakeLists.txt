cmake_minimum_required(VERSION 3.28)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION"
  EC_SYSTEMCORE_VERSION
  LIMIT_COUNT 1
)
if(NOT EC_SYSTEMCORE_VERSION MATCHES
   "^[0-9]+\\.[0-9]+\\.[0-9]+([-.][0-9A-Za-z.]+)?$")
  message(FATAL_ERROR "../VERSION does not contain a valid semantic version")
endif()

project(ec-systemcore-daemon
  VERSION "${EC_SYSTEMCORE_VERSION}"
  LANGUAGES C CXX
)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EC_SYSTEMCORE_FULLY_STATIC
  "Link the daemon fully statically" OFF
)

find_package(Threads REQUIRED)

set(SOEM_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(
  "${CMAKE_CURRENT_LIST_DIR}/../third_party/SOEM"
  "${CMAKE_CURRENT_BINARY_DIR}/third_party/SOEM"
  EXCLUDE_FROM_ALL
)

add_library(ec-systemcore-client
  src/client.cpp
)
add_library(ec-systemcore::client ALIAS ec-systemcore-client)
set_target_properties(ec-systemcore-client PROPERTIES
  EXPORT_NAME client
  OUTPUT_NAME ec-systemcore-client
  VERSION "${PROJECT_VERSION}"
  SOVERSION "${PROJECT_VERSION_MAJOR}"
)
target_compile_features(ec-systemcore-client PUBLIC cxx_std_23)
target_include_directories(ec-systemcore-client PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(ec-systemcore-client PUBLIC Threads::Threads)
target_compile_definitions(ec-systemcore-client PUBLIC
  EC_SYSTEMCORE_VERSION="${PROJECT_VERSION}"
)

add_executable(ec-systemcore-daemon
  src/main.cpp
)
target_compile_features(ec-systemcore-daemon PRIVATE cxx_std_23)
target_include_directories(ec-systemcore-daemon PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include"
)
target_link_libraries(ec-systemcore-daemon PRIVATE
  soem
  Threads::Threads
)
target_compile_definitions(ec-systemcore-daemon PRIVATE
  EC_SYSTEMCORE_VERSION="${PROJECT_VERSION}"
)

foreach(target IN ITEMS ec-systemcore-client ec-systemcore-daemon)
  target_compile_options("${target}" PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wformat=2
    -Wundef
  )
endforeach()

if(EC_SYSTEMCORE_FULLY_STATIC)
  if(NOT UNIX)
    message(FATAL_ERROR "EC_SYSTEMCORE_FULLY_STATIC requires a Unix toolchain")
  endif()
  target_link_options(ec-systemcore-daemon PRIVATE -static)
endif()

if(BUILD_TESTING)
  add_executable(ec-systemcore-unit-tests
    tests/unit_tests.cpp
  )
  target_compile_features(ec-systemcore-unit-tests PRIVATE cxx_std_23)
  target_include_directories(ec-systemcore-unit-tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
  )
  target_link_libraries(ec-systemcore-unit-tests PRIVATE Threads::Threads)
  add_test(NAME ec-systemcore-unit COMMAND ec-systemcore-unit-tests)

  add_executable(ec-systemcore-adapter-lifecycle-test
    tests/adapter_lifecycle_test.cpp
  )
  target_compile_features(ec-systemcore-adapter-lifecycle-test PRIVATE cxx_std_23)
  target_include_directories(ec-systemcore-adapter-lifecycle-test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
  )
  target_link_libraries(ec-systemcore-adapter-lifecycle-test PRIVATE
    Threads::Threads
  )
  add_test(
    NAME ec-systemcore-adapter-lifecycle
    COMMAND ec-systemcore-adapter-lifecycle-test
  )

  add_executable(ec-systemcore-client-reconnect-test
    tests/client_reconnect_test.cpp
  )
  target_compile_features(ec-systemcore-client-reconnect-test PRIVATE cxx_std_23)
  target_include_directories(ec-systemcore-client-reconnect-test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
  )
  target_link_libraries(ec-systemcore-client-reconnect-test PRIVATE
    ec-systemcore-client
    Threads::Threads
  )
  add_test(
    NAME ec-systemcore-client-reconnect
    COMMAND ec-systemcore-client-reconnect-test
  )

  foreach(target IN ITEMS
      ec-systemcore-unit-tests
      ec-systemcore-adapter-lifecycle-test
      ec-systemcore-client-reconnect-test)
    target_compile_options("${target}" PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wshadow
      -Wformat=2
      -Wundef
    )
  endforeach()
endif()

configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/cmake/ec-systemcore-clientConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/ec-systemcore-clientConfig.cmake"
  INSTALL_DESTINATION
    "${CMAKE_INSTALL_LIBDIR}/cmake/ec-systemcore-client"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/ec-systemcore-clientConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)

install(TARGETS ec-systemcore-daemon
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(TARGETS ec-systemcore-client
  EXPORT ec-systemcore-clientTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(DIRECTORY include/ec_systemcore
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(EXPORT ec-systemcore-clientTargets
  FILE ec-systemcore-clientTargets.cmake
  NAMESPACE ec-systemcore::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ec-systemcore-client"
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/ec-systemcore-clientConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/ec-systemcore-clientConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ec-systemcore-client"
)
