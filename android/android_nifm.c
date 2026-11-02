/* Minimal Horizon network-interface service for an offline Android game host.
 * Protocol reference: switchbrew/libnx nx/source/services/nifm.c and nifm.h.
 * This host has no Switch network interface; initialization succeeds, network
 * availability remains false. Unimplemented operations fail explicitly.
 */
#include "runtime.h"
#include "ipc.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct { uint32_t state_event, result_event; } NifmRequest;
static void nifm_general(IpcSession *,const IpcRequest *,IpcResponse *);
static void nifm_request(IpcSession *,const IpcRequest *,IpcResponse *);

static void add_object(IpcSession *s,const IpcRequest *q,IpcResponse *r,
                       const char *name,IpcHandler handler,void *state) {
    if(q->domain) {
        uint32_t id=runtime_ipc_domain_new_object(s,name,handler,state);
        r->domain_objects[r->ndomain_objects++]=id;
    } else {
        r->move[r->nmove++]=runtime_ipc_new_session(name,handler,state,free);
    }
}
static void nifm_service(IpcSession *s,const IpcRequest *q,IpcResponse *r) {
    fprintf(stderr,"[android-nifm] static command %u\n",q->cmd);
    if(q->cmd==4 || q->cmd==5) add_object(s,q,r,"nifm:general",nifm_general,NULL);
    else ipc_unimplemented(s,q);
}
static void nifm_general(IpcSession *s,const IpcRequest *q,IpcResponse *r) {
    fprintf(stderr,"[android-nifm] general command %u\n",q->cmd);
    switch(q->cmd) {
    case 1: { /* GetClientId: fixed-size output pointer, u32. */
        uint32_t id=1;
        if(q->nc && q->c[0].size>=sizeof id) memcpy(ipc_guest(q->c[0].addr,sizeof id),&id,sizeof id);
        else ipc_unimplemented(s,q);
        break;
    }
    case 4: {
        NifmRequest *state=calloc(1,sizeof *state);
        if(!state) runtime_fail("nifm request allocation");
        state->state_event=runtime_event_new();state->result_event=runtime_event_new();
        add_object(s,q,r,"nifm:request",nifm_request,state);
        break;
    }
    case 17: case 20: case 21: case 22: {
        uint8_t available=0;IPC_OUT(r,available);break;
    }
    case 18: { uint8_t status[3]={0,0,0};ipc_out(r,status,sizeof status);break; }
    default: ipc_unimplemented(s,q);
    }
}
static void nifm_request(IpcSession *s,const IpcRequest *q,IpcResponse *r) {
    NifmRequest *state=s->state;
    if(q->domain) {
        state=NULL;
        for(unsigned i=0;i<s->ndomain_objects;i++)
            if(s->domain_objects[i].id==q->object_id) state=s->domain_objects[i].state;
    }
    if(!state) runtime_fail("nifm request object state missing");
    fprintf(stderr,"[android-nifm] request command %u\n",q->cmd);
    switch(q->cmd) {
    case 0: { uint32_t status=1;IPC_OUT(r,status);break; } /* not accepted/available */
    case 1: r->result=(311u<<9)|110u;break; /* libnx's initial unavailable request result */
    case 2:
        r->copy[r->ncopy++]=runtime_event_readable(state->state_event);
        r->copy[r->ncopy++]=runtime_event_readable(state->result_event);break;
    case 3: case 4:
        runtime_event_signal(state->state_event);runtime_event_signal(state->result_event);break;
    default: ipc_unimplemented(s,q);
    }
}
/* BSD initialization is unconditional in Unreal, even for offline play.
 * Register/monitor use the actual service protocol. Socket creation reports
 * Linux/Horizon ENETDOWN (100); no Android network socket is opened.
 * Reference: libnx bsd.c; switchbrew.org/wiki/Sockets_services. */
static void offline_bsd(IpcSession *s,const IpcRequest *q,IpcResponse *r) {
    fprintf(stderr,"[android-bsd] command %u data=%u handles=%u\n",q->cmd,q->data_size,q->ncopy);
    switch(q->cmd) {
    case 0: { uint64_t client=1;IPC_OUT(r,client);break; }
    case 1: break;
    case 2: case 3: { int32_t result[2]={-1,100};ipc_out(r,result,sizeof result);break; }
    default: ipc_unimplemented(s,q);
    }
}
/* nn::nifm at function level, as a console without a usable network: requests fail with "network not available"
   and the error is returned without the connection applet (whose library applet this host does not have). Unreal's
   menu submits a request and hands the failure to HandleNetworkRequestErrorResult (Double Exposure, 26 Oct 2026). */
#define NIFM_NO_NETWORK ((311u<<9)|110u)   /* the same result as IRequest::GetResult above */
static uint32_t nifm_fn_initialize(void) { return 0; }
static bool nifm_fn_available(void) { return false; }
static void nifm_fn_submit_and_wait(void) { fprintf(stderr,"[android-nifm] network request submitted: offline, not accepted\n"); }
static void nifm_fn_cancel(void) {}
static bool nifm_fn_handle_error(void) { fprintf(stderr,"[android-nifm] network request error not handled offline\n"); return false; }
static uint32_t nifm_fn_ip(uint32_t *out) { if(out)*out=0; return NIFM_NO_NETWORK; }
static uint32_t nifm_fn_proxy(void *out) { (void)out; return NIFM_NO_NETWORK; }
/* Every other online feature the game imports is switched off here, so nothing reaches a Switch online service:
   local wireless (nn::ldn), the eShop (nn::ec), the web applet (nn::web) and host name resolution (nn::nsd) report
   "not available" at once. Argument setters and constructors stay in the SDK (they only fill the caller's struct). */
static uint32_t offline_fail(void) { static int said; if (!said++) fprintf(stderr,"[android-nifm] online feature requested: not available offline\n"); return NIFM_NO_NETWORK; }
static int ldn_state(void) { return 0; }   /* nn::ldn::State_None: local wireless not initialized */
static void offline_void(void) {}
static uint32_t ldn_scan(void *info, int *count, int max, const void *filter, int channel) { (void)info; (void)max; (void)filter; (void)channel; if (count) *count = 0; return offline_fail(); }
static uint32_t shop_response_size(void *self, uint64_t *size) { (void)self; if (size) *size = 0; return offline_fail(); }
static uint32_t ldn_scan_private(void *info, int *count, int max, const void *filter, int channel) { return ldn_scan(info, count, max, filter, channel); }
static void ldn_state_event(void *event) { if (!event) runtime_fail("ldn state change event"); hle_bind_system_event(event, 1); }
static int ldn_disconnect_reason(void) { return 0; }   /* nn::ldn::DisconnectReason_None */
static void nifm_local_mode(unsigned char on) { (void)on; }
static uint32_t nifm_ip_config(void *a, void *b, void *c, void *d, void *e) { (void)a; (void)b; (void)c; (void)d; (void)e; return NIFM_NO_NETWORK; }
static uint32_t nsd_environment(char *out) { if (!out) runtime_fail("nsd environment output"); memset(out, 0, 8); memcpy(out, "lp1", 3); return 0; }   /* retail */
/* nn::bcat (data delivery) as on a console that never went online: the library starts, the delivery cache storage
   mounts empty (no directories, no files), and a sync request ends at once without internet access. Constructors and
   destructors stay in the SDK; nothing here opens a handle for them to close. Legends Z-A starts bcat at boot. */
#define BCAT_NOT_FOUND ((2u<<9)|122u)
#define BCAT_NOT_OPEN ((7u<<9)|122u)
#define BCAT_INTERNET_DENIED ((8u<<9)|122u)
static void bcat_initialize(void) { fprintf(stderr,"[android-nifm] bcat: delivery cache empty, no sync (offline)\n"); }
static uint32_t bcat_mount(void) { return 0; }
static void bcat_unmount(void) {}
static uint32_t bcat_enumerate(int *count, void *names, int max) { (void)names; (void)max; if (!count) runtime_fail("bcat directory count"); *count = 0; return 0; }
static uint32_t bcat_not_found(void *self, const void *name) { (void)self; (void)name; return BCAT_NOT_FOUND; }
static uint32_t bcat_dir_read(void *self, int *count, void *entries, int max) { (void)self; (void)entries; (void)max; if (count) *count = 0; return BCAT_NOT_OPEN; }
static uint32_t bcat_file_read(void *self, uint64_t *done, int64_t offset, void *buf, uint64_t size) { (void)self; (void)offset; (void)buf; (void)size; if (done) *done = 0; return BCAT_NOT_OPEN; }
static int64_t bcat_file_size(void *self) { (void)self; return 0; }
static void bcat_close(void *self) { (void)self; }
static uint32_t bcat_sync(void *progress) { (void)progress; return BCAT_INTERNET_DENIED; }
static void bcat_progress_update(void *self) { (void)self; }
static int bcat_progress_status(const void *self) { (void)self; return 9; }   /* DeliveryCacheProgressStatus_Done */
static uint32_t bcat_progress_result(const void *self) { (void)self; return BCAT_INTERNET_DENIED; }
void *runtime_network_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn3ldn10InitializeEv", offline_fail}, {"_ZN2nn3ldn8FinalizeEv", offline_void}, {"_ZN2nn3ldn8GetStateEv", ldn_state},
        {"_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", offline_fail},
        {"_ZN2nn3ldn15OpenAccessPointEv", offline_fail}, {"_ZN2nn3ldn16CloseAccessPointEv", offline_fail},
        {"_ZN2nn3ldn11OpenStationEv", offline_fail}, {"_ZN2nn3ldn12CloseStationEv", offline_fail},
        {"_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", offline_fail},
        {"_ZN2nn3ldn14DestroyNetworkEv", offline_fail}, {"_ZN2nn3ldn16SetAdvertiseDataEPKvm", offline_fail},
        {"_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_scan},
        {"_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", offline_fail},
        {"_ZN2nn2ec33InitializeForShopServiceAccessorsEPvm", offline_fail}, {"_ZN2nn2ec31FinalizeForShopServiceAccessorsEv", offline_void},
        {"_ZN2nn2ec19ShopServiceAccessor10InitializeERKNS0_11ShopServiceE", offline_fail},
        {"_ZN2nn2ec19ShopServiceAccessor7RequestEPNS1_13AsyncResponseERKNS_7account3UidENS0_11ShopService6MethodEPKcRKNS1_8PostDataENS_8TimeSpanE", offline_fail},
        {"_ZN2nn2ec19ShopServiceAccessor13AsyncResponse6CancelEv", offline_void},
        {"_ZN2nn2ec19ShopServiceAccessor13AsyncResponse7TryWaitEv", offline_fail},
        {"_ZN2nn2ec19ShopServiceAccessor13AsyncResponse7GetSizeEPm", shop_response_size},
        {"_ZN2nn2ec19ShopServiceAccessor13AsyncResponse3GetEPvm", offline_fail},
        {"_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE", offline_fail},
        {"_ZN2nn2ec24ShowShopSubscriptionListENS_13ApplicationIdERKNS_7account10UserHandleE", offline_fail},
        {"_ZN2nn2ec26ShowShopConsumableItemListENS_13ApplicationIdERKNS_7account10UserHandleE", offline_fail},
        {"_ZN2nn2ec30ShowShopApplicationInformationENS_13ApplicationIdERKNS_7account10UserHandleE", offline_fail},
        {"_ZN2nn2ec28ShowShopConsumableItemDetailENS_13ApplicationIdERKNS0_12ConsumableIdENS0_5NsUidERKNS_7account10UserHandleE", offline_fail},
        {"_ZN2nn2ec27ShowShopSubscriptionDetailsERKNS0_8CourseIdENS0_5NsUidERKNS_7account10UserHandleE", offline_fail},
        {"_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE", offline_fail},
        {"_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", offline_fail},
        {"_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", offline_fail},
        {"_ZN2nn4nifm10InitializeEv", nifm_fn_initialize}, {"_ZN2nn4nifm18IsNetworkAvailableEv", nifm_fn_available},
        {"_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", nifm_fn_submit_and_wait}, {"_ZN2nn4nifm20CancelNetworkRequestEv", nifm_fn_cancel},
        {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", nifm_fn_handle_error},
        {"_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", nifm_fn_ip},
        {"_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_fn_proxy},
        {"_ZN2nn4nifm19SetLocalNetworkModeEb", nifm_local_mode},
        {"_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", nifm_ip_config},
        {"_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", nsd_environment},
        {"_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ldn_state_event},
        {"_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE", offline_fail},
        {"_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", offline_fail},
        {"_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", offline_fail},
        {"_ZN2nn3ldn10DisconnectEv", offline_fail},
        {"_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_scan_private},
        {"_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", offline_fail},
        {"_ZN2nn3ldn20AddAcceptFilterEntryENS0_10MacAddressE", offline_fail},
        {"_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE", offline_fail},
        {"_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", offline_fail},
        {"_ZN2nn3ldn19GetDisconnectReasonEv", ldn_disconnect_reason},
        {"_ZN2nn4bcat10InitializeEv", bcat_initialize},
        {"_ZN2nn4bcat25MountDeliveryCacheStorageEv", bcat_mount},
        {"_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", bcat_unmount},
        {"_ZN2nn4bcat31EnumerateDeliveryCacheDirectoryEPiPNS0_13DirectoryNameEi", bcat_enumerate},
        {"_ZN2nn4bcat22DeliveryCacheDirectory4OpenEPKc", bcat_not_found},
        {"_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE", bcat_not_found},
        {"_ZN2nn4bcat22DeliveryCacheDirectory4ReadEPiPNS0_27DeliveryCacheDirectoryEntryEi", bcat_dir_read},
        {"_ZN2nn4bcat22DeliveryCacheDirectory5CloseEv", bcat_close},
        {"_ZN2nn4bcat17DeliveryCacheFile4OpenEPKc", bcat_not_found},
        {"_ZN2nn4bcat17DeliveryCacheFile4ReadEPmlPvm", bcat_file_read},
        {"_ZN2nn4bcat17DeliveryCacheFile7GetSizeEv", bcat_file_size},
        {"_ZN2nn4bcat17DeliveryCacheFile5CloseEv", bcat_close},
        {"_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE", bcat_sync},
        {"_ZN2nn4bcat21DeliveryCacheProgress6UpdateEv", bcat_progress_update},
        {"_ZNK2nn4bcat21DeliveryCacheProgress9GetStatusEv", bcat_progress_status},
        {"_ZNK2nn4bcat21DeliveryCacheProgress9GetResultEv", bcat_progress_result}};
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
void android_nifm_register(void) {
    runtime_ipc_register("nifm:u",nifm_service,NULL,NULL);
    runtime_ipc_register("bsd:s",offline_bsd,NULL,NULL);
    runtime_ipc_register("bsd:u",offline_bsd,NULL,NULL);
}
