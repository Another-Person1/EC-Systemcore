cmake_minimum_required(VERSION 3.28)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" EC_SYSTEMCORE_VERSION
     LIMIT_COUNT 1)
if(NOT EC_SYSTEMCORE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must contain one stable numeric semantic version")
endif()

project(ec-systemcore VERSION "${EC_SYSTEMCORE_VERSION}" LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)
include(CheckIPOSupported)
include(CheckPIESupported)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EC_SYSTEMCORE_BUILD_CONFIGURATION "Build the ec-systemcore configuration" ON)
option(EC_SYSTEMCORE_BUILD_CLIENT_SDKS
       "Build and validate the Java robot-code client" ON)
option(EC_SYSTEMCORE_ENABLE_HARDENING "Enable release binary hardening" ON)
option(EC_SYSTEMCORE_ENABLE_IPO "Enable release interprocedural optimization" ON)
option(EC_SYSTEMCORE_WARNINGS_AS_ERRORS
       "Reject first-party compiler warnings (recommended for CI)" OFF)
set(EC_SYSTEMCORE_BUN_ARM64_EXECUTABLE "" CACHE FILEPATH
    "Verified official ARM64 Bun executable to bundle in the target package")
set(EC_SYSTEMCORE_BUN_ARM64_SHA256_FILE "" CACHE FILEPATH
    "Digest generated from the verified, extracted ARM64 Bun executable")
set(EC_SYSTEMCORE_BUN_ARM64_PROVENANCE_FILE "" CACHE FILEPATH
    "Manifest tying the verified ARM64 Bun archive to its executable")

add_subdirectory(daemon)

set(EC_SYSTEMCORE_JAVA_JAR
    "${CMAKE_CURRENT_BINARY_DIR}/clients/java/ec-systemcore-client.jar")
if(EC_SYSTEMCORE_BUILD_CLIENT_SDKS)
  find_package(Java 25 REQUIRED COMPONENTS Development Runtime)
  if(NOT Java_VERSION_STRING MATCHES "^25([.]|$)")
    message(FATAL_ERROR
      "WPILib 2027 client validation requires JDK 25; found ${Java_VERSION_STRING}")
  endif()

  file(GLOB_RECURSE EC_SYSTEMCORE_JAVA_MAIN_SOURCES CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/clients/java/src/main/java/*.java")
  file(GLOB_RECURSE EC_SYSTEMCORE_JAVA_TEST_SOURCES CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/clients/java/src/test/java/*.java")
  set(EC_SYSTEMCORE_JAVA_MAIN_CLASSES
      "${CMAKE_CURRENT_BINARY_DIR}/clients/java/main-classes")
  set(EC_SYSTEMCORE_JAVA_TEST_CLASSES
      "${CMAKE_CURRENT_BINARY_DIR}/clients/java/test-classes")
  set(EC_SYSTEMCORE_JAVA_EXAMPLE_CLASSES
      "${CMAKE_CURRENT_BINARY_DIR}/clients/java/example-classes")
  set(EC_SYSTEMCORE_JAVA_MAIN_STAMP
      "${CMAKE_CURRENT_BINARY_DIR}/clients/java/main-classes.stamp")
  set(EC_SYSTEMCORE_JAVA_TEST_STAMP
      "${CMAKE_CURRENT_BINARY_DIR}/clients/java/test-classes.stamp")
  set(EC_SYSTEMCORE_JAVA_EXAMPLE_STAMP
      "${CMAKE_CURRENT_BINARY_DIR}/clients/java/example-classes.stamp")

  add_custom_command(
    OUTPUT "${EC_SYSTEMCORE_JAVA_MAIN_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf
            "${EC_SYSTEMCORE_JAVA_MAIN_CLASSES}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${EC_SYSTEMCORE_JAVA_MAIN_CLASSES}"
    COMMAND "${Java_JAVAC_EXECUTABLE}" --release 17 -encoding UTF-8
            -Xlint:all -Werror
            -d "${EC_SYSTEMCORE_JAVA_MAIN_CLASSES}"
            ${EC_SYSTEMCORE_JAVA_MAIN_SOURCES}
    COMMAND "${CMAKE_COMMAND}" -E touch "${EC_SYSTEMCORE_JAVA_MAIN_STAMP}"
    DEPENDS ${EC_SYSTEMCORE_JAVA_MAIN_SOURCES}
    COMMENT "Compiling the Java client with JDK 25 and Java 17 bytecode"
    VERBATIM)
  add_custom_command(
    OUTPUT "${EC_SYSTEMCORE_JAVA_JAR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/clients/java"
    COMMAND "${CMAKE_COMMAND}"
            "-DJAR_EXECUTABLE=${Java_JAR_EXECUTABLE}"
            "-DINPUT_ROOT=${EC_SYSTEMCORE_JAVA_MAIN_CLASSES}"
            "-DOUTPUT_PATH=${EC_SYSTEMCORE_JAVA_JAR}"
            "-DENTRY_DATE=2000-01-01T00:00:00Z"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/create_deterministic_jar.cmake"
    DEPENDS
      "${EC_SYSTEMCORE_JAVA_MAIN_STAMP}"
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/create_deterministic_jar.cmake"
    COMMENT "Creating deterministic ec-systemcore Java client JAR"
    VERBATIM)
  add_custom_command(
    OUTPUT "${EC_SYSTEMCORE_JAVA_TEST_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf
            "${EC_SYSTEMCORE_JAVA_TEST_CLASSES}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${EC_SYSTEMCORE_JAVA_TEST_CLASSES}"
    COMMAND "${Java_JAVAC_EXECUTABLE}" --release 17 -encoding UTF-8
            -Xlint:all -Werror
            -classpath "${EC_SYSTEMCORE_JAVA_MAIN_CLASSES}"
            -d "${EC_SYSTEMCORE_JAVA_TEST_CLASSES}"
            ${EC_SYSTEMCORE_JAVA_TEST_SOURCES}
    COMMAND "${CMAKE_COMMAND}" -E touch "${EC_SYSTEMCORE_JAVA_TEST_STAMP}"
    DEPENDS
      "${EC_SYSTEMCORE_JAVA_MAIN_STAMP}"
      ${EC_SYSTEMCORE_JAVA_TEST_SOURCES}
    COMMENT "Compiling Java client protocol and reconnect tests"
    VERBATIM)
  add_custom_command(
    OUTPUT "${EC_SYSTEMCORE_JAVA_EXAMPLE_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf
            "${EC_SYSTEMCORE_JAVA_EXAMPLE_CLASSES}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${EC_SYSTEMCORE_JAVA_EXAMPLE_CLASSES}"
    COMMAND "${Java_JAVAC_EXECUTABLE}" --release 17 -encoding UTF-8
            -Xlint:all -Werror
            -classpath "${EC_SYSTEMCORE_JAVA_MAIN_CLASSES}"
            -d "${EC_SYSTEMCORE_JAVA_EXAMPLE_CLASSES}"
            "${CMAKE_CURRENT_SOURCE_DIR}/clients/java/examples/RobotLoopExample.java"
    COMMAND "${CMAKE_COMMAND}" -E touch "${EC_SYSTEMCORE_JAVA_EXAMPLE_STAMP}"
    DEPENDS
      "${EC_SYSTEMCORE_JAVA_MAIN_STAMP}"
      "${CMAKE_CURRENT_SOURCE_DIR}/clients/java/examples/RobotLoopExample.java"
    COMMENT "Compiling the Java robot-loop example"
    VERBATIM)
  add_custom_target(ec-systemcore-java-client ALL
    DEPENDS
      "${EC_SYSTEMCORE_JAVA_JAR}"
      "${EC_SYSTEMCORE_JAVA_TEST_STAMP}"
      "${EC_SYSTEMCORE_JAVA_EXAMPLE_STAMP}")

  install(FILES "${EC_SYSTEMCORE_JAVA_JAR}"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/java")
  install(FILES
          "${CMAKE_CURRENT_SOURCE_DIR}/clients/protocol-v1-golden.properties"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/ec-systemcore")
  install(FILES
          "${CMAKE_CURRENT_SOURCE_DIR}/clients/java/README.md"
          "${CMAKE_CURRENT_SOURCE_DIR}/clients/java/examples/RobotLoopExample.java"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/ec-systemcore/java")
endif()

if(EC_SYSTEMCORE_WARNINGS_AS_ERRORS)
  get_property(EC_SYSTEMCORE_DAEMON_DIRECTORY_TARGETS
               DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/daemon"
               PROPERTY BUILDSYSTEM_TARGETS)
  foreach(EC_SYSTEMCORE_FIRST_PARTY_TARGET IN LISTS
          EC_SYSTEMCORE_DAEMON_DIRECTORY_TARGETS)
    if(EC_SYSTEMCORE_FIRST_PARTY_TARGET MATCHES "^ec-systemcore-")
      get_target_property(EC_SYSTEMCORE_FIRST_PARTY_TARGET_TYPE
                          "${EC_SYSTEMCORE_FIRST_PARTY_TARGET}" TYPE)
      if(NOT EC_SYSTEMCORE_FIRST_PARTY_TARGET_TYPE STREQUAL "UTILITY" AND
         NOT EC_SYSTEMCORE_FIRST_PARTY_TARGET_TYPE STREQUAL "INTERFACE_LIBRARY")
        target_compile_options("${EC_SYSTEMCORE_FIRST_PARTY_TARGET}" PRIVATE
          -Werror)
      endif()
    endif()
  endforeach()
endif()

if(EC_SYSTEMCORE_ENABLE_HARDENING AND TARGET ec-systemcore-daemon)
  foreach(EC_SYSTEMCORE_HARDENED_TARGET IN ITEMS
          ec-systemcore-daemon ec-systemcore-client)
    if(TARGET "${EC_SYSTEMCORE_HARDENED_TARGET}")
      target_compile_definitions("${EC_SYSTEMCORE_HARDENED_TARGET}" PRIVATE
        $<$<CONFIG:Release>:_FORTIFY_SOURCE=3>
      )
      target_compile_options("${EC_SYSTEMCORE_HARDENED_TARGET}" PRIVATE
        $<$<CONFIG:Release>:-fstack-protector-strong>
        $<$<CONFIG:Release>:-fstack-clash-protection>
      )
    endif()
  endforeach()

  check_pie_supported()
  target_link_options(ec-systemcore-daemon PRIVATE
    $<$<CONFIG:Release>:-Wl,-z,relro>
    $<$<CONFIG:Release>:-Wl,-z,now>
    $<$<CONFIG:Release>:-Wl,-z,noexecstack>
    $<$<CONFIG:Release>:-Wl,--as-needed>
  )
  set_property(TARGET ec-systemcore-daemon PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()

if(EC_SYSTEMCORE_ENABLE_IPO AND TARGET ec-systemcore-daemon)
  check_ipo_supported(RESULT EC_SYSTEMCORE_IPO_SUPPORTED
                      OUTPUT EC_SYSTEMCORE_IPO_ERROR)
  if(EC_SYSTEMCORE_IPO_SUPPORTED)
    set_property(TARGET ec-systemcore-daemon
                 PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  else()
    message(WARNING "IPO is unavailable: ${EC_SYSTEMCORE_IPO_ERROR}")
  endif()
endif()

set(EC_SYSTEMCORE_CONFIGURATION_OUTPUT
    "${CMAKE_CURRENT_BINARY_DIR}/configuration/server.js")
if(EC_SYSTEMCORE_BUILD_CONFIGURATION)
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/.bun-arm64-zip.sha256"
       EC_SYSTEMCORE_BUN_ARM64_ZIP_SHA256 LIMIT_COUNT 1)
  string(LENGTH "${EC_SYSTEMCORE_BUN_ARM64_ZIP_SHA256}"
         EC_SYSTEMCORE_BUN_ARM64_ZIP_SHA256_LENGTH)
  if(NOT EC_SYSTEMCORE_BUN_ARM64_ZIP_SHA256 MATCHES "^[0-9a-f]+$" OR
     NOT EC_SYSTEMCORE_BUN_ARM64_ZIP_SHA256_LENGTH EQUAL 64)
    message(FATAL_ERROR
      ".bun-arm64-zip.sha256 must contain one archive SHA-256 digest")
  endif()
  if(NOT IS_ABSOLUTE "${EC_SYSTEMCORE_BUN_ARM64_EXECUTABLE}" OR
     NOT EXISTS "${EC_SYSTEMCORE_BUN_ARM64_EXECUTABLE}" OR
     IS_DIRECTORY "${EC_SYSTEMCORE_BUN_ARM64_EXECUTABLE}")
    message(FATAL_ERROR
      "EC_SYSTEMCORE_BUN_ARM64_EXECUTABLE must name the verified target runtime")
  endif()
  if(NOT IS_ABSOLUTE "${EC_SYSTEMCORE_BUN_ARM64_SHA256_FILE}" OR
     NOT EXISTS "${EC_SYSTEMCORE_BUN_ARM64_SHA256_FILE}" OR
     IS_DIRECTORY "${EC_SYSTEMCORE_BUN_ARM64_SHA256_FILE}")
    message(FATAL_ERROR
      "EC_SYSTEMCORE_BUN_ARM64_SHA256_FILE must name its generated digest")
  endif()
  if(NOT IS_ABSOLUTE "${EC_SYSTEMCORE_BUN_ARM64_PROVENANCE_FILE}" OR
     NOT EXISTS "${EC_SYSTEMCORE_BUN_ARM64_PROVENANCE_FILE}" OR
     IS_DIRECTORY "${EC_SYSTEMCORE_BUN_ARM64_PROVENANCE_FILE}")
    message(FATAL_ERROR
      "EC_SYSTEMCORE_BUN_ARM64_PROVENANCE_FILE must name the verified archive manifest")
  endif()
  file(STRINGS "${EC_SYSTEMCORE_BUN_ARM64_SHA256_FILE}"
       EC_SYSTEMCORE_BUN_ARM64_SHA256 LIMIT_COUNT 1)
  string(LENGTH "${EC_SYSTEMCORE_BUN_ARM64_SHA256}"
         EC_SYSTEMCORE_BUN_ARM64_SHA256_LENGTH)
  if(NOT EC_SYSTEMCORE_BUN_ARM64_SHA256 MATCHES "^[0-9a-f]+$" OR
     NOT EC_SYSTEMCORE_BUN_ARM64_SHA256_LENGTH EQUAL 64)
    message(FATAL_ERROR "generated ARM64 Bun executable digest is invalid")
  endif()
  file(SHA256 "${EC_SYSTEMCORE_BUN_ARM64_EXECUTABLE}"
       EC_SYSTEMCORE_BUN_ARM64_ACTUAL_SHA256)
  if(NOT EC_SYSTEMCORE_BUN_ARM64_ACTUAL_SHA256 STREQUAL
         EC_SYSTEMCORE_BUN_ARM64_SHA256)
    message(FATAL_ERROR
      "ARM64 Bun runtime hash does not match its generated executable digest")
  endif()
  file(READ "${EC_SYSTEMCORE_BUN_ARM64_PROVENANCE_FILE}"
       EC_SYSTEMCORE_BUN_ARM64_PROVENANCE)
  string(JSON EC_SYSTEMCORE_BUN_PROVENANCE_VERSION GET
         "${EC_SYSTEMCORE_BUN_ARM64_PROVENANCE}" version)
  string(JSON EC_SYSTEMCORE_BUN_PROVENANCE_ASSET GET
         "${EC_SYSTEMCORE_BUN_ARM64_PROVENANCE}" asset)
  string(JSON EC_SYSTEMCORE_BUN_PROVENANCE_ARCHIVE_SHA256 GET
         "${EC_SYSTEMCORE_BUN_ARM64_PROVENANCE}" archive_sha256)
  string(JSON EC_SYSTEMCORE_BUN_PROVENANCE_EXECUTABLE_SHA256 GET
         "${EC_SYSTEMCORE_BUN_ARM64_PROVENANCE}" executable_sha256)
  if(NOT EC_SYSTEMCORE_BUN_PROVENANCE_VERSION STREQUAL "1.3.14" OR
     NOT EC_SYSTEMCORE_BUN_PROVENANCE_ASSET STREQUAL
         "bun-linux-aarch64.zip" OR
     NOT EC_SYSTEMCORE_BUN_PROVENANCE_ARCHIVE_SHA256 STREQUAL
         EC_SYSTEMCORE_BUN_ARM64_ZIP_SHA256 OR
     NOT EC_SYSTEMCORE_BUN_PROVENANCE_EXECUTABLE_SHA256 STREQUAL
         EC_SYSTEMCORE_BUN_ARM64_SHA256)
    message(FATAL_ERROR
      "ARM64 Bun provenance does not tie the pinned release archive to the runtime")
  endif()

  find_program(EC_SYSTEMCORE_BUN_EXECUTABLE NAMES bun REQUIRED)
  execute_process(
    COMMAND "${EC_SYSTEMCORE_BUN_EXECUTABLE}" --version
    RESULT_VARIABLE EC_SYSTEMCORE_BUN_VERSION_RESULT
    OUTPUT_VARIABLE EC_SYSTEMCORE_BUN_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT EC_SYSTEMCORE_BUN_VERSION_RESULT EQUAL 0 OR
     NOT EC_SYSTEMCORE_BUN_VERSION STREQUAL "1.3.14")
    message(FATAL_ERROR
      "Configuration builds require Bun 1.3.14; found '${EC_SYSTEMCORE_BUN_VERSION}'")
  endif()
  file(GLOB_RECURSE EC_SYSTEMCORE_CONFIGURATION_INPUTS CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/configuration/public/*")
  file(GLOB EC_SYSTEMCORE_CONFIGURATION_SOURCES CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/configuration/*.ts")
  list(APPEND EC_SYSTEMCORE_CONFIGURATION_INPUTS
       ${EC_SYSTEMCORE_CONFIGURATION_SOURCES})
  list(APPEND EC_SYSTEMCORE_CONFIGURATION_INPUTS
       "${CMAKE_CURRENT_SOURCE_DIR}/configuration/package.json"
       "${CMAKE_CURRENT_SOURCE_DIR}/configuration/tsconfig.json")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/configuration/bun.lock")
    list(APPEND EC_SYSTEMCORE_CONFIGURATION_INPUTS
         "${CMAKE_CURRENT_SOURCE_DIR}/configuration/bun.lock")
  endif()

  add_custom_command(
    OUTPUT "${EC_SYSTEMCORE_CONFIGURATION_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/configuration"
    COMMAND "${EC_SYSTEMCORE_BUN_EXECUTABLE}" build
            "${CMAKE_CURRENT_SOURCE_DIR}/configuration/server.ts"
            --target=bun
            --minify
            "--define=process.env.EC_SYSTEMCORE_VERSION=\"${PROJECT_VERSION}\""
            "--outfile=${EC_SYSTEMCORE_CONFIGURATION_OUTPUT}"
    DEPENDS ${EC_SYSTEMCORE_CONFIGURATION_INPUTS}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/configuration"
    COMMENT "Bundling ec-systemcore configuration ${PROJECT_VERSION}"
    VERBATIM
  )
  add_custom_target(ec-systemcore-configuration ALL
                    DEPENDS "${EC_SYSTEMCORE_CONFIGURATION_OUTPUT}")

  install(FILES "${EC_SYSTEMCORE_CONFIGURATION_OUTPUT}"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/ec-systemcore/configuration")
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/configuration/public"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/ec-systemcore/configuration")
  install(PROGRAMS "${EC_SYSTEMCORE_BUN_ARM64_EXECUTABLE}"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/ec-systemcore/runtime"
          RENAME bun)
  install(FILES "${EC_SYSTEMCORE_BUN_ARM64_SHA256_FILE}"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/ec-systemcore"
          RENAME bun-runtime.sha256)
  install(FILES "${EC_SYSTEMCORE_BUN_ARM64_PROVENANCE_FILE}"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/ec-systemcore"
          RENAME bun-runtime-provenance.json)
endif()

install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/daemon/ec-systemcore.service"
        "${CMAKE_CURRENT_SOURCE_DIR}/daemon/ec-systemcore-configuration.service"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/system")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/config/ec-systemcore.example.json"
        # DESTDIR intentionally relocates this absolute GNU system path into
        # the package stage as /etc/ec-systemcore, never /usr/etc.
        DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/ec-systemcore"
        RENAME ec-systemcore.json)
install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/client-api.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/licenses/BUN-LICENSE.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SOEM/LICENSE.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/ec-systemcore")

file(GENERATE
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ec-systemcore-build-metadata.json"
  CONTENT
"{
  \"name\": \"ec-systemcore\",
  \"version\": \"${PROJECT_VERSION}\",
  \"target_system\": \"${CMAKE_SYSTEM_NAME}\",
  \"target_processor\": \"${CMAKE_SYSTEM_PROCESSOR}\",
  \"build_type\": \"${CMAKE_BUILD_TYPE}\",
  \"cxx_standard\": ${CMAKE_CXX_STANDARD},
  \"cxx_compiler_id\": \"${CMAKE_CXX_COMPILER_ID}\",
  \"cxx_compiler_version\": \"${CMAKE_CXX_COMPILER_VERSION}\",
  \"warnings_as_errors\": \"${EC_SYSTEMCORE_WARNINGS_AS_ERRORS}\",
  \"target_glibc_max\": \"2.36\",
  \"java_validation_runtime\": \"25.0.3+9\",
  \"java_bytecode_release\": 17,
  \"bun_runtime_version\": \"1.3.14\",
  \"bun_runtime_asset\": \"bun-linux-aarch64.zip\",
  \"bun_runtime_asset_sha256\": \"${EC_SYSTEMCORE_BUN_ARM64_ZIP_SHA256}\",
  \"bun_runtime_sha256\": \"${EC_SYSTEMCORE_BUN_ARM64_SHA256}\"
}
")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ec-systemcore-build-metadata.json"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/ec-systemcore")

if(BUILD_TESTING)
  find_program(EC_SYSTEMCORE_BASH_EXECUTABLE NAMES bash REQUIRED)
  add_test(
    NAME ec-systemcore-branding
    COMMAND "${EC_SYSTEMCORE_BASH_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/validate_branding.sh")
  add_test(
    NAME ec-systemcore-systemd-units
    COMMAND "${EC_SYSTEMCORE_BASH_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_systemd.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}")
  if(EC_SYSTEMCORE_BUILD_CLIENT_SDKS)
    if(CMAKE_HOST_WIN32)
      set(EC_SYSTEMCORE_JAVA_TEST_CLASSPATH
          "${EC_SYSTEMCORE_JAVA_MAIN_CLASSES};${EC_SYSTEMCORE_JAVA_TEST_CLASSES}")
    else()
      set(EC_SYSTEMCORE_JAVA_TEST_CLASSPATH
          "${EC_SYSTEMCORE_JAVA_MAIN_CLASSES}:${EC_SYSTEMCORE_JAVA_TEST_CLASSES}")
    endif()
    add_test(
      NAME ec-systemcore-java-client
      COMMAND "${Java_JAVA_EXECUTABLE}" -ea
              -classpath "${EC_SYSTEMCORE_JAVA_TEST_CLASSPATH}"
              ec_systemcore.SystemCoreClientTest
              "${CMAKE_CURRENT_SOURCE_DIR}/clients/protocol-v1-golden.properties")
  endif()
endif()

set(CPACK_PACKAGE_NAME "ec-systemcore")
set(CPACK_PACKAGE_VENDOR "ec-systemcore maintainers")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_SOURCE_IGNORE_FILES "/build/;/.git/;/.venv/;configuration/node_modules/")
include(CPack)
