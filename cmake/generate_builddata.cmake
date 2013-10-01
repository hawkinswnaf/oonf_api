#!/bin/cmake

# look for git executable
SET(found_git false) 
find_program(found_git git)

SET(OONF_LIB_GIT "cannot read git repository")

IF(NOT ${found_git} STREQUAL "found_git-NOTFOUND")
	# get git description WITH dirty flag
	execute_process(COMMAND git describe --always --long --tags --dirty --match "v[0-9]*"
		OUTPUT_VARIABLE OONF_LIB_GIT_TAG OUTPUT_STRIP_TRAILING_WHITESPACE)
	STRING(REGEX REPLACE "^v(.*)-[^-]*-[^-]*$" "\\1" OONF_LIB_GIT ${OONF_LIB_GIT_TAG})
ENDIF()

# compare with version number
STRING(REGEX MATCH "${OONF_VERSION}" FOUND_VERSION ${OONF_LIB_GIT})
IF (NOT FOUND_VERSION)
	message (FATAL_ERROR "Library version '${OONF_VERSION}'"
		" is not present in git description '${OONF_LIB_GIT}'."
		" Please re-run 'cmake ..' to update build files")
ENDIF (NOT FOUND_VERSION)

# create builddata file
configure_file (${SRC} ${DST})
