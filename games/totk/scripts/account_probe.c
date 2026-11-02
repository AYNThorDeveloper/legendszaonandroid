#include "../runtime/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
_Noreturn void runtime_fail(const char *s){fprintf(stderr,"%s\n",s);exit(1);}
typedef struct{uint64_t lo,hi;} Uid;
typedef struct{Uid uid;uint64_t token;} Handle;
int main(void){
 void(*init)(void)=runtime_account_override("_ZN2nn7account10InitializeEv");
 unsigned(*open)(Handle *)=runtime_account_override("_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE");
 uint32_t(*get)(Uid *,const Handle *)=runtime_account_override("_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE");
 void(*close)(const Handle *)=runtime_account_override("_ZN2nn7account9CloseUserERKNS0_10UserHandleE");
 uint32_t(*exists)(unsigned char *,const Uid *)=runtime_account_override("_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE");
 uint32_t(*nick)(char *,const Uid *)=runtime_account_override("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE");
 uint32_t(*network)(bool *,const Handle *)=runtime_account_override("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE");
 init();Handle h;Uid uid;if(!open(&h)||get(&uid,&h))runtime_fail("user open/get");
 unsigned char value=0;if(exists(&value,&uid)||value!=1)runtime_fail("user existence");
 Uid absent={0};if(exists(&value,&absent)||value!=0)runtime_fail("absent user existence");
 struct{char data[33];unsigned char canary[16];} name;memset(&name,0xa5,sizeof(name));if(nick(name.data,&uid)||strcmp(name.data,"Local"))runtime_fail("nickname");
 for(unsigned i=0;i<16;i++)if(name.canary[i]!=0xa5)runtime_fail("nickname overflow");
 bool online=true;if(!network||network(&online,&h)||online)runtime_fail("local profile network availability");
 close(&h);if(!get(&absent,&h))runtime_fail("closed handle accepted");
 if(!network(&online,&h))runtime_fail("closed network handle accepted");
 printf("{\"status\":\"pass\",\"uid\":\"%016llx%016llx\"}\n",(unsigned long long)uid.hi,(unsigned long long)uid.lo);
}
