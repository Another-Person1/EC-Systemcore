foreach(REQUIRED_VARIABLE IN ITEMS
        JAR_EXECUTABLE INPUT_ROOT OUTPUT_PATH ENTRY_DATE)
  if(NOT DEFINED "${REQUIRED_VARIABLE}" OR
     "${${REQUIRED_VARIABLE}}" STREQUAL "")
    message(FATAL_ERROR "${REQUIRED_VARIABLE} is required")
  endif()
endforeach()

if(NOT IS_DIRECTORY "${INPUT_ROOT}")
  message(FATAL_ERROR "JAR input root does not exist: ${INPUT_ROOT}")
endif()

file(GLOB_RECURSE JAR_ENTRIES
     LIST_DIRECTORIES false
     RELATIVE "${INPUT_ROOT}"
     "${INPUT_ROOT}/*")
list(SORT JAR_ENTRIES)
if(NOT JAR_ENTRIES)
  message(FATAL_ERROR "Refusing to create an empty JAR from ${INPUT_ROOT}")
endif()

file(REMOVE "${OUTPUT_PATH}")
execute_process(
  COMMAND "${JAR_EXECUTABLE}"
          --create
          "--file=${OUTPUT_PATH}"
          "--date=${ENTRY_DATE}"
          ${JAR_ENTRIES}
  WORKING_DIRECTORY "${INPUT_ROOT}"
  RESULT_VARIABLE JAR_RESULT
  OUTPUT_VARIABLE JAR_OUTPUT
  ERROR_VARIABLE JAR_ERROR)
if(NOT JAR_RESULT EQUAL 0)
  message(FATAL_ERROR
    "jar failed with status ${JAR_RESULT}\n${JAR_OUTPUT}\n${JAR_ERROR}")
endif()
