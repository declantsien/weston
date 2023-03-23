#ifndef CRT_COMPATIBILITY_H
#define CRT_COMPATIBILITY_H

#ifdef __FreeBSD__
#define program_invocation_short_name "weston"
#define ETIME ENOENT
#endif

#endif
