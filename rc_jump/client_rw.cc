#include "client_rw.h"
using ll = long long;


int main(){
    RDMAClientRW client;
    client.connect("192.168.200.12", forwarder_port);
    auto data = (char*)malloc(sendBytes+8);
    client.reg_mr(data, sendBytes+8);
    for(ll i{};i<(sendBytes);++i)data[i] = 'a' + i % 26;
    LOG(__LINE__, "connected");
    for(ll i{};i<(sendBytes-grain);i+=grain){
        client.remote_write(data, i, grain);
        client.update_remote_offset();
    }
    client.close();
    free(data);
    return 0;
}