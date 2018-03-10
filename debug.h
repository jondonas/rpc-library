#define DEBUG_ENABLED 1
#define DEBUG(fmt, ...) \
    do { if (DEBUG_ENABLED) fprintf(stderr, fmt, ##__VA_ARGS__); } while (0)
