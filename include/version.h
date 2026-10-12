#ifndef _VERSION_H
#define _VERSION_H

/* The release version, keep in step with debian/changelog */
#define IN10DFS_VERSION "0.4.0"

/* "<version>", followed by " (<git hash>[-dirty])" when built from a
   git checkout outside a Debian package build, see cmake/git_version.cmake */
const char* version_string();

#endif
