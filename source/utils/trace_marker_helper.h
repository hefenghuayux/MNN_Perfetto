#ifndef TRACE_MARKER_HELPER_H_
#define TRACE_MARKER_HELPER_H_

#include <cstdlib>
#include <cstring>
#include <string>

inline bool mnn_hybrid_instrumentation_enabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("MNN_ENABLE_HYBRID_INSTRUMENT");
        if (value == nullptr) {
            return false;
        }
        return std::strcmp(value, "1") == 0 ||
               std::strcmp(value, "true") == 0 ||
               std::strcmp(value, "TRUE") == 0 ||
               std::strcmp(value, "on") == 0 ||
               std::strcmp(value, "ON") == 0;
    }();
    return enabled;
}

__attribute__((visibility("default"))) void begin_trace_marker(const std::string & message);
__attribute__((visibility("default"))) void end_trace_marker();

#endif
