// Harissa64 V2 - version string, from the VERSION file at the repository
// root (passed by every build as H64_VERSION).
#ifndef H64_VERSION_H
#define H64_VERSION_H

#define H64_STRINGIFY2(x) #x
#define H64_STRINGIFY(x) H64_STRINGIFY2(x)

#ifdef H64_VERSION
#define H64_VERSION_STRING H64_STRINGIFY(H64_VERSION)
#else
#define H64_VERSION_STRING "unknown"
#endif

#endif
