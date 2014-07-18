# - Look for GNU Flex++, the scanner generator
# Based off a news post from Andy Cedilnik at Kitware
# Defines the following:
#  FLEXPP_EXECUTABLE - path to the bison executable
#  FLEXPP_FILE - parse a file with bison
#  FLEXPP_PREFIX_OUTPUTS - Set to true to make BISON_FILE produce prefixed
#                         symbols in the generated output based on filename.
#                         So for ${filename}.y, you'll get ${filename}parse(), etc.
#                         instead of yyparse().
#  FLEXPP_GENERATE_DEFINES - Set to true to make BISON_FILE output the matching
#                           .h file for a .c file. You want this if you're using
#                           flex.

#Modified by Markus for flex++

IF(NOT DEFINED FLEXPP_PREFIX_OUTPUTS)
 SET(FLEXPP_PREFIX_OUTPUTS FALSE)
ENDIF(NOT DEFINED FLEXPP_PREFIX_OUTPUTS)

IF(NOT DEFINED FLEXPP_GENERATE_DEFINES)
 SET(FLEXPP_GENERATE_DEFINES FALSE)
ENDIF(NOT DEFINED FLEXPP_GENERATE_DEFINES)

IF(NOT FLEXPP_EXECUTABLE)
 MESSAGE(STATUS "Looking for flex++")
 FIND_PROGRAM(FLEXPP_EXECUTABLE flex++)
 IF(FLEXPP_EXECUTABLE)
   MESSAGE(STATUS "Looking for flex++ -- ${FLEXPP_EXECUTABLE}")
 ENDIF(FLEXPP_EXECUTABLE)
ENDIF(NOT FLEXPP_EXECUTABLE)

IF(FLEXPP_EXECUTABLE)
 MACRO(FLEXPP_FILE FILENAME)
   GET_FILENAME_COMPONENT(PATH "${FILENAME}" PATH)
   IF("${PATH}" STREQUAL "")
     SET(PATH_OPT "")
   ELSE("${PATH}" STREQUAL "")
     SET(PATH_OPT "/${PATH}")
   ENDIF("${PATH}" STREQUAL "")
   GET_FILENAME_COMPONENT(HEAD "${FILENAME}" NAME_WE)
   IF(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}${PATH_OPT}")
     FILE(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}${PATH_OPT}")
   ENDIF(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}${PATH_OPT}")
   IF(FLEXPP_PREFIX_OUTPUTS)
     SET(PREFIX "${HEAD}")
   ELSE(FLEXPP_PREFIX_OUTPUTS)
     SET(PREFIX "yy")
   ENDIF(FLEXPP_PREFIX_OUTPUTS)
   SET(OUTFILE "${CMAKE_CURRENT_BINARY_DIR}${PATH_OPT}/${HEAD}.tab.cpp")
   IF(FLEXPP_GENERATE_DEFINES)
     SET(HEADER "${CMAKE_CURRENT_BINARY_DIR}${PATH_OPT}/${HEAD}.tab.h")
     ADD_CUSTOM_COMMAND(
       OUTPUT "${OUTFILE}" "${HEADER}"
       COMMAND "${FLEXPP_EXECUTABLE}"
       ARGS "--name-prefix=${PREFIX}"
       "--defines"
       "--output-file=${OUTFILE}"
       "${CMAKE_CURRENT_SOURCE_DIR}/${FILENAME}"
       DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${FILENAME}")
     SET_SOURCE_FILES_PROPERTIES("${OUTFILE}" "${HEADER}" PROPERTIES GENERATED TRUE)
     SET_SOURCE_FILES_PROPERTIES("${HEADER}" PROPERTIES HEADER_FILE_ONLY TRUE)
   ELSE(FLEXPP_GENERATE_DEFINES)
     ADD_CUSTOM_COMMAND(
       OUTPUT "${OUTFILE}"
       COMMAND "${FLEXPP_EXECUTABLE}"
       ARGS "--name-prefix=${PREFIX}"
       "--output-file=${OUTFILE}"
       "${CMAKE_CURRENT_SOURCE_DIR}/${FILENAME}"
       DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${FILENAME}")
     SET_SOURCE_FILES_PROPERTIES("${OUTFILE}" PROPERTIES GENERATED TRUE)
   ENDIF(FLEXPP_GENERATE_DEFINES)
 ENDMACRO(FLEXPP_FILE) 
ENDIF(FLEXPP_EXECUTABLE)
