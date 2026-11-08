/* SPDX-License-Identifier: MIT */
/* The pinned Sony C sources use GCC's unused parameter annotation. Do not
 * edit the submodule; MSVC needs no equivalent annotation for correctness. */
#if defined(_MSC_VER) && !defined(__clang__)
#include <stdint.h>
#define __attribute__(annotation)
#endif
