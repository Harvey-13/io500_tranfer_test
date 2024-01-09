#include "server.h"

int main(){
    RDMAServer server;
    server.listen("0.0.0.0", 13333);
    server.stop();
    return 0;
}