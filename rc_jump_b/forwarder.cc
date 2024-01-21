#include "forwarder_old.h"

int main(int argc, char**argv){
    RDMAForwarder forwarder;
    forwarder.transfer("0.0.0.0", "192.168.200.10", forwarder_port);
    return 0;
}