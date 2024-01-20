#include "client.h"

class RDMAForwarder{
public:
    RDMAForwarder() = default;
    void transfer(char* source, char* dest, int listen_port){
        strncpy(dest_, dest, strlen(dest));
        int num_devices{};
        pd_ = ibv_alloc_pd(rdma_get_devices(&num_devices)[0]);
        if(!pd_)LOG(__LINE__, "failed to alloc pd");
        data_ = (char*)malloc(sendBytes+8);
        data_mr_ = ibv_reg_mr(pd_, data_, sendBytes+8, IBV_ACCESS_LOCAL_WRITE|
                                                            IBV_ACCESS_REMOTE_READ|
                                                            IBV_ACCESS_REMOTE_WRITE);
        memset(data_, 0, sizeof(data_));
        chan_ = rdma_create_event_channel();
        if(!chan_)LOG(__LINE__, "failed to create event channel");

        rdma_create_id(chan_, &listen_id_, nullptr, RDMA_PS_TCP);
        if(!listen_id_)LOG(__LINE__, "failed to create listen cm_id");

        sockaddr_in sin{
        .sin_family = AF_INET,
        .sin_port = htons(listen_port),
        .sin_addr = {
            .s_addr = inet_addr(source)
        }
        };
        if(rdma_bind_addr(listen_id_, (sockaddr*)&sin))LOG(__LINE__, "failed to bind addr");
        if(rdma_listen(listen_id_, 1))LOG(__LINE__, "failed to begin rdma listen");
        // pd_ = ibv_alloc_pd(listen_id_->verbs);
        
        LOG(__LINE__, "ready to listen");

        std::thread watcher([this](){
            void* ptr = this->data_;
            for(;;){
                if(this->stop_)break;
                if(*(uint64_t*)(this->data_+sendBytes)!=this->offset_){
                    //TODO:use different clients to multiple dest
                    this->client_map_.begin()->second->post_send(this->data_, this->offset_, grain);
                    this->offset_ += grain;
                }
            }
        });

        rdma_cm_event *event{};
        for(;;){
            if(stop_)break;
            if(rdma_get_cm_event(chan_, &event))LOG(__LINE__, "failed to get cm event");
            switch (event->event)
            {
            case RDMA_CM_EVENT_CONNECT_REQUEST:{
                rdma_cm_id *cm_id{event->id};
                rdma_ack_cm_event(event);
                create_connection(cm_id);
                break;
            }
            case RDMA_CM_EVENT_DISCONNECTED:{
                client_map_[event->id]->close();
                delete client_map_[event->id];
                client_map_.erase(event->id);
                rdma_ack_cm_event(event);
                break;
            }
            default:
                rdma_ack_cm_event(event);
                break;
            }
        }
    }
    void stop(){
        stop_=true;
    }
    ~RDMAForwarder(){
        ibv_dealloc_pd(pd_);
        rdma_destroy_id(listen_id_);
        rdma_destroy_event_channel(chan_);
    }
private:
    void create_connection(rdma_cm_id* cm_id){
        int num_devices{};

        // ibv_comp_channel *comp_chan{ibv_create_comp_channel(listen_id_->verbs)};
        // ibv_comp_channel *comp_chan{ibv_create_comp_channel(rdma_get_devices(&num_devices)[0])};
        // if(!comp_chan)LOG(__LINE__, "failed to create ibv comp channel");

        // ibv_cq *cq{ibv_create_cq(listen_id_->verbs, 2, nullptr, comp_chan, 0)};
        // ibv_cq *cq{ibv_create_cq(rdma_get_devices(&num_devices)[0], 2, nullptr, comp_chan, 0)};
        ibv_cq *cq{ibv_create_cq(rdma_get_devices(&num_devices)[0], 2*cq_len, nullptr, nullptr, 0)};
        if(!cq)LOG(__LINE__, "failed to create cq");
        // if(ibv_req_notify_cq(cq, 0))LOG(__LINE__, "failed to notify cq");

        ibv_qp_init_attr qp_init_attr{
                        .send_cq = cq,
                        .recv_cq = cq,
                        .cap{
                            .max_send_wr = cq_len,
                            .max_recv_wr = cq_len,
                            .max_send_sge = 1,
                            .max_recv_sge = 1
                        },
                        .qp_type = IBV_QPT_RC
                    };
        if(rdma_create_qp(cm_id, pd_, &qp_init_attr))LOG(__LINE__, "failed to create qp");

        CData cdata{};
        cdata.rkey = data_mr_->rkey;
        cdata.data = data_;
        rdma_conn_param conn_param{};
        conn_param.private_data = &cdata;
        conn_param.private_data_len = sizeof(cdata);
        conn_param.responder_resources = 1;
        if(rdma_accept(cm_id, &conn_param))LOG(__LINE__, "failed to accept connection");
        LOG(cm_id, "connected");
        client_map_[cm_id] = new RDMAClient();
        client_map_[cm_id]->connect(dest_, server_port);
    }
    bool stop_{};
    char dest_[20];
    rdma_event_channel *chan_{};
    rdma_cm_id *listen_id_{};
    ibv_pd *pd_{};
    char* data_{};
    ibv_mr* data_mr_{};
    uint64_t offset_{};
    std::map<rdma_cm_id*, RDMAClient*> client_map_{};
};
