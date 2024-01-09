#include "client.h"

class RDMAForwarder{
public:
    RDMAForwarder() = default;
    void transfer(char* source, char* dest, int listen_port){
        strncpy(dest_, dest, strlen(dest));
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
        int num_devices{};
        pd_ = ibv_alloc_pd(rdma_get_devices(&num_devices)[0]);
        if(!pd_)LOG(__LINE__, "failed to alloc pd");
        LOG(__LINE__, "ready to listen");

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
                worker_map_[event->id]->stop();
                worker_map_[event->id]->t_->join();
                delete worker_map_[event->id];
                worker_map_.erase(event->id);
                LOG("client", event->id, "disconnected");
                rdma_ack_cm_event(event);
                break;
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
        for(auto&[id, worker]:worker_map_)worker->stop(), worker->t_->join();
    }
    ~RDMAForwarder(){
        ibv_dealloc_pd(pd_);
        rdma_destroy_id(listen_id_);
        rdma_destroy_event_channel(chan_);
        for(auto&[id, worker]:worker_map_)worker->stop(), worker->t_->join();
    }
private:
    class Worker{
    public:
        Worker(char* dest){
            client_.connect(dest, server_port);
            data_ = (char*)malloc(sendBytes);
            data_mr_ = client_.reg_mr(data_, sendBytes);
        }
        void run(){
            ibv_wc wc[cq_len]{};
            uint64_t timer{}, start{};
            uint64_t recv_wait{};
            for(;;){
                if(stop_)return;
                if(recv_wait<cq_len)post_recv(grain), recv_wait++;
                int wc_num = ibv_poll_cq(cq_,cq_len,wc);
                if(wc_num){
                    // LOG(__LINE__, "received wc num:", wc_num);
                    // if(ibv_get_cq_event(comp_chan_, &cq_, &ctx))LOG(__LINE__, "failed to get cq event");
                    // if(!ibv_poll_cq(cq_,1,&wc))LOG(__LINE__, "failed to poll cq");
                    for(int i{};i<wc_num;++i)
                        switch (wc[i].opcode)
                        {
                        case IBV_WC_SEND:{
                            recv_wait--;
                            // for(uint64_t i{(uintptr_t)data_+send_offset_};i<grain;++i)LOG(data_[i]);
                            // LOG('\n');
                            client_.post_send(data_, send_offset_, grain);
                            break;
                        }
                        default:
                            break;
                        }
                    // ibv_ack_cq_events(cq_, 1);
                    // if(ibv_req_notify_cq(cq_, 0))LOG(__LINE__, "failed to notify cq");
                }
            }
        }
        void stop(){
            stop_=true;
            ibv_destroy_cq(cq_);
            ibv_destroy_qp(cm_id_->qp);
            client_.close();
            // rdma_destroy_id(cm_id_);
            // ibv_destroy_comp_channel(comp_chan_);
        }
        ~Worker(){
            // ibv_dereg_mr(resp_mr_);
            // ibv_dereg_mr(msg_mr_);
            // ibv_destroy_cq(cq_);
            // ibv_destroy_comp_channel(comp_chan_);
            free(data_);
            LOG(__LINE__, "worker destroyed");
        }
        rdma_cm_id *cm_id_{};
        char* data_{};
        ibv_mr* data_mr_{};
        // ibv_comp_channel *comp_chan_{};
        ibv_cq *cq_{};
        uint64_t recv_offset_{}, send_offset_{};
        bool stop_{};
        std::thread *t_{};
    private:
        RDMAClient client_;
        void post_recv(int len){
            ibv_sge sge{
                .addr = (uint64_t)data_+recv_offset_,
                .length = len,
                .lkey = data_mr_->lkey
            };
            ibv_recv_wr wr{
                .next = nullptr,
                .sg_list = &sge,
                .num_sge = 1
            }, *bad_wr{};
            if(ibv_post_recv(cm_id_->qp, &wr, &bad_wr))LOG(__LINE__, "failed to post recv");
            recv_offset_ += grain;
        }
        void post_send(int len){
            ibv_sge sge{
                .addr = (uint64_t)data_+send_offset_,
                .length = len,
                .lkey = data_mr_->lkey
            };
            ibv_send_wr wr{
                .next = nullptr,
                .sg_list = &sge,
                .num_sge = 1,
                .opcode = IBV_WR_SEND,
                .send_flags = IBV_SEND_SIGNALED
            }, *bad_wr;
            if(ibv_post_send(cm_id_->qp, &wr, &bad_wr))LOG(__LINE__, "failed to post send");
            send_offset_ += grain;
        }
    };
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

        Worker *worker = new Worker(dest_);
        worker->cm_id_ = cm_id, worker->cq_ = cq;//, worker->comp_chan_ = comp_chan;
        // worker->msg_mr_ = ibv_reg_mr(pd_, worker->msg_buf_, sizeof(worker->msg_buf_), IBV_ACCESS_LOCAL_WRITE|
        //                                                                               IBV_ACCESS_REMOTE_READ|
        //                                                                               IBV_ACCESS_REMOTE_WRITE);
        // worker->resp_mr_ = ibv_reg_mr(pd_, worker->resp_buf_, sizeof(worker->resp_buf_), IBV_ACCESS_LOCAL_WRITE|
        //                                                                                  IBV_ACCESS_REMOTE_READ|
        //                                                                                  IBV_ACCESS_REMOTE_WRITE);
        worker->t_ = new std::thread(&Worker::run, worker);
        worker_map_[cm_id] = worker;
        if(rdma_accept(cm_id, nullptr))LOG(__LINE__, "failed to accept connection");
        LOG(worker->cm_id_, "connection created");
    }
    bool stop_{};
    char dest_[20];
    rdma_event_channel *chan_{};
    rdma_cm_id *listen_id_{};
    ibv_pd *pd_{};
    std::map<rdma_cm_id*, Worker*> worker_map_;
};
