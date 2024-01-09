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

        cq_ = ibv_create_cq(cm_id_->verbs, cq_len, nullptr, nullptr, 0);//comp_chan_, 0);
        if(!cq_)LOG(__LINE__, "failed to create cq");

        ibv_qp_init_attr_ex attr_ex{};
        mlx5dv_qp_init_attr attr_dv{};

        attr_ex.qp_type = IBV_QPT_DRIVER;
        attr_ex.send_cq = attr_ex.recv_cq = cq_;

        attr_ex.comp_mask |= IBV_QP_INIT_ATTR_PD;
        attr_ex.pd = pd_;
        
        /* create DCI */
		attr_dv.comp_mask |= MLX5DV_QP_INIT_ATTR_MASK_DC;
		attr_dv.dc_init_attr.dc_type = MLX5DV_DCTYPE_DCI;

		attr_ex.cap.max_send_wr = cq_len;
		attr_ex.cap.max_send_sge = 1;

		attr_ex.comp_mask |= IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;
		attr_ex.send_ops_flags = IBV_QP_EX_WITH_RDMA_WRITE;

		attr_dv.comp_mask |= MLX5DV_QP_INIT_ATTR_MASK_QP_CREATE_FLAGS;
		attr_dv.create_flags |= MLX5DV_QP_CREATE_DISABLE_SCATTER_TO_CQE; 

		qp_ = mlx5dv_create_qp(cm_id_->verbs, &attr_ex, &attr_dv);
        qpex_ = ibv_qp_to_qp_ex(qp_);
        mqpex_ = mlx5dv_qp_ex_from_ibv_qp_ex(qpex_);
        modify_qp_to_rts();
        // ibv_qp_init_attr qp_init_attr{
        //                 .send_cq = cq_,
        //                 .recv_cq = cq_,
        //                 .cap{
        //                     .max_send_wr = cq_len,
        //                     .max_recv_wr = cq_len,
        //                     .max_send_sge = 1,
        //                     .max_recv_sge = 1
        //                 },
        //                 .qp_type = IBV_QPT_RC
        //             };
        // if(rdma_create_qp(cm_id_, pd_, &qp_init_attr))LOG(__LINE__, "failed to create qp");
        if(rdma_connect(cm_id_, nullptr))LOG(__LINE__, "failed to connect");
        if(rdma_get_cm_event(chan_, &event))LOG(__LINE__, "failed to get cm event");

        if(event->event != RDMA_CM_EVENT_ESTABLISHED)LOG(__LINE__, "failed to establish connect");
        rdma_ack_cm_event(event);
        msg_mr_ = ibv_reg_mr(pd_, msg_buf_, sizeof(msg_buf_), IBV_ACCESS_LOCAL_WRITE|
                                                              IBV_ACCESS_REMOTE_READ|
                                                              IBV_ACCESS_REMOTE_WRITE);
        resp_mr_ = ibv_reg_mr(pd_, resp_buf_, sizeof(resp_buf_), IBV_ACCESS_LOCAL_WRITE|
                                                                 IBV_ACCESS_REMOTE_READ|
                                                                 IBV_ACCESS_REMOTE_WRITE);
        rdma_establish(cm_id_);
    }
    // void reg_mr(void* ptr, int len){
    //     outer_mrs_.emplace_back(ibv_reg_mr(pd_, ptr, len, IBV_ACCESS_LOCAL_WRITE|
    //                                              IBV_ACCESS_REMOTE_READ|
    //                                              IBV_ACCESS_REMOTE_WRITE));
    // }
    void post_send(char* msg, int len){
        // LOG("wc waiting:", wc_wait_);
        while(wc_wait_>=cq_len){
            wc_wait_ -= ibv_poll_cq(cq_,cq_len,wc_);
            // LOG(wc_wait_);
        }
        memset(msg_buf_, 0, sizeof(msg_buf_));
        strncpy(msg_buf_, msg, len);
        ibv_sge sge{
                .addr = (uint64_t)msg_buf_,
                .length = len,
                .lkey = msg_mr_->lkey
            };
            ibv_send_wr wr{
                .next = nullptr,
                .sg_list = &sge,
                .num_sge = 1,
                .opcode = IBV_WR_SEND,
                .send_flags = IBV_SEND_SIGNALED
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
    }
    std::string post_recv(int len){
        while(wc_wait_>=cq_len)wc_wait_ -= ibv_poll_cq(cq_,cq_len,wc_);
        memset(resp_buf_, 0, sizeof(resp_buf_));
        ibv_sge sge{
                .addr = (uint64_t)resp_buf_,
                .length = len,
                .lkey = resp_mr_->lkey
            };
            ibv_recv_wr wr{
                .next = nullptr,
                .sg_list = &sge,
                .num_sge = 1
            }, *bad_wr{};

        // void *ctx{};
        ibv_wc wc{};        
        if(ibv_post_recv(cm_id_->qp, &wr, &bad_wr))LOG(__LINE__, "failed to post recv");
        // if(ibv_get_cq_event(comp_chan_, &cq_, &ctx))LOG(__LINE__, "failed to get cq event");
        // if(!ibv_poll_cq(cq_, 1, &wc))LOG(__LINE__, "failed to poll cq");
        // if(ibv_req_notify_cq(cq_, 0))LOG(__LINE__, "failed to notify cq");
        // ibv_ack_cq_events(cq_, 1);
        wc_wait_++;
        wc_wait_ -= ibv_poll_cq(cq_,cq_len,wc_);
        return resp_buf_;
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
    void modify_qp_to_rts(){
        int attr_mask{};
        /* modify QP to INIT */
        {
            attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;

            struct ibv_qp_attr attr = {
                .qp_state        = IBV_QPS_INIT,
                .pkey_index      = 0,
                .port_num        = cm_id_->port_num,
            };
            if (ibv_modify_qp(qp_, &attr, attr_mask))LOG(__LINE__,"failed to modify QP to IBV_QPS_INIT");
        }

        /* modify QP to RTR */
        {
            attr_mask = IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_AV;
            ibv_port_attr port_attr;
            ibv_query_port(cm_id_->verbs, cm_id_->port_num, &port_attr);
            struct ibv_qp_attr attr = {
                .qp_state               = IBV_QPS_RTR,
                .path_mtu               = port_attr.active_mtu,
                .rq_psn                 = 0,
                .ah_attr                {
                    .sl             = 0,
                    .src_path_bits  = 0,
                    .is_global      = 1,
                    .port_num       = cm_id_->port_num,
                },
                .min_rnr_timer          = 0x10
            };
            attr.ah_attr.grh.hop_limit  = 1;
            attr.ah_attr.grh.sgid_index = 0;
            attr.ah_attr.grh.traffic_class = 0;
            if (ibv_modify_qp(qp_, &attr, attr_mask)) LOG(__LINE__, "failed to modify QP to IBV_QPS_RTR");
        }

        /* modify QP to RTS */
        attr_mask = IBV_QP_STATE | IBV_QP_TIMEOUT |
            IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
            IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
            // Optional: IB_QP_MIN_RNR_TIMER

        struct ibv_qp_attr attr = {
            .qp_state               = IBV_QPS_RTS,
            .sq_psn                 = 0,
            .max_rd_atomic          = 1,
            .timeout                = 0x10,
            .retry_cnt              = 7,
            .rnr_retry              = 7,
        };
        if (ibv_modify_qp(qp_, &attr, attr_mask))LOG(__LINE__,"failed to modify QP to IBV_QPS_RTS");
    }
    rdma_event_channel *chan_{};
    rdma_cm_id *cm_id_{};
    ibv_pd *pd_{};
    // ibv_comp_channel *comp_chan_{};
    ibv_cq *cq_{};
    ibv_qp *qp_{};		/* DCI (client) or DCT (server) */
    ibv_qp_ex *qpex_{};		
    mlx5dv_qp_ex *mqpex_{};	
	ibv_ah *ah_{};
    ibv_wc wc_[cq_len]{};
    ibv_mr *msg_mr_{}, *resp_mr_{};
    // std::vector<ibv_mr*> outer_mrs_;
    char msg_buf_[grain]{}, resp_buf_[grain]{}, *data_;
    uint64_t wc_wait_{};
};