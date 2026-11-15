/* com.apple.BlueTool for the WCN399x (launchd: /usr/sbin/BlueTool -R, user mobile).
 * 20H392 BlueTool -R listens on the com.apple.BlueTool mach service; requests
 * are {kMsgId, kMsgArgs}. bluetoothd sends RunBuiltinScript {script, btaddress}
 * and reads result (0 success; 10 and 13 are its special failures),
 * manufacturer and firmware from the reply. The original scripts power and
 * patch Broadcom controllers over the UART; here the kernel performs the
 * WCN399x bring-up (hci_qca/btqca sequence), so "boot" and "init" report that
 * real outcome through kern.qbt_power_on, which blocks until it is known.
 * "deepsleep" observes the completed kernel power-down without restarting it.
 * Commands and
 * external scripts are Broadcom tools and are reported as unsupported. */
#include <errno.h>
#include <dispatch/dispatch.h>
#include <os/log.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

typedef void *xpc_object_t;
typedef const struct _xpc_type_s *xpc_type_t;
extern const struct _xpc_type_s _xpc_type_connection,_xpc_type_dictionary;
#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1ULL<<0)
xpc_object_t xpc_connection_create_mach_service(const char *,dispatch_queue_t,uint64_t);
void xpc_connection_set_event_handler(xpc_object_t,void (^)(xpc_object_t));
void xpc_connection_resume(xpc_object_t);
void xpc_connection_send_message(xpc_object_t,xpc_object_t);
xpc_type_t xpc_get_type(xpc_object_t);
xpc_object_t xpc_dictionary_get_value(xpc_object_t,const char *);
const char *xpc_dictionary_get_string(xpc_object_t,const char *);
xpc_object_t xpc_dictionary_create_reply(xpc_object_t);
xpc_object_t xpc_dictionary_get_remote_connection(xpc_object_t);
void xpc_dictionary_set_int64(xpc_object_t,const char *,int64_t);
void xpc_dictionary_set_string(xpc_object_t,const char *,const char *);
void xpc_release(xpc_object_t);
void xpc_transaction_begin(void);

/* Bluetooth SIG company identifier of Qualcomm (HCI manufacturer name). */
#define MANUFACTURER_QUALCOMM 29

static os_log_t logger;

static void run_builtin_script(xpc_object_t message,xpc_object_t reply)
{
    xpc_object_t args=xpc_dictionary_get_value(message,"kMsgArgs");
    const char *script=args&&xpc_get_type(args)==&_xpc_type_dictionary?xpc_dictionary_get_string(args,"script"):NULL;
    if(!script) {
        os_log_error(logger,"RunBuiltinScript without a script string");
        xpc_dictionary_set_int64(reply,"result",1);return;
    }
    if(strcmp(script,"boot") && strcmp(script,"init") && strcmp(script,"deepsleep")) {
        os_log_error(logger,"builtin script %{public}s is Broadcom-only; not supported on WCN399x",script);
        xpc_dictionary_set_int64(reply,"result",1);return;
    }
    int boot=-1;size_t size=sizeof(boot);
    bool sleeping=!strcmp(script,"deepsleep");
    if(sysctlbyname(sleeping?"kern.qbt_boot":"kern.qbt_power_on",&boot,&size,NULL,0)) {
        os_log_error(logger,"kern.qbt_power_on unavailable");
        xpc_dictionary_set_int64(reply,"result",1);return;
    }
    char firmware[64]={0};size=sizeof(firmware)-1;
    if(sysctlbyname("kern.qbt_firmware",firmware,&size,NULL,0)) firmware[0]=0;
    os_log(logger,"builtin script %{public}s: kernel bring-up error %d, firmware %{public}s",script,boot,firmware);
    if(sleeping?boot!=ENETDOWN:boot!=0) {xpc_dictionary_set_int64(reply,"result",1);return;}
    xpc_dictionary_set_int64(reply,"result",0);
    xpc_dictionary_set_int64(reply,"manufacturer",MANUFACTURER_QUALCOMM);
    xpc_dictionary_set_string(reply,"firmware",firmware);
}

static void handle(xpc_object_t message)
{
    if(xpc_get_type(message)!=&_xpc_type_dictionary) return;
    const char *id=xpc_dictionary_get_string(message,"kMsgId");
    xpc_object_t reply=xpc_dictionary_create_reply(message);
    if(!reply) return;
    if(id && !strcmp(id,"RunBuiltinScript")) run_builtin_script(message,reply);
    else {
        os_log_error(logger,"%{public}s is a Broadcom tool request; not supported on WCN399x",id?id:"(no kMsgId)");
        xpc_dictionary_set_int64(reply,"result",1);
        if(id && !strcmp(id,"RunCommand")) xpc_dictionary_set_string(reply,"output","not supported on WCN399x");
    }
    xpc_connection_send_message(xpc_dictionary_get_remote_connection(message),reply);
    xpc_release(reply);
}

int main(int argc,char **argv)
{
    if(argc!=2 || strcmp(argv[1],"-R")) {fprintf(stderr,"usage: %s -R\n",argv[0]);return 64;}
    xpc_transaction_begin(); /* Keep the launchd pressured-exit service alive. */
    logger=os_log_create("com.apple.bluetooth","BlueTool");
    xpc_object_t listener=xpc_connection_create_mach_service("com.apple.BlueTool",dispatch_get_main_queue(),XPC_CONNECTION_MACH_SERVICE_LISTENER);
    xpc_connection_set_event_handler(listener,^(xpc_object_t peer) {
        if(xpc_get_type(peer)!=&_xpc_type_connection) return;
        xpc_connection_set_event_handler(peer,^(xpc_object_t message) {handle(message);});
        xpc_connection_resume(peer);
    });
    xpc_connection_resume(listener);
    dispatch_main();
}
