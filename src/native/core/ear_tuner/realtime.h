#pragma once

/** Marks a function nonblocking when built with RealtimeSanitizer support. */
#if defined(EAR_TUNER_ENABLE_RTSAN)
#define EAR_TUNER_NONBLOCKING [[clang::nonblocking]]
#else
#define EAR_TUNER_NONBLOCKING
#endif
