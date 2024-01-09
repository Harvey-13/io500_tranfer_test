#include "client.h"
using ll = long long;
const ll sendBytes{1ll<<34};

int main(){
    RDMAClient client;
    client.connect("192.168.200.12", 13333);
    auto data = (char*)malloc(sendBytes);
    client.reg_mr(data, sendBytes);
    for(ll i{};i<(sendBytes);++i)data[i] = 'a' + i % 26;
    LOG(__LINE__, "connected");
    for(ll i{};i<(sendBytes);i+=grain)client.post_send(data, i, grain);
    client.close();
    free(data);
    return 0;
}