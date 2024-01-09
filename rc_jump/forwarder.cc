#include "forwarder.h"

int main(){
    RDMAForwarder forwarder;
    forwarder.transfer("0.0.0.0", "192.168.200.12", forwarder_port);
    return 0;
}