/*
 * jvmdump - JVMTI based class file dumper for live HotSpot JVMs.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef JVMD_AGENT_VERSION
#define JVMD_AGENT_VERSION "1.0.0"
#endif

#ifndef JVMD_NAME
#define JVMD_NAME "jvmdump"
#endif

/* Maximum length of the option string handed to Agent_OnAttach.
 * HotSpot's attach protocol limits a single argument to 1024 bytes. */
#define JVMD_MAX_OPTIONS 900

/* Compile-time fallback location of libjvmdump.so (set by the Makefile). */
#ifndef JVMD_INSTALL_AGENT
#define JVMD_INSTALL_AGENT "/usr/local/lib/jvmdump/libjvmdump.so"
#endif

#define JVMD_AGENT_LIBNAME "libjvmdump.so"
