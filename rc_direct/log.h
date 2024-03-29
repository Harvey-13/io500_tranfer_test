#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <map>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <vector>
#include <iostream>
#include <algorithm>

#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <infiniband/verbs.h>
#include "../histogram/histogram.h"
const int grain{64*1024};
const int cq_len{64};
const int server_port{13333};
const int forwarder_port{14444};
uint64_t sendPacks{10000};

struct LOGGER {
    template<typename... Args>
    void operator()(Args&&... args) const {
        ((std::cout << args << ' '), ...);
        std::cout << std::endl; 
    }
}LOG;
