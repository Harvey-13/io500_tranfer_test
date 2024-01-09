#include "client.h"
using ll = long long;


int main(){
    RDMAClient client;
    client.connect("192.168.200.12", forwarder_port);
    auto data = (char*)malloc(sendBytes);
    client.reg_mr(data, sendBytes);
    for(ll i{};i<(sendBytes);++i)data[i] = 'a' + i % 26;
    LOG(__LINE__, "connected");
    for(ll i{};i<(sendBytes-grain);i+=grain)client.post_send(data, i, grain);
    client.close();
    free(data);
    return 0;
}