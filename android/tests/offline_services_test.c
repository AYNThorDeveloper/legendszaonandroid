#include <assert.h>
#include <string.h>
#include "../android_offline_ssl.c"
#include "../android_offline_friends.c"
static IpcSession sessions[16];static unsigned ns;
_Noreturn void runtime_fail(const char*s){fprintf(stderr,"%s\n",s);abort();}
_Noreturn void ipc_unimplemented(const IpcSession*s,const IpcRequest*q){(void)s;(void)q;abort();}
void runtime_ipc_register(const char*n,IpcHandler h,IpcOpen o,void(*c)(void*)){(void)n;(void)h;(void)o;(void)c;}
uint32_t runtime_ipc_new_session(const char*n,IpcHandler h,void*p,void(*c)(void*)){(void)c;sessions[++ns]=(IpcSession){.name=n,.handler=h,.state=p};return ns;}
uint32_t runtime_ipc_domain_new_object(IpcSession*s,const char*n,IpcHandler h,void*p){uint32_t id=++s->next_domain_id;s->domain_objects[s->ndomain_objects++]=(IpcDomainObject){id,n,h,p};return id;}
void ipc_out(IpcResponse*r,const void*p,uint32_t n){memcpy(r->data+r->data_size,p,n);r->data_size+=n;}
const void *ipc_arg(const IpcRequest*q,uint32_t off,uint32_t n){assert(off+n<=q->data_size);return q->data+off;}
uint32_t runtime_event_new(void){return 40;}
uint32_t runtime_event_readable(uint32_t h){return h+64;}
void runtime_event_clear(uint32_t h){assert(h==40);}
int main(void){
 for(int domain=0;domain<2;domain++){
  uint32_t version=3;IpcSession root={.name="ssl"};IpcRequest q={.cmd=5,.data=(uint8_t*)&version,.data_size=4,.domain=domain};IpcResponse r={0};
  ssl_service(&root,&q,&r);assert(r.result==0);
  q.cmd=0;r=(IpcResponse){0};ssl_service(&root,&q,&r);
  uint32_t id=domain?r.domain_objects[0]:0;IpcSession *ctx=domain?&root:&sessions[r.move[0]];
  uint32_t option[2]={1,1};q=(IpcRequest){.cmd=0,.domain=domain,.object_id=id,.data=(uint8_t*)option,.data_size=8};r=(IpcResponse){0};ssl_context(ctx,&q,&r);
  q.cmd=1;r=(IpcResponse){0};ssl_context(ctx,&q,&r);assert(r.data_size==4&&r.data[0]==1);
  q.cmd=2;r=(IpcResponse){0};ssl_context(ctx,&q,&r);assert(r.result!=0&&!r.nmove&&!r.ndomain_objects);
  q.cmd=3;r=(IpcResponse){0};ssl_context(ctx,&q,&r);assert(r.data_size==4&&r.data[0]==0);
  free(domain?root.domain_objects[0].state:ctx->state);
  root=(IpcSession){.name="friend:u"};q=(IpcRequest){.cmd=2,.domain=domain};r=(IpcResponse){0};creator(&root,&q,&r);assert(domain?r.ndomain_objects==1:r.nmove==1);
  q.cmd=0;r=(IpcResponse){0};creator(&root,&q,&r);
  IpcSession *friends=domain?&root:&sessions[r.move[0]];
  q=(IpcRequest){.cmd=10101,.domain=domain};r=(IpcResponse){0};friend_service(friends,&q,&r);assert(r.result==0&&r.data_size==4&&r.data[0]==0);
  q.cmd=99999;r=(IpcResponse){0};friend_service(friends,&q,&r);assert(r.result!=0);
  q.cmd=0;r=(IpcResponse){0};notification(friends,&q,&r);assert(r.ncopy==1&&r.copy[0]==104);
 }
 puts("PASS: SSL initializes/configures without transport; friends are empty; unknown operations return errors; both session/domain shapes covered.");
}
