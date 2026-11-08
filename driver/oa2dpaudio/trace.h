/* SPDX-License-Identifier: MIT */
#pragma once

/* wdm.h selects the kernel ETW entry points. No addresses, file names or
 * PCM content are recorded. The provider is disabled unless enabled by ETW. */
#include <wdm.h>
#include <TraceLoggingProvider.h>

TRACELOGGING_DECLARE_PROVIDER(g_Oa2dpAudioTrace);
