/* SPDX-License-Identifier: MIT */
#pragma once

/* Kernel TraceLogging is opt-in. Record control-state scalars only: no device
 * addresses, object pointers, interface paths, audio content or pairing data. */
#include <wdm.h>
#include <TraceLoggingProvider.h>

TRACELOGGING_DECLARE_PROVIDER(g_Oa2dpTransportTrace);
