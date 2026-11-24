#include <libavcodec/avcodec.h>

int main(void) {
    return (avcodec_find_encoder_by_name("h264_rkmpp") != 0 &&
            avcodec_find_encoder_by_name("aac") != 0) ? 0 : 1;
}
