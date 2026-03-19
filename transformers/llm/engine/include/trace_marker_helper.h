#ifndef TRACE_MARKER_HELPER_H_
#define TRACE_MARKER_HELPER_H_

#include <string>
#include <fcntl.h>

static int trace_marker_fd = open("/sys/kernel/tracing/trace_marker", O_WRONLY);
static bool ftrace_flag = true;
__attribute__((visibility("default"))) void begin_trace_marker(const std::string& name);

__attribute__((visibility("default"))) void end_trace_marker();

#endif