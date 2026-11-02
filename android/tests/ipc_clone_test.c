#include <assert.h>
#include "../../games/totk/runtime/ipc.c"
pthread_key_t runtime_keys[8];
static void *handles[64];static unsigned count;
uint32_t runtime_handle_new(int type,void *p){(void)type;handles[++count]=p;return count;}
void *runtime_handle_get(uint32_t h,int type){(void)type;return handles[h];}
_Noreturn void runtime_fail(const char*s){fprintf(stderr,"FAIL %s\n",s);abort();}
void runtime_trace_guest(uint64_t fp,uint64_t lr){(void)fp;(void)lr;}
static unsigned closed;
static void close_state(void *p){closed++;free(p);}
static void service(IpcSession*s,const IpcRequest*q,IpcResponse*r){
 assert(q->domain&&q->cmd==17);unsigned *value=NULL;
 for(unsigned i=0;i<s->ndomain_objects;i++)if(s->domain_objects[i].id==q->object_id)value=s->domain_objects[i].state;
 assert(value);(*value)++;IPC_OUT(r,*value);
}
static void request(IpcSession*s,uint32_t id,unsigned expected){
 uint32_t m[64]={0};m[0]=4;m[1]=10;m[4]=0x00100001;m[5]=id;m[8]=CMIF_IN_MAGIC;m[10]=17;
 dispatch(s,m);assert(m[12]==expected);
}
int main(void){
 unsigned *state=calloc(1,sizeof *state);uint32_t h=runtime_ipc_new_session("clone-test",service,state,close_state);IpcSession*s=handles[h];
 IpcRequest q={.cmd=0};IpcResponse r={0};control(s,&q,&r);uint32_t root;memcpy(&root,r.data,4);
 q.cmd=2;r=(IpcResponse){0};control(s,&q,&r);IpcSession*a=handles[r.move[0]];
 q.cmd=4;r=(IpcResponse){0};control(a,&q,&r);IpcSession*b=handles[r.move[0]];
 request(a,root,1);request(b,root,2);
 unsigned child_state=20;uint32_t child=runtime_ipc_domain_new_object(s,"late-child",service,&child_state);
 request(a,child,21);request(b,child,22);
 runtime_ipc_session_closed(s);assert(closed==0);request(a,root,3);
 runtime_ipc_session_closed(a);assert(closed==0);request(b,root,4);
 runtime_ipc_session_closed(b);assert(closed==1);
 puts("PASS: domain requests survive clones, later objects are shared, original may close first, state closes exactly once.");
}
