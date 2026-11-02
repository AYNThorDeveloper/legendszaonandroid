/* Empty local friends service for offline play. No online identity or presence
 * is advertised. Protocol: https://switchbrew.org/wiki/Friend_services */
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
#include <pthread.h>
static pthread_once_t once=PTHREAD_ONCE_INIT;
static uint32_t event;
static void init_event(void){event=runtime_event_new();}
static void get_event(IpcResponse*r){pthread_once(&once,init_event);r->copy[r->ncopy++]=runtime_event_readable(event);}
static void friend_service(IpcSession*s,const IpcRequest*q,IpcResponse*r){
 (void)s;fprintf(stderr,"[android-friends] friend command %u\n",q->cmd);
 switch(q->cmd){
 case 0:get_event(r);break;
 case 1:break;
 case 10100:case 10101:case 10400:{int32_t count=0;IPC_OUT(r,count);break;}
 default:r->result=IPC_RESULT_UNKNOWN_COMMAND;break;
 }
}
static void notification(IpcSession*s,const IpcRequest*q,IpcResponse*r){
 (void)s;
 if(q->cmd==0)get_event(r);
 else if(q->cmd==1){pthread_once(&once,init_event);runtime_event_clear(event);}
 else r->result=IPC_RESULT_UNKNOWN_COMMAND;
}
static void daemon_session(IpcSession*s,const IpcRequest*q,IpcResponse*r){(void)s;(void)q;r->result=IPC_RESULT_UNKNOWN_COMMAND;}
static void creator(IpcSession*s,const IpcRequest*q,IpcResponse*r){
 const char *name;IpcHandler handler;
 fprintf(stderr,"[android-friends] creator command %u\n",q->cmd);
 switch(q->cmd){
 case 0:name="friend:offline";handler=friend_service;break;
 case 1:name="friend:notification";handler=notification;break;
 case 2:name="friend:daemon-session";handler=daemon_session;break;
 default:r->result=IPC_RESULT_UNKNOWN_COMMAND;return;
 }
 if(q->domain)r->domain_objects[r->ndomain_objects++]=runtime_ipc_domain_new_object(s,name,handler,NULL);
 else r->move[r->nmove++]=runtime_ipc_new_session(name,handler,NULL,NULL);
}
void android_offline_friends_register(void){runtime_ipc_register("friend:u",creator,NULL,NULL);}
