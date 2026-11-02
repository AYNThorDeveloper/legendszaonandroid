/* Offline initialization of Unreal's SSL library, without a TLS transport.
 * IPC layouts: https://switchbrew.org/wiki/SSL_services
 * Context configuration is local; connection creation returns an error. */
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { uint32_t option; uint64_t pki_next; } OfflineSslContext;
static void ssl_context(IpcSession *,const IpcRequest *,IpcResponse *);
static void ssl_service(IpcSession *s,const IpcRequest *q,IpcResponse *r) {
    fprintf(stderr,"[android-ssl] service command %u\n",q->cmd);
    switch(q->cmd) {
    case 5: (void)IPC_ARG(q,uint32_t,0);break; /* SetInterfaceVersion */
    case 0: {
        (void)IPC_ARG(q,uint32_t,0);
        OfflineSslContext *context=calloc(1,sizeof *context);
        if(!context)runtime_fail("offline SSL context allocation");
        if(q->domain)r->domain_objects[r->ndomain_objects++]=runtime_ipc_domain_new_object(s,"ssl:offline-context",ssl_context,context);
        else r->move[r->nmove++]=runtime_ipc_new_session("ssl:offline-context",ssl_context,context,free);
        break;
    }
    default: ipc_unimplemented(s,q);
    }
}
static void ssl_context(IpcSession *s,const IpcRequest *q,IpcResponse *r) {
    OfflineSslContext *context=s->state;
    if(q->domain)for(unsigned i=0;i<s->ndomain_objects;i++)if(s->domain_objects[i].id==q->object_id)context=s->domain_objects[i].state;
    if(!context)runtime_fail("offline SSL context state missing");
    fprintf(stderr,"[android-ssl] context command %u\n",q->cmd);
    switch(q->cmd) {
    case 0: (void)IPC_ARG(q,uint32_t,0);context->option=IPC_ARG(q,uint32_t,4);break;
    case 1: IPC_OUT(r,context->option);break;
    case 2: r->result=IPC_RESULT_UNKNOWN_COMMAND;break; /* no connection transport */
    case 3: { uint32_t connections=0;IPC_OUT(r,connections);break; }
    case 4: { uint64_t id=++context->pki_next;IPC_OUT(r,id);break; } /* local opaque PKI id; never used to verify a connection */
    case 6: break;
    default: ipc_unimplemented(s,q);
    }
}
void android_offline_ssl_register(void) { runtime_ipc_register("ssl",ssl_service,NULL,NULL); }
