/**
 * @file client.c
 * @author Ambroz Bizjak <ambrop7@gmail.com>
 * 
 * @section LICENSE
 * 
 * This file is part of BadVPN.
 * 
 * BadVPN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 * 
 * BadVPN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#include <protocol/msgproto.h>
#include <protocol/addr.h>
#include <protocol/dataproto.h>
#include <misc/version.h>
#include <misc/debug.h>
#include <misc/offset.h>
#include <misc/byteorder.h>
#include <misc/nsskey.h>
#include <misc/loglevel.h>
#include <misc/loggers_string.h>
#include <misc/string_begins_with.h>
#include <structure/LinkedList2.h>
#include <base/DebugObject.h>
#include <base/BLog.h>
#include <security/BSecurity.h>
#include <security/BRandom.h>
#include <system/BSignal.h>
#include <system/BTime.h>
#include <system/BNetwork.h>
#include <nspr_support/DummyPRFileDesc.h>
#include <nspr_support/BSSLConnection.h>
#include <server_connection/ServerConnection.h>
#include <tuntap/BTap.h>

#ifndef BADVPN_USE_WINAPI
#include <base/BLog_syslog.h>
#endif

#include <client/client.h>

#include <generated/blog_channel_client.h>

#define TRANSPORT_MODE_UDP 0
#define TRANSPORT_MODE_TCP 1

#define LOGGER_STDOUT 1
#define LOGGER_SYSLOG 2

// declares and initializes a pointer x to y
#define POINTER(x, y) typeof (y) *(x) = &(y);

// command-line options
struct {
    int help;
    int version;
    int logger;
    #ifndef BADVPN_USE_WINAPI
    char *logger_syslog_facility;
    char *logger_syslog_ident;
    #endif
    int loglevel;
    int loglevels[BLOG_NUM_CHANNELS];
    int threads;
    int ssl;
    char *nssdb;
    char *client_cert_name;
    char *server_name;
    char *server_addr;
    int num_bind_addrs;
    struct {
        char *addr;
        int num_ports;
        int num_ext_addrs;
        struct {
            char *addr;
            char *scope;
        } ext_addrs[MAX_EXT_ADDRS];
    } bind_addrs[MAX_BIND_ADDRS];
    char *tapdev;
    int transport_mode;
    int encryption_mode;
    int hash_mode;
    int otp_mode;
    int otp_num;
    int otp_num_warn;
    int fragmentation_latency;
    int peer_ssl;
    int peer_tcp_socket_sndbuf;
    char *scopes[MAX_SCOPES];
    int num_scopes;
    int send_buffer_size;
    int send_buffer_relay_size;
    int max_macs;
    int max_groups;
    int igmp_group_membership_interval;
    int igmp_last_member_query_time;
} options;

// bind addresses
int num_bind_addrs;
struct {
    BAddr addr;
    int num_ports;
    int num_ext_addrs;
    struct {
        int server_reported_port;
        BAddr addr; // if server_reported_port>=0, defined only after hello received
        char scope[64];
    } ext_addrs[MAX_EXT_ADDRS];
} bind_addrs[MAX_BIND_ADDRS];

// TCP listeners
PasswordListener listeners[MAX_BIND_ADDRS];

// SPProto parameters (UDP only)
struct spproto_security_params sp_params;

// server address we connect to
BAddr server_addr;

// server name to use for SSL
char server_name[256];

// reactor
BReactor ss;

// thread work dispatcher
BThreadWorkDispatcher twd;

// client certificate if using SSL
CERTCertificate *client_cert;

// client private key if using SSL
SECKEYPrivateKey *client_key;

// device
BTap device;
int device_mtu;

// DataProtoSource for device input (reading)
DataProtoSource device_input_dpd;

// DPReceiveDevice for device output (writing)
DPReceiveDevice device_output_dprd;

// data communication MTU
int data_mtu;

// peers list
LinkedList2 peers;
int num_peers;

// frame decider
FrameDecider frame_decider;

// peers that can be user as relays
LinkedList2 relays;

// peers than need a relay
LinkedList2 waiting_relay_peers;

// server connection
ServerConnection server;

// my ID, defined only after server_ready
peerid_t my_id;

// fair queue for sending peer messages to the server
PacketPassFairQueue server_queue;

// whether server is ready
int server_ready;

// dying server flow
struct server_flow *dying_server_flow;

// stops event processing, causing the program to exit
static void terminate (void);

// prints program name and version to standard output
static void print_help (const char *name);

// prints program name and version to standard output
static void print_version (void);

// parses the command line
static int parse_arguments (int argc, char *argv[]);

// processes certain command line options
static int process_arguments (void);

// handler for program termination request
static void signal_handler (void *unused);

// adds a new peer
static void peer_add (peerid_t id, int flags, const uint8_t *cert, int cert_len);

// removes a peer
static void peer_remove (struct peer_data *peer, int exiting);

// passes a message to the logger, prepending it info about the peer
static void peer_log (struct peer_data *peer, int level, const char *fmt, ...);

// see if we are the master relative to this peer
static int peer_am_master (struct peer_data *peer);

// initializes the link
static int peer_init_link (struct peer_data *peer);

// frees link resources
static void peer_free_link (struct peer_data *peer);

// creates a fresh link
static int peer_new_link (struct peer_data *peer);

// registers the peer as a relay provider
static void peer_enable_relay_provider (struct peer_data *peer);

// unregisters the peer as a relay provider
static void peer_disable_relay_provider (struct peer_data *peer);

// install relaying for a peer
static void peer_install_relaying (struct peer_data *peer, struct peer_data *relay);

// uninstall relaying for a peer
static void peer_free_relaying (struct peer_data *peer);

// handle a peer that needs a relay
static void peer_need_relay (struct peer_data *peer);

// inserts the peer into the need relay list
static void peer_register_need_relay (struct peer_data *peer);

// removes the peer from the need relay list
static void peer_unregister_need_relay (struct peer_data *peer);

// handle a link setup failure
static void peer_reset (struct peer_data *peer);

// handle incoming peer messages
static void peer_chat_handler_message (struct peer_data *peer, uint8_t *data, int data_len);

// handlers for different message types
static void peer_msg_youconnect (struct peer_data *peer, uint8_t *data, int data_len);
static void peer_msg_cannotconnect (struct peer_data *peer, uint8_t *data, int data_len);
static void peer_msg_cannotbind (struct peer_data *peer, uint8_t *data, int data_len);
static void peer_msg_seed (struct peer_data *peer, uint8_t *data, int data_len);
static void peer_msg_confirmseed (struct peer_data *peer, uint8_t *data, int data_len);
static void peer_msg_youretry (struct peer_data *peer, uint8_t *data, int data_len);

// handler from DatagramPeerIO when we should generate a new OTP send seed
static void peer_udp_pio_handler_seed_warning (struct peer_data *peer);

// handler from DatagramPeerIO when a new OTP seed can be recognized once it was provided to it
static void peer_udp_pio_handler_seed_ready (struct peer_data *peer);

// handler from DatagramPeerIO when an error occurs on the connection
static void peer_udp_pio_handler_error (struct peer_data *peer);

// handler from StreamPeerIO when an error occurs on the connection
static void peer_tcp_pio_handler_error (struct peer_data *peer);

// peer retry timer handler. The timer is used only on the master side,
// wither when we detect an error, or the peer reports an error.
static void peer_reset_timer_handler (struct peer_data *peer);

// start binding, according to the protocol
static void peer_start_binding (struct peer_data *peer);

// tries binding on one address, according to the protocol
static void peer_bind (struct peer_data *peer);

static void peer_bind_one_address (struct peer_data *peer, int addr_index, int *cont);

static void peer_connect (struct peer_data *peer, BAddr addr, uint8_t *encryption_key, uint64_t password);

static int peer_start_msg (struct peer_data *peer, void **data, int type, int len);

static void peer_end_msg (struct peer_data *peer);

// sends a message with no payload to the peer
static void peer_send_simple (struct peer_data *peer, int msgid);

static void peer_send_conectinfo (struct peer_data *peer, int addr_index, int port_adjust, uint8_t *enckey, uint64_t pass);

static void peer_generate_and_send_seed (struct peer_data *peer);

static void peer_send_confirmseed (struct peer_data *peer, uint16_t seed_id);

// handler for peer DataProto up state changes
static void peer_dataproto_handler (struct peer_data *peer, int up);

// looks for a peer with the given ID
static struct peer_data * find_peer_by_id (peerid_t id);

// device error handler
static void device_error_handler (void *unused);

// DataProtoSource handler for packets from the device
static void device_input_dpd_handler (void *unused, const uint8_t *frame, int frame_len);

// assign relays to clients waiting for them
static void assign_relays (void);

// checks if the given address scope is known (i.e. we can connect to an address in it)
static char * address_scope_known (uint8_t *name, int name_len);

// handlers for server messages
static void server_handler_error (void *user);
static void server_handler_ready (void *user, peerid_t param_my_id, uint32_t ext_ip);
static void server_handler_newclient (void *user, peerid_t peer_id, int flags, const uint8_t *cert, int cert_len);
static void server_handler_endclient (void *user, peerid_t peer_id);
static void server_handler_message (void *user, peerid_t peer_id, uint8_t *data, int data_len);

// jobs
static void peer_job_send_seed_after_binding (struct peer_data *peer);
static void peer_job_init (struct peer_data *peer);

static struct server_flow * server_flow_init (peerid_t peer_id, PacketRecvInterface *input);
static void server_flow_free (struct server_flow *flow);
static void server_flow_die (struct server_flow *flow);
static void server_flow_qflow_handler_busy (struct server_flow *flow);

int main (int argc, char *argv[])
{
    if (argc <= 0) {
        return 1;
    }
    
    // parse command-line arguments
    if (!parse_arguments(argc, argv)) {
        fprintf(stderr, "Failed to parse arguments\n");
        print_help(argv[0]);
        goto fail0;
    }
    
    // handle --help and --version
    if (options.help) {
        print_version();
        print_help(argv[0]);
        return 0;
    }
    if (options.version) {
        print_version();
        return 0;
    }
    
    // initialize logger
    switch (options.logger) {
        case LOGGER_STDOUT:
            BLog_InitStdout();
            break;
        #ifndef BADVPN_USE_WINAPI
        case LOGGER_SYSLOG:
            if (!BLog_InitSyslog(options.logger_syslog_ident, options.logger_syslog_facility)) {
                fprintf(stderr, "Failed to initialize syslog logger\n");
                goto fail0;
            }
            break;
        #endif
        default:
            ASSERT(0);
    }
    
    // configure logger channels
    for (int i = 0; i < BLOG_NUM_CHANNELS; i++) {
        if (options.loglevels[i] >= 0) {
            BLog_SetChannelLoglevel(i, options.loglevels[i]);
        }
        else if (options.loglevel >= 0) {
            BLog_SetChannelLoglevel(i, options.loglevel);
        }
    }
    
    BLog(BLOG_NOTICE, "initializing "GLOBAL_PRODUCT_NAME" "PROGRAM_NAME" "GLOBAL_VERSION);
    
    // initialize network
    if (!BNetwork_GlobalInit()) {
        BLog(BLOG_ERROR, "BNetwork_GlobalInit failed");
        goto fail1;
    }
    
    // init time
    BTime_Init();
    
    // process arguments
    if (!process_arguments()) {
        BLog(BLOG_ERROR, "Failed to process arguments");
        goto fail1;
    }
    
    // init reactor
    if (!BReactor_Init(&ss)) {
        BLog(BLOG_ERROR, "BReactor_Init failed");
        goto fail1;
    }
    
    // setup signal handler
    if (!BSignal_Init(&ss, signal_handler, NULL)) {
        BLog(BLOG_ERROR, "BSignal_Init failed");
        goto fail2;
    }
    
    // init thread work dispatcher
    if (!BThreadWorkDispatcher_Init(&twd, &ss, options.threads)) {
        BLog(BLOG_ERROR, "BThreadWorkDispatcher_Init failed");
        goto fail2a;
    }
    
    // init BSecurity
    if (BThreadWorkDispatcher_UsingThreads(&twd)) {
        if (!BSecurity_GlobalInitThreadSafe()) {
            BLog(BLOG_ERROR, "BSecurity_GlobalInitThreadSafe failed");
            goto fail2b;
        }
    }
    
    if (options.ssl) {
        // init NSPR
        PR_Init(PR_USER_THREAD, PR_PRIORITY_NORMAL, 0);
        
        // register local NSPR file types
        if (!DummyPRFileDesc_GlobalInit()) {
            BLog(BLOG_ERROR, "DummyPRFileDesc_GlobalInit failed");
            goto fail3;
        }
        if (!BSSLConnection_GlobalInit()) {
            BLog(BLOG_ERROR, "BSSLConnection_GlobalInit failed");
            goto fail3;
        }
        
        // init NSS
        if (NSS_Init(options.nssdb) != SECSuccess) {
            BLog(BLOG_ERROR, "NSS_Init failed (%d)", (int)PR_GetError());
            goto fail3;
        }
        
        // set cipher policy
        if (NSS_SetDomesticPolicy() != SECSuccess) {
            BLog(BLOG_ERROR, "NSS_SetDomesticPolicy failed (%d)", (int)PR_GetError());
            goto fail4;
        }
        
        // init server cache
        if (SSL_ConfigServerSessionIDCache(0, 0, 0, NULL) != SECSuccess) {
            BLog(BLOG_ERROR, "SSL_ConfigServerSessionIDCache failed (%d)", (int)PR_GetError());
            goto fail4;
        }
        
        // open server certificate and private key
        if (!open_nss_cert_and_key(options.client_cert_name, &client_cert, &client_key)) {
            BLog(BLOG_ERROR, "Cannot open certificate and key");
            goto fail5;
        }
    }
    
    // init listeners
    int num_listeners = 0;
    if (options.transport_mode == TRANSPORT_MODE_TCP) {
        while (num_listeners < num_bind_addrs) {
            POINTER(addr, bind_addrs[num_listeners])
            if (!PasswordListener_Init(
                &listeners[num_listeners], &ss, addr->addr, 50, options.peer_ssl,
                (options.peer_ssl ? client_cert : NULL),
                (options.peer_ssl ? client_key : NULL)
            )) {
                BLog(BLOG_ERROR, "PasswordListener_Init failed");
                goto fail6;
            }
            num_listeners++;
        }
    }
    
    // init device
    if (!BTap_Init(&device, &ss, options.tapdev, device_error_handler, NULL, 0)) {
        BLog(BLOG_ERROR, "BTap_Init failed");
        goto fail6;
    }
    
    // remember device MTU
    device_mtu = BTap_GetMTU(&device);
    
    BLog(BLOG_INFO, "device MTU is %d", device_mtu);
    
    // calculate data MTU
    if (device_mtu > INT_MAX - DATAPROTO_MAX_OVERHEAD) {
        BLog(BLOG_ERROR, "Device MTU is too large");
        goto fail7;
    }
    data_mtu = DATAPROTO_MAX_OVERHEAD + device_mtu;
    
    // init device input
    if (!DataProtoSource_Init(&device_input_dpd, BTap_GetOutput(&device), device_input_dpd_handler, NULL, &ss)) {
        BLog(BLOG_ERROR, "DataProtoSource_Init failed");
        goto fail7;
    }
    
    // init device output
    if (!DPReceiveDevice_Init(&device_output_dprd, device_mtu, (DPReceiveDevice_output_func)BTap_Send, &device, &ss, options.send_buffer_relay_size, PEER_RELAY_FLOW_INACTIVITY_TIME)) {
        BLog(BLOG_ERROR, "DPReceiveDevice_Init failed");
        goto fail7a;
    }
    
    // init peers list
    LinkedList2_Init(&peers);
    num_peers = 0;
    
    // init frame decider
    FrameDecider_Init(&frame_decider, options.max_macs, options.max_groups, options.igmp_group_membership_interval, options.igmp_last_member_query_time, &ss);
    
    // init relays list
    LinkedList2_Init(&relays);
    
    // init need relay list
    LinkedList2_Init(&waiting_relay_peers);
    
    // start connecting to server
    if (!ServerConnection_Init(
        &server, &ss, server_addr, SC_KEEPALIVE_INTERVAL, SERVER_BUFFER_MIN_PACKETS, options.ssl, client_cert, client_key, server_name, NULL,
        server_handler_error, server_handler_ready, server_handler_newclient, server_handler_endclient, server_handler_message
    )) {
        BLog(BLOG_ERROR, "ServerConnection_Init failed");
        goto fail9;
    }
    
    // set server not ready
    server_ready = 0;
    
    // set no dying flow
    dying_server_flow = NULL;
    
    // enter event loop
    BLog(BLOG_NOTICE, "entering event loop");
    BReactor_Exec(&ss);
    
    // allow freeing server queue flows
    if (server_ready) {
        PacketPassFairQueue_PrepareFree(&server_queue);
    }
    
    // free peers
    LinkedList2Node *node;
    while (node = LinkedList2_GetFirst(&peers)) {
        struct peer_data *peer = UPPER_OBJECT(node, struct peer_data, list_node);
        peer_remove(peer, 1);
    }
    
    // free dying server flow
    if (dying_server_flow) {
        server_flow_free(dying_server_flow);
    }
    
    if (server_ready) {
        PacketPassFairQueue_Free(&server_queue);
    }
    ServerConnection_Free(&server);
fail9:
    FrameDecider_Free(&frame_decider);
    DPReceiveDevice_Free(&device_output_dprd);
fail7a:
    DataProtoSource_Free(&device_input_dpd);
fail7:
    BTap_Free(&device);
fail6:
    if (options.transport_mode == TRANSPORT_MODE_TCP) {
        while (num_listeners-- > 0) {
            PasswordListener_Free(&listeners[num_listeners]);
        }
    }
    if (options.ssl) {
        CERT_DestroyCertificate(client_cert);
        SECKEY_DestroyPrivateKey(client_key);
fail5:
        ASSERT_FORCE(SSL_ShutdownServerSessionIDCache() == SECSuccess)
fail4:
        SSL_ClearSessionCache();
        ASSERT_FORCE(NSS_Shutdown() == SECSuccess)
fail3:
        ASSERT_FORCE(PR_Cleanup() == PR_SUCCESS)
        PL_ArenaFinish();
    }
    if (BThreadWorkDispatcher_UsingThreads(&twd)) {
        BSecurity_GlobalFreeThreadSafe();
    }
fail2b:
    BThreadWorkDispatcher_Free(&twd);
fail2a:
    BSignal_Finish();
fail2:
    BReactor_Free(&ss);
fail1:
    BLog(BLOG_NOTICE, "exiting");
    BLog_Free();
fail0:
    // finish objects
    DebugObjectGlobal_Finish();
    return 1;
}

void terminate (void)
{
    BLog(BLOG_NOTICE, "tearing down");
    
    // exit event loop
    BReactor_Quit(&ss, 0);
}

void print_help (const char *name)
{
    printf(
        "Usage:\n"
        "    %s\n"
        "        [--help]\n"
        "        [--version]\n"
        "        [--logger <"LOGGERS_STRING">]\n"
        #ifndef BADVPN_USE_WINAPI
        "        (logger=syslog?\n"
        "            [--syslog-facility <string>]\n"
        "            [--syslog-ident <string>]\n"
        "        )\n"
        #endif
        "        [--loglevel <0-5/none/error/warning/notice/info/debug>]\n"
        "        [--channel-loglevel <channel-name> <0-5/none/error/warning/notice/info/debug>] ...\n"
        "        [--threads <integer>]\n"
        "        [--ssl --nssdb <string> --client-cert-name <string>]\n"
        "        [--server-name <string>]\n"
        "        --server-addr <addr>\n"
        "        [--tapdev <name>]\n"
        "        [--scope <scope_name>] ...\n"
        "        [\n"
        "            --bind-addr <addr>\n"
        "            (transport-mode=udp? --num-ports <num>)\n"
        "            [--ext-addr <addr / {server_reported}:port> <scope_name>] ...\n"
        "        ] ...\n"
        "        --transport-mode <udp/tcp>\n"
        "        (transport-mode=udp?\n"
        "            --encryption-mode <blowfish/aes/none>\n"
        "            --hash-mode <md5/sha1/none>\n"
        "            [--otp <blowfish/aes> <num> <num-warn>]\n"
        "            [--fragmentation-latency <milliseconds>]\n"
        "        )\n"
        "        (transport-mode=tcp?\n"
        "            (ssl? [--peer-ssl])\n"
        "            [--peer-tcp-socket-sndbuf <bytes / 0>]\n"
        "        )\n"
        "        [--send-buffer-size <num-packets>]\n"
        "        [--send-buffer-relay-size <num-packets>]\n"
        "        [--max-macs <num>]\n"
        "        [--max-groups <num>]\n"
        "        [--igmp-group-membership-interval <ms>]\n"
        "        [--igmp-last-member-query-time <ms>]\n"
        "Address format is a.b.c.d:port (IPv4) or [addr]:port (IPv6).\n",
        name
    );
}

void print_version (void)
{
    printf(GLOBAL_PRODUCT_NAME" "PROGRAM_NAME" "GLOBAL_VERSION"\n"GLOBAL_COPYRIGHT_NOTICE"\n");
}

int parse_arguments (int argc, char *argv[])
{
    if (argc <= 0) {
        return 0;
    }
    
    options.help = 0;
    options.version = 0;
    options.logger = LOGGER_STDOUT;
    #ifndef BADVPN_USE_WINAPI
    options.logger_syslog_facility = "daemon";
    options.logger_syslog_ident = argv[0];
    #endif
    options.loglevel = -1;
    for (int i = 0; i < BLOG_NUM_CHANNELS; i++) {
        options.loglevels[i] = -1;
    }
    options.threads = 0;
    options.ssl = 0;
    options.nssdb = NULL;
    options.client_cert_name = NULL;
    options.server_name = NULL;
    options.server_addr = NULL;
    options.tapdev = NULL;
    options.num_scopes = 0;
    options.num_bind_addrs = 0;
    options.transport_mode = -1;
    options.encryption_mode = -1;
    options.hash_mode = -1;
    options.otp_mode = SPPROTO_OTP_MODE_NONE;
    options.fragmentation_latency = PEER_DEFAULT_UDP_FRAGMENTATION_LATENCY;
    options.peer_ssl = 0;
    options.peer_tcp_socket_sndbuf = -1;
    options.send_buffer_size = PEER_DEFAULT_SEND_BUFFER_SIZE;
    options.send_buffer_relay_size = PEER_DEFAULT_SEND_BUFFER_RELAY_SIZE;
    options.max_macs = PEER_DEFAULT_MAX_MACS;
    options.max_groups = PEER_DEFAULT_MAX_GROUPS;
    options.igmp_group_membership_interval = DEFAULT_IGMP_GROUP_MEMBERSHIP_INTERVAL;
    options.igmp_last_member_query_time = DEFAULT_IGMP_LAST_MEMBER_QUERY_TIME;
    
    int have_fragmentation_latency = 0;
    
    int i;
    for (i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (!strcmp(arg, "--help")) {
            options.help = 1;
        }
        else if (!strcmp(arg, "--version")) {
            options.version = 1;
        }
        else if (!strcmp(arg, "--logger")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            char *arg2 = argv[i + 1];
            if (!strcmp(arg2, "stdout")) {
                options.logger = LOGGER_STDOUT;
            }
            #ifndef BADVPN_USE_WINAPI
            else if (!strcmp(arg2, "syslog")) {
                options.logger = LOGGER_SYSLOG;
            }
            #endif
            else {
                fprintf(stderr, "%s: wrong argument\n", arg);
                return 0;
            }
            i++;
        }
        #ifndef BADVPN_USE_WINAPI
        else if (!strcmp(arg, "--syslog-facility")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            options.logger_syslog_facility = argv[i + 1];
            i++;
        }
        else if (!strcmp(arg, "--syslog-ident")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            options.logger_syslog_ident = argv[i + 1];
            i++;
        }
        #endif
        else if (!strcmp(arg, "--loglevel")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            if ((options.loglevel = parse_loglevel(argv[i + 1])) < 0) {
                fprintf(stderr, "%s: wrong argument\n", arg);
                return 0;
            }
            i++;
        }
        else if (!strcmp(arg, "--channel-loglevel")) {
            if (2 >= argc - i) {
                fprintf(stderr, "%s: requires two arguments\n", arg);
                return 0;
            }
            int channel = BLogGlobal_GetChannelByName(argv[i + 1]);
            if (channel < 0) {
                fprintf(stderr, "%s: wrong channel argument\n", arg);
                return 0;
            }
            int loglevel = parse_loglevel(argv[i + 2]);
            if (loglevel < 0) {
                fprintf(stderr, "%s: wrong loglevel argument\n", arg);
                return 0;
            }
            options.loglevels[channel] = loglevel;
            i += 2;
        }
        else if (!strcmp(arg, "--threads")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            options.threads = atoi(argv[i + 1]);
            i++;
        }
        else if (!strcmp(arg, "--ssl")) {
            options.ssl = 1;
        }
        else if (!strcmp(arg, "--nssdb")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            options.nssdb = argv[i + 1];
            i++;
        }
        else if (!strcmp(arg, "--client-cert-name")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            options.client_cert_name = argv[i + 1];
            i++;
        }
        else if (!strcmp(arg, "--server-name")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            options.server_name = argv[i + 1];
            i++;
        }
        else if (!strcmp(arg, "--server-addr")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            options.server_addr = argv[i + 1];
            i++;
        }
        else if (!strcmp(arg, "--tapdev")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            options.tapdev = argv[i + 1];
            i++;
        }
        else if (!strcmp(arg, "--scope")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            if (options.num_scopes == MAX_SCOPES) {
                fprintf(stderr, "%s: too many\n", arg);
                return 0;
            }
            options.scopes[options.num_scopes] = argv[i + 1];
            options.num_scopes++;
            i++;
        }
        else if (!strcmp(arg, "--bind-addr")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            if (options.num_bind_addrs == MAX_BIND_ADDRS) {
                fprintf(stderr, "%s: too many\n", arg);
                return 0;
            }
            POINTER(addr, options.bind_addrs[options.num_bind_addrs])
            addr->addr = argv[i + 1];
            addr->num_ports = -1;
            addr->num_ext_addrs = 0;
            options.num_bind_addrs++;
            i++;
        }
        else if (!strcmp(arg, "--num-ports")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            if (options.num_bind_addrs == 0) {
                fprintf(stderr, "%s: must folow --bind-addr\n", arg);
                return 0;
            }
            POINTER(addr, options.bind_addrs[options.num_bind_addrs - 1])
            if ((addr->num_ports = atoi(argv[i + 1])) < 0) {
                fprintf(stderr, "%s: wrong argument\n", arg);
                return 0;
            }
            i++;
        }
        else if (!strcmp(arg, "--ext-addr")) {
            if (2 >= argc - i) {
                fprintf(stderr, "%s: requires two arguments\n", arg);
                return 0;
            }
            if (options.num_bind_addrs == 0) {
                fprintf(stderr, "%s: must folow --bind-addr\n", arg);
                return 0;
            }
            POINTER(addr, options.bind_addrs[options.num_bind_addrs - 1])
            if (addr->num_ext_addrs == MAX_EXT_ADDRS) {
                fprintf(stderr, "%s: too many\n", arg);
                return 0;
            }
            POINTER(eaddr, addr->ext_addrs[addr->num_ext_addrs])
            eaddr->addr = argv[i + 1];
            eaddr->scope = argv[i + 2];
            addr->num_ext_addrs++;
            i += 2;
        }
        else if (!strcmp(arg, "--transport-mode")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            char *arg2 = argv[i + 1];
            if (!strcmp(arg2, "udp")) {
                options.transport_mode = TRANSPORT_MODE_UDP;
            }
            else if (!strcmp(arg2, "tcp")) {
                options.transport_mode = TRANSPORT_MODE_TCP;
            }
            else {
                fprintf(stderr, "%s: wrong argument\n", arg);
                return 0;
            }
            i++;
        }
        else if (!strcmp(arg, "--encryption-mode")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            char *arg2 = argv[i + 1];
            if (!strcmp(arg2, "none")) {
                options.encryption_mode = SPPROTO_ENCRYPTION_MODE_NONE;
            }
            else if (!strcmp(arg2, "blowfish")) {
                options.encryption_mode = BENCRYPTION_CIPHER_BLOWFISH;
            }
            else if (!strcmp(arg2, "aes")) {
                options.encryption_mode = BENCRYPTION_CIPHER_AES;
            }
            else {
                fprintf(stderr, "%s: wrong argument\n", arg);
                return 0;
            }
            i++;
        }
        else if (!strcmp(arg, "--hash-mode")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            char *arg2 = argv[i + 1];
            if (!strcmp(arg2, "none")) {
                options.hash_mode = SPPROTO_HASH_MODE_NONE;
            }
            else if (!strcmp(arg2, "md5")) {
                options.hash_mode = BHASH_TYPE_MD5;
            }
            else if (!strcmp(arg2, "sha1")) {
                options.hash_mode = BHASH_TYPE_SHA1;
            }
            else {
                fprintf(stderr, "%s: wrong argument\n", arg);
                return 0;
            }
            i++;
        }
        else if (!strcmp(arg, "--otp")) {
            if (3 >= argc - i) {
                fprintf(stderr, "%s: requires three arguments\n", arg);
                return 0;
            }
            char *otp_mode = argv[i + 1];
            char *otp_num = argv[i + 2];
            char *otp_num_warn = argv[i + 3];
            if (!strcmp(otp_mode, "blowfish")) {
                options.otp_mode = BENCRYPTION_CIPHER_BLOWFISH;
            }
            else if (!strcmp(otp_mode, "aes")) {
                options.otp_mode = BENCRYPTION_CIPHER_AES;
            }
            else {
                fprintf(stderr, "%s: wrong mode\n", arg);
                return 0;
            }
            if ((options.otp_num = atoi(otp_num)) <= 0) {
                fprintf(stderr, "%s: wrong num\n", arg);
                return 0;
            }
            options.otp_num_warn = atoi(otp_num_warn);
            if (options.otp_num_warn <= 0 || options.otp_num_warn > options.otp_num) {
                fprintf(stderr, "%s: wrong num warn\n", arg);
                return 0;
            }
            i += 3;
        }
        else if (!strcmp(arg, "--fragmentation-latency")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            options.fragmentation_latency = atoi(argv[i + 1]);
            have_fragmentation_latency = 1;
            i++;
        }
        else if (!strcmp(arg, "--peer-ssl")) {
            options.peer_ssl = 1;
        }
        else if (!strcmp(arg, "--peer-tcp-socket-sndbuf")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            if ((options.peer_tcp_socket_sndbuf = atoi(argv[i + 1])) < 0) {
                fprintf(stderr, "%s: wrong argument\n", arg);
                return 0;
            }
            i++;
        }
        else if (!strcmp(arg, "--send-buffer-size")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            if ((options.send_buffer_size = atoi(argv[i + 1])) <= 0) {
                fprintf(stderr, "%s: wrong argument\n", arg);
                return 0;
            }
            i++;
        }
        else if (!strcmp(arg, "--send-buffer-relay-size")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            if ((options.send_buffer_relay_size = atoi(argv[i + 1])) <= 0) {
                fprintf(stderr, "%s: wrong argument\n", arg);
                return 0;
            }
            i++;
        }
        else if (!strcmp(arg, "--max-macs")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            if ((options.max_macs = atoi(argv[i + 1])) <= 0) {
                fprintf(stderr, "%s: wrong argument\n", arg);
                return 0;
            }
            i++;
        }
        else if (!strcmp(arg, "--max-groups")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            if ((options.max_groups = atoi(argv[i + 1])) <= 0) {
                fprintf(stderr, "%s: wrong argument\n", arg);
                return 0;
            }
            i++;
        }
        else if (!strcmp(arg, "--igmp-group-membership-interval")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            if ((options.igmp_group_membership_interval = atoi(argv[i + 1])) <= 0) {
                fprintf(stderr, "%s: wrong argument\n", arg);
                return 0;
            }
            i++;
        }
        else if (!strcmp(arg, "--igmp-last-member-query-time")) {
            if (1 >= argc - i) {
                fprintf(stderr, "%s: requires an argument\n", arg);
                return 0;
            }
            if ((options.igmp_last_member_query_time = atoi(argv[i + 1])) <= 0) {
                fprintf(stderr, "%s: wrong argument\n", arg);
                return 0;
            }
            i++;
        }
        else {
            fprintf(stderr, "unknown option: %s\n", arg);
            return 0;
        }
    }
    
    if (options.help || options.version) {
        return 1;
    }
    
    if (options.ssl != !!options.nssdb) {
        fprintf(stderr, "False: --ssl <=> --nssdb\n");
        return 0;
    }
    
    if (options.ssl != !!options.client_cert_name) {
        fprintf(stderr, "False: --ssl <=> --client-cert-name\n");
        return 0;
    }
    
    if (!options.server_addr) {
        fprintf(stderr, "False: --server-addr\n");
        return 0;
    }
    
    if (options.transport_mode < 0) {
        fprintf(stderr, "False: --transport-mode\n");
        return 0;
    }
    
    if ((options.transport_mode == TRANSPORT_MODE_UDP) != (options.encryption_mode >= 0)) {
        fprintf(stderr, "False: UDP <=> --encryption-mode\n");
        return 0;
    }
    
    if ((options.transport_mode == TRANSPORT_MODE_UDP) != (options.hash_mode >= 0)) {
        fprintf(stderr, "False: UDP <=> --hash-mode\n");
        return 0;
    }
    
    if (!(!(options.otp_mode != SPPROTO_OTP_MODE_NONE) || (options.transport_mode == TRANSPORT_MODE_UDP))) {
        fprintf(stderr, "False: --otp => UDP\n");
        return 0;
    }
    
    if (!(!have_fragmentation_latency || (options.transport_mode == TRANSPORT_MODE_UDP))) {
        fprintf(stderr, "False: --fragmentation-latency => UDP\n");
        return 0;
    }
    
    if (!(!options.peer_ssl || (options.ssl && options.transport_mode == TRANSPORT_MODE_TCP))) {
        fprintf(stderr, "False: --peer-ssl => (--ssl && TCP)\n");
        return 0;
    }
    
    if (!(!(options.peer_tcp_socket_sndbuf >= 0) || options.transport_mode == TRANSPORT_MODE_TCP)) {
        fprintf(stderr, "False: --peer-tcp-socket-sndbuf => TCP\n");
        return 0;
    }
    
    return 1;
}

int process_arguments (void)
{
    // resolve server address
    ASSERT(options.server_addr)
    if (!BAddr_Parse(&server_addr, options.server_addr, server_name, sizeof(server_name))) {
        BLog(BLOG_ERROR, "server addr: BAddr_Parse failed");
        return 0;
    }
    if (!addr_supported(server_addr)) {
        BLog(BLOG_ERROR, "server addr: not supported");
        return 0;
    }
    
    // override server name if requested
    if (options.server_name) {
        snprintf(server_name, sizeof(server_name), "%s", options.server_name);
    }
    
    // resolve bind addresses and external addresses
    num_bind_addrs = 0;
    for (int i = 0; i < options.num_bind_addrs; i++) {
        POINTER(addr, options.bind_addrs[i])
        POINTER(out, bind_addrs[num_bind_addrs])
        
        // read addr
        if (!BAddr_Parse(&out->addr, addr->addr, NULL, 0)) {
            BLog(BLOG_ERROR, "bind addr: BAddr_Parse failed");
            return 0;
        }
        
        // read num ports
        if (options.transport_mode == TRANSPORT_MODE_UDP) {
            if (addr->num_ports < 0) {
                BLog(BLOG_ERROR, "bind addr: num ports missing");
                return 0;
            }
            out->num_ports = addr->num_ports;
        }
        else if (addr->num_ports >= 0) {
            BLog(BLOG_ERROR, "bind addr: num ports given, but not using UDP");
            return 0;
        }
        
        // read ext addrs
        out->num_ext_addrs = 0;
        for (int j = 0; j < addr->num_ext_addrs; j++) {
            POINTER(eaddr, addr->ext_addrs[j])
            POINTER(eout, out->ext_addrs[out->num_ext_addrs])
            
            // read addr
            if (string_begins_with(eaddr->addr, "{server_reported}:")) {
                char *colon = strstr(eaddr->addr, ":");
                if ((eout->server_reported_port = atoi(colon + 1)) < 0) {
                    BLog(BLOG_ERROR, "ext addr: wrong port");
                    return 0;
                }
            } else {
                eout->server_reported_port = -1;
                if (!BAddr_Parse(&eout->addr, eaddr->addr, NULL, 0)) {
                    BLog(BLOG_ERROR, "ext addr: BAddr_Parse failed");
                    return 0;
                }
            }
            
            // read scope
            snprintf(eout->scope, sizeof(eout->scope), "%s", eaddr->scope);
            
            out->num_ext_addrs++;
        }
        
        num_bind_addrs++;
    }
    
    // initialize SPProto parameters
    if (options.transport_mode == TRANSPORT_MODE_UDP) {
        sp_params.encryption_mode = options.encryption_mode;
        sp_params.hash_mode = options.hash_mode;
        sp_params.otp_mode = options.otp_mode;
        if (options.otp_mode > 0) {
            sp_params.otp_num = options.otp_num;
        }
    }
    
    return 1;
}

void signal_handler (void *unused)
{
    BLog(BLOG_NOTICE, "termination requested");
    
    terminate();
}

void peer_add (peerid_t id, int flags, const uint8_t *cert, int cert_len)
{
    ASSERT(ServerConnection_IsReady(&server))
    ASSERT(num_peers < MAX_PEERS)
    ASSERT(!find_peer_by_id(id))
    ASSERT(id != my_id)
    ASSERT(cert_len >= 0)
    ASSERT(cert_len <= SCID_NEWCLIENT_MAX_CERT_LEN)
    
    // allocate structure
    struct peer_data *peer = malloc(sizeof(*peer));
    if (!peer) {
        BLog(BLOG_ERROR, "peer %d: failed to allocate memory", (int)id);
        goto fail0;
    }
    
    // remember id
    peer->id = id;
    
    // remember flags
    peer->flags = flags;
    
    // set no common name
    peer->common_name = NULL;
    
    // init jobs
    BPending_Init(&peer->job_send_seed_after_binding, BReactor_PendingGroup(&ss), (BPending_handler)peer_job_send_seed_after_binding, peer);
    BPending_Init(&peer->job_init, BReactor_PendingGroup(&ss), (BPending_handler)peer_job_init, peer);
    
    // set init job (must be before initing server flow so we can send)
    BPending_Set(&peer->job_init);
    
    // init chat
    if (!PeerChat_Init(&peer->chat, peer->id, BReactor_PendingGroup(&ss), peer, NULL,
                                                                                (PeerChat_handler_message)peer_chat_handler_message
    )) {
        peer_log(peer, BLOG_ERROR, "PeerChat_Init failed");
        goto fail1;
    }
    
    // init server flow
    if (!(peer->chat_send_flow = server_flow_init(peer->id, PeerChat_GetSendOutput(&peer->chat)))) {
        peer_log(peer, BLOG_ERROR, "server_flow_init failed");
        goto fail1a;
    }
    
    // init chat send writer
    BufferWriter_Init(&peer->chat_send_writer, SC_MAX_MSGLEN, BReactor_PendingGroup(&ss));
    
    // init chat send buffer
    if (!PacketBuffer_Init(&peer->chat_send_buffer, BufferWriter_GetOutput(&peer->chat_send_writer), PeerChat_GetSendInput(&peer->chat), SERVER_BUFFER_MIN_PACKETS, BReactor_PendingGroup(&ss))) {
        peer_log(peer, BLOG_ERROR, "PacketBuffer_Init failed");
        goto fail1b;
    }
    
    // set no message
    peer->chat_send_msg_len = -1;
    
    if (options.ssl) {
        // remember certificate
        memcpy(peer->cert, cert, cert_len);
        peer->cert_len = cert_len;
        
        // make sure that CERT_DecodeCertFromPackage will interpretet the input as raw DER and not base64,
        // in which case following workaroud wouldn't help
        if (!(cert_len > 0 && (cert[0] & 0x1f) == 0x10)) {
            peer_log(peer, BLOG_ERROR, "certificate does not look like DER");
            goto fail1c;
        }
        
        // copy the certificate and append it a good load of zero bytes,
        // hopefully preventing the crappy CERT_DecodeCertFromPackage from crashing
        // by reading past the of its input
        uint8_t *certbuf = malloc(cert_len + 100);
        if (!certbuf) {
            peer_log(peer, BLOG_ERROR, "malloc failed");
            goto fail1c;
        }
        memcpy(certbuf, cert, cert_len);
        memset(certbuf + cert_len, 0, 100);
        
        // decode certificate, so we can extract the common name
        CERTCertificate *nsscert = CERT_DecodeCertFromPackage((char *)certbuf, cert_len);
        if (!nsscert) {
            peer_log(peer, BLOG_ERROR, "CERT_DecodeCertFromPackage failed (%d)", PORT_GetError());
            free(certbuf);
            goto fail1c;
        }
        
        free(certbuf);
        
        // remember common name
        if (!(peer->common_name = CERT_GetCommonName(&nsscert->subject))) {
            peer_log(peer, BLOG_ERROR, "CERT_GetCommonName failed");
            CERT_DestroyCertificate(nsscert);
            goto fail1c;
        }
        
        CERT_DestroyCertificate(nsscert);
    }
    
    // init local flow
    if (!DataProtoFlow_Init(&peer->local_dpflow, &device_input_dpd, my_id, peer->id, options.send_buffer_size, -1, NULL, NULL)) {
        peer_log(peer, BLOG_ERROR, "DataProtoFlow_Init failed");
        goto fail2;
    }
    
    // init frame decider peer
    if (!FrameDeciderPeer_Init(&peer->decider_peer, &frame_decider)) {
        peer_log(peer, BLOG_ERROR, "FrameDeciderPeer_Init failed");
        goto fail3;
    }
    
    // init receive peer
    DPReceivePeer_Init(&peer->receive_peer, &device_output_dprd, peer->id, &peer->decider_peer, !!(peer->flags & SCID_NEWCLIENT_FLAG_RELAY_CLIENT));
    
    // have no link
    peer->have_link = 0;
    
    // have no relaying
    peer->relaying_peer = NULL;
    
    // not waiting for relay
    peer->waiting_relay = 0;
    
    // init reset timer
    BTimer_Init(&peer->reset_timer, PEER_RETRY_TIME, (BTimer_handler)peer_reset_timer_handler, peer);
    
    // is not relay server
    peer->is_relay = 0;
    
    // init binding
    peer->binding = 0;
    
    // add to peers list
    LinkedList2_Append(&peers, &peer->list_node);
    num_peers++;
    
    peer_log(peer, BLOG_INFO, "initialized");
    
    return;
    
fail3:
    DataProtoFlow_Free(&peer->local_dpflow);
fail2:
    if (peer->common_name) {
        PORT_Free(peer->common_name);
    }
fail1c:
    PacketBuffer_Free(&peer->chat_send_buffer);
fail1b:
    BufferWriter_Free(&peer->chat_send_writer);
    server_flow_free(peer->chat_send_flow);
fail1a:
    PeerChat_Free(&peer->chat);
fail1:
    BPending_Free(&peer->job_init);
    BPending_Free(&peer->job_send_seed_after_binding);
    free(peer);
fail0:;
}

void peer_remove (struct peer_data *peer, int exiting)
{
    peer_log(peer, BLOG_INFO, "removing");
    
    // cleanup
    if (peer->have_link) {
        if (peer->is_relay) {
            peer_disable_relay_provider(peer);
        }
        peer_free_link(peer);
    }
    else if (peer->relaying_peer) {
        peer_free_relaying(peer);
    }
    else if (peer->waiting_relay) {
        peer_unregister_need_relay(peer);
    }
    
    ASSERT(!peer->relaying_peer)
    ASSERT(!peer->is_relay)
    ASSERT(!peer->waiting_relay)
    ASSERT(!peer->have_link)
    
    // remove from peers list
    LinkedList2_Remove(&peers, &peer->list_node);
    num_peers--;
    
    // free reset timer
    BReactor_RemoveTimer(&ss, &peer->reset_timer);
    
    // free receive peer
    DPReceivePeer_Free(&peer->receive_peer);
    
    // free frame decider
    FrameDeciderPeer_Free(&peer->decider_peer);
    
    // free local flow
    DataProtoFlow_Free(&peer->local_dpflow);
    
    // free chat send buffer
    PacketBuffer_Free(&peer->chat_send_buffer);
    
    // free chat send writer
    BufferWriter_Free(&peer->chat_send_writer);
    
    // free/die server flow
    if (exiting || !PacketPassFairQueueFlow_IsBusy(&peer->chat_send_flow->qflow)) {
        server_flow_free(peer->chat_send_flow);
    } else {
        server_flow_die(peer->chat_send_flow);
    }
    
    // free chat
    PeerChat_Free(&peer->chat);
    
    // free jobs
    BPending_Free(&peer->job_init);
    BPending_Free(&peer->job_send_seed_after_binding);
    
    // free common name
    if (peer->common_name) {
        PORT_Free(peer->common_name);
    }
    
    // free peer structure
    free(peer);
}

void peer_log (struct peer_data *peer, int level, const char *fmt, ...)
{
    va_list vl;
    va_start(vl, fmt);
    BLog_Append("peer %d", (int)peer->id);
    if (peer->common_name) {
        BLog_Append(" (%s)", peer->common_name);
    }
    BLog_Append(": ");
    BLog_LogToChannelVarArg(BLOG_CURRENT_CHANNEL, level, fmt, vl);
    va_end(vl);
}

int peer_am_master (struct peer_data *peer)
{
    return (my_id > peer->id);
}

int peer_init_link (struct peer_data *peer)
{
    ASSERT(!peer->have_link)
    ASSERT(!peer->relaying_peer)
    ASSERT(!peer->waiting_relay)
    
    ASSERT(!peer->is_relay)
    
    // init receive receiver
    DPReceiveReceiver_Init(&peer->receive_receiver, &peer->receive_peer);
    PacketPassInterface *recv_if = DPReceiveReceiver_GetInput(&peer->receive_receiver);
    
    // init transport-specific link objects
    PacketPassInterface *link_if;
    if (options.transport_mode == TRANSPORT_MODE_UDP) {
        // init DatagramPeerIO
        if (!DatagramPeerIO_Init(
            &peer->pio.udp.pio, &ss, data_mtu, CLIENT_UDP_MTU, sp_params,
            options.fragmentation_latency, PEER_UDP_ASSEMBLER_NUM_FRAMES, recv_if,
            options.otp_num_warn, &twd
        )) {
            peer_log(peer, BLOG_ERROR, "DatagramPeerIO_Init failed");
            goto fail1;
        }
        
        // set handlers
        DatagramPeerIO_SetHandlers(&peer->pio.udp.pio, peer,
            (DatagramPeerIO_handler_error)peer_udp_pio_handler_error,
            (DatagramPeerIO_handler_otp_warning)peer_udp_pio_handler_seed_warning,
            (DatagramPeerIO_handler_otp_ready)peer_udp_pio_handler_seed_ready
        );
        
        // init send seed state
        if (SPPROTO_HAVE_OTP(sp_params)) {
            peer->pio.udp.sendseed_nextid = 0;
            peer->pio.udp.sendseed_sent = 0;
        }
        
        link_if = DatagramPeerIO_GetSendInput(&peer->pio.udp.pio);
    } else {
        // init StreamPeerIO
        if (!StreamPeerIO_Init(
            &peer->pio.tcp.pio, &ss, options.peer_ssl,
            (options.peer_ssl ? peer->cert : NULL),
            (options.peer_ssl ? peer->cert_len : -1),
            data_mtu,
            (options.peer_tcp_socket_sndbuf >= 0 ? options.peer_tcp_socket_sndbuf : PEER_DEFAULT_TCP_SOCKET_SNDBUF),
            recv_if,
            (StreamPeerIO_handler_error)peer_tcp_pio_handler_error, peer
        )) {
            peer_log(peer, BLOG_ERROR, "StreamPeerIO_Init failed");
            goto fail1;
        }
        
        link_if = StreamPeerIO_GetSendInput(&peer->pio.tcp.pio);
    }
    
    // init sending
    if (!DataProtoSink_Init(&peer->send_dp, &ss, link_if, PEER_KEEPALIVE_INTERVAL, PEER_KEEPALIVE_RECEIVE_TIMER, (DataProtoSink_handler)peer_dataproto_handler, peer)) {
        peer_log(peer, BLOG_ERROR, "DataProto_Init failed");
        goto fail2;
    }
    
    // attach local flow to our DataProtoSink
    DataProtoFlow_Attach(&peer->local_dpflow, &peer->send_dp);
    
    // attach receive peer to our DataProtoSink
    DPReceivePeer_AttachSink(&peer->receive_peer, &peer->send_dp);
    
    peer->have_link = 1;
    
    return 1;
    
fail2:
    if (options.transport_mode == TRANSPORT_MODE_UDP) {
        DatagramPeerIO_Free(&peer->pio.udp.pio);
    } else {
        StreamPeerIO_Free(&peer->pio.tcp.pio);
    }
fail1:
    DPReceiveReceiver_Free(&peer->receive_receiver);
    return 0;
}

void peer_free_link (struct peer_data *peer)
{
    ASSERT(peer->have_link)
    ASSERT(!peer->is_relay)
    
    ASSERT(!peer->relaying_peer)
    ASSERT(!peer->waiting_relay)
    
    // detach receive peer from our DataProtoSink
    DPReceivePeer_DetachSink(&peer->receive_peer);
    
    // detach local flow from our DataProtoSink
    DataProtoFlow_Detach(&peer->local_dpflow);
    
    // free sending
    DataProtoSink_Free(&peer->send_dp);
    
    // free transport-specific link objects
    if (options.transport_mode == TRANSPORT_MODE_UDP) {
        DatagramPeerIO_Free(&peer->pio.udp.pio);
    } else {
        StreamPeerIO_Free(&peer->pio.tcp.pio);
    }
    
    // free receive receiver
    DPReceiveReceiver_Free(&peer->receive_receiver);
    
    peer->have_link = 0;
}

int peer_new_link (struct peer_data *peer)
{
    if (peer->have_link) {
        if (peer->is_relay) {
            peer_disable_relay_provider(peer);
        }
        peer_free_link(peer);
    }
    else if (peer->relaying_peer) {
        peer_free_relaying(peer);
    }
    else if (peer->waiting_relay) {
        peer_unregister_need_relay(peer);
    }
    
    if (!peer_init_link(peer)) {
        return 0;
    }
    
    return 1;
}

void peer_enable_relay_provider (struct peer_data *peer)
{
    ASSERT(peer->have_link)
    ASSERT(!peer->is_relay)
    
    ASSERT(!peer->relaying_peer)
    ASSERT(!peer->waiting_relay)
    
    // add to relays list
    LinkedList2_Append(&relays, &peer->relay_list_node);
    
    // init users list
    LinkedList2_Init(&peer->relay_users);
    
    peer->is_relay = 1;
    
    // assign relays
    assign_relays();
}

void peer_disable_relay_provider (struct peer_data *peer)
{
    ASSERT(peer->is_relay)
    
    ASSERT(peer->have_link)
    ASSERT(!peer->relaying_peer)
    ASSERT(!peer->waiting_relay)
    
    // disconnect relay users
    LinkedList2Node *list_node;
    while (list_node = LinkedList2_GetFirst(&peer->relay_users)) {
        struct peer_data *relay_user = UPPER_OBJECT(list_node, struct peer_data, relaying_list_node);
        ASSERT(relay_user->relaying_peer == peer)
        
        // disconnect relay user
        peer_free_relaying(relay_user);
        
        // add it to need relay list
        peer_register_need_relay(relay_user);
    }
    
    // remove from relays list
    LinkedList2_Remove(&relays, &peer->relay_list_node);
    
    peer->is_relay = 0;
    
    // assign relays
    assign_relays();
}

void peer_install_relaying (struct peer_data *peer, struct peer_data *relay)
{
    ASSERT(!peer->relaying_peer)
    ASSERT(!peer->have_link)
    ASSERT(!peer->waiting_relay)
    ASSERT(relay->is_relay)
    
    ASSERT(!peer->is_relay)
    ASSERT(relay->have_link)
    
    peer_log(peer, BLOG_INFO, "installing relaying through %d", (int)relay->id);
    
    // add to relay's users list
    LinkedList2_Append(&relay->relay_users, &peer->relaying_list_node);
    
    // attach local flow to relay
    DataProtoFlow_Attach(&peer->local_dpflow, &relay->send_dp);
    
    peer->relaying_peer = relay;
}

void peer_free_relaying (struct peer_data *peer)
{
    ASSERT(peer->relaying_peer)
    
    ASSERT(!peer->have_link)
    ASSERT(!peer->waiting_relay)
    
    struct peer_data *relay = peer->relaying_peer;
    ASSERT(relay->is_relay)
    ASSERT(relay->have_link)
    
    peer_log(peer, BLOG_INFO, "uninstalling relaying through %d", (int)relay->id);
    
    // detach local flow from relay
    DataProtoFlow_Detach(&peer->local_dpflow);
    
    // remove from relay's users list
    LinkedList2_Remove(&relay->relay_users, &peer->relaying_list_node);
    
    peer->relaying_peer = NULL;
}

void peer_need_relay (struct peer_data *peer)
{
    ASSERT(!peer->is_relay)
    
    if (peer->waiting_relay) {
        // already waiting for relay, do nothing
        return;
    }
    
    if (peer->have_link) {
        peer_free_link(peer);
    }
    else if (peer->relaying_peer) {
        peer_free_relaying(peer);
    }
    
    // register the peer as needing a relay
    peer_register_need_relay(peer);
    
    // assign relays
    assign_relays();
}

void peer_register_need_relay (struct peer_data *peer)
{
    ASSERT(!peer->waiting_relay)
    ASSERT(!peer->have_link)
    ASSERT(!peer->relaying_peer)
    
    ASSERT(!peer->is_relay)
    
    // add to need relay list
    LinkedList2_Append(&waiting_relay_peers, &peer->waiting_relay_list_node);
    
    peer->waiting_relay = 1;
}

void peer_unregister_need_relay (struct peer_data *peer)
{
    ASSERT(peer->waiting_relay)
    
    ASSERT(!peer->have_link)
    ASSERT(!peer->relaying_peer)
    ASSERT(!peer->is_relay)
    
    // remove from need relay list
    LinkedList2_Remove(&waiting_relay_peers, &peer->waiting_relay_list_node);
    
    peer->waiting_relay = 0;
}

void peer_reset (struct peer_data *peer)
{
    peer_log(peer, BLOG_NOTICE, "resetting");
    
    // free link
    if (peer->have_link) {
        if (peer->is_relay) {
            peer_disable_relay_provider(peer);
        }
        peer_free_link(peer);
    }
    
    if (peer_am_master(peer)) {
        // if we're the master, schedule retry
        BReactor_SetTimer(&ss, &peer->reset_timer);
    } else {
        // if we're the slave, report to master
        peer_send_simple(peer, MSGID_YOURETRY);
    }
}

void peer_chat_handler_message (struct peer_data *peer, uint8_t *data, int data_len)
{
    ASSERT(data_len >= 0)
    ASSERT(data_len <= SC_MAX_MSGLEN)
    
    // parse message
    msgParser parser;
    if (!msgParser_Init(&parser, data, data_len)) {
        peer_log(peer, BLOG_NOTICE, "msg: failed to parse");
        return;
    }
    
    // read message
    uint16_t type;
    ASSERT_EXECUTE(msgParser_Gettype(&parser, &type))
    uint8_t *payload;
    int payload_len;
    ASSERT_EXECUTE(msgParser_Getpayload(&parser, &payload, &payload_len))
    
    // dispatch according to message type
    switch (type) {
        case MSGID_YOUCONNECT:
            peer_msg_youconnect(peer, payload, payload_len);
            return;
        case MSGID_CANNOTCONNECT:
            peer_msg_cannotconnect(peer, payload, payload_len);
            return;
        case MSGID_CANNOTBIND:
            peer_msg_cannotbind(peer, payload, payload_len);
            return;
        case MSGID_YOURETRY:
            peer_msg_youretry(peer, payload, payload_len);
            return;
        case MSGID_SEED:
            peer_msg_seed(peer, payload, payload_len);
            return;
        case MSGID_CONFIRMSEED:
            peer_msg_confirmseed(peer, payload, payload_len);
            return;
        default:
            BLog(BLOG_NOTICE, "msg: unknown type");
            return;
    }
}

void peer_msg_youconnect (struct peer_data *peer, uint8_t *data, int data_len)
{
    // init parser
    msg_youconnectParser parser;
    if (!msg_youconnectParser_Init(&parser, data, data_len)) {
        peer_log(peer, BLOG_WARNING, "msg_youconnect: failed to parse");
        return;
    }
    
    // try addresses
    BAddr addr;
    while (1) {
        // get address message
        uint8_t *addrmsg_data;
        int addrmsg_len;
        if (!msg_youconnectParser_Getaddr(&parser, &addrmsg_data, &addrmsg_len)) {
            peer_log(peer, BLOG_NOTICE, "msg_youconnect: no usable addresses");
            peer_send_simple(peer, MSGID_CANNOTCONNECT);
            return;
        }
        
        // parse address message
        msg_youconnect_addrParser aparser;
        if (!msg_youconnect_addrParser_Init(&aparser, addrmsg_data, addrmsg_len)) {
            peer_log(peer, BLOG_WARNING, "msg_youconnect: failed to parse address message");
            return;
        }
        
        // check if the address scope is known
        uint8_t *name_data;
        int name_len;
        ASSERT_EXECUTE(msg_youconnect_addrParser_Getname(&aparser, &name_data, &name_len))
        char *name;
        if (!(name = address_scope_known(name_data, name_len))) {
            continue;
        }
        
        // read address
        uint8_t *addr_data;
        int addr_len;
        ASSERT_EXECUTE(msg_youconnect_addrParser_Getaddr(&aparser, &addr_data, &addr_len))
        if (!addr_read(addr_data, addr_len, &addr)) {
            peer_log(peer, BLOG_WARNING, "msg_youconnect: failed to read address");
            continue;
        }
        
        peer_log(peer, BLOG_NOTICE, "msg_youconnect: using address in scope '%s'", name);
        break;
    }
    
    // discard further addresses
    msg_youconnectParser_Forwardaddr(&parser);
    
    uint8_t *key = NULL;
    uint64_t password = 0;
    
    // read additonal parameters
    if (options.transport_mode == TRANSPORT_MODE_UDP) {
        if (SPPROTO_HAVE_ENCRYPTION(sp_params)) {
            int key_len;
            if (!msg_youconnectParser_Getkey(&parser, &key, &key_len)) {
                peer_log(peer, BLOG_WARNING, "msg_youconnect: no key");
                return;
            }
            if (key_len != BEncryption_cipher_key_size(sp_params.encryption_mode)) {
                peer_log(peer, BLOG_WARNING, "msg_youconnect: wrong key size");
                return;
            }
        }
    } else {
        if (!msg_youconnectParser_Getpassword(&parser, &password)) {
            peer_log(peer, BLOG_WARNING, "msg_youconnect: no password");
            return;
        }
    }
    
    if (!msg_youconnectParser_GotEverything(&parser)) {
        peer_log(peer, BLOG_WARNING, "msg_youconnect: stray data");
        return;
    }
    
    peer_log(peer, BLOG_INFO, "connecting");
    
    peer_connect(peer, addr, key, password);
}

void peer_msg_cannotconnect (struct peer_data *peer, uint8_t *data, int data_len)
{
    if (data_len != 0) {
        peer_log(peer, BLOG_WARNING, "msg_cannotconnect: invalid length");
        return;
    }
    
    if (!peer->binding) {
        peer_log(peer, BLOG_WARNING, "msg_cannotconnect: not binding");
        return;
    }
    
    peer_log(peer, BLOG_INFO, "peer could not connect");
    
    // continue trying bind addresses
    peer_bind(peer);
    return;
}

void peer_msg_cannotbind (struct peer_data *peer, uint8_t *data, int data_len)
{
    if (data_len != 0) {
        peer_log(peer, BLOG_WARNING, "msg_cannotbind: invalid length");
        return;
    }
    
    peer_log(peer, BLOG_INFO, "peer cannot bind");
    
    if (!peer_am_master(peer)) {
        peer_start_binding(peer);
    } else {
        if (!peer->is_relay) {
            peer_need_relay(peer);
        }
    }
}

void peer_msg_seed (struct peer_data *peer, uint8_t *data, int data_len)
{
    msg_seedParser parser;
    if (!msg_seedParser_Init(&parser, data, data_len)) {
        peer_log(peer, BLOG_WARNING, "msg_seed: failed to parse");
        return;
    }
    
    // read message
    uint16_t seed_id;
    ASSERT_EXECUTE(msg_seedParser_Getseed_id(&parser, &seed_id))
    uint8_t *key;
    int key_len;
    ASSERT_EXECUTE(msg_seedParser_Getkey(&parser, &key, &key_len))
    uint8_t *iv;
    int iv_len;
    ASSERT_EXECUTE(msg_seedParser_Getiv(&parser, &iv, &iv_len))
    
    if (options.transport_mode != TRANSPORT_MODE_UDP) {
        peer_log(peer, BLOG_WARNING, "msg_seed: not in UDP mode");
        return;
    }
    
    if (!SPPROTO_HAVE_OTP(sp_params)) {
        peer_log(peer, BLOG_WARNING, "msg_seed: OTPs disabled");
        return;
    }
    
    if (key_len != BEncryption_cipher_key_size(sp_params.otp_mode)) {
        peer_log(peer, BLOG_WARNING, "msg_seed: wrong key length");
        return;
    }
    
    if (iv_len != BEncryption_cipher_block_size(sp_params.otp_mode)) {
        peer_log(peer, BLOG_WARNING, "msg_seed: wrong IV length");
        return;
    }
    
    if (!peer->have_link) {
        peer_log(peer, BLOG_WARNING, "msg_seed: have no link");
        return;
    }
    
    peer_log(peer, BLOG_DEBUG, "received OTP receive seed");
    
    // add receive seed
    DatagramPeerIO_AddOTPRecvSeed(&peer->pio.udp.pio, seed_id, key, iv);
    
    // remember seed ID so we can confirm it from peer_udp_pio_handler_seed_ready
    peer->pio.udp.pending_recvseed_id = seed_id;
}

void peer_msg_confirmseed (struct peer_data *peer, uint8_t *data, int data_len)
{
    msg_confirmseedParser parser;
    if (!msg_confirmseedParser_Init(&parser, data, data_len)) {
        peer_log(peer, BLOG_WARNING, "msg_confirmseed: failed to parse");
        return;
    }
    
    // read message
    uint16_t seed_id;
    ASSERT_EXECUTE(msg_confirmseedParser_Getseed_id(&parser, &seed_id))
    
    if (options.transport_mode != TRANSPORT_MODE_UDP) {
        peer_log(peer, BLOG_WARNING, "msg_confirmseed: not in UDP mode");
        return;
    }
    
    if (!SPPROTO_HAVE_OTP(sp_params)) {
        peer_log(peer, BLOG_WARNING, "msg_confirmseed: OTPs disabled");
        return;
    }
    
    if (!peer->have_link) {
        peer_log(peer, BLOG_WARNING, "msg_confirmseed: have no link");
        return;
    }
    
    if (!peer->pio.udp.sendseed_sent) {
        peer_log(peer, BLOG_WARNING, "msg_confirmseed: no seed has been sent");
        return;
    }
    
    if (seed_id != peer->pio.udp.sendseed_sent_id) {
        peer_log(peer, BLOG_WARNING, "msg_confirmseed: invalid seed: expecting %d, received %d", (int)peer->pio.udp.sendseed_sent_id, (int)seed_id);
        return;
    }
    
    peer_log(peer, BLOG_DEBUG, "OTP send seed confirmed");
    
    // no longer waiting for confirmation
    peer->pio.udp.sendseed_sent = 0;
    
    // start using the seed
    DatagramPeerIO_SetOTPSendSeed(&peer->pio.udp.pio, peer->pio.udp.sendseed_sent_id, peer->pio.udp.sendseed_sent_key, peer->pio.udp.sendseed_sent_iv);
}

void peer_msg_youretry (struct peer_data *peer, uint8_t *data, int data_len)
{
    if (data_len != 0) {
        peer_log(peer, BLOG_WARNING, "msg_youretry: invalid length");
        return;
    }
    
    if (!peer_am_master(peer)) {
        peer_log(peer, BLOG_WARNING, "msg_youretry: we are not master");
        return;
    }
    
    peer_log(peer, BLOG_NOTICE, "requests reset");
    
    peer_reset(peer);
}

void peer_udp_pio_handler_seed_warning (struct peer_data *peer)
{
    ASSERT(options.transport_mode == TRANSPORT_MODE_UDP)
    ASSERT(SPPROTO_HAVE_OTP(sp_params))
    ASSERT(peer->have_link)
    
    // generate and send a new seed
    if (!peer->pio.udp.sendseed_sent) {
        peer_generate_and_send_seed(peer);
    }
}

void peer_udp_pio_handler_seed_ready (struct peer_data *peer)
{
    ASSERT(options.transport_mode == TRANSPORT_MODE_UDP)
    ASSERT(SPPROTO_HAVE_OTP(sp_params))
    ASSERT(peer->have_link)
    
    // send confirmation
    peer_send_confirmseed(peer, peer->pio.udp.pending_recvseed_id);
}

void peer_udp_pio_handler_error (struct peer_data *peer)
{
    ASSERT(options.transport_mode == TRANSPORT_MODE_UDP)
    ASSERT(peer->have_link)
    
    peer_log(peer, BLOG_NOTICE, "UDP connection failed");
    
    peer_reset(peer);
    return;
}

void peer_tcp_pio_handler_error (struct peer_data *peer)
{
    ASSERT(options.transport_mode == TRANSPORT_MODE_TCP)
    ASSERT(peer->have_link)
    
    peer_log(peer, BLOG_NOTICE, "TCP connection failed");
    
    peer_reset(peer);
    return;
}

void peer_reset_timer_handler (struct peer_data *peer)
{
    ASSERT(peer_am_master(peer))
    
    BLog(BLOG_NOTICE, "retry timer expired");
    
    // start setup process
    peer_start_binding(peer);
}

void peer_start_binding (struct peer_data *peer)
{
    peer->binding = 1;
    peer->binding_addrpos = 0;
    
    peer_bind(peer);
}

void peer_bind (struct peer_data *peer)
{
    ASSERT(peer->binding)
    ASSERT(peer->binding_addrpos >= 0)
    ASSERT(peer->binding_addrpos <= num_bind_addrs)
    
    int res;
    
    while (peer->binding_addrpos < num_bind_addrs) {
        // if there are no external addresses, skip bind address
        if (bind_addrs[peer->binding_addrpos].num_ext_addrs == 0) {
            peer->binding_addrpos++;
            continue;
        }
        
        // try to bind
        int cont;
        peer_bind_one_address(peer, peer->binding_addrpos, &cont);
        
        // increment address counter
        peer->binding_addrpos++;
        
        if (!cont) {
            return;
        }
    }
    
    peer_log(peer, BLOG_NOTICE, "no more addresses to bind to");
    
    // no longer binding
    peer->binding = 0;
    
    // tell the peer we failed to bind
    peer_send_simple(peer, MSGID_CANNOTBIND);
    
    // if we are the slave, setup relaying
    if (!peer_am_master(peer)) {
        if (!peer->is_relay) {
            peer_need_relay(peer);
        }
    }
}

void peer_bind_one_address (struct peer_data *peer, int addr_index, int *cont)
{
    ASSERT(addr_index >= 0)
    ASSERT(addr_index < num_bind_addrs)
    ASSERT(bind_addrs[addr_index].num_ext_addrs > 0)
    
    // get a fresh link
    if (!peer_new_link(peer)) {
        peer_log(peer, BLOG_ERROR, "cannot get link");
        *cont = 0;
        peer_reset(peer);
        return;
    }
    
    if (options.transport_mode == TRANSPORT_MODE_UDP) {
        // get addr
        POINTER(addr, bind_addrs[addr_index]);
        
        // try binding to all ports in the range
        int port_add;
        for (port_add = 0; port_add < addr->num_ports; port_add++) {
            BAddr tryaddr = addr->addr;
            BAddr_SetPort(&tryaddr, hton16(ntoh16(BAddr_GetPort(&tryaddr)) + port_add));
            if (DatagramPeerIO_Bind(&peer->pio.udp.pio, tryaddr)) {
                break;
            }
        }
        if (port_add == addr->num_ports) {
            BLog(BLOG_NOTICE, "failed to bind to any port");
            *cont = 1;
            return;
        }
        
        uint8_t key[BENCRYPTION_MAX_KEY_SIZE];
        
        // generate and set encryption key
        if (SPPROTO_HAVE_ENCRYPTION(sp_params)) {
            BRandom_randomize(key, BEncryption_cipher_key_size(sp_params.encryption_mode));
            DatagramPeerIO_SetEncryptionKey(&peer->pio.udp.pio, key);
        }
        
        // schedule sending OTP seed
        if (SPPROTO_HAVE_OTP(sp_params)) {
            BPending_Set(&peer->job_send_seed_after_binding);
        }
        
        // send connectinfo
        peer_send_conectinfo(peer, addr_index, port_add, key, 0);
    } else {
        // order StreamPeerIO to listen
        uint64_t pass;
        StreamPeerIO_Listen(&peer->pio.tcp.pio, &listeners[addr_index], &pass);
        
        // send connectinfo
        peer_send_conectinfo(peer, addr_index, 0, NULL, pass);
    }
    
    peer_log(peer, BLOG_NOTICE, "bound to address number %d", addr_index);
    
    *cont = 0;
}

void peer_connect (struct peer_data *peer, BAddr addr, uint8_t* encryption_key, uint64_t password)
{
    ASSERT(!BAddr_IsInvalid(&addr))
    
    // get a fresh link
    if (!peer_new_link(peer)) {
        peer_log(peer, BLOG_ERROR, "cannot get link");
        peer_reset(peer);
        return;
    }
    
    if (options.transport_mode == TRANSPORT_MODE_UDP) {
        // order DatagramPeerIO to connect
        if (!DatagramPeerIO_Connect(&peer->pio.udp.pio, addr)) {
            peer_log(peer, BLOG_NOTICE, "DatagramPeerIO_Connect failed");
            return peer_reset(peer);
        }
        
        // set encryption key
        if (SPPROTO_HAVE_ENCRYPTION(sp_params)) {
            DatagramPeerIO_SetEncryptionKey(&peer->pio.udp.pio, encryption_key);
        }
        
        // generate and send a send seed
        if (SPPROTO_HAVE_OTP(sp_params)) {
            peer_generate_and_send_seed(peer);
        }
    } else {
        // order StreamPeerIO to connect
        if (!StreamPeerIO_Connect(
            &peer->pio.tcp.pio, addr, password,
            (options.peer_ssl ? client_cert : NULL),
            (options.peer_ssl ? client_key : NULL)
        )) {
            peer_log(peer, BLOG_NOTICE, "StreamPeerIO_Connect failed");
            return peer_reset(peer);
        }
    }
}

static int peer_start_msg (struct peer_data *peer, void **data, int type, int len)
{
    ASSERT(len >= 0)
    ASSERT(len <= MSG_MAX_PAYLOAD)
    ASSERT(!(len > 0) || data)
    ASSERT(peer->chat_send_msg_len == -1)
    
    // obtain buffer location
    uint8_t *packet;
    if (!BufferWriter_StartPacket(&peer->chat_send_writer, &packet)) {
        BLog(BLOG_ERROR, "out of peer server buffer, exiting");
        terminate();
        return -1;
    }
    
    // write fields
    msgWriter writer;
    msgWriter_Init(&writer, packet);
    msgWriter_Addtype(&writer, type);
    uint8_t *payload_dst = msgWriter_Addpayload(&writer, len);
    msgWriter_Finish(&writer);
    
    // set have message
    peer->chat_send_msg_len = len;
    
    if (data) {
        *data = payload_dst;
    }
    return 0;
}

static void peer_end_msg (struct peer_data *peer)
{
    ASSERT(peer->chat_send_msg_len >= 0)
    
    // submit packet to buffer
    BufferWriter_EndPacket(&peer->chat_send_writer, msg_SIZEtype + msg_SIZEpayload(peer->chat_send_msg_len));
    
    // set no message
    peer->chat_send_msg_len = -1;
}

void peer_send_simple (struct peer_data *peer, int msgid)
{
    if (peer_start_msg(peer, NULL, msgid, 0) < 0) {
        return;
    }
    peer_end_msg(peer);
}

void peer_send_conectinfo (struct peer_data *peer, int addr_index, int port_adjust, uint8_t *enckey, uint64_t pass)
{
    ASSERT(addr_index >= 0)
    ASSERT(addr_index < num_bind_addrs)
    ASSERT(bind_addrs[addr_index].num_ext_addrs > 0)
    
    // get address
    POINTER(bind_addr, bind_addrs[addr_index]);
    
    // remember encryption key size
    int key_size;
    if (options.transport_mode == TRANSPORT_MODE_UDP && SPPROTO_HAVE_ENCRYPTION(sp_params)) {
        key_size = BEncryption_cipher_key_size(sp_params.encryption_mode);
    }
    
    // calculate message length ..
    int msg_len = 0;
    
    // addresses
    for (int i = 0; i < bind_addr->num_ext_addrs; i++) {
        int addrmsg_len =
            msg_youconnect_addr_SIZEname(strlen(bind_addr->ext_addrs[i].scope)) +
            msg_youconnect_addr_SIZEaddr(addr_size(bind_addr->ext_addrs[i].addr));
        msg_len += msg_youconnect_SIZEaddr(addrmsg_len);
    }
    
    // encryption key
    if (options.transport_mode == TRANSPORT_MODE_UDP && SPPROTO_HAVE_ENCRYPTION(sp_params)) {
        msg_len += msg_youconnect_SIZEkey(key_size);
    }
    
    // password
    if (options.transport_mode == TRANSPORT_MODE_TCP) {
        msg_len += msg_youconnect_SIZEpassword;
    }
    
    // check if it's too big (because of the addresses)
    if (msg_len > MSG_MAX_PAYLOAD) {
        BLog(BLOG_ERROR, "cannot send too big youconnect message");
        return;
    }
        
    // start message
    uint8_t *msg;
    if (peer_start_msg(peer, (void **)&msg, MSGID_YOUCONNECT, msg_len) < 0) {
        return;
    }
        
    // init writer
    msg_youconnectWriter writer;
    msg_youconnectWriter_Init(&writer, msg);
        
    // write addresses
    for (int i = 0; i < bind_addr->num_ext_addrs; i++) {
        int name_len = strlen(bind_addr->ext_addrs[i].scope);
        int addr_len = addr_size(bind_addr->ext_addrs[i].addr);
        
        // get a pointer for writing the address
        int addrmsg_len =
            msg_youconnect_addr_SIZEname(name_len) +
            msg_youconnect_addr_SIZEaddr(addr_len);
        uint8_t *addrmsg_dst = msg_youconnectWriter_Addaddr(&writer, addrmsg_len);
        
        // init address writer
        msg_youconnect_addrWriter awriter;
        msg_youconnect_addrWriter_Init(&awriter, addrmsg_dst);
        
        // write scope
        uint8_t *name_dst = msg_youconnect_addrWriter_Addname(&awriter, name_len);
        memcpy(name_dst, bind_addr->ext_addrs[i].scope, name_len);
        
        // write address with adjusted port
        BAddr addr = bind_addr->ext_addrs[i].addr;
        BAddr_SetPort(&addr, hton16(ntoh16(BAddr_GetPort(&addr)) + port_adjust));
        uint8_t *addr_dst = msg_youconnect_addrWriter_Addaddr(&awriter, addr_len);
        addr_write(addr_dst, addr);
        
        // finish address writer
        msg_youconnect_addrWriter_Finish(&awriter);
    }
    
    // write encryption key
    if (options.transport_mode == TRANSPORT_MODE_UDP && SPPROTO_HAVE_ENCRYPTION(sp_params)) {
        uint8_t *key_dst = msg_youconnectWriter_Addkey(&writer, key_size);
        memcpy(key_dst, enckey, key_size);
    }
    
    // write password
    if (options.transport_mode == TRANSPORT_MODE_TCP) {
        msg_youconnectWriter_Addpassword(&writer, pass);
    }
    
    // finish writer
    msg_youconnectWriter_Finish(&writer);
    
    // end message
    peer_end_msg(peer);
}

void peer_generate_and_send_seed (struct peer_data *peer)
{
    ASSERT(options.transport_mode == TRANSPORT_MODE_UDP)
    ASSERT(SPPROTO_HAVE_OTP(sp_params))
    ASSERT(peer->have_link)
    ASSERT(!peer->pio.udp.sendseed_sent)
    
    peer_log(peer, BLOG_DEBUG, "sending OTP send seed");
    
    int key_len = BEncryption_cipher_key_size(sp_params.otp_mode);
    int iv_len = BEncryption_cipher_block_size(sp_params.otp_mode);
    
    // generate seed
    peer->pio.udp.sendseed_sent_id = peer->pio.udp.sendseed_nextid;
    BRandom_randomize(peer->pio.udp.sendseed_sent_key, key_len);
    BRandom_randomize(peer->pio.udp.sendseed_sent_iv, iv_len);
    
    // set as sent, increment next seed ID
    peer->pio.udp.sendseed_sent = 1;
    peer->pio.udp.sendseed_nextid++;
    
    // send seed to the peer
    int msg_len = msg_seed_SIZEseed_id + msg_seed_SIZEkey(key_len) + msg_seed_SIZEiv(iv_len);
    uint8_t *msg;
    if (peer_start_msg(peer, (void **)&msg, MSGID_SEED, msg_len) < 0) {
        return;
    }
    msg_seedWriter writer;
    msg_seedWriter_Init(&writer, msg);
    msg_seedWriter_Addseed_id(&writer, peer->pio.udp.sendseed_sent_id);
    uint8_t *key_dst = msg_seedWriter_Addkey(&writer, key_len);
    memcpy(key_dst, peer->pio.udp.sendseed_sent_key, key_len);
    uint8_t *iv_dst = msg_seedWriter_Addiv(&writer, iv_len);
    memcpy(iv_dst, peer->pio.udp.sendseed_sent_iv, iv_len);
    msg_seedWriter_Finish(&writer);
    peer_end_msg(peer);
}

void peer_send_confirmseed (struct peer_data *peer, uint16_t seed_id)
{
    ASSERT(options.transport_mode == TRANSPORT_MODE_UDP)
    ASSERT(SPPROTO_HAVE_OTP(sp_params))
    
    // send confirmation
    int msg_len = msg_confirmseed_SIZEseed_id;
    uint8_t *msg;
    if (peer_start_msg(peer, (void **)&msg, MSGID_CONFIRMSEED, msg_len) < 0) {
        return;
    }
    msg_confirmseedWriter writer;
    msg_confirmseedWriter_Init(&writer, msg);
    msg_confirmseedWriter_Addseed_id(&writer, seed_id);
    msg_confirmseedWriter_Finish(&writer);
    peer_end_msg(peer);
}

void peer_dataproto_handler (struct peer_data *peer, int up)
{
    ASSERT(peer->have_link)
    
    if (up) {
        peer_log(peer, BLOG_INFO, "up");
        
        // if it can be a relay provided, enable it
        if ((peer->flags&SCID_NEWCLIENT_FLAG_RELAY_SERVER) && !peer->is_relay) {
            peer_enable_relay_provider(peer);
        }
    } else {
        peer_log(peer, BLOG_INFO, "down");
        
        // if it is a relay provider, disable it
        if (peer->is_relay) {
            peer_disable_relay_provider(peer);
        }
    }
}

struct peer_data * find_peer_by_id (peerid_t id)
{
    LinkedList2Node *node = LinkedList2_GetFirst(&peers);
    while (node) {
        struct peer_data *peer = UPPER_OBJECT(node, struct peer_data, list_node);
        if (peer->id == id) {
            return peer;
        }
        node = LinkedList2Node_Next(node);
    }
    
    return NULL;
}

void device_error_handler (void *unused)
{
    BLog(BLOG_ERROR, "device error");
    
    terminate();
    return;
}

void device_input_dpd_handler (void *unused, const uint8_t *frame, int frame_len)
{
    ASSERT(frame_len >= 0)
    
    // give frame to decider
    FrameDecider_AnalyzeAndDecide(&frame_decider, frame, frame_len);
    
    // forward frame to peers
    FrameDeciderPeer *decider_peer = FrameDecider_NextDestination(&frame_decider);
    while (decider_peer) {
        FrameDeciderPeer *next = FrameDecider_NextDestination(&frame_decider);
        struct peer_data *peer = UPPER_OBJECT(decider_peer, struct peer_data, decider_peer);
        DataProtoFlow_Route(&peer->local_dpflow, !!next);
        decider_peer = next;
    }
}

void assign_relays (void)
{
    LinkedList2Node *list_node;
    while (list_node = LinkedList2_GetFirst(&waiting_relay_peers)) {
        struct peer_data *peer = UPPER_OBJECT(list_node, struct peer_data, waiting_relay_list_node);
        ASSERT(peer->waiting_relay)
        
        ASSERT(!peer->relaying_peer)
        ASSERT(!peer->have_link)
        
        // get a relay
        LinkedList2Node *list_node2 = LinkedList2_GetFirst(&relays);
        if (!list_node2) {
            BLog(BLOG_NOTICE, "no relays");
            return;
        }
        struct peer_data *relay = UPPER_OBJECT(list_node2, struct peer_data, relay_list_node);
        ASSERT(relay->is_relay)
        
        // no longer waiting for relay
        peer_unregister_need_relay(peer);
        
        // install the relay
        peer_install_relaying(peer, relay);
    }
}

char * address_scope_known (uint8_t *name, int name_len)
{
    ASSERT(name_len >= 0)
    
    for (int i = 0; i < options.num_scopes; i++) {
        if (name_len == strlen(options.scopes[i]) && !memcmp(name, options.scopes[i], name_len)) {
            return options.scopes[i];
        }
    }
    
    return NULL;
}

void server_handler_error (void *user)
{
    BLog(BLOG_ERROR, "server connection failed, exiting");
    
    terminate();
    return;
}

void server_handler_ready (void *user, peerid_t param_my_id, uint32_t ext_ip)
{
    ASSERT(!server_ready)
    
    // remember our ID
    my_id = param_my_id;
    
    // store server reported addresses
    for (int i = 0; i < num_bind_addrs; i++) {
        POINTER(addr, bind_addrs[i]);
        for (int j = 0; j < addr->num_ext_addrs; j++) {
            POINTER(eaddr, addr->ext_addrs[j]);
            if (eaddr->server_reported_port >= 0) {
                if (ext_ip == 0) {
                    BLog(BLOG_ERROR, "server did not provide our address");
                    terminate();
                    return;
                }
                BAddr_InitIPv4(&eaddr->addr, ext_ip, hton16(eaddr->server_reported_port));
                char str[BADDR_MAX_PRINT_LEN];
                BAddr_Print(&eaddr->addr, str);
                BLog(BLOG_INFO, "external address (%d,%d): server reported %s", i, j, str);
            }
        }
    }
    
    // give receive device the ID
    DPReceiveDevice_SetPeerID(&device_output_dprd, my_id);
    
    // init server queue
    PacketPassFairQueue_Init(&server_queue, ServerConnection_GetSendInterface(&server), BReactor_PendingGroup(&ss), 0, 1);
    
    // set server ready
    server_ready = 1;
    
    BLog(BLOG_INFO, "server: ready, my ID is %d", (int)my_id);
}

void server_handler_newclient (void *user, peerid_t peer_id, int flags, const uint8_t *cert, int cert_len)
{
    ASSERT(ServerConnection_IsReady(&server))
    ASSERT(cert_len >= 0)
    ASSERT(cert_len <= SCID_NEWCLIENT_MAX_CERT_LEN)
    
    // check if the peer already exists
    if (find_peer_by_id(peer_id)) {
        BLog(BLOG_WARNING, "server: newclient: peer already known");
        return;
    }
    
    // make sure it's not the same ID as us
    if (peer_id == my_id) {
        BLog(BLOG_WARNING, "server: newclient: peer has our ID");
        return;
    }
    
    // check if there is spece for the peer
    if (num_peers >= MAX_PEERS) {
        BLog(BLOG_WARNING, "server: newclient: no space for new peer (maximum number reached)");
        return;
    }
    
    if (!options.ssl && cert_len > 0) {
        BLog(BLOG_WARNING, "server: newclient: certificate supplied, but not using TLS");
        return;
    }
    
    peer_add(peer_id, flags, cert, cert_len);
}

void server_handler_endclient (void *user, peerid_t peer_id)
{
    ASSERT(ServerConnection_IsReady(&server))
    
    // find peer
    struct peer_data *peer = find_peer_by_id(peer_id);
    if (!peer) {
        BLog(BLOG_WARNING, "server: endclient: peer %d not known", (int)peer_id);
        return;
    }
    
    // remove peer
    peer_remove(peer, 0);
}

void server_handler_message (void *user, peerid_t peer_id, uint8_t *data, int data_len)
{
    ASSERT(ServerConnection_IsReady(&server))
    ASSERT(data_len >= 0)
    ASSERT(data_len <= SC_MAX_MSGLEN)
    
    // find peer
    struct peer_data *peer = find_peer_by_id(peer_id);
    if (!peer) {
        BLog(BLOG_WARNING, "server: message: peer not known");
        return;
    }
    
    // pass message to chat
    PeerChat_InputReceived(&peer->chat, data, data_len);
}

void peer_job_send_seed_after_binding (struct peer_data *peer)
{
    ASSERT(options.transport_mode == TRANSPORT_MODE_UDP)
    ASSERT(SPPROTO_HAVE_OTP(sp_params))
    ASSERT(peer->have_link)
    ASSERT(!peer->pio.udp.sendseed_sent)
    
    peer_generate_and_send_seed(peer);
}

void peer_job_init (struct peer_data *peer)
{
    // start setup process
    if (peer_am_master(peer)) {
        peer_start_binding(peer);
    }
}

struct server_flow * server_flow_init (peerid_t peer_id, PacketRecvInterface *input)
{
    ASSERT(server_ready)
    
    // allocate structure
    struct server_flow *flow = malloc(sizeof(*flow));
    if (!flow) {
        BLog(BLOG_ERROR, "malloc failed");
        goto fail0;
    }
    
    // init queue flow
    PacketPassFairQueueFlow_Init(&flow->qflow, &server_queue);
    
    // init connector
    PacketRecvConnector_Init(&flow->connector, sizeof(struct packetproto_header) + SC_MAX_ENC, BReactor_PendingGroup(&ss));
    
    // init encoder buffer
    if (!SinglePacketBuffer_Init(&flow->encoder_buffer, PacketRecvConnector_GetOutput(&flow->connector), PacketPassFairQueueFlow_GetInput(&flow->qflow), BReactor_PendingGroup(&ss))) {
        BLog(BLOG_ERROR, "SinglePacketBuffer_Init failed");
        goto fail1;
    }
    
    // connect input
    PacketRecvConnector_ConnectInput(&flow->connector, input);
    
    return flow;
    
fail1:
    PacketRecvConnector_Free(&flow->connector);
    PacketPassFairQueueFlow_Free(&flow->qflow);
    free(flow);
fail0:
    return NULL;
}

void server_flow_free (struct server_flow *flow)
{
    PacketPassFairQueueFlow_AssertFree(&flow->qflow);
    
    // remove dying flow reference
    if (flow == dying_server_flow) {
        dying_server_flow = NULL;
    }
    
    // free encoder buffer
    SinglePacketBuffer_Free(&flow->encoder_buffer);
    
    // free connector
    PacketRecvConnector_Free(&flow->connector);
    
    // free queue flow
    PacketPassFairQueueFlow_Free(&flow->qflow);
    
    // free structure
    free(flow);
}

void server_flow_die (struct server_flow *flow)
{
    ASSERT(PacketPassFairQueueFlow_IsBusy(&flow->qflow))
    ASSERT(!dying_server_flow)
    
    // disconnect input
    PacketRecvConnector_DisconnectInput(&flow->connector);
    
    // request notification when flow is done
    PacketPassFairQueueFlow_SetBusyHandler(&flow->qflow, (PacketPassFairQueue_handler_busy)server_flow_qflow_handler_busy, flow);
    
    // set dying flow
    dying_server_flow = flow;
}

void server_flow_qflow_handler_busy (struct server_flow *flow)
{
    ASSERT(flow == dying_server_flow)
    PacketPassFairQueueFlow_AssertFree(&flow->qflow);
    
    // finally free flow
    server_flow_free(flow);
}
