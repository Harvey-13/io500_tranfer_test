#include "log.h"

class RDMAServer{
public:
    RDMAServer() = default;
    void listen(char* ip, int port){
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
    ~RDMAServer(){
        ibv_dealloc_pd(pd_);
        rdma_destroy_id(listen_id_);
        rdma_destroy_event_channel(chan_);
        for(auto&[id, worker]:worker_map_)worker->stop(), worker->t_->join();
    }
private:
    class Worker{
    public:
        Worker(){
            lat_hist_.Clear();
        };
        void run(){
            ibv_wc wc[cq_len]{};
            void *ctx{};
            uint64_t timer{}, start{};
            uint64_t recv_bytes{}, recv_wait{}, recv_cnt{};
            for(;;){
                if(stop_){
                    LOG(lat_hist_.ToString());
                    LOG("throughput(MB/s):", recv_bytes/1024.0/1024/(timer-start)*1e9);
                    return;
                }
                if(recv_wait<cq_len)post_recv(grain), recv_wait++;
                int wc_num = ibv_poll_cq(cq_,cq_len,wc);
                recv_wait -= wc_num;

                if(wc_num){
                    if(!start)start = timer = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock().now().time_since_epoch()).count();
                    else{
                        uint64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock().now().time_since_epoch()).count();
                        lat_hist_.Add((now - timer)/1000.0/wc_num);
                        timer = now;
                    }
                    for(int i{};i<wc_num;++i)
                        switch (wc[i].opcode)
                        {
                        case IBV_WC_RECV:{
                            recv_bytes += grain, recv_cnt++;
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
            ibv_dereg_mr(resp_mr_);
            ibv_dereg_mr(msg_mr_);
            ibv_destroy_cq(cq_);
            ibv_destroy_qp(cm_id_->qp);
        }
        ~Worker(){
            LOG(cm_id_, "worker destroyed");
        }
        rdma_cm_id *cm_id_{};
        ibv_mr *resp_mr_{}, *msg_mr_{};
        char resp_buf_[grain*cq_len]{}, msg_buf_[grain*cq_len]{};
        ibv_cq *cq_{};

        bool stop_{};
        std::thread *t_{};
    private:
        leveldb::Histogram lat_hist_;
        void post_recv(int len){
            ibv_sge sge{
                .addr = (uint64_t)msg_buf_,
                .length = len,
                .lkey = msg_mr_->lkey
            };
            ibv_recv_wr wr{
                .next = nullptr,
                .sg_list = &sge,
                .num_sge = 1
            }, *bad_wr{};
            if(ibv_post_recv(cm_id_->qp, &wr, &bad_wr))LOG(__LINE__, "failed to post recv");
        }
        void post_send(int len){
            ibv_sge sge{
                .addr = (uint64_t)resp_buf_,
                .length = len,
                .lkey = resp_mr_->lkey
            };
            ibv_send_wr wr{
                .next = nullptr,
                .sg_list = &sge,
                .num_sge = 1,
                .opcode = IBV_WR_SEND,
                .send_flags = IBV_SEND_SIGNALED
            }, *bad_wr;
            if(ibv_post_send(cm_id_->qp, &wr, &bad_wr))LOG(__LINE__, "failed to post send");
        }
    };
    void create_connection(rdma_cm_id* cm_id){
        int num_devices{};
        ibv_cq *cq{ibv_create_cq(rdma_get_devices(&num_devices)[0], cq_len, nullptr, nullptr, 0)};
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

        Worker *worker = new Worker;
        worker->cm_id_ = cm_id, worker->cq_ = cq;
        worker->msg_mr_ = ibv_reg_mr(pd_, worker->msg_buf_, sizeof(worker->msg_buf_), IBV_ACCESS_LOCAL_WRITE|
                                                                                      IBV_ACCESS_REMOTE_READ|
                                                                                      IBV_ACCESS_REMOTE_WRITE);
        worker->resp_mr_ = ibv_reg_mr(pd_, worker->resp_buf_, sizeof(worker->resp_buf_), IBV_ACCESS_LOCAL_WRITE|
                                                                                         IBV_ACCESS_REMOTE_READ|
                                                                                         IBV_ACCESS_REMOTE_WRITE);
        worker->t_ = new std::thread(&Worker::run, worker);
        worker_map_[cm_id] = worker;
        if(rdma_accept(cm_id, nullptr))LOG(__LINE__, "failed to accept connection");
    }
    bool stop_{};
    rdma_event_channel *chan_{};
    rdma_cm_id *listen_id_{};
    ibv_pd *pd_{};
    std::map<rdma_cm_id*, Worker*> worker_map_;
};