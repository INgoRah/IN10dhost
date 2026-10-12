# Writes OUT with IN10DFS_GIT_HASH, the abbreviated commit of SRC and
# "-dirty" for local changes. Empty in a Debian package build (dpkg-
# buildpackage and sbuild set DEB_*_ARCH, the sources are not a git
# checkout there anyway) or when git is not available.
# Run on every build; OUT is only rewritten when the hash changed, so
# nothing is recompiled for an unchanged commit.
set(hash "")
if(NOT DEFINED ENV{DEB_BUILD_ARCH} AND NOT DEFINED ENV{DEB_HOST_ARCH})
	find_package(Git QUIET)
	if(GIT_FOUND)
		# --match with a pattern no tag has: always the plain hash
		execute_process(
			COMMAND ${GIT_EXECUTABLE} -C ${SRC} describe --always --dirty --abbrev=8 --match=no-tag-matches-this
			OUTPUT_VARIABLE hash
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET
			RESULT_VARIABLE res)
		if(NOT res EQUAL 0)
			set(hash "")
		endif()
	endif()
endif()

file(WRITE ${OUT}.tmp "#define IN10DFS_GIT_HASH \"${hash}\"\n")
configure_file(${OUT}.tmp ${OUT} COPYONLY)
file(REMOVE ${OUT}.tmp)
