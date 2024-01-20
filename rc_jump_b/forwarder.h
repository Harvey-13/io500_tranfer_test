#include "client.h"

class RDMAForwarder{
public:
    RDMAForwarder() = default;
    void transfer(char* source, char* dest, int listen_port){
        strncpy(dest_, dest, strlen(dest));

        int num_devices{};
        ctx_list_ = rdma_get_devices(&num_devices);
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
        if(rdma_listen(listen_id_, std::thread::hardware_concurrency()))LOG(__LINE__, "failed to begin rdma listen");
        pd_ = ibv_alloc_pd(ctx_list_[0]);
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
        rdma_free_devices(ctx_list_);
        ibv_dealloc_pd(pd_);
        rdma_destroy_id(listen_id_);
        rdma_destroy_event_channel(chan_);
        for(auto&[id, worker]:worker_map_)worker->stop(), worker->t_->join();
    }
private:
    class Worker{
    public:
        Worker(char* dest){
            //TODO:client移到外面，一个client足够
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
                int wc_num = ibv_poll_cq(cq_, cq_len, wc);
                if(wc_num)for(int i{};i<wc_num;++i){
                    switch (wc[i].opcode){
                        case IBV_WC_SEND:{
                            recv_wait--;
                            client_.post_send(data_, send_offset_, grain);
                            break;
                        }
                        default:
                            break;
                    }
                }
            }
        }
        void stop(){
            stop_=true;
            ibv_destroy_cq(cq_);
            ibv_destroy_qp(cm_id_->qp);
            client_.close();
        }
        ~Worker(){
            free(data_);
            LOG(__LINE__, "worker destroyed");
        }
        rdma_cm_id *cm_id_{};
        char* data_{};
        ibv_mr* data_mr_{};
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
        ibv_cq *cq{ibv_create_cq(ctx_list_[0], 1, nullptr, nullptr, 0)};
        if(!cq)LOG(__LINE__, "failed to create cq");

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
        worker->cm_id_ = cm_id, worker->cq_ = cq;
        worker->t_ = new std::thread(&Worker::run, worker);
        worker_map_[cm_id] = worker;
        if(rdma_accept(cm_id, nullptr))LOG(__LINE__, "failed to accept connection");
        LOG(worker->cm_id_, "connection created");
    }
    bool stop_{};
    char dest_[20];
    ibv_context** ctx_list_{};
    rdma_event_channel *chan_{};
    rdma_cm_id *listen_id_{};
    ibv_pd *pd_{};
    std::map<rdma_cm_id*, Worker*> worker_map_;
};
