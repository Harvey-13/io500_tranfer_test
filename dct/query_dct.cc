#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <rdma/rdma_verbs.h>
const int cq_len{64};
const uint64_t dct_key{123};

int main(){
    // int num_devices{};
    // ibv_context* ctx = rdma_get_devices(&num_devices)[0];
    // ibv_pd* pd = ibv_alloc_pd(ctx);
    // ibv_cq* cq = ibv_create_cq(ctx, cq_len, nullptr, nullptr, 0);

    // ibv_qp_init_attr_ex qp_init_attr_ex{};
    // qp_init_attr_ex.cap = {
    //     .max_send_wr = cq_len,
    //     .max_recv_wr = cq_len,
    //     .max_send_sge = 1,
    //     .max_recv_sge = 1
    // };
    // qp_init_attr_ex.send_cq = qp_init_attr_ex.recv_cq = cq;

    // mlx5dv_qp_init_attr mlx5qp_init_attr{};
    // mlx5qp_init_attr.dc_init_attr.dc_type = MLX5DV_DCTYPE_DCI;
    // mlx5qp_init_attr.dc_init_attr.dct_access_key = dct_key;

    // ibv_qp* qp = mlx5dv_create_qp(ctx, &qp_init_attr_ex, &mlx5qp_init_attr);
    // ibv_query_ece
    struct ibv_device **dev_list;
    struct ibv_context *ctx;
    struct ibv_device_attr dev_attr;

    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        fprintf(stderr, "Failed to get IB devices list\n");
        return 1;
    }

    ctx = ibv_open_device(dev_list[0]);
    if (!ctx) {
        fprintf(stderr, "Failed to open IB device\n");
        ibv_free_device_list(dev_list);
        return 1;
    }

    // 查询设备属性
    if (ibv_query_device(ctx, &dev_attr) != 0) {
        fprintf(stderr, "Failed to query device attributes\n");
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        return 1;
    }

    printf("Max MR size: %lu GB\n", dev_attr.max_mr_size/1024/1024/1024);

    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);
    return 0;
}