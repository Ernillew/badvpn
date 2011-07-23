/**
 * @file PasswordListener.c
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

#include <stdlib.h>

#include <prerror.h>

#include <ssl.h>

#include <misc/offset.h>
#include <misc/byteorder.h>
#include <misc/balloc.h>
#include <base/BLog.h>
#include <security/BRandom.h>
#include <nspr_support/DummyPRFileDesc.h>

#include <client/PasswordListener.h>

#include <generated/blog_channel_PasswordListener.h>

static int password_comparator (void *user, uint64_t *p1, uint64_t *p2);
static void remove_client (struct PasswordListenerClient *client);
static void listener_handler (PasswordListener *l);
static void client_connection_handler (struct PasswordListenerClient *client, int event);
static void client_sslcon_handler (struct PasswordListenerClient *client, int event);
static void client_receiver_handler (struct PasswordListenerClient *client);

int password_comparator (void *user, uint64_t *p1, uint64_t *p2)
{
    if (*p1 < *p2) {
        return -1;
    }
    if (*p1 > *p2) {
        return 1;
    }
    return 0;
}

void remove_client (struct PasswordListenerClient *client)
{
    PasswordListener *l = client->l;
    
    // free receiver
    SingleStreamReceiver_Free(&client->receiver);
    
    // free SSL
    if (l->ssl) {
        BSSLConnection_Free(&client->sslcon);
        ASSERT_FORCE(PR_Close(client->sock->ssl_prfd) == PR_SUCCESS)
    }
    
    // free connection interfaces
    BConnection_RecvAsync_Free(&client->sock->con);
    BConnection_SendAsync_Free(&client->sock->con);
    
    // free connection
    BConnection_Free(&client->sock->con);
    
    // free sslsocket structure
    free(client->sock);
    
    // move to free list
    LinkedList2_Remove(&l->clients_used, &client->list_node);
    LinkedList2_Append(&l->clients_free, &client->list_node);
}

void listener_handler (PasswordListener *l)
{
    DebugObject_Access(&l->d_obj);
    
    // obtain client entry
    if (LinkedList2_IsEmpty(&l->clients_free)) {
        struct PasswordListenerClient *client = UPPER_OBJECT(LinkedList2_GetFirst(&l->clients_used), struct PasswordListenerClient, list_node);
        remove_client(client);
    }
    struct PasswordListenerClient *client = UPPER_OBJECT(LinkedList2_GetLast(&l->clients_free), struct PasswordListenerClient, list_node);
    LinkedList2_Remove(&l->clients_free, &client->list_node);
    LinkedList2_Append(&l->clients_used, &client->list_node);
    
    // allocate sslsocket structure
    if (!(client->sock = malloc(sizeof(*client->sock)))) {
        BLog(BLOG_ERROR, "malloc failedt");
        goto fail0;
    }
    
    // accept connection
    if (!BConnection_Init(&client->sock->con, BCONNECTION_SOURCE_LISTENER(&l->listener, NULL), l->bsys, client, (BConnection_handler)client_connection_handler)) {
        BLog(BLOG_ERROR, "BConnection_Init failed");
        goto fail1;
    }
    
    BLog(BLOG_INFO, "Connection accepted");
    
    // init connection interfaces
    BConnection_SendAsync_Init(&client->sock->con);
    BConnection_RecvAsync_Init(&client->sock->con);
    
    StreamPassInterface *send_if = BConnection_SendAsync_GetIf(&client->sock->con);
    StreamRecvInterface *recv_if = BConnection_RecvAsync_GetIf(&client->sock->con);
    
    if (l->ssl) {
        // create bottom NSPR file descriptor
        if (!BSSLConnection_MakeBackend(&client->sock->bottom_prfd, send_if, recv_if)) {
            BLog(BLOG_ERROR, "BSSLConnection_MakeBackend failed");
            goto fail2;
        }
        
        // create SSL file descriptor from the bottom NSPR file descriptor
        if (!(client->sock->ssl_prfd = SSL_ImportFD(l->model_prfd, &client->sock->bottom_prfd))) {
            ASSERT_FORCE(PR_Close(&client->sock->bottom_prfd) == PR_SUCCESS)
            goto fail2;
        }
        
        // set server mode
        if (SSL_ResetHandshake(client->sock->ssl_prfd, PR_TRUE) != SECSuccess) {
            BLog(BLOG_ERROR, "SSL_ResetHandshake failed");
            goto fail3;
        }
        
        // set require client certificate
        if (SSL_OptionSet(client->sock->ssl_prfd, SSL_REQUEST_CERTIFICATE, PR_TRUE) != SECSuccess) {
            BLog(BLOG_ERROR, "SSL_OptionSet(SSL_REQUEST_CERTIFICATE) failed");
            goto fail3;
        }
        if (SSL_OptionSet(client->sock->ssl_prfd, SSL_REQUIRE_CERTIFICATE, PR_TRUE) != SECSuccess) {
            BLog(BLOG_ERROR, "SSL_OptionSet(SSL_REQUIRE_CERTIFICATE) failed");
            goto fail3;
        }
        
        // initialize SSLConnection
        BSSLConnection_Init(&client->sslcon, client->sock->ssl_prfd, 0, BReactor_PendingGroup(l->bsys), client, (BSSLConnection_handler)client_sslcon_handler);
        
        send_if = BSSLConnection_GetSendIf(&client->sslcon);
        recv_if = BSSLConnection_GetRecvIf(&client->sslcon);
    }
    
    // init receiver
    SingleStreamReceiver_Init(&client->receiver, (uint8_t *)&client->recv_buffer, sizeof(client->recv_buffer), recv_if, BReactor_PendingGroup(l->bsys), client, (SingleStreamReceiver_handler)client_receiver_handler);
    
    return;
    
    // cleanup on error
fail3:
    if (l->ssl) {
        ASSERT_FORCE(PR_Close(client->sock->ssl_prfd) == PR_SUCCESS)
    }
fail2:
    BConnection_RecvAsync_Free(&client->sock->con);
    BConnection_SendAsync_Free(&client->sock->con);
    BConnection_Free(&client->sock->con);
fail1:
    free(client->sock);
fail0:
    LinkedList2_Remove(&l->clients_used, &client->list_node);
    LinkedList2_Append(&l->clients_free, &client->list_node);
}

void client_connection_handler (struct PasswordListenerClient *client, int event)
{
    PasswordListener *l = client->l;
    DebugObject_Access(&l->d_obj);
    
    if (event == BCONNECTION_EVENT_RECVCLOSED) {
        BLog(BLOG_INFO, "connection closed");
    } else {
        BLog(BLOG_INFO, "connection error");
    }
    
    remove_client(client);
}

void client_sslcon_handler (struct PasswordListenerClient *client, int event)
{
    PasswordListener *l = client->l;
    DebugObject_Access(&l->d_obj);
    ASSERT(l->ssl)
    ASSERT(event == BSSLCONNECTION_EVENT_ERROR)
    
    BLog(BLOG_INFO, "SSL error");
    
    remove_client(client);
}

void client_receiver_handler (struct PasswordListenerClient *client)
{
    PasswordListener *l = client->l;
    DebugObject_Access(&l->d_obj);
    
    // check password
    uint64_t received_pass = ltoh64(client->recv_buffer);
    BAVLNode *pw_tree_node = BAVL_LookupExact(&l->passwords, &received_pass);
    if (!pw_tree_node) {
        BLog(BLOG_WARNING, "unknown password");
        remove_client(client);
        return;
    }
    PasswordListener_pwentry *pw_entry = UPPER_OBJECT(pw_tree_node, PasswordListener_pwentry, tree_node);
    
    BLog(BLOG_INFO, "Password recognized");
    
    // remove password entry
    BAVL_Remove(&l->passwords, &pw_entry->tree_node);
    
    // free receiver
    SingleStreamReceiver_Free(&client->receiver);
    
    if (l->ssl) {
        // free SSL connection
        BSSLConnection_Free(&client->sslcon);
    } else {
        // free connection interfaces
        BConnection_RecvAsync_Free(&client->sock->con);
        BConnection_SendAsync_Free(&client->sock->con);
    }
    
    // remove connection handler
    BConnection_SetHandlers(&client->sock->con, NULL, NULL);
    
    // move client entry to free list
    LinkedList2_Remove(&l->clients_used, &client->list_node);
    LinkedList2_Append(&l->clients_free, &client->list_node);
    
    // give the socket to the handler
    pw_entry->handler_client(pw_entry->user, client->sock);
    return;
}

int PasswordListener_Init (PasswordListener *l, BReactor *bsys, BAddr listen_addr, int max_clients, int ssl, CERTCertificate *cert, SECKEYPrivateKey *key)
{
    ASSERT(BConnection_AddressSupported(listen_addr))
    ASSERT(max_clients > 0)
    ASSERT(ssl == 0 || ssl == 1)
    
    // init arguments
    l->bsys = bsys;
    l->ssl = ssl;
    
    // allocate client entries
    if (!(l->clients_data = BAllocArray(max_clients, sizeof(struct PasswordListenerClient)))) {
        BLog(BLOG_ERROR, "BAllocArray failed");
        goto fail0;
    }
    
    if (l->ssl) {
        // initialize model SSL fd
        DummyPRFileDesc_Create(&l->model_dprfd);
        if (!(l->model_prfd = SSL_ImportFD(NULL, &l->model_dprfd))) {
            BLog(BLOG_ERROR, "SSL_ImportFD failed");
            ASSERT_FORCE(PR_Close(&l->model_dprfd) == PR_SUCCESS)
            goto fail1;
        }
        
        // set server certificate
        if (SSL_ConfigSecureServer(l->model_prfd, cert, key, NSS_FindCertKEAType(cert)) != SECSuccess) {
            BLog(BLOG_ERROR, "SSL_ConfigSecureServer failed");
            goto fail2;
        }
    }
    
    // initialize client entries
    LinkedList2_Init(&l->clients_free);
    LinkedList2_Init(&l->clients_used);
    for (int i = 0; i < max_clients; i++) {
        struct PasswordListenerClient *conn = &l->clients_data[i];
        conn->l = l;
        LinkedList2_Append(&l->clients_free, &conn->list_node);
    }
    
    // initialize passwords tree
    BAVL_Init(&l->passwords, OFFSET_DIFF(PasswordListener_pwentry, password, tree_node), (BAVL_comparator)password_comparator, NULL);
    
    // initialize listener
    if (!BListener_Init(&l->listener, listen_addr,  l->bsys, l, (BListener_handler)listener_handler)) {
        BLog(BLOG_ERROR, "Listener_Init failed");
        goto fail2;
    }
    
    DebugObject_Init(&l->d_obj);
    return 1;
    
    // cleanup
fail2:
    if (l->ssl) {
        ASSERT_FORCE(PR_Close(l->model_prfd) == PR_SUCCESS)
    }
fail1:
    BFree(l->clients_data);
fail0:
    return 0;
}

void PasswordListener_Free (PasswordListener *l)
{
    DebugObject_Free(&l->d_obj);

    // free clients
    LinkedList2Node *node;
    while (node = LinkedList2_GetFirst(&l->clients_used)) {
        struct PasswordListenerClient *client = UPPER_OBJECT(node, struct PasswordListenerClient, list_node);
        remove_client(client);
    }
    
    // free listener
    BListener_Free(&l->listener);
    
    // free model SSL file descriptor
    if (l->ssl) {
        ASSERT_FORCE(PR_Close(l->model_prfd) == PR_SUCCESS)
    }
    
    // free client entries
    BFree(l->clients_data);
}

uint64_t PasswordListener_AddEntry (PasswordListener *l, PasswordListener_pwentry *entry, PasswordListener_handler_client handler_client, void *user)
{
    DebugObject_Access(&l->d_obj);
    
    while (1) {
        // generate password
        BRandom_randomize((uint8_t *)&entry->password, sizeof(entry->password));
        
        // try inserting
        if (BAVL_Insert(&l->passwords, &entry->tree_node, NULL)) {
            break;
        }
    }
    
    entry->handler_client = handler_client;
    entry->user = user;
    
    return entry->password;
}

void PasswordListener_RemoveEntry (PasswordListener *l, PasswordListener_pwentry *entry)
{
    DebugObject_Access(&l->d_obj);
    
    // remove
    BAVL_Remove(&l->passwords, &entry->tree_node);
}
