#include "log.h"

// class RingBuffer{
// public:

// private:
//     void* data_{};
//     void* read_ptr_{}, *write_ptr_{};
// };

class RDMAClientRW{
public:
    RDMAClientRW()=default;
    void connect(char* ip, int port){
        chan_ = rdma_create_event_channel();
        if(!chan_)LOG(__LINE__, "failed to create rdma chan");

        if(rdma_create_id(chan_, &cm_id_, nullptr, RDMA_PS_TCP))LOG(__LINE__, "failed to create cmid");

        if (!cm_id_) {
            abort();
        }

        addrinfo *res;
        if(getaddrinfo(ip, std::to_string(port).c_str(), nullptr, &res)<0)LOG(__LINE__, "failed to resolve addr");

        addrinfo *t{res};
        for(;t;t=t->ai_next)
            if(!rdma_resolve_addr(cm_id_, nullptr, t->ai_addr, 500))break;
        if(!t)LOG(__LINE__, "failed to resolve addr");

        rdma_cm_event *event{};
        if(rdma_get_cm_event(chan_, &event))LOG(__LINE__, "failed to get cm event");
        if(event->event != RDMA_CM_EVENT_ADDR_RESOLVED)LOG(__LINE__, "failed to resolve addr info");
        rdma_ack_cm_event(event);

        if(rdma_resolve_route(cm_id_, 1000))LOG(__LINE__, "failed to resolve route");
        if(rdma_get_cm_event(chan_, &event))LOG(__LINE__, "failed to get cm event");
        if(event->event != RDMA_CM_EVENT_ROUTE_RESOLVED)LOG(__LINE__, "failed to resolve route");
        rdma_ack_cm_event(event);
        
        pd_ = ibv_alloc_pd(cm_id_->verbs);
        if(!pd_)LOG(__LINE__, "failed to alloc pd");

        // comp_chan_ = ibv_create_comp_channel(cm_id_->verbs);
        // if(!comp_chan_)LOG(__LINE__, "failed to create comp chan");

        cq_ = ibv_create_cq(cm_id_->verbs, cq_len, nullptr, nullptr, 0);//comp_chan_, 0);
        if(!cq_)LOG(__LINE__, "failed to create cq");

        // if(ibv_req_notify_cq(cq_, 0))LOG(__LINE__, "failed to notify cq");
        
        ibv_qp_init_attr qp_init_attr{
                        .send_cq = cq_,
                        .recv_cq = cq_,
                        .cap{
                            .max_send_wr = cq_len,
                            .max_recv_wr = cq_len,
                            .max_send_sge = 1,
                            .max_recv_sge = 1
                        },
                        .qp_type = IBV_QPT_RC
                    };
        if(rdma_create_qp(cm_id_, pd_, &qp_init_attr))LOG(__LINE__, "failed to create qp");
        rdma_conn_param conn_param{};
        if(rdma_connect(cm_id_, &conn_param))LOG(__LINE__, "failed to connect");
        remote_bg_= remote_ptr_ =(uint64_t)((CData*)conn_param.private_data)->data;
        rkey_ = ((CData*)conn_param.private_data)->rkey;
        if(rdma_get_cm_event(chan_, &event))LOG(__LINE__, "failed to get cm event");

        if(event->event != RDMA_CM_EVENT_ESTABLISHED)LOG(__LINE__, "failed to establish connect");
        rdma_ack_cm_event(event);
        // msg_mr_ = ibv_reg_mr(pd_, msg_buf_, sizeof(msg_buf_), IBV_ACCESS_LOCAL_WRITE|
        //                                                       IBV_ACCESS_REMOTE_READ|
        //                                                       IBV_ACCESS_REMOTE_WRITE);
        // resp_mr_ = ibv_reg_mr(pd_, resp_buf_, sizeof(resp_buf_), IBV_ACCESS_LOCAL_WRITE|
        //                                                          IBV_ACCESS_REMOTE_READ|
        //                                                          IBV_ACCESS_REMOTE_WRITE);
    }
    ibv_mr* reg_mr(void* ptr, uint64_t len){
        return outer_mr_map_[ptr] = ibv_reg_mr(pd_, ptr, len, IBV_ACCESS_LOCAL_WRITE|
                                                 IBV_ACCESS_REMOTE_READ|
                                                 IBV_ACCESS_REMOTE_WRITE);
    }
    void dereg_mr(void* ptr){
        ibv_dereg_mr(outer_mr_map_[ptr]);
        outer_mr_map_.erase(ptr);
    }
    void remote_write(void* msg, uint64_t offset, uint64_t len){
        // LOG("wc waiting:", wc_wait_);
        while(wc_wait_>=cq_len)wc_wait_ -= ibv_poll_cq(cq_,cq_len,wc_);
        ibv_sge sge{
                .addr = (uint64_t)msg+offset,
                .length = len,
                .lkey = outer_mr_map_[msg]->lkey
            };
            ibv_send_wr wr{
                .next = nullptr,
                .sg_list = &sge,
                .num_sge = 1,
                .opcode = IBV_WR_RDMA_WRITE,
                .send_flags = IBV_SEND_SIGNALED,
                .wr{
                    .rdma{
                    .remote_addr = remote_ptr_,
                    .rkey = rkey_
                    }
                }
            }, *bad_wr;

        // void *ctx{};
        if(ibv_post_send(cm_id_->qp, &wr, &bad_wr))LOG(__LINE__, "failed to post send");
        wc_wait_++;
        wc_wait_ -= ibv_poll_cq(cq_,cq_len,wc_);
        remote_ptr_ += grain;
    }
    void update_remote_offset(void* msg, uint64_t offset){
        while(wc_wait_>=cq_len)wc_wait_ -= ibv_poll_cq(cq_,cq_len,wc_);
        ibv_sge sge{
                .addr = (uint64_t)msg+offset,
                .length = 8,
                .lkey = outer_mr_map_[msg]->lkey
            };
            ibv_send_wr wr{
                .next = nullptr,
                .sg_list = &sge,
                .num_sge = 1,
                .opcode = IBV_WR_ATOMIC_FETCH_AND_ADD,
                .send_flags = IBV_SEND_SIGNALED,
            }, *bad_wr;
        wr.wr.atomic.remote_addr = remote_bg_ + sendBytes;
        wr.wr.atomic.rkey = rkey_;
        wr.wr.atomic.compare_add = grain;
        // void *ctx{};
        if(ibv_post_send(cm_id_->qp, &wr, &bad_wr))LOG(__LINE__, "failed to post send");
        wc_wait_++;
        wc_wait_ -= ibv_poll_cq(cq_,cq_len,wc_);
    }
    void close(){
        while(wc_wait_)wc_wait_ -= ibv_poll_cq(cq_,cq_len,wc_);
        rdma_disconnect(cm_id_);
        for(auto&[ptr, mr]:outer_mr_map_)ibv_dereg_mr(mr);
        outer_mr_map_.clear();
        // rdma_cm_event *event{};
        // if(rdma_get_cm_event(chan_, &event))LOG(__LINE__, "failed to get cm event");
        // if(event->event != RDMA_CM_EVENT_DISCONNECTED)LOG(__LINE__, "failed to disconnect");
        // rdma_ack_cm_event(event);
        LOG(__LINE__, "client closed");
    }
    ~RDMAClientRW(){
        ibv_dealloc_pd(pd_);
        // ibv_dereg_mr(msg_mr_), ibv_dereg_mr(resp_mr_);
        // for(auto&mr:outer_mrs_)ibv_dereg_mr(mr);
        ibv_destroy_cq(cq_);
        // ibv_destroy_comp_channel(comp_chan_);
        ibv_destroy_qp(cm_id_->qp);
        rdma_destroy_id(cm_id_);
        rdma_destroy_event_channel(chan_);
    }
private:
    rdma_event_channel *chan_{};
    rdma_cm_id *cm_id_{};
    ibv_pd *pd_{};
    // ibv_comp_channel *comp_chan_{};
    ibv_cq *cq_{};
    ibv_wc wc_[cq_len]{};
    std::map<void*, ibv_mr*> outer_mr_map_{};
    uint64_t wc_wait_{}, rkey_{}, remote_ptr_{}, remote_bg_{};
    
};