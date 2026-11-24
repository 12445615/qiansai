/* 拆解计时：纯 rknn_run / inputs_set / outputs_get / fp16 转换 各自耗时
 * mode 0 = 单上下文单线程；mode 1 = 3 上下文 3 线程并行 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/time.h>
#include "rknn_api.h"
#include "Float16.h"

static int64_t now_us(void){ struct timeval tv; gettimeofday(&tv,NULL); return (int64_t)tv.tv_sec*1000000LL+tv.tv_usec; }
static unsigned char *load(const char *p,int *sz){ FILE*f=fopen(p,"rb"); if(!f){printf("open fail\n");exit(1);} fseek(f,0,SEEK_END);*sz=ftell(f);fseek(f,0,SEEK_SET); unsigned char*d=malloc(*sz); if(fread(d,1,*sz,f)!=(size_t)*sz){exit(1);} fclose(f); return d; }

static rknn_input_output_num io;
static rknn_tensor_attr ia;
static int is_fp16;
static uint32_t rgb_size;

typedef struct { rknn_context ctx; int runs; double run_ms, set_ms, get_ms, conv_ms; } Arg;

static void bench_one(Arg *a)
{
    rknn_input in; memset(&in,0,sizeof(in));
    in.index=0; in.fmt=RKNN_TENSOR_NHWC;
    void *buf = malloc(ia.size); memset(buf,0x40,ia.size);
    in.buf=buf;
    if (is_fp16){ in.type=RKNN_TENSOR_FLOAT16; in.size=ia.size; in.pass_through=1; }
    else { in.type=RKNN_TENSOR_UINT8; in.size=rgb_size; in.pass_through=0; }

    rknn_output outs[4]; memset(outs,0,sizeof(outs));
    for (uint32_t i=0;i<io.n_output;i++){ outs[i].want_float=1; }

    /* warmup */
    rknn_inputs_set(a->ctx,1,&in); rknn_run(a->ctx,NULL);
    rknn_outputs_get(a->ctx,io.n_output,outs,NULL); rknn_outputs_release(a->ctx,io.n_output,outs);

    int64_t t;
    t=now_us(); for(int i=0;i<a->runs;i++){ rknn_run(a->ctx,NULL);} a->run_ms=(now_us()-t)/1000.0/a->runs;
    t=now_us(); for(int i=0;i<a->runs;i++){ rknn_inputs_set(a->ctx,1,&in);} a->set_ms=(now_us()-t)/1000.0/a->runs;
    t=now_us(); for(int i=0;i<a->runs;i++){ rknn_outputs_get(a->ctx,io.n_output,outs,NULL); rknn_outputs_release(a->ctx,io.n_output,outs);} a->get_ms=(now_us()-t)/1000.0/a->runs;
    if (is_fp16){
        t=now_us();
        for(int i=0;i<a->runs;i++){
            uint8_t *rgb=(uint8_t*)buf; rknpu2::float16 *h=(rknpu2::float16*)buf;
            for (int64_t k=(int64_t)rgb_size-1;k>=0;--k) h[k]=(float)rgb[k]/255.0f;
            memset(buf,0x40,ia.size);
        }
        a->conv_ms=(now_us()-t)/1000.0/a->runs;
    }
    free(buf);
}

static void *th(void *p){ bench_one((Arg*)p); return NULL; }

int main(int argc,char**argv)
{
    const char *model=argc>1?argv[1]:"/root/model/best.rknn";
    int mode=argc>2?atoi(argv[2]):0;
    int sz=0; unsigned char*d=load(model,&sz);
    rknn_context c0; if(rknn_init(&c0,d,sz,0,NULL)<0){printf("init fail\n");return 1;} free(d);

    rknn_query(c0,RKNN_QUERY_IN_OUT_NUM,&io,sizeof(io));
    memset(&ia,0,sizeof(ia)); ia.index=0; rknn_query(c0,RKNN_QUERY_INPUT_ATTR,&ia,sizeof(ia));
    is_fp16 = (ia.type==RKNN_TENSOR_FLOAT16);
    rgb_size = ia.dims[1]*ia.dims[2]*ia.dims[3];
    printf("[bench2] %s input=%s size=%u\n", model, is_fp16?"FP16":"UINT8", ia.size);

    if (mode==0) {
        rknn_set_core_mask(c0, RKNN_NPU_CORE_0);
        Arg a={0}; a.ctx=c0; a.runs=15;
        bench_one(&a);
        printf("[bench2] single   run=%.1f  inputs_set=%.1f  outputs_get=%.1f  fp16conv=%.1f  (ms)\n",
               a.run_ms,a.set_ms,a.get_ms,a.conv_ms);
    } else {
        rknn_context c[3]; c[0]=c0;
        for(int i=1;i<3;i++) rknn_dup_context(&c0,&c[i]);
        rknn_core_mask m[3]={RKNN_NPU_CORE_0,RKNN_NPU_CORE_1,RKNN_NPU_CORE_2};
        pthread_t t[3]; Arg a[3];
        for(int i=0;i<3;i++){ rknn_set_core_mask(c[i],m[i]); memset(&a[i],0,sizeof(Arg)); a[i].ctx=c[i]; a[i].runs=15; }
        int64_t t0=now_us();
        for(int i=0;i<3;i++) pthread_create(&t[i],NULL,th,&a[i]);
        for(int i=0;i<3;i++) pthread_join(t[i],NULL);
        double wall=(now_us()-t0)/1000.0;
        for(int i=0;i<3;i++)
            printf("[bench2] ctx%d  run=%.1f  inputs_set=%.1f  outputs_get=%.1f  fp16conv=%.1f  (ms)\n",
                   i,a[i].run_ms,a[i].set_ms,a[i].get_ms,a[i].conv_ms);
        printf("[bench2] parallel3 wall=%.0f ms for 45 frames -> %.1f fps\n", wall, 45000.0/wall);
    }
    return 0;
}
