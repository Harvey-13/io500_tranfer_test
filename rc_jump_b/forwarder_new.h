#include "server.h"
#include "client.h"

class RDMAForwarder{
public:
    RDMAForwarder(std::vector<std::string> dests){
        server_.listen("0.0.0.0", forwarder_port);
        for(auto&ip:dests){
            clients_[ip] = new RDMAClient;
            clients_[ip]->connect(ip.c_str(), server_port);
        }
    }
private:
    RDMAServer server_;

    std::map<std::string, RDMAClient*> clients_;
};