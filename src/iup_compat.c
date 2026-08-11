#include <stddef.h>
// Compatibility shim for IUP on MinGW
#ifdef __MINGW32__
// Provide the triple-underscore symbols that IUP expects
int ___argc = 0;
char ***___argv = NULL;
#endif
