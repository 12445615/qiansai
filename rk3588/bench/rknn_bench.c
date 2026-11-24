#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/time.h>
#include "rknn_api.h"

static int64_t now_us(void){ struct timeval tv; gettimeofday(&tv,NULL); return (int64_t)tv.tv_sec*1000000LL+tv.tv_usec; }

static unsigned char *load(const char *p, int *sz){
    FILE *f=fopen(p,"rb"); if(!f){printf("open %s fail\n",p);exit(1);}
    fseek(f,0,SEEK_END); *sz=ftell(f); fseek(f,0,SEEK_SET);
    unsigned char *d=malloc(*sz); fread(d,1,*sz,f); fclose(f); return d;
}

typedef struct { rknn_context ctx; rknn_input in; int runs; double avg_ms; int mask_name; } ParArg;

static void *par_thread(void *a){
    ParArg *p=(ParArg*)a;
    int64_t t0=now_us();
    for(int i=0;i<p->runs;i++){
        if(rknn_inputs_set(p->ctx,1,&p->in)<0){printf("inputs_set fail\n");break;}
        if(rknn_run(p->ctx,NULL)<0){printf("run fail\n");break;}
    }
    p->avg_ms=(double)(now_us()-t0)/1000.0/p->runs;
    return NULL;
}

int main(int argc,char**argv){
    const char *model = argc>1?argv[1]:"/root/model/best.rknn";
    int mode = argc>2?atoi(argv[2]):0;   /* 0=single AUTO 1=CORE_0 2=CORE_0_1_2 3=parallel3 */
    int sz=0; unsigned char *data=load(model,&sz);

    rknn_context ctx;
    int64_t t_init=now_us();
    if(rknn_init(&ctx,data,sz,0,NULL)<0){printf("rknn_init fail\n");return 1;}
    printf("[bench] init %.1f ms  model=%s\n",(now_us()-t_init)/1000.0,model);

    rknn_input_output_num io; rknn_query(ctx,RKNN_QUERY_IN_OUT_NUM,&io,sizeof(io));
    rknn_tensor_attr ia; memset(&ia,0,sizeof(ia)); ia.index=0;
    rknn_query(ctx,RKNN_QUERY_INPUT_ATTR,&ia,sizeof(ia));
    uint32_t rgb = ia.dims[1]*ia.dims[2]*ia.dims[3];
    printf("[bench] input type=%d n_dims=%u dims=[%d,%d,%d,%d] size=%u rgb=%u\n",
           ia.type,ia.n_dims,ia.dims[0],ia.dims[1],ia.dims[2],ia.dims[3],ia.size,rgb);

    void *buf = malloc(ia.size); memset(buf,0x40,ia.size);
    rknn_input in; memset(&in,0,sizeof(in));
    in.index=0; in.fmt=RKNN_TENSOR_NHWC; in.buf=buf;
    if(ia.type==RKNN_TENSOR_FLOAT16){ in.type=RKNN_TENSOR_FLOAT16; in.size=ia.size; in.pass_through=1; }
    else { in.type=RKNN_TENSOR_UINT8; in.size=rgb; in.pass_through=0; }

    const char *names[]={"AUTO","CORE_0","CORE_0_1_2","PARALLEL3"};
    if(mode<3){
        rknn_core_mask cm = RKNN_NPU_CORE_AUTO;
        if(mode==1) cm=RKNN_NPU_CORE_0;
        if(mode==2) cm=RKNN_NPU_CORE_0_1_2;
        if(rknn_set_core_mask(ctx,cm)<0) printf("[bench] set_core_mask failed\n");
        /* warmup */
        for(int i=0;i<3;i++){ rknn_inputs_set(ctx,1,&in); rknn_run(ctx,NULL); }
        int R=20; int64_t t0=now_us();
        for(int i=0;i<R;i++){ rknn_inputs_set(ctx,1,&in); rknn_run(ctx,NULL); }
        double per=(now_us()-t0)/1000.0/R;
        printf("[bench] single ctx mask=%s -> %.2f ms/frame  (%.1f fps)\n",names[mode],per,1000.0/per);
    } else {
        /* 3 contexts, dup from first, pinned to core0/1/2 */
        rknn_context c[3]; c[0]=ctx;
        for(int i=1;i<3;i++){ if(rknn_dup_context(&ctx,&c[i])<0){printf("dup fail\n");return 1;} }
        rknn_core_mask masks[3]={RKNN_NPU_CORE_0,RKNN_NPU_CORE_1,RKNN_NPU_CORE_2};
        pthread_t th[3]; ParArg args[3];
        for(int i=0;i<3;i++){
            rknn_set_core_mask(c[i],masks[i]);
            memset(&args[i],0,sizeof(ParArg));
            args[i].ctx=c[i]; args[i].in=in; args[i].runs=20;
            /* 每个 ctx 独立输入缓冲 */
            args[i].in.buf = malloc(ia.size); memset(args[i].in.buf,0x40,ia.size);
        }
        int64_t t0=now_us();
        for(int i=0;i<3;i++) pthread_create(&th[i],NULL,par_thread,&args[i]);
        for(int i=0;i<3;i++) pthread_join(th[i],NULL);
        double wall=(now_us()-t0)/1000.0;
        printf("[bench] parallel3 (core0/1/2) wall=%.1f ms for 60 frames -> %.1f fps total, per-core: %.2f / %.2f / %.2f ms\n",
               wall, 60000.0/wall, args[0].avg_ms,args[1].avg_ms,args[2].avg_ms);
    }
    free(buf); free(data);
    return 0;
}
