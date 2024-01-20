#include "client.h"
using ll = long long;
ll sendBytes{1ll<<20};

int main(int argc, char**argv){
    if(argc==2)sendBytes = atoi(argv[1])*sendBytes;
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