#include <cstring>
#include <rknn/rknn_api.h>
#include <rga/im2d.h>

int main() {
    rknn_input_output_num io;
    std::memset(&io, 0, sizeof(io));
    (void)rknn_query(0, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    rga_buffer_t src;
    rga_buffer_t dst;
    std::memset(&src, 0, sizeof(src));
    std::memset(&dst, 0, sizeof(dst));
    (void)imcheck(src, dst, {}, {});
    return 0;
}
