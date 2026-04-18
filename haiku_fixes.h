#if defined(__x86_64__)
#ifndef HAIKU_FIXES_H
#define HAIKU_FIXES_H

#include <cstring>
#include <cstdlib>

#ifdef __HAIKU__
// Haiku doesn't have strsep in libroot, so we provide a private inline version
// 'static inline' prevents "multiple definition" errors during linking.
static inline char *strsep(char **stringp, const char *delim) {
    char *s;
    const char *spanp;
    int c, sc;
    char *tok;
    if ((s = *stringp) == NULL)
        return (NULL);
    for (tok = s;;) {
        c = *s++;
        spanp = delim;
        do {
            if ((sc = *spanp++) == c) {
                if (c == 0)
                    s = NULL;
                else
                    s[-1] = 0;
                *stringp = s;
                return (tok);
            }
        } while (sc != 0);
    }
}
#endif

#endif
#elif defined(__i386__)
    // Do notta
#endif



/*#ifndef HAIKU_FIXES_H
#define HAIKU_FIXES_H
#include <cstring>
static inline char *strsep(char **stringp, const char *delim) {
    char *s; const char *spanp; int c, sc; char *tok;
    if ((s = *stringp) == NULL) return (NULL);
    for (tok = s;;) {
        c = *s++; spanp = delim;
        do {
            if ((sc = *spanp++) == c) {
                if (c == 0) s = NULL; else s[-1] = 0;
                *stringp = s; return (tok);
            }
        } while (sc != 0);
    }
}
#endif
*/
