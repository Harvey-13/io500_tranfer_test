#include "client.h"
using ll = long long;

int main(){
    RDMAClient client;
    auto data = (char*)malloc(1ll<<32);
    for(ll i{};i<(1ll<<32);++i)data[i] = 'a' + i % 26;
    client.connect("192.168.200.12", 13333);
    LOG(__LINE__, "connected");
    for(ll i{};i<(1ll<<32);i+=grain)client.remote_write(data+i, grain);
    client.close();
    return 0;
}