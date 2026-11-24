/* 在 bench2 基础上加入 post_process 计时，用于解释 app 侧 last_us */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/time.h>
#include "rknn_api.h"
#include "postprocess.h"
#include "Float16.h"

static int64_t now_us(void){ struct timeval tv; gettimeofday(&tv,NULL); return (int64_t)tv.tv_sec*1000000LL+tv.tv_usec; }
static unsigned char *load(const char *p,int *sz){ FILE*f=fopen(p,"rb"); if(!f){printf("open fail\n");exit(1);} fseek(f,0,SEEK_END);*sz=ftell(f);fseek(f,0,SEEK_SET); unsigned char*d=(unsigned char*)malloc(*sz); if(fread(d,1,*sz,f)!=(size_t)*sz){exit(1);} fclose(f); return d; }

static rknn_input_output_num io;
static rknn_tensor_attr ia, oa[8];
static int is_fp16; static uint32_t rgb_size;

typedef struct { rknn_context ctx; int runs; double run_ms, conv_ms, get_ms, pp_ms; } Arg;

static void bench_one(Arg *a)
{
    rknn_input in; memset(&in,0,sizeof(in));
    in.index=0; in.fmt=RKNN_TENSOR_NHWC;
    void *buf=malloc(ia.size); memset(buf,0x40,ia.size); in.buf=buf;
    if(is_fp16){ in.type=RKNN_TENSOR_FLOAT16; in.size=ia.size; in.pass_through=1; }
    else { in.type=RKNN_TENSOR_UINT8; in.size=rgb_size; in.pass_through=0; }

    rknn_output outs[8];
    int64_t t;

    rknn_inputs_set(a->ctx,1,&in); rknn_run(a->ctx,NULL);
    memset(outs,0,sizeof(outs)); for(uint32_t i=0;i<io.n_output;i++) outs[i].want_float=1;
    rknn_outputs_get(a->ctx,io.n_output,outs,NULL);

    /* 单独测 run */
    t=now_us(); for(int i=0;i<a->runs;i++) rknn_run(a->ctx,NULL); a->run_ms=(now_us()-t)/1000.0/a->runs;

    /* 单独测 fp16 转换 */
    if(is_fp16){
        t=now_us();
        for(int i=0;i<a->runs;i++){
            uint8_t*rgb=(uint8_t*)buf; rknpu2::float16*h=(rknpu2::float16*)buf;
            for(int64_t k=(int64_t)rgb_size-1;k>=0;--k) h[k]=(float)rgb[k]/255.0f;
            memset(buf,0x40,ia.size);
        }
        a->conv_ms=(now_us()-t)/1000.0/a->runs;
    }

    /* 测 outputs_get（含反量化） */
    t=now_us();
    for(int i=0;i<a->runs;i++){
        rknn_outputs_release(a->ctx,io.n_output,outs);
        memset(outs,0,sizeof(outs)); for(uint32_t k=0;k<io.n_output;k++) outs[k].want_float=1;
        rknn_outputs_get(a->ctx,io.n_output,outs,NULL);
    }
    a->get_ms=(now_us()-t)/1000.0/a->runs;

    /* 测后处理（复用刚拿到的输出） */
    object_detect_result_list od;
    t=now_us();
    for(int i=0;i<a->runs;i++) post_process(outs,&oa[0],io.n_output,0.5f,0.45f,&od);
    a->pp_ms=(now_us()-t)/1000.0/a->runs;

    rknn_outputs_release(a->ctx,io.n_output,outs);
    free(buf);
}

static void *th(void*p){ bench_one((Arg*)p); return NULL; }

int main(int argc,char**argv)
{
    const char*model=argc>1?argv[1]:"/root/model/best.rknn";
    int mode=argc>2?atoi(argv[2]):0;
    int sz=0; unsigned char*d=load(model,&sz);
    rknn_context c0; if(rknn_init(&c0,d,sz,0,NULL)<0){printf("init fail\n");return 1;} free(d);
    rknn_query(c0,RKNN_QUERY_IN_OUT_NUM,&io,sizeof(io));
    memset(&ia,0,sizeof(ia)); ia.index=0; rknn_query(c0,RKNN_QUERY_INPUT_ATTR,&ia,sizeof(ia));
    for(uint32_t i=0;i<io.n_output && i<8;i++){ memset(&oa[i],0,sizeof(oa[i])); oa[i].index=i; rknn_query(c0,RKNN_QUERY_OUTPUT_ATTR,&oa[i],sizeof(oa[i])); }
    is_fp16=(ia.type==RKNN_TENSOR_FLOAT16);
    rgb_size=ia.dims[1]*ia.dims[2]*ia.dims[3];
    printf("[bench3] %s input=%s\n",model,is_fp16?"FP16":"UINT8");

    if(mode==0){
        rknn_set_core_mask(c0,RKNN_NPU_CORE_0);
        Arg a={0}; a.ctx=c0; a.runs=10; bench_one(&a);
        printf("[bench3] single  run=%.1f conv=%.1f outs_get=%.1f postproc=%.1f  total=%.1f (ms)\n",
               a.run_ms,a.conv_ms,a.get_ms,a.pp_ms,a.run_ms+a.conv_ms+a.get_ms+a.pp_ms);
    } else {
        rknn_context c[3]; c[0]=c0;
        for(int i=1;i<3;i++) rknn_dup_context(&c0,&c[i]);
        rknn_core_mask m[3]={RKNN_NPU_CORE_0,RKNN_NPU_CORE_1,RKNN_NPU_CORE_2};
        pthread_t t[3]; Arg a[3];
        for(int i=0;i<3;i++){ rknn_set_core_mask(c[i],m[i]); memset(&a[i],0,sizeof(Arg)); a[i].ctx=c[i]; a[i].runs=10; }
        for(int i=0;i<3;i++) pthread_create(&t[i],NULL,th,&a[i]);
        for(int i=0;i<3;i++) pthread_join(t[i],NULL);
        for(int i=0;i<3;i++)
            printf("[bench3] ctx%d  run=%.1f conv=%.1f outs_get=%.1f postproc=%.1f  total=%.1f (ms)\n",
                   i,a[i].run_ms,a[i].conv_ms,a[i].get_ms,a[i].pp_ms,a[i].run_ms+a[i].conv_ms+a[i].get_ms+a[i].pp_ms);
    }
    return 0;
}
