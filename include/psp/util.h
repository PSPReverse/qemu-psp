#ifndef PSP_UTIL_H
#define PSP_UTIL_H

/** Simplifies error handling for functions that take an errp and return void */
#define CALL_AND_CHECK(func, ...)                                              \
    do {                                                                       \
        func(__VA_ARGS__);                                                     \
        if (*errp != NULL) {                                                   \
            return;                                                            \
        }                                                                      \
    } while (0)

#endif // PSP_UTIL_H
