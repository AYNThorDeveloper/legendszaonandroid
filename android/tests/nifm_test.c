#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../android_nifm.c"
static IpcHandler registered;
static IpcSession sessions[16]; static unsigned used;
static unsigned signals[128]; static uint32_t events=16;
_Noreturn void runtime_fail(const char *s){fprintf(stderr,"%s\n",s);abort();}
_Noreturn void ipc_unimplemented(const IpcSession *s,const IpcRequest *q){(void)s;(void)q;abort();}
void runtime_ipc_register(const char *n,IpcHandler h,IpcOpen o,void(*c)(void*)){(void)o;(void)c;if(!strcmp(n,"nifm:u")) registered=h;}
uint32_t runtime_ipc_new_session(const char*n,IpcHandler h,void*s,void(*c)(void*)){(void)c;sessions[++used]=(IpcSession){.name=n,.handler=h,.state=s};return used;}
uint32_t runtime_ipc_domain_new_object(IpcSession*s,const char*n,IpcHandler h,void*st){uint32_t id=++s->next_domain_id;s->domain_objects[s->ndomain_objects++]=(IpcDomainObject){id,n,h,st};return id;}
void ipc_out(IpcResponse*r,const void*p,uint32_t n){assert(r->data_size+n<=sizeof r->data);memcpy(r->data+r->data_size,p,n);r->data_size+=n;}
void *ipc_guest(uint64_t p,uint64_t n){(void)n;assert(p);return(void*)(uintptr_t)p;}
uint32_t runtime_event_new(void){return events++;}
uint32_t runtime_event_readable(uint32_t w){return w+64;}
void runtime_event_signal(uint32_t w){signals[w]++;}
int main(void){
 android_nifm_register();assert(registered);
 for(int domain=0;domain<2;domain++){
  IpcSession s={.name="nifm:u",.handler=registered,.domain=domain};IpcRequest q={.cmd=5,.domain=domain};IpcResponse r={0};
  registered(&s,&q,&r);assert(domain?r.ndomain_objects==1:r.nmove==1);
  IpcHandler general=domain?s.domain_objects[0].handler:sessions[r.move[0]].handler;
  IpcSession *g=domain?&s:&sessions[r.move[0]];
  uint32_t client=0; q=(IpcRequest){.cmd=1,.domain=domain,.nc=1,.c={{(uint64_t)(uintptr_t)&client,4,0}}};r=(IpcResponse){0};general(g,&q,&r);assert(client==1);
  q=(IpcRequest){.cmd=4,.domain=domain};r=(IpcResponse){0};general(g,&q,&r);
  IpcSession *request=domain?&s:&sessions[r.move[0]];uint32_t object=domain?r.domain_objects[0]:0;
  IpcHandler handler=domain?s.domain_objects[1].handler:request->handler;
  q=(IpcRequest){.cmd=2,.domain=domain,.object_id=object};r=(IpcResponse){0};handler(request,&q,&r);assert(r.ncopy==2&&r.copy[0]!=r.copy[1]);
  uint32_t first=r.copy[0]-64,second=r.copy[1]-64;
  q.cmd=4;r=(IpcResponse){0};handler(request,&q,&r);assert(signals[first]&&signals[second]);
  q.cmd=0;r=(IpcResponse){0};handler(request,&q,&r);uint32_t state;memcpy(&state,r.data,4);assert(state==1);
  q.cmd=1;r=(IpcResponse){0};handler(request,&q,&r);assert(r.result!=0);
  q=(IpcRequest){.cmd=21,.domain=domain};r=(IpcResponse){0};general(g,&q,&r);assert(r.data_size==1&&r.data[0]==0);
  free(domain?s.domain_objects[1].state:request->state);
 }
 IpcSession b={.name="bsd:s",.handler=offline_bsd};IpcRequest q={.cmd=0};IpcResponse r={0};
 offline_bsd(&b,&q,&r);assert(r.result==0&&r.data_size==8);
 q.cmd=1;r=(IpcResponse){0};offline_bsd(&b,&q,&r);assert(r.result==0);
 q.cmd=2;r=(IpcResponse){0};offline_bsd(&b,&q,&r);int32_t result[2];memcpy(result,r.data,8);assert(result[0]==-1&&result[1]==100);
 puts("PASS: domain and session handshakes, client output, distinct event handles, completion signaling and offline availability.");
}
