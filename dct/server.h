#include "log.h"

class RDMAServer{
public:
    RDMAServer() = default;
    void listen(char* ip, int port){
        int num_devices{};
        ctx_ = rdma_get_devices(&num_devices)[0];
        if(!ctx_)LOG(__LINE__, "failed to open device");

        chan_ = rdma_create_event_channel();
        if(!chan_)LOG(__LINE__, "failed to create event channel");

        rdma_create_id(chan_, &listen_id_, nullptr, RDMA_PS_TCP);
        if(!listen_id_)LOG(__LINE__, "failed to create listen cm_id");

        sockaddr_in sin{
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = {
            .s_addr = inet_addr(ip)
        }
        };
        if(rdma_bind_addr(listen_id_, (sockaddr*)&sin))LOG(__LINE__, "failed to bind addr");
        if(rdma_listen(listen_id_, 1))LOG(__LINE__, "failed to begin rdma listen");
        // pd_ = ibv_alloc_pd(listen_id_->verbs);

        pd_ = ibv_alloc_pd(ctx_);
        if(!pd_)LOG(__LINE__, "failed to alloc pd");

        ibv_srq_init_attr srq_init_attr{};
        srq_init_attr.srq_context = ctx_;
        srq_init_attr.attr = {
            .max_wr = cq_len,
            .max_sge = 1,
            .srq_limit = 0
        };

        srq_ = ibv_create_srq(pd_, &srq_init_attr);
        if(!srq_)LOG(__LINE__, "failed to create srq");

        cq_ = ibv_create_cq(ctx_, cq_len, nullptr, nullptr, 0);
        if(!cq_)LOG(__LINE__, "failed to create cq");

        ibv_qp_init_attr_ex attr_ex{};
        mlx5dv_qp_init_attr attr_dv{};
        attr_ex.qp_type = IBV_QPT_DRIVER;
        attr_ex.send_cq = cq_;
        attr_ex.comp_mask |= IBV_QP_INIT_ATTR_PD;
        attr_ex.pd = pd_;
        attr_ex.srq = srq_;
        attr_dv.comp_mask |= MLX5DV_QP_INIT_ATTR_MASK_DC;
		attr_dv.dc_init_attr.dc_type = MLX5DV_DCTYPE_DCT;
		attr_dv.dc_init_attr.dct_access_key = dc_key;
        qp_ = mlx5dv_create_qp(ctx_, &attr_ex, &attr_dv);
        if(!qp_)LOG(__LINE__, "failed to create qp");
        modify_qp_to_rtr();

        LOG(__LINE__, "ready to listen");

        std::thread receiver([this](){
            char buffer_[grain*cq_len]{};
            ibv_mr *buf_mr_ = ibv_reg_mr(this->pd_, buffer_, sizeof(buffer_), IBV_ACCESS_LOCAL_WRITE|
                                                                              IBV_ACCESS_REMOTE_READ|
                                                                              IBV_ACCESS_REMOTE_WRITE);
            ibv_sge sge{
                .addr = (uint64_t)buffer_,
                .length = grain,
                .lkey = buf_mr_->lkey
            };
            ibv_recv_wr wr{
                .next = nullptr,
                .sg_list = &sge,
                .num_sge = 1
            }, *bad_wr{};
            ibv_wc wc{};
            for(;;){
                if(this->stop_)break;
                ibv_post_srq_recv(this->srq_, &wr, &bad_wr);
                while(!ibv_poll_cq(this->qp_->recv_cq, 1, &wc));
                LOG("received:", buffer_);
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
                if(rdma_accept(cm_id, nullptr))LOG(__LINE__, "failed to accept connection");
                break;
            }
            case RDMA_CM_EVENT_DISCONNECTED:{
                // for(auto&w:workers_)
                //     if(w->cm_id_==event->id){
                //         w->stop();
                //         w->t_->join();
                //         LOG("client", w->cm_id_, "disconnected");
                //         workers_.pop_back();
                //         break;
                //     }
                
                break;
            }
            default:
                break;
            }
            rdma_ack_cm_event(event);
        }
    }
    void stop(){
        stop_=true;
    }
    ~RDMAServer(){
        ibv_dealloc_pd(pd_);
        rdma_destroy_id(listen_id_);
        rdma_destroy_event_channel(chan_);
    }
private:
    void modify_qp_to_rtr(){
        int attr_mask = 0;

        /* modify QP to INIT */
        {
            attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;

            struct ibv_qp_attr attr = {
                .qp_state        = IBV_QPS_INIT,
                .pkey_index      = 0,
                .port_num        = listen_id_->port_num,
            };

            attr_mask |= IBV_QP_ACCESS_FLAGS;
            attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;


            if (ibv_modify_qp(qp_, &attr, attr_mask))LOG(__LINE__, "failed to modify qp to init");
        }

        /* modify QP to RTR */
        {
            attr_mask = IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_AV;
            ibv_port_attr port_attr;
            ibv_query_port(listen_id_->verbs, listen_id_->port_num, &port_attr);
            struct ibv_qp_attr attr = {
                .qp_state               = IBV_QPS_RTR,
                .path_mtu               = port_attr.active_mtu,
                .rq_psn                 = 0,
                .ah_attr                {
                    .sl             = 0,
                    .src_path_bits  = 0,
                    .is_global      = 1,
                    .port_num       = listen_id_->port_num,
                },
                .min_rnr_timer          = 0x10
            };
            attr.ah_attr.grh.hop_limit  = 1;
            attr.ah_attr.grh.sgid_index = 0;
            attr.ah_attr.grh.traffic_class = 0;
            attr_mask |= IBV_QP_MIN_RNR_TIMER;

            if (ibv_modify_qp(qp_, &attr, attr_mask))LOG(__LINE__, "failed to modify QP to IBV_QPS_RTR");
        }
    }
    bool stop_{};
    rdma_event_channel *chan_{};
    rdma_cm_id *listen_id_{};
    ibv_pd *pd_{};
    ibv_cq *cq_{};
    ibv_qp *qp_{};
    ibv_srq *srq_{};
    ibv_context* ctx_{};
};