#include "trace_marker_helper.h"

#include <iostream>
#include <sstream>

#include "unistd.h"

void begin_trace_marker(const std::string & message) {
    if(!ftrace_flag){ 
        return;
    }
    //std::cout<<trace_marker_fd<<std::endl;
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
    if((len_written = write(trace_marker_fd, buffer, len)) != len)
    {
        // std::cout <<  len_written << " != " << len << std::endl;
        //std::cout << "Failed to open trace_marker file" << std::endl;
    }
    else{
      //std::cout <<"success"<<std::endl;
    }
}

void end_trace_marker() {
    if(!ftrace_flag){ 
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
    if((len_written = write(trace_marker_fd, buffer, len)) != len)
    {
        //std::cout <<  len_written << " != " << len << std::endl;
        //std::cout << "Failed to open trace_marker file" << std::endl;
    }
}