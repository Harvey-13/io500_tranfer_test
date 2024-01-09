#include "log.h"

class RDMAClient{
public:
    RDMAClient()=default;
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
        bg_=now_=((CData*)conn_param.private_data)->data;
        rkey_ = ((CData*)conn_param.private_data)->rkey;

        if(rdma_get_cm_event(chan_, &event))LOG(__LINE__, "failed to get cm event");

        if(event->event != RDMA_CM_EVENT_ESTABLISHED)LOG(__LINE__, "failed to establish connect");
        rdma_ack_cm_event(event);
        msg_mr_ = ibv_reg_mr(pd_, msg_buf_, sizeof(msg_buf_), IBV_ACCESS_LOCAL_WRITE|
                                                              IBV_ACCESS_REMOTE_READ|
                                                              IBV_ACCESS_REMOTE_WRITE);
        resp_mr_ = ibv_reg_mr(pd_, resp_buf_, sizeof(resp_buf_), IBV_ACCESS_LOCAL_WRITE|
                                                                 IBV_ACCESS_REMOTE_READ|
                                                                 IBV_ACCESS_REMOTE_WRITE);
    }
    // void reg_mr(void* ptr, int len){
    //     outer_mrs_.emplace_back(ibv_reg_mr(pd_, ptr, len, IBV_ACCESS_LOCAL_WRITE|
    //                                              IBV_ACCESS_REMOTE_READ|
    //                                              IBV_ACCESS_REMOTE_WRITE));
    // }
    void remote_write(char* msg, int len){
        // LOG("wc waiting:", wc_wait_);
        while(wc_wait_>=cq_len){
            wc_wait_ -= ibv_poll_cq(cq_,cq_len,wc_);
            // LOG(wc_wait_);
        }
        strncpy(msg_buf_ptr_, msg, len);
        ibv_sge sge{
                .addr = (uint64_t)msg_buf_ptr_,
                .length = len,
                .lkey = msg_mr_->lkey
            };
        ibv_send_wr wr{
            .next = nullptr,
            .sg_list = &sge,
            .num_sge = 1,
            .opcode = IBV_WR_RDMA_WRITE,
            .send_flags = IBV_SEND_SIGNALED,
            .wr{
                .rdma{
                .remote_addr = (uintptr_t)now_,
                .rkey = rkey_
                }
            }
        }, *bad_wr;

        // void *ctx{};
        if(ibv_post_send(cm_id_->qp, &wr, &bad_wr))LOG(__LINE__, "failed to post send");
        // if(ibv_get_cq_event(comp_chan_, &cq_, &ctx))LOG(__LINE__, "failed to get cq event");
        // if(!ibv_poll_cq(cq_,1,&wc))LOG(__LINE__, "failed to poll cq");
        // if(ibv_req_notify_cq(cq_, 0))LOG(__LINE__, "failed to notify cq");
        // ibv_ack_cq_events(cq_, 1);
        wc_wait_++;
        // LOG(wc_wait_);
        wc_wait_ -= ibv_poll_cq(cq_,cq_len,wc_);
        // LOG(wc_wait_);
        now_ = now_ + grain;
        if(now_==bg_+grain*cq_len)now_ = bg_;
        remote_update();
        msg_buf_ptr_ += grain;
        if(msg_buf_ptr_ == msg_buf_ + grain * cq_len)msg_buf_ptr_ = msg_buf_;
    }
    std::string remote_read(int len){
        while(wc_wait_>=cq_len)wc_wait_ -= ibv_poll_cq(cq_,cq_len,wc_);
        memset(resp_buf_, 0, sizeof(resp_buf_));
        ibv_sge sge{
                .addr = (uint64_t)resp_buf_,
                .length = len,
                .lkey = resp_mr_->lkey
            };
        ibv_send_wr wr{
            .next = nullptr,
            .sg_list = &sge,
            .num_sge = 1,
            .opcode = IBV_WR_RDMA_READ,
            .send_flags = IBV_SEND_SIGNALED,
            .wr{
                .rdma{
                .remote_addr = (uintptr_t)now_,
                .rkey = rkey_
                }
            }
        }, *bad_wr;

        // void *ctx{};
        ibv_wc wc{};        
        if(ibv_post_send(cm_id_->qp, &wr, &bad_wr))LOG(__LINE__, "failed to post recv");
        // if(ibv_get_cq_event(comp_chan_, &cq_, &ctx))LOG(__LINE__, "failed to get cq event");
        // if(!ibv_poll_cq(cq_, 1, &wc))LOG(__LINE__, "failed to poll cq");
        // if(ibv_req_notify_cq(cq_, 0))LOG(__LINE__, "failed to notify cq");
        // ibv_ack_cq_events(cq_, 1);
        wc_wait_++;
        wc_wait_ -= ibv_poll_cq(cq_,cq_len,wc_);
        return resp_buf_;
    }
    void remote_update(){
        while(wc_wait_>=cq_len)wc_wait_ -= ibv_poll_cq(cq_,cq_len,wc_);
        strncpy(msg_buf_+grain*cq_len, (char*)&now_, 8);
        ibv_sge sge{
                .addr = (uint64_t)(msg_buf_+grain*cq_len),
                .length = 8,
                .lkey = msg_mr_->lkey
            };
        ibv_send_wr wr{
            .next = nullptr,
            .sg_list = &sge,
            .num_sge = 1,
            .opcode = IBV_WR_RDMA_WRITE,
            .send_flags = IBV_SEND_SIGNALED,
            .wr{
                .rdma{
                .remote_addr = (uintptr_t)(bg_+grain*cq_len),
                .rkey = rkey_
                }
            }
        }, *bad_wr;
        ibv_wc wc{};        
        if(ibv_post_send(cm_id_->qp, &wr, &bad_wr))LOG(__LINE__, "failed to update");
        wc_wait_++;
        wc_wait_ -= ibv_poll_cq(cq_,cq_len,wc_);
    }
    void close(){
        while(wc_wait_)wc_wait_ -= ibv_poll_cq(cq_,cq_len,wc_);
        rdma_disconnect(cm_id_);
        rdma_cm_event *event{};
        if(rdma_get_cm_event(chan_, &event))LOG(__LINE__, "failed to get cm event");
        if(event->event != RDMA_CM_EVENT_DISCONNECTED)LOG(__LINE__, "failed to disconnect");
        rdma_ack_cm_event(event);
        LOG(__LINE__, "client closed");
    }
    ~RDMAClient(){
        ibv_dealloc_pd(pd_);
        ibv_dereg_mr(msg_mr_), ibv_dereg_mr(resp_mr_);
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
    ibv_mr *msg_mr_{}, *resp_mr_{};
    uint32_t rkey_{};
    void *bg_{}, *now_{};
    // std::vector<ibv_mr*> outer_mrs_;
    char msg_buf_[grain*cq_len+8]{}, resp_buf_[grain*cq_len]{}, *data_;
    char*msg_buf_ptr_{msg_buf_}, *resp_buf_ptr_{resp_buf_};
    uint64_t wc_wait_{};
};