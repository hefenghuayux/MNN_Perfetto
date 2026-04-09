#include "trace_marker_helper.h"

#include <fcntl.h>
#include <iostream>
#include <sstream>

#include "unistd.h"

namespace {

int trace_marker_fd() {
    static int fd = mnn_schedule_instrumentation_enabled()
        ? open("/sys/kernel/tracing/trace_marker", O_WRONLY)
        : -1;
    return fd;
}

}

void begin_trace_marker(const std::string & message) {
    if (!mnn_schedule_instrumentation_enabled()) {
        return;
    }
    const int fd = trace_marker_fd();
    if (fd < 0) {
        return;
    }
    std::stringstream ss;
    pid_t tid = gettid();
    pid_t pid = getpid();
    ss << "B|" << pid << "|" << message<<tid;
    char buffer[128];
    int  len = 0;
    //std::cout<<ss.str()<<std::endl;
    len = snprintf(buffer, 128, "%s", ss.str().c_str()); 
    int len_written;
    //std::cout<<len<<std::endl;
    if((len_written = write(fd, buffer, len)) != len)
    {
        // std::cout <<  len_written << " != " << len << std::endl;
        //std::cout << "Failed to open trace_marker file" << std::endl;
    }
    else{
      //std::cout <<"success"<<std::endl;
    }
}

void end_trace_marker() {
    if (!mnn_schedule_instrumentation_enabled()) {
        return;
    }
    const int fd = trace_marker_fd();
    if (fd < 0) {
        return;
    }
    std::stringstream ss;
    pid_t pid = getpid();
    pid_t tid = gettid();
    ss << "E|" << pid;
    char buffer[128];
    int  len = 0;
    //std::cout<<ss.str()<<" "<<tid<<std::endl;
    len = snprintf(buffer, 128, "%s", ss.str().c_str()); 
    int len_written;
    if((len_written = write(fd, buffer, len)) != len)
    {
        //std::cout <<  len_written << " != " << len << std::endl;
        //std::cout << "Failed to open trace_marker file" << std::endl;
    }
}
