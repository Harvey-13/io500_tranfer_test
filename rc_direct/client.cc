#include "client.h"
using ll = long long;

int main(int argc, char**argv){
    RDMAClient client;
    client.connect(argv[1], server_port);
    auto data_size = sendPacks*grain;
    auto data = (char*)malloc(data_size);
    client.reg_mr(data, data_size);
    for(ll i{};i<data_size;++i)data[i] = 'a' + i % 26;
    LOG(__LINE__, "connected");
    for(ll i{};i<data_size;i+=grain)client.post_send(data, i, grain);
    sleep(5);//wait forwarder to transfer totally
    client.close();
    free(data);
    return 0;
}