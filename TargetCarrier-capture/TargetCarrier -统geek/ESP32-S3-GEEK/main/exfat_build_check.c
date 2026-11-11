/* Fail the build if any consumer sees a different FatFs ABI/configuration. */
#include "ff.h"
#if FF_FS_EXFAT != 1 || FF_USE_LFN != 3
#error "GEEK recording requires exFAT and heap-based long filename support"
#endif
_Static_assert(sizeof(FSIZE_t) == 8, "exFAT file offsets must be 64 bit");
