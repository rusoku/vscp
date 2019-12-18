// websocketserver.cpp
//
// This file is part of the VSCP (http://www.vscp.org)
//
// The MIT License (MIT)
//
// Copyright (C) 2000-2019 Ake Hedman, Grodans Paradis AB
// <info@grodansparadis.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#ifdef __GNUG__
//#pragma implementation
#endif

#define _POSIX

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/msg.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include "web_css.h"
#include "web_js.h"
#include "web_template.h"

#include <civetweb.h>
#include <expat.h>
#include <json.hpp> // Needs C++11  -std=c++11

#include <actioncodes.h>
#include <controlobject.h>
#include <devicelist.h>
#include <mdf.h>
#include <remotevariablecodes.h>
#include <version.h>
#include <vscp.h>
#include <vscp_aes.h>
#include <vscphelper.h>
#include <websrv.h>

#include "websocket.h"

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#define XML_BUFF_SIZE 0xffff

// for convenience
using json = nlohmann::json;

///////////////////////////////////////////////////
//                 GLOBALS
///////////////////////////////////////////////////

extern CControlObject *gpobj;

// Webserver
extern struct mg_mgr gmgr;

// Linked list of all active sessions. (webserv.h)
extern struct websrv_Session *gp_websrv_sessions;

// Session structure for REST API
extern struct websrv_rest_session *gp_websrv_rest_sessions;

// Prototypes
int
webserv_url_decode(const char *src,
                   int src_len,
                   char *dst,
                   int dst_len,
                   int is_form_url_encoded);

void
webserv_util_sendheader(struct mg_connection *nc,
                        const int returncode,
                        const char *content);

////////////////////////////////////////////////////
//            Forward declarations
////////////////////////////////////////////////////

void
ws1_command(struct mg_connection *conn,
            struct websock_session *pSession,
            std::string &strCmd);

bool
ws1_message(struct mg_connection *conn,
            websock_session *pSession,
            std::string &strWsPkt);

bool
ws2_command(struct mg_connection *conn,
            struct websock_session *pSession,
            json &obj);

bool
ws2_message(struct mg_connection *conn,
            websock_session *pSession,
            std::string &strWsPkt);

///////////////////////////////////////////////////
//                 WEBSOCKETS
///////////////////////////////////////////////////

// Linked list of websocket sessions
// Protected by the websocketSexxionMutex
// static struct websock_session *gp_websock_sessions;

websock_session::websock_session(void)
{
    m_wstypes = WS_TYPE_1;  // ws1
    m_conn       = NULL;
    m_conn_state = WEBSOCK_CONN_STATE_NULL;
    memset(m_websocket_key, 0, 33);
    memset(m_sid, 0, 33);
    m_version        = 0;
    lastActiveTime   = 0;
    m_pClientItem    = NULL;
    bEventTrigger    = false;
    triggerTimeout   = 0;
    bVariableTrigger = false;
};

websock_session::~websock_session(void){

};

// w2msg - Message holder for W2

w2msg::w2msg(void)
{
    m_type = MSG_TYPE_COMMAND;
    memset(&m_ex, 0., sizeof(vscpEventEx));
};

w2msg::~w2msg(void) {}

///////////////////////////////////////////////////////////////////////////////
// websock_authentication
//
// client sends
//      "AUTH;iv;AES128("username:password)
//

bool
websock_authentication(struct mg_connection *conn,
                       struct websock_session *pSession,
                       std::string &strIV,
                       std::string &strCrypto)
{
    uint8_t buf[2048], secret[2048];
    uint8_t iv[16];
    std::string strUser, strPassword;

    struct mg_context *ctx;
    const struct mg_request_info *reqinfo;
    bool bValidHost = false;

    // Check pointers
    if ((NULL == conn) || (NULL == pSession) || !(ctx = mg_get_context(conn)) ||
        !(reqinfo = mg_get_request_info(conn))) {
        syslog(LOG_ERR,
               "[Websocket Client] Authentication: Invalid "
               "pointers. ");
        return false;
    }

    if (0 == vscp_hexStr2ByteArray(iv, 16, (const char *)strIV.c_str())) {
        syslog(LOG_ERR,
               "[Websocket Client] Authentication: No room "
               "for iv block. ");
        return false; // Not enough room in buffer
    }

    size_t len;
    if (0 == (len = vscp_hexStr2ByteArray(secret,
                                           strCrypto.length(),
                                           (const char *)strCrypto.c_str()))) {
        syslog(LOG_ERR,
               "[Websocket Client] Authentication: No room "
               "for crypto block. ");
        return false; // Not enough room in buffer
    }

    memset(buf, 0, sizeof(buf));
    AES_CBC_decrypt_buffer(AES128, buf, secret, len, gpobj->m_systemKey, iv);

    std::string str = std::string((const char *)buf);
    std::deque<std::string> tokens;
    vscp_split(tokens, str, ":");
    // std::stringTokenizer tkz( str, (":"), xxTOKEN_RET_EMPTY_ALL );  // TODO
    // ?????

    // Get username
    if (tokens.empty()) {
        syslog(LOG_ERR,
               "[Websocket Client] Authentication: Missing "
               "username from client. ");
        return false; // No username
    }

    strUser = tokens.front();
    tokens.pop_front();
    vscp_trim(strUser);

    // Get password
    if (tokens.empty()) {
        syslog(LOG_ERR,
               "[Websocket Client] Authentication: Missing "
               "password from client. ");
        return false; // No username
    }

    strPassword = tokens.front();
    tokens.pop_front();
    vscp_trim(strPassword);

    // Check if user is valid
    CUserItem *pUserItem = gpobj->m_userList.getUser(strUser);
    if (NULL == pUserItem) {
        syslog(LOG_ERR,
               "[Websocket Client] Authentication: CUserItem "
               "allocation problem ");
        return false;
    }

    // Check if remote ip is valid
    bValidHost = pUserItem->isAllowedToConnect(inet_addr(reqinfo->remote_addr));

    if (!bValidHost) {
        // Log valid login
        syslog(LOG_ERR,
               "[Websocket Client] Authentication: Host "
               "[%s] NOT allowed to connect.",
               reqinfo->remote_addr);
        return false;
    }

    if (!vscp_isPasswordValid(pUserItem->getPassword(), strPassword)) {
        syslog(LOG_ERR,
               "[Websocket Client] Authentication: User %s at host "
               "[%s] gave wrong password.",
               (const char *)strUser.c_str(),
               reqinfo->remote_addr);
        return false;
    }

    pSession->m_pClientItem->bAuthenticated = true;

    // Add user to client
    pSession->m_pClientItem->m_pUserItem = pUserItem;

    // Copy in the user filter
    memcpy(&pSession->m_pClientItem->m_filter,
           pUserItem->getUserFilter(),
           sizeof(vscpEventFilter));

    // Log valid login
    syslog(LOG_ERR,
           "[Websocket Client] Authentication: Host [%s] "
           "User [%s] allowed to connect.",
           reqinfo->remote_addr,
           (const char *)strUser.c_str());

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// websock_new_session
//

websock_session *
websock_new_session(const struct mg_connection *conn)
{
    const char *pHeader;
    char ws_version[10];
    char ws_key[33];
    websock_session *pSession = NULL;

    // Check pointer
    if (NULL == conn) return NULL;

    // user
    memset(ws_version, 0, sizeof(ws_version));
    if (NULL != (pHeader = mg_get_header(conn, "Sec-WebSocket-Version"))) {
        strncpy(ws_version,
                pHeader,
                std::min(strlen(pHeader) + 1, sizeof(ws_version)));
    }
    memset(ws_key, 0, sizeof(ws_key));
    if (NULL != (pHeader = mg_get_header(conn, "Sec-WebSocket-Key"))) {
        strncpy(ws_key, pHeader, std::min(strlen(pHeader) + 1, sizeof(ws_key)));
    }

    // create fresh session
    pSession = new websock_session;
    if (NULL == pSession) {
        syslog(LOG_ERR,
               "[Websockets] New session: Unable to create session object.");
        return NULL;
    }

    // Generate the sid
    unsigned char iv[16];
    char hexiv[33];
    getRandomIV(iv, 16); // Generate 16 random bytes
    memset(hexiv, 0, sizeof(hexiv));
    vscp_byteArray2HexStr(hexiv, iv, 16);

    memset(pSession->m_sid, 0, sizeof(pSession->m_sid));
    memcpy(pSession->m_sid, hexiv, 32);
    memset(pSession->m_websocket_key, 0, sizeof(pSession->m_websocket_key));

    // Init.
    strcpy(pSession->m_websocket_key, ws_key); // Save key
    pSession->m_conn       = (struct mg_connection *)conn;
    pSession->m_conn_state = WEBSOCK_CONN_STATE_CONNECTED;
    pSession->m_version    = atoi(ws_version); // Store protocol version

    pSession->m_pClientItem = new CClientItem(); // Create client
    if (NULL == pSession->m_pClientItem) {
        syslog(LOG_ERR,
               "[Websockets] New session: Unable to create client object.");
        delete pSession;
        return NULL;
    }

    pSession->m_pClientItem->bAuthenticated = false; // Not authenticated in yet
    vscp_clearVSCPFilter(&pSession->m_pClientItem->m_filter); // Clear filter
    pSession->bEventTrigger    = false;
    pSession->triggerTimeout   = 0;
    pSession->bVariableTrigger = false;

    // This is an active client
    pSession->m_pClientItem->m_bOpen = false;
    pSession->m_pClientItem->m_type =
      CLIENT_ITEM_INTERFACE_TYPE_CLIENT_WEBSOCKET;
    pSession->m_pClientItem->m_strDeviceName = ("Internal websocket client.");
    pSession->m_pClientItem->m_strDeviceName += ("|Started at ");
    vscpdatetime now = vscpdatetime::Now();
    pSession->m_pClientItem->m_strDeviceName += now.getISODateTime();

    // Add the client to the Client List
    pthread_mutex_lock(&gpobj->m_clientList.m_mutexItemList);
    if (!gpobj->addClient(pSession->m_pClientItem)) {
        // Failed to add client
        delete pSession->m_pClientItem;
        pSession->m_pClientItem = NULL;
        pthread_mutex_unlock(&gpobj->m_clientList.m_mutexItemList);
        syslog(LOG_ERR,
               ("Websocket server: Failed to add client. Terminating thread."));
        return NULL;
    }
    pthread_mutex_unlock(&gpobj->m_clientList.m_mutexItemList);

    pthread_mutex_lock(&gpobj->m_websocketSessionMutex);
    gpobj->m_websocketSessions.push_back(pSession);
    pthread_mutex_unlock(&gpobj->m_websocketSessionMutex);

    // Use the session object as user data
    mg_set_user_connection_data(pSession->m_conn, (void *)pSession);

    return pSession;
}

///////////////////////////////////////////////////////////////////////////////
// websock_expire_sessions
//

/*
void
websock_expire_sessions( struct mg_connection *conn )
{
    websock_session *pos;
    websock_session *prev;
    websock_session *next;
    time_t now;

    //return;

    // Check pointer
    if (NULL == nc) return;

    CControlObject *pObject = (CControlObject *)nc->mgr->user_data;
    if (NULL == pObject) return;

    now = time( NULL );
    prev = NULL;
    pos = gp_websock_sessions;

    pthread_mutex_lock(&m_websockSessionMutex);

    while ( NULL != pos ) {

        next = pos->m_next;

        if ( ( now - pos->lastActiveTime ) > ( WEBSOCKET_EXPIRE_TIME) ) {

            // expire sessions after WEBSOCKET_EXPIRE_TIME
            if ( NULL == prev ) {
                // First pos in list
                gp_websock_sessions = pos->m_next;
            }
            else {
                // Point to item after this one
                prev->m_next = next;
            }

            // Remove client and session item
            pthread_mutex_lock(&pObject->m_clientList.m_mutexItemList);
            pObject->removeClient( pos->m_pClientItem );
            pos->m_pClientItem = NULL;
            pthread_mutex_unlock(&pObject->m_clientList.m_mutexItemList);

            // Free session data
            free( pos );

        }
        else {
            prev = pos;
        }

        pos = next;
    }

    pthread_mutex_unlock(&m_websockSessionMutex);
}
*/

///////////////////////////////////////////////////////////////////////////////
// websock_sendevent
//
// Send event to all other clients.
//

bool
websock_sendevent(struct mg_connection *conn,
                  websock_session *pSession,
                  vscpEvent *pEvent)
{
    bool bSent = false;
    bool rv    = true;

    // Check pointer
    if (NULL == conn) return false;
    if (NULL == pSession) return false;

    // Level II events between 512-1023 is recognized by the daemon and
    // sent to the correct interface as Level I events if the interface
    // is addressed by the client.
    if ((pEvent->vscp_class <= 1023) && (pEvent->vscp_class >= 512) &&
        (pEvent->sizeData >= 16)) {

        // This event should be sent to the correct interface if it is
        // available on this machine. If not it should be sent to
        // the rest of the network as normal

        cguid destguid;
        destguid.getFromArray(pEvent->pdata);
        destguid.setAt(0, 0);
        destguid.setAt(1, 0);
        // unsigned char destGUID[16];  TODO ???
        // memcpy(destGUID, pEvent->pdata, 16); // get destination GUID
        // destGUID[0] = 0; // Interface GUID's have LSB bytes nilled
        // destGUID[1] = 0;

        pthread_mutex_lock(&gpobj->m_clientList.m_mutexItemList);

        // Find client
        CClientItem *pDestClientItem = NULL;
        std::deque<CClientItem *>::iterator it;
        for (it = gpobj->m_clientList.m_itemList.begin();
             it != gpobj->m_clientList.m_itemList.end();
             ++it) {

            CClientItem *pItem = *it;
            if (pItem->m_guid == destguid) {
                // Found
                pDestClientItem = pItem;
                break;
            }
        }

        if (NULL != pDestClientItem) {

            // If the client queue is full for this client then the
            // client will not receive the message
            if (pDestClientItem->m_clientInputQueue.size() <=
                gpobj->m_maxItemsInClientReceiveQueue) {

                // Create copy of event
                vscpEvent *pnewEvent = new vscpEvent;

                if (NULL != pnewEvent) {

                    pnewEvent->pdata = NULL;

                    vscp_copyEvent(pnewEvent, pEvent);

                    // Add the new event to the inputqueue
                    pthread_mutex_lock(
                      &pDestClientItem->m_mutexClientInputQueue);
                    pDestClientItem->m_clientInputQueue.push_back(pnewEvent);
                    sem_post(&pDestClientItem->m_semClientInputQueue);
                    pthread_mutex_unlock(
                      &pDestClientItem->m_mutexClientInputQueue);

                    bSent = true;
                } else {
                    bSent = false;
                }

            } else {
                // Overun - No room for event
                // vscp_deleteEvent(pEvent);
                bSent = true;
                rv    = false;
            }

        } // Client found

        pthread_mutex_unlock(&gpobj->m_clientList.m_mutexItemList);
    }

    if (!bSent) {

        // There must be room in the send queue
        if (gpobj->m_maxItemsInClientReceiveQueue >
            gpobj->m_clientOutputQueue.size()) {

            // Create copy of event
            vscpEvent *pnewEvent = new vscpEvent;

            if (NULL != pnewEvent) {

                pnewEvent->pdata = NULL;

                vscp_copyEvent(pnewEvent, pEvent);

                pthread_mutex_lock(&gpobj->m_mutexClientOutputQueue);
                gpobj->m_clientOutputQueue.push_back(pnewEvent);
                sem_post(&gpobj->m_semClientOutputQueue);
                pthread_mutex_unlock(&gpobj->m_mutexClientOutputQueue);
            }

        } else {
            rv = false;
        }
    }

    return rv;
}

///////////////////////////////////////////////////////////////////////////////
// websocket_post_incomingEvent
//

void
websock_post_incomingEvents(void)
{
    pthread_mutex_lock(&gpobj->m_websocketSessionMutex);

    std::list<websock_session *>::iterator iter;
    for (iter = gpobj->m_websocketSessions.begin();
         iter != gpobj->m_websocketSessions.end();
         ++iter) {

        websock_session *pSession = *iter;
        if (NULL == pSession) continue;

        // Should be a client item... hmm.... client disconnected
        if (NULL == pSession->m_pClientItem) {
            continue;
        }

        if (pSession->m_conn_state < WEBSOCK_CONN_STATE_CONNECTED) continue;

        if (NULL == pSession->m_conn) continue;

        if (pSession->m_pClientItem->m_bOpen &&
            pSession->m_pClientItem->m_clientInputQueue.size()) {

            vscpEvent *pEvent;

            pthread_mutex_lock(
              &pSession->m_pClientItem->m_mutexClientInputQueue);
            pEvent = pSession->m_pClientItem->m_clientInputQueue.front();
            pSession->m_pClientItem->m_clientInputQueue.pop_front();
            pthread_mutex_unlock(
              &pSession->m_pClientItem->m_mutexClientInputQueue);

            if (NULL != pEvent) {

                // Run event through filter
                if (vscp_doLevel2Filter(pEvent,
                                        &pSession->m_pClientItem->m_filter)) {

                    std::string str;
                    if (vscp_convertEventToString( str,pEvent)) {

                        // Write it out
                        if ( WS_TYPE_1 == pSession->m_wstypes ) {
                        str = ("E;") + str;
                        mg_websocket_write(pSession->m_conn,
                                           MG_WEBSOCKET_OPCODE_TEXT,
                                           (const char *)str.c_str(),
                                           str.length());
                        }
                        else if ( WS_TYPE_2 == pSession->m_wstypes ) {
                            std::string strEvent;
                            vscp_convertEventToJSON(strEvent, pEvent );
                            std::string str = vscp_str_format (WS2_EVENT, strEvent.c_str() );
                            mg_websocket_write(pSession->m_conn,
                                           MG_WEBSOCKET_OPCODE_TEXT,
                                           (const char *)str.c_str(),
                                           str.length());
                        }
                    }
                }

                // Remove the event
                vscp_deleteEvent(pEvent);

            } // Valid pEvent pointer

        } // events available

    } // for

    pthread_mutex_unlock(&gpobj->m_websocketSessionMutex);
}

///////////////////////////////////////////////////////////////////////////////
// websock_post_variableTrigger
//
// op = 0 - Variable changed    "V"
// op = 1 - Variable created    "N"
// op = 2 - Variable deleted    "D"
//

// void
// websock_post_variableTrigger(uint8_t op, CVariable *pVar)
// {
//     pthread_mutex_lock(&gpobj->m_websocketSessionMutex);

//     std::list<websock_session *>::iterator iter;
//     for (iter = gpobj->m_websocketSessions.begin();
//          iter != gpobj->m_websocketSessions.end();
//          ++iter) {

//         websock_session *pSession = *iter;
//         if (NULL == pSession) continue;

//         // Should be a client item... hmm.... client disconnected
//         if (NULL == pSession->m_pClientItem) {
//             continue;
//         }

//         if (pSession->m_conn_state < WEBSOCK_CONN_STATE_CONNECTED) continue;

//         if (NULL == pSession->m_conn) continue;

//         if (pSession->m_pClientItem->m_bOpen) {

//             std::string outstr;
//             outstr = "V;"; // Variable trigger
//             mg_websocket_write(pSession->m_conn,
//                                 MG_WEBSOCKET_OPCODE_TEXT,
//                                 (const char *)outstr.c_str(),
//                                 outstr.length());

//         } // open

//     } // for

//     pthread_mutex_unlock(&gpobj->m_websocketSessionMutex);
// }

////////////////////////////////////////////////////////////////////////////////
// ws1_connectHandler
//

int
ws1_connectHandler(const struct mg_connection *conn, void *cbdata)
{
    struct mg_context *ctx = mg_get_context(conn);
    int reject             = 1;

    // Check pointers
    if (NULL == conn) return 1;
    if (NULL == ctx) return 1;

    mg_lock_context(ctx);
    websock_session *pSession = websock_new_session(conn);

    if (NULL != pSession) {
        reject = 0;
    }
    mg_unlock_context(ctx);

    syslog(LOG_ERR,
           "[Websocket] Connection: client %s",
           (reject ? "rejected" : "accepted"));

    return reject;
}

////////////////////////////////////////////////////////////////////////////////
// ws1_closeHandler
//

void
ws1_closeHandler(const struct mg_connection *conn, void *cbdata)
{
    struct mg_context *ctx = mg_get_context(conn);
    websock_session *pSession =
      (websock_session *)mg_get_user_connection_data(conn);

    if (NULL == conn) return;
    if (NULL == pSession) return;
    if (pSession->m_conn != conn) return;
    if (pSession->m_conn_state < WEBSOCK_CONN_STATE_CONNECTED) return;

    mg_lock_context(ctx);

    // Record activity
    pSession->lastActiveTime = time(NULL);

    pSession->m_conn_state = WEBSOCK_CONN_STATE_NULL;
    pSession->m_conn       = NULL;
    gpobj->m_clientList.removeClient(pSession->m_pClientItem);
    pSession->m_pClientItem = NULL;

    pthread_mutex_lock(&gpobj->m_websocketSessionMutex);
    /*  TODO - Must remove session - gpobj->m_websocketSessions.DeleteContents(
    true ); if ( !gpobj->m_websocketSessions.DeleteObject( pSession )  ) {
        syslog( LOG_ERR,"[Websocket] Failed to delete session object.");
    }*/
    pthread_mutex_unlock(&gpobj->m_websocketSessionMutex);

    mg_unlock_context(ctx);
}

////////////////////////////////////////////////////////////////////////////////
// ws1_readyHandler
//

void
ws1_readyHandler(struct mg_connection *conn, void *cbdata)
{
    websock_session *pSession =
      (websock_session *)mg_get_user_connection_data(conn);

    // Check pointers
    if (NULL == conn) return;
    if (NULL == pSession) return;
    if (pSession->m_conn != conn) return;
    if (pSession->m_conn_state < WEBSOCK_CONN_STATE_CONNECTED) return;

    // Record activity
    pSession->lastActiveTime = time(NULL);

    // Start authentication
    std::string str = vscp_str_format(("+;AUTH0;%s"), pSession->m_sid);
    mg_websocket_write(
      conn, MG_WEBSOCKET_OPCODE_TEXT, (const char *)str.c_str(), str.length());

    pSession->m_conn_state = WEBSOCK_CONN_STATE_DATA;
}

////////////////////////////////////////////////////////////////////////////////
// ws1_dataHandler
//

int
ws1_dataHandler(
  struct mg_connection *conn, int bits, char *data, size_t len, void *cbdata)
{
    std::string strWsPkt;
    websock_session *pSession =
      (websock_session *)mg_get_user_connection_data(conn);

    // Check pointers
    if (NULL == conn) return WEB_ERROR;
    if (NULL == pSession) return WEB_ERROR;
    if (pSession->m_conn != conn) return WEB_ERROR;
    if (pSession->m_conn_state < WEBSOCK_CONN_STATE_CONNECTED) return WEB_ERROR;

    // Record activity
    pSession->lastActiveTime = time(NULL);

    switch (((unsigned char)bits) & 0x0F) {

        case MG_WEBSOCKET_OPCODE_CONTINUATION:

            // Save and concatenate mesage
            pSession->m_strConcatenated += std::string(data, len);

            // if last process is
            if (1 & bits) {
                if (!ws1_message(conn, pSession, pSession->m_strConcatenated)) {
                    return WEB_ERROR;
                }
            }
            break;

        // https://developer.mozilla.org/en-US/docs/Web/API/WebSockets_API/Writing_WebSocket_servers
        case MG_WEBSOCKET_OPCODE_TEXT:
            if (1 & bits) {
                strWsPkt = std::string(data, len);
                if (!ws1_message(conn, pSession, strWsPkt)) {
                    return WEB_ERROR;
                }
            } else {
                // Store first part
                pSession->m_strConcatenated = std::string(data, len);
            }
            break;

        case MG_WEBSOCKET_OPCODE_BINARY:
            break;

        case MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE:
            break;

        case MG_WEBSOCKET_OPCODE_PING:
            // fprintf( stdout, "Ping received" );
            break;

        case MG_WEBSOCKET_OPCODE_PONG:
            // fprintf( stdout, "Pong received" );
            break;

        default:
            break;
    }

    return WEB_OK;
}

///////////////////////////////////////////////////////////////////////////////
// ws1_message
//

bool
ws1_message(struct mg_connection *conn,
            websock_session *pSession,
            std::string &strWsPkt)
{
    std::string str;

    // Check pointer
    if (NULL == conn) return false;
    if (NULL == pSession) return false;

    vscp_trim(strWsPkt);

    switch (strWsPkt[0]) {

        // Command - | 'C' | command type (byte) | data |
        case 'C':
            // Point beyond initial info "C;"
            strWsPkt = vscp_str_right(strWsPkt, strWsPkt.length() - 2);
            ws1_command(conn, pSession, strWsPkt);
            break;

        // Event | 'E' ; head(byte) , vscp_class(unsigned short) ,
        // vscp_type(unsigned
        //              short) , GUID(16*byte), data(0-487 bytes) |
        case 'E': {
            // Must be authorised to do this
            if ((NULL == pSession->m_pClientItem) ||
                !pSession->m_pClientItem->bAuthenticated) {

                str = vscp_str_format(("-;%d;%s"),
                                      (int)WEBSOCK_ERROR_NOT_AUTHORISED,
                                      WEBSOCK_STR_ERROR_NOT_AUTHORISED);
                mg_websocket_write(conn,
                                   MG_WEBSOCKET_OPCODE_TEXT,
                                   (const char *)str.c_str(),
                                   str.length());
                return true;
            }

            // Point beyond initial info "E;"
            strWsPkt = vscp_str_right(strWsPkt, strWsPkt.length() - 2);
            vscpEvent vscp_event;

            if (vscp_convertStringToEvent(&vscp_event, strWsPkt)) {

                // If GUID is all null give it GUID of interface
                if (vscp_isGUIDEmpty(vscp_event.GUID)) {
                    pSession->m_pClientItem->m_guid.writeGUID(vscp_event.GUID);
                }

                // Check if this user is allowed to send this event
                if (!pSession->m_pClientItem->m_pUserItem
                       ->isUserAllowedToSendEvent(vscp_event.vscp_class,
                                                  vscp_event.vscp_type)) {
                    syslog(LOG_ERR,
                           "websocket] User [%s] not allowed to "
                           "send event class=%d type=%d.",
                           pSession->m_pClientItem->m_pUserItem->getUserName()
                             .c_str(),
                           vscp_event.vscp_class,
                           vscp_event.vscp_type);
                }

                vscp_event.obid = pSession->m_pClientItem->m_clientID;
                if (websock_sendevent(conn, pSession, &vscp_event)) {
                    mg_websocket_write(
                      conn, MG_WEBSOCKET_OPCODE_TEXT, "+;EVENT", 7);
                } else {
                    str = vscp_str_format(("-;%d;%s"),
                                          (int)WEBSOCK_ERROR_TX_BUFFER_FULL,
                                          WEBSOCK_STR_ERROR_TX_BUFFER_FULL);
                    mg_websocket_write(conn,
                                       MG_WEBSOCKET_OPCODE_TEXT,
                                       (const char *)str.c_str(),
                                       str.length());
                }
            }

        } break;

        // Unknown command
        default:
            break;
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////
// ws1_command
//

void
ws1_command(struct mg_connection *conn,
            struct websock_session *pSession,
            std::string &strCmd)
{
    std::string str; // Worker string
    std::string strTok;

    // Check pointer
    if (NULL == conn) return;
    if (NULL == pSession) return;

    syslog(LOG_ERR, "[Websocket] Command = %s", strCmd.c_str());

    std::deque<std::string> tokens;
    vscp_split(tokens, strCmd, ";");

    // Get command
    if (!tokens.empty()) {
        strTok = tokens.front();
        tokens.pop_front();
        vscp_trim(strTok);
        vscp_makeUpper(strTok);
    } else {
        std::string str = vscp_str_format(("-;%d;%s"),
                                          (int)WEBSOCK_ERROR_SYNTAX_ERROR,
                                          WEBSOCK_STR_ERROR_SYNTAX_ERROR);
        mg_websocket_write(
          conn, MG_WEBSOCKET_OPCODE_TEXT, (const char *)str.c_str(), str.length());
        return;
    }

    // ------------------------------------------------------------------------
    //                                NOOP
    //-------------------------------------------------------------------------

    if (vscp_startsWith(strTok, "NOOP")) {

        mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, "+;NOOP", 6);

        // Send authentication challenge
        if ((NULL == pSession->m_pClientItem) ||
            !pSession->m_pClientItem->bAuthenticated) {
            // TODO
        }

    }

    // ------------------------------------------------------------------------
    //                               CHALLENGE
    //-------------------------------------------------------------------------

    else if (vscp_startsWith(strTok, "CHALLENGE")) {

        // Send authentication challenge
        if ((NULL == pSession->m_pClientItem) ||
            !pSession->m_pClientItem->bAuthenticated) {

            // Start authentication
            str = vscp_str_format(("+;AUTH0;%s"), pSession->m_sid);
            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());
        }

    }

    // ------------------------------------------------------------------------
    //                                AUTH
    //-------------------------------------------------------------------------

    // AUTH;iv;aes128
    else if (vscp_startsWith(strTok, "AUTH")) {

        std::string str;
        std::string strUser;
        std::string strIV = tokens.front();
        tokens.pop_front();
        std::string strCrypto = tokens.front();
        tokens.pop_front();
        if (websock_authentication(conn, pSession, strIV, strCrypto)) {
            std::string userSettings;
            pSession->m_pClientItem->m_pUserItem->getAsString(userSettings);
            str = vscp_str_format(("+;AUTH1;%s"),
                                  (const char *)userSettings.c_str());
            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());
        } else {

            str = vscp_str_format(("-;AUTH;%d;%s"),
                                  (int)WEBSOCK_ERROR_NOT_AUTHORISED,
                                  WEBSOCK_STR_ERROR_NOT_AUTHORISED);
            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());
            pSession->m_pClientItem->bAuthenticated = false; // Authenticated
        }
    }

    // ------------------------------------------------------------------------
    //                                OPEN
    //-------------------------------------------------------------------------

    else if (vscp_startsWith(strTok, "OPEN")) {

        // Must be authorised to do this
        if ((NULL == pSession->m_pClientItem) ||
            !pSession->m_pClientItem->bAuthenticated) {

            str = vscp_str_format(("-;OPEN;%d;%s"),
                                  (int)WEBSOCK_ERROR_NOT_AUTHORISED,
                                  WEBSOCK_STR_ERROR_NOT_AUTHORISED);

            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());

            return; // We still leave channel open
        }

        pSession->m_pClientItem->m_bOpen = true;
        mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, "+;OPEN", 6);
    }

    // ------------------------------------------------------------------------
    //                                CLOSE
    //-------------------------------------------------------------------------

    else if (vscp_startsWith(strTok, "CLOSE")) {
        pSession->m_pClientItem->m_bOpen = false;
        mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, "+;CLOSE", 7);
    }

    // ------------------------------------------------------------------------
    //                             SETFILTER/SF
    //-------------------------------------------------------------------------

    else if (vscp_startsWith(strTok, "SETFILTER") ||
             vscp_startsWith(strTok, "SF")) {

        unsigned char ifGUID[16];
        memset(ifGUID, 0, 16);

        // Must be authorized to do this
        if ((NULL == pSession->m_pClientItem) ||
            !pSession->m_pClientItem->bAuthenticated) {

            str = vscp_str_format(("-;SF;%d;%s"),
                                  (int)WEBSOCK_ERROR_NOT_AUTHORISED,
                                  WEBSOCK_STR_ERROR_NOT_AUTHORISED);

            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());

            syslog(LOG_ERR,
                   "[Websocket] User/host not authorised to set a filter.");

            return; // We still leave channel open
        }

        // Check privilege
        if ((pSession->m_pClientItem->m_pUserItem->getUserRights(0) & 0xf) <
            6) {

            str = vscp_str_format(("-;SF;%d;%s"),
                                  (int)WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
                                  WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT);

            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());

            syslog(LOG_ERR,
                   "[Websocket] User [%s] not "
                   "allowed to set a filter.\n",
                   pSession->m_pClientItem->m_pUserItem->getUserName().c_str());
            return; // We still leave channel open
        }

        // Get filter
        if (!tokens.empty()) {

            strTok = tokens.front();
            tokens.pop_front();

            pthread_mutex_lock(
              &pSession->m_pClientItem->m_mutexClientInputQueue);
            if (!vscp_readFilterFromString(&pSession->m_pClientItem->m_filter,
                                           strTok)) {

                str = vscp_str_format(("-;SF;%d;%s"),
                                      (int)WEBSOCK_ERROR_SYNTAX_ERROR,
                                      WEBSOCK_STR_ERROR_SYNTAX_ERROR);

                mg_websocket_write(conn,
                                   MG_WEBSOCKET_OPCODE_TEXT,
                                   (const char *)str.c_str(),
                                   str.length());

                pthread_mutex_unlock(
                  &pSession->m_pClientItem->m_mutexClientInputQueue);
                return;
            }

            pthread_mutex_unlock(
              &pSession->m_pClientItem->m_mutexClientInputQueue);
        } else {

            str = vscp_str_format(("-;SF;%d;%s"),
                                  (int)WEBSOCK_ERROR_SYNTAX_ERROR,
                                  WEBSOCK_STR_ERROR_SYNTAX_ERROR);

            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());

            return;
        }

        // Get mask
        if (!tokens.empty()) {

            strTok = tokens.front();
            tokens.pop_front();

            pthread_mutex_lock(
              &pSession->m_pClientItem->m_mutexClientInputQueue);
            if (!vscp_readMaskFromString(&pSession->m_pClientItem->m_filter,
                                         strTok)) {

                str = vscp_str_format(("-;SF;%d;%s"),
                                      (int)WEBSOCK_ERROR_SYNTAX_ERROR,
                                      WEBSOCK_STR_ERROR_SYNTAX_ERROR);

                mg_websocket_write(conn,
                                   MG_WEBSOCKET_OPCODE_TEXT,
                                   (const char *)str.c_str(),
                                   str.length());

                pthread_mutex_unlock(
                  &pSession->m_pClientItem->m_mutexClientInputQueue);
                return;
            }

            pthread_mutex_unlock(
              &pSession->m_pClientItem->m_mutexClientInputQueue);

        } else {
            str = vscp_str_format(("-;SF;%d;%s"),
                                  (int)WEBSOCK_ERROR_SYNTAX_ERROR,
                                  WEBSOCK_STR_ERROR_SYNTAX_ERROR);

            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());
            return;
        }

        // Positive response
        mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, "+;SF", 4);

    }

    // ------------------------------------------------------------------------
    //                           CLRQ/CLRQUEUE
    //-------------------------------------------------------------------------

    // Clear the event queue
    else if (vscp_startsWith(strTok, "CLRQUEUE") ||
             vscp_startsWith(strTok, "CLRQ")) {

        // Must be authorised to do this
        if ((NULL == pSession->m_pClientItem) ||
            !pSession->m_pClientItem->bAuthenticated) {

            str = vscp_str_format(("-;CLRQ;%d;%s"),
                                  (int)WEBSOCK_ERROR_NOT_AUTHORISED,
                                  WEBSOCK_STR_ERROR_NOT_AUTHORISED);

            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());

            syslog(LOG_ERR,
                   "[Websocket] User/host not authorised to clear the queue.");

            return; // We still leave channel open
        }

        // Check privilege
        if ((pSession->m_pClientItem->m_pUserItem->getUserRights(0) & 0xf) <
            1) {

            str = vscp_str_format(("-;CLRQ;%d;%s"),
                                  (int)WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
                                  WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT);

            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());

            syslog(LOG_ERR,
                   "[Websocket] User [%s] "
                   "not allowed to clear the queue.\n",
                   pSession->m_pClientItem->m_pUserItem->getUserName().c_str());
            return; // We still leave channel open
        }

        std::deque<vscpEvent *>::iterator it;
        pthread_mutex_lock(&pSession->m_pClientItem->m_mutexClientInputQueue);

        for (it = pSession->m_pClientItem->m_clientInputQueue.begin();
             it != pSession->m_pClientItem->m_clientInputQueue.end();
             ++it) {
            vscpEvent *pEvent =
              pSession->m_pClientItem->m_clientInputQueue.front();
            pSession->m_pClientItem->m_clientInputQueue.pop_front();
            vscp_deleteEvent(pEvent);
        }

        pSession->m_pClientItem->m_clientInputQueue.clear();
        pthread_mutex_unlock(&pSession->m_pClientItem->m_mutexClientInputQueue);

        mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, "+;CLRQ", 6);

    }

    ////////////////////////////////////////////////////////////////////////////
    //                              VARIABLES
    ////////////////////////////////////////////////////////////////////////////

    // ------------------------------------------------------------------------
    //                            CVAR/CREATEVAR
    //-------------------------------------------------------------------------

    // else if (vscp_startsWith(strTok, "CREATEVAR") ||
    //          vscp_startsWith(strTok, "CVAR")) {

    //     std::string name;
    //     std::string value;
    //     std::string note;
    //     uint8_t type          = VSCP_DAEMON_VARIABLE_CODE_STRING;
    //     bool bPersistent      = false;
    //     uint32_t accessrights = 744;

    //     // Must be authorised to do this
    //     if ((NULL == pSession->m_pClientItem) ||
    //         !pSession->m_pClientItem->bAuthenticated) {

    //         str = vscp_str_format(("-;CVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_NOT_AUTHORISED,
    //                                  WEBSOCK_STR_ERROR_NOT_AUTHORISED);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         syslog(
    //           LOG_ERR,
    //           "[Websocket] User/host not authorised to create a variable.");

    //         return; // We still leave channel open
    //     }

    //     // Check privilege
    //     if ((pSession->m_pClientItem->m_pUserItem->getUserRights(0) & 0xf) <
    //         6) {

    //         str = vscp_str_format(("-;CVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
    //                                  WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         syslog(LOG_ERR,
    //                "[Websocket] User [%s] not "
    //                "allowed to create a variable.",
    //                pSession->m_pClientItem->m_pUserItem->getUserName().c_str());
    //         return; // We still leave channel open
    //     }

    //     // “C;CVAR;name;type;accessrights;bPersistens;value;note”

    //     // Get variable name
    //     if (!tokens.empty()) {
    //         name = tokens.front();
    //         tokens.pop_front();
    //     } else {

    //         str = vscp_str_format(("-;CVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_SYNTAX_ERROR,
    //                                  WEBSOCK_STR_ERROR_SYNTAX_ERROR);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         return;
    //     }

    //     // Check if the variable exist
    //     CVariable variable;
    //     bool bVariableExist = false;
    //     if (0 != gpobj->m_variables.find(name,
    //     pSession->m_pClientItem->m_pUserItem, variable)) {
    //         bVariableExist = true;
    //     }

    //     // name can not start with "vscp." - reserved for a stock variable
    //     if (vscp_startsWith(vscp_lower(name), "vscp.")) {

    //         str = vscp_str_format(("-;CVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_VARIABLE_NO_STOCK,
    //                                  WEBSOCK_STR_ERROR_VARIABLE_NO_STOCK);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         return;
    //     }

    //     // Get variable type
    //     bool bInputType = false;
    //     if (!tokens.empty()) {
    //         std::string str = tokens.front();
    //         tokens.pop_front();
    //         vscp_trim(str);
    //         if (str.length()) bInputType = true;
    //         type = vscp_readStringValue(str);
    //     }

    //     // Get variable access rights
    //     bool bInputAccessRights = false;
    //     if (!tokens.empty()) {
    //         std::string str = tokens.front();
    //         tokens.pop_front();
    //         vscp_trim(str);
    //         if (str.length()) bInputAccessRights = true;
    //         accessrights = vscp_readStringValue(str);
    //     }

    //     // Get variable Persistence 0/1
    //     bool bInputPersistent = false;
    //     if (!tokens.empty()) {
    //         std::string str = tokens.front();
    //         tokens.pop_front();
    //         vscp_trim(str);
    //         if (str.length()) bInputPersistent = true;
    //         int val = vscp_readStringValue(str);

    //         if (0 == val) {
    //             bPersistent = false;
    //         } else if (1 == val) {
    //             bPersistent = true;
    //         } else {
    //             str = vscp_str_format(("-;CVAR;%d;%s"),
    //                                      (int)WEBSOCK_ERROR_SYNTAX_ERROR,
    //                                      WEBSOCK_STR_ERROR_SYNTAX_ERROR);

    //             mg_websocket_write(conn,
    //                                 MG_WEBSOCKET_OPCODE_TEXT,
    //                                 (const char *)str.c_str(),
    //                                 str.length());
    //             return;
    //         }
    //     }

    //     // Get variable value
    //     bool bInputValue = false;
    //     if (!tokens.empty()) {
    //         bInputValue = true;
    //         value       = tokens.front();
    //         tokens.pop_front();
    //         vscp_base64_std_decode(value);
    //     }

    //     // Get variable note
    //     bool bInputNote = false;
    //     if (!tokens.empty()) {
    //         bInputNote = true;
    //         note       = tokens.front();
    //         tokens.pop_front();
    //         vscp_base64_std_decode(value);
    //     }

    //     if (!bVariableExist) {

    //         // Add the variable
    //         if (!gpobj->m_variables.add(
    //               name,
    //               value,
    //               type,
    //               pSession->m_pClientItem->m_pUserItem->getUserID(),
    //               bPersistent,
    //               accessrights,
    //               note)) {

    //             str = vscp_str_format(("-;CVAR;%d;%s"),
    //                                      (int)WEBSOCK_ERROR_SYNTAX_ERROR,
    //                                      WEBSOCK_STR_ERROR_SYNTAX_ERROR);

    //             mg_websocket_write(conn,
    //                                 MG_WEBSOCKET_OPCODE_TEXT,
    //                                 (const char *)str.c_str(),
    //                                 str.length());
    //             return;
    //         }
    //     } else {

    //         // Save changes to the variable

    //         if (bInputType) {
    //             variable.setType(type);
    //         }

    //         if (bInputAccessRights) {
    //             variable.setAccessRights(accessrights);
    //         }

    //         // Persistence can't be changed

    //         if (bInputValue) {
    //             variable.setValue(value);
    //         }

    //         if (bInputNote) {
    //             variable.setNote(note);
    //         }

    //         // Save the changed variable
    //         if
    //         (!gpobj->m_variables.update(variable,pSession->m_pClientItem->m_pUserItem))
    //         {

    //             str = vscp_str_format(("-;CVAR;%d;%s"),
    //                                      (int)WEBSOCK_ERROR_SYNTAX_ERROR,
    //                                      WEBSOCK_STR_ERROR_SYNTAX_ERROR);

    //             mg_websocket_write(conn,
    //                                 MG_WEBSOCKET_OPCODE_TEXT,
    //                                 (const char *)str.c_str(),
    //                                 str.length());
    //             return;
    //         }
    //     }

    //     std::string strResult = ("+;CVAR;");
    //     strResult += name;

    //     mg_websocket_write(conn,
    //                         MG_WEBSOCKET_OPCODE_TEXT,
    //                         (const char *)strResult.c_str(),
    //                         strResult.length());

    // }

    // //
    // ------------------------------------------------------------------------
    // //                              RVAR/READVAR
    // //-------------------------------------------------------------------------

    // else if (vscp_startsWith(strTok, "READVAR") ||
    //          vscp_startsWith(strTok, "RVAR")) {

    //     CVariable variable;
    //     uint8_t type;
    //     std::string strvalue;

    //     // Must be authorised to do this
    //     if ((NULL == pSession->m_pClientItem) ||
    //         !pSession->m_pClientItem->bAuthenticated) {

    //         str = vscp_str_format(("-;RVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_NOT_AUTHORISED,
    //                                  WEBSOCK_STR_ERROR_NOT_AUTHORISED);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         syslog(LOG_ERR,
    //                "[Websocket] User/host not authorised to read a
    //                variable.");

    //         return; // We still leave channel open
    //     }

    //     // Check privilege
    //     if ((pSession->m_pClientItem->m_pUserItem->getUserRights(0) & 0xf) <
    //         4) {

    //         str = vscp_str_format(("-;RVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
    //                                  WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         syslog(LOG_ERR,
    //                "[Websocket] User [%s] "
    //                "not allowed to read a variable.",
    //                pSession->m_pClientItem->m_pUserItem->getUserName().c_str());
    //         return; // We still leave channel open
    //     }

    //     strTok = tokens.front();
    //     tokens.pop_front();
    //     if (0 == gpobj->m_variables.find(strTok,
    //     pSession->m_pClientItem->m_pUserItem,variable)) {

    //         str = vscp_str_format(("-;RVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_VARIABLE_UNKNOWN,
    //                                  WEBSOCK_STR_ERROR_VARIABLE_UNKNOWN);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());
    //         return;
    //     }

    //     // name;type;bPersistent;userid;rights;lastchanged;value;note
    //     std::string strResult = ("+;RVAR;");
    //     strResult += variable.getAsString(false);
    //     mg_websocket_write(conn,
    //                         MG_WEBSOCKET_OPCODE_TEXT,
    //                         (const char *)strResult.c_str(),
    //                         strResult.length());
    // }

    // //
    // ------------------------------------------------------------------------
    // //                                WVAR/WRITEVAR
    // //-------------------------------------------------------------------------

    // else if (vscp_startsWith(strTok, "WRITEVAR") ||
    //          vscp_startsWith(strTok, "WVAR")) {

    //     CVariable variable;
    //     std::string strvalue;
    //     uint8_t type;

    //     // Must be authorised to do this
    //     if ((NULL == pSession->m_pClientItem) ||
    //         !pSession->m_pClientItem->bAuthenticated) {

    //         str = vscp_str_format(("-;WVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_NOT_AUTHORISED,
    //                                  WEBSOCK_STR_ERROR_NOT_AUTHORISED);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         syslog(LOG_ERR,
    //                "[Websocket] User/host not authorised to write a
    //                variable.");

    //         return; // We still leave channel open
    //     }

    //     // Check privilege
    //     if ((pSession->m_pClientItem->m_pUserItem->getUserRights(0) & 0xf) <
    //         6) {

    //         str = vscp_str_format(("-;WVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
    //                                  WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         syslog(LOG_ERR,
    //                "[Websocket] User [%s] not allowed to do write
    //                variable.\n",
    //                pSession->m_pClientItem->m_pUserItem->getUserName().c_str());
    //         return; // We still leave channel open
    //     }

    //     // Get variable name
    //     std::string strVarName;
    //     if (!tokens.empty()) {

    //         strVarName = tokens.front();
    //         tokens.pop_front();
    //         if (0 ==
    //             gpobj->m_variables.find(vscp_upper(strVarName),
    //             pSession->m_pClientItem->m_pUserItem,variable)) {

    //             str = vscp_str_format(("-;WVAR;%d;%s"),
    //                                      (int)WEBSOCK_ERROR_VARIABLE_UNKNOWN,
    //                                      WEBSOCK_STR_ERROR_VARIABLE_UNKNOWN);

    //             mg_websocket_write(conn,
    //                                 MG_WEBSOCKET_OPCODE_TEXT,
    //                                 (const char *)str.c_str(),
    //                                 str.length());

    //             return;
    //         }

    //         // Get variable value
    //         if (!tokens.empty()) {

    //             strTok = tokens.front();
    //             tokens.pop_front();

    //             if (!variable.setValueFromString(variable.getType(),
    //                                              strTok,
    //                                              true)) { // decode

    //                 str = vscp_str_format(("-;WVAR;%d;%s"),
    //                                          (int)WEBSOCK_ERROR_SYNTAX_ERROR,
    //                                          WEBSOCK_STR_ERROR_SYNTAX_ERROR);

    //                 mg_websocket_write(conn,
    //                                     MG_WEBSOCKET_OPCODE_TEXT,
    //                                     (const char *)str.c_str(),
    //                                     str.length());
    //                 return;
    //             }

    //             // Update the variable
    //             if
    //             (!gpobj->m_variables.update(variable,pSession->m_pClientItem->m_pUserItem))
    //             {

    //                 str = vscp_str_format(("-;WVAR;%d;%s"),
    //                                          (int)WEBSOCK_ERROR_SYNTAX_ERROR,
    //                                          WEBSOCK_STR_ERROR_VARIABLE_UPDATE);

    //                 mg_websocket_write(conn,
    //                                     MG_WEBSOCKET_OPCODE_TEXT,
    //                                     (const char *)str.c_str(),
    //                                     str.length());
    //                 return;
    //             }
    //         } else {
    //             str = vscp_str_format(("-;WVAR;%d;%s"),
    //                                      (int)WEBSOCK_ERROR_SYNTAX_ERROR,
    //                                      WEBSOCK_STR_ERROR_SYNTAX_ERROR);

    //             mg_websocket_write(conn,
    //                                 MG_WEBSOCKET_OPCODE_TEXT,
    //                                 (const char *)str.c_str(),
    //                                 str.length());
    //             return;
    //         }
    //     } else {
    //         str = vscp_str_format(("-;WVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_SYNTAX_ERROR,
    //                                  WEBSOCK_STR_ERROR_SYNTAX_ERROR);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());
    //         return;
    //     }

    //     std::string strResult = ("+;WVAR;");
    //     strResult += strVarName;

    //     // Positive reply
    //     mg_websocket_write(conn,
    //                         MG_WEBSOCKET_OPCODE_TEXT,
    //                         (const char *)strResult.c_str(),
    //                         strResult.length());

    // }

    // //
    // ------------------------------------------------------------------------
    // //                             RSTVAR/RESETVAR
    // //-------------------------------------------------------------------------

    // else if (vscp_startsWith(strTok, "RESETVAR") ||
    //          vscp_startsWith(strTok, "RSTVAR")) {

    //     CVariable variable;
    //     std::string strvalue;
    //     uint8_t type;

    //     // Must be authorised to do this
    //     if ((NULL == pSession->m_pClientItem) ||
    //         !pSession->m_pClientItem->bAuthenticated) {

    //         str = vscp_str_format(("-;RSTVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_NOT_AUTHORISED,
    //                                  WEBSOCK_STR_ERROR_NOT_AUTHORISED);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         return; // We still leave channel open
    //     }

    //     // Check privilege
    //     if ((pSession->m_pClientItem->m_pUserItem->getUserRights(0) & 0xf) <
    //         6) {

    //         str = vscp_str_format(("-;RSTVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
    //                                  WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         syslog(LOG_ERR,
    //                "[Websocket] User [%s] not allowed to reset a
    //                variable.\n",
    //                pSession->m_pClientItem->m_pUserItem->getUserName().c_str());
    //         return; // We still leave channel open
    //     }

    //     strTok = tokens.front();
    //     tokens.pop_front();
    //     if (0 == gpobj->m_variables.find(strTok,
    //     pSession->m_pClientItem->m_pUserItem,variable)) {

    //         str = vscp_str_format(("-;RSTVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_VARIABLE_UNKNOWN,
    //                                  WEBSOCK_STR_ERROR_VARIABLE_UNKNOWN);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         syslog(LOG_ERR,
    //                "[Websocket] User/host not "
    //                "authorised to reset a variable.");

    //         return;
    //     }

    //     variable.reset();

    //     variable.writeValueToString(strvalue);
    //     type = variable.getType();

    //     std::string strResult = ("+;RSTVAR;");
    //     strResult += strTok;
    //     strResult += (";");
    //     strResult += vscp_str_format(("%d"), type);
    //     strResult += (";");
    //     strResult += strvalue;

    //     // Positive reply
    //     mg_websocket_write(conn,
    //                         MG_WEBSOCKET_OPCODE_TEXT,
    //                         (const char *)strResult.c_str(),
    //                         strResult.length());

    // }

    // //
    // ------------------------------------------------------------------------
    // //                                 DELVAR/REMOVEVAR
    // //-------------------------------------------------------------------------

    // else if (vscp_startsWith(strTok, "DELVAR") ||
    //          vscp_startsWith(strTok, "REMOVEVAR")) {

    //     CVariable variable;
    //     std::string name;

    //     // Must be authorised to do this
    //     if ((NULL == pSession->m_pClientItem) ||
    //         !pSession->m_pClientItem->bAuthenticated) {

    //         str = vscp_str_format(("-;DELVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_NOT_AUTHORISED,
    //                                  WEBSOCK_STR_ERROR_NOT_AUTHORISED);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         syslog(
    //           LOG_ERR,
    //           "[Websocket] User/host not authorised to delete a variable.");

    //         return; // We still leave channel open
    //     }

    //     // Check privilege
    //     if ((pSession->m_pClientItem->m_pUserItem->getUserRights(0) & 0xf) <
    //         6) {

    //         str = vscp_str_format(("-;DELVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
    //                                  WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         syslog(LOG_ERR,
    //                "[Websocket] User [%s] "
    //                "not allowed to delete a variable.",
    //                pSession->m_pClientItem->m_pUserItem->getUserName().c_str());
    //         return; // We still leave channel open
    //     }

    //     name = tokens.front();
    //     tokens.pop_front();
    //     if (0 == gpobj->m_variables.find(name,
    //     pSession->m_pClientItem->m_pUserItem,variable)) {

    //         str = vscp_str_format(("-;DELVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_VARIABLE_UNKNOWN,
    //                                  WEBSOCK_STR_ERROR_VARIABLE_UNKNOWN);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         return;
    //     }

    //     pthread_mutex_lock(&gpobj->m_variableMutex);
    //     gpobj->m_variables.remove(name,pSession->m_pClientItem->m_pUserItem);
    //     pthread_mutex_unlock(&gpobj->m_variableMutex);

    //     std::string strResult = ("+;DELVAR;");
    //     strResult += name;

    //     // Positive reply
    //     mg_websocket_write(conn,
    //                         MG_WEBSOCKET_OPCODE_TEXT,
    //                         (const char *)strResult.c_str(),
    //                         strResult.length());

    // }

    // //
    // ------------------------------------------------------------------------
    // //                             LENVAR/LENGTHVAR
    // //-------------------------------------------------------------------------

    // else if (vscp_startsWith(strTok, "LENGTHVAR") ||
    //          vscp_startsWith(strTok, "LENVAR")) {

    //     CVariable variable;

    //     // Must be authorised to do this
    //     if ((NULL == pSession->m_pClientItem) ||
    //         !pSession->m_pClientItem->bAuthenticated) {

    //         str = vscp_str_format(("-;LENVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_NOT_AUTHORISED,
    //                                  WEBSOCK_STR_ERROR_NOT_AUTHORISED);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         syslog(LOG_ERR,
    //                "[Websocket] User/host not "
    //                "authorised to get length of variable.");

    //         return; // We still leave channel open
    //     }

    //     // Check privilege
    //     if ((pSession->m_pClientItem->m_pUserItem->getUserRights(0) & 0xf) <
    //         4) {

    //         str = vscp_str_format(("-;LENVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
    //                                  WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         syslog(
    //           LOG_ERR,
    //           "[Websocket] User [%s] not allowed to get length of variable.",
    //           pSession->m_pClientItem->m_pUserItem->getUserName().c_str());
    //         return; // We still leave channel open
    //     }

    //     strTok = tokens.front();
    //     tokens.pop_front();
    //     if (0 == gpobj->m_variables.find(strTok,
    //     pSession->m_pClientItem->m_pUserItem,variable)) {

    //         str = vscp_str_format(("-;LENVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_VARIABLE_UNKNOWN,
    //                                  WEBSOCK_STR_ERROR_VARIABLE_UNKNOWN);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());
    //         return;
    //     }

    //     std::string strResult = ("+;LENVAR;");
    //     strResult += strTok;
    //     strResult += (";");
    //     strResult += vscp_str_format(("%d"), variable.getValue().length());

    //     // Positive reply
    //     mg_websocket_write(conn,
    //                         MG_WEBSOCKET_OPCODE_TEXT,
    //                         (const char *)strResult.c_str(),
    //                         strResult.length());

    // }

    // //
    // ------------------------------------------------------------------------
    // //                           LCVAR/LASTCHANGEVAR
    // //-------------------------------------------------------------------------

    // else if (vscp_startsWith(strTok, "LASTCHANGEVAR") ||
    //          vscp_startsWith(strTok, "LCVAR")) {

    //     CVariable variable;
    //     std::string strvalue;

    //     // Must be authorised to do this
    //     if ((NULL == pSession->m_pClientItem) ||
    //         !pSession->m_pClientItem->bAuthenticated) {

    //         str = vscp_str_format(("-;LCVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_NOT_AUTHORISED,
    //                                  WEBSOCK_STR_ERROR_NOT_AUTHORISED);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         syslog(LOG_ERR,
    //                "[Websocket] User/host not authorised "
    //                "to get last change date of variable.");

    //         return; // We still leave channel open
    //     }

    //     // Check privilege
    //     if ((pSession->m_pClientItem->m_pUserItem->getUserRights(0) & 0xf) <
    //         4) {

    //         str = vscp_str_format(("-;LCVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
    //                                  WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         syslog(LOG_ERR,
    //                "[Websocket] User [%s] not allowed "
    //                "to get last change date of variable.\n",
    //                pSession->m_pClientItem->m_pUserItem->getUserName().c_str());
    //         return; // We still leave channel open
    //     }

    //     strTok = tokens.front();
    //     tokens.pop_front();
    //     if (0 == gpobj->m_variables.find(strTok,
    //     pSession->m_pClientItem->m_pUserItem,variable)) {

    //         str = vscp_str_format(("-;LCVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_VARIABLE_UNKNOWN,
    //                                  WEBSOCK_STR_ERROR_VARIABLE_UNKNOWN);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         return;
    //     }

    //     variable.writeValueToString(strvalue);

    //     std::string strResult = ("+;LCVAR;");
    //     strResult += strTok;
    //     strResult += (";");
    //     strResult += variable.getLastChange().getISODateTime();

    //     // Positive reply
    //     mg_websocket_write(conn,
    //                         MG_WEBSOCKET_OPCODE_TEXT,
    //                         (const char *)strResult.c_str(),
    //                         strResult.length());

    // }

    // //
    // ------------------------------------------------------------------------
    // //                               LSTVAR/LISTVAR
    // //-------------------------------------------------------------------------

    // else if (vscp_startsWith(strTok, "LISTVAR") ||
    //          vscp_startsWith(strTok, "LSTVAR")) {

    //     CVariable variable;
    //     std::string strvalue;
    //     std::string strSearch;

    //     // Must be authorised to do this
    //     if ((NULL == pSession->m_pClientItem) ||
    //         !pSession->m_pClientItem->bAuthenticated) {

    //         str = vscp_str_format(("-;LSTVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_NOT_AUTHORISED,
    //                                  WEBSOCK_STR_ERROR_NOT_AUTHORISED);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         syslog(LOG_ERR,
    //                "[Websocket] User/host not "
    //                "authorised to list variable(s).");

    //         return; // We still leave channel open
    //     }

    //     // Check privilege
    //     if ((pSession->m_pClientItem->m_pUserItem->getUserRights(0) & 0xf) <
    //         4) {

    //         str = vscp_str_format(("-;LSTVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
    //                                  WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());

    //         syslog(LOG_ERR,
    //                "[Websocket] User [%s] "
    //                "not allowed to list variable(s).",
    //                pSession->m_pClientItem->m_pUserItem->getUserName().c_str());
    //         return; // We still leave channel open
    //     }

    //     int i = 0;
    //     std::string strResult;
    //     std::string strWork;
    //     pthread_mutex_lock(&gpobj->m_variableMutex);

    //     if (!tokens.empty()) {
    //         strSearch = tokens.front();
    //         tokens.pop_front();
    //         vscp_trim(strSearch);
    //         if (strSearch.empty()) {
    //             strSearch = ("(.*)"); // list all
    //         }
    //     } else {
    //         strSearch = ("(.*)"); // List all
    //     }

    //     std::string str;
    //     std::deque<std::string> arrayVars;
    //     gpobj->m_variables.getVarlistFromRegExp(arrayVars, strSearch);

    //     if (arrayVars.size()) {

    //         //
    //         +;LSTVAR;ordinal;cnt;name;type;userid;accessrights;persistance;last_change
    //         for (int i = 0; i < arrayVars.size(); i++) {
    //             if (0 != gpobj->m_variables.find(arrayVars[i],
    //             pSession->m_pClientItem->m_pUserItem,variable)) {

    //                 str = vscp_str_format(
    //                   ("+;LSTVAR;%d;%zu;"), i, arrayVars.size());
    //                 str += variable.getAsString();

    //                 mg_websocket_write(conn,
    //                                     MG_WEBSOCKET_OPCODE_TEXT,
    //                                     (const char *)str.c_str(),
    //                                     str.length());
    //             }
    //         }

    //     } else {

    //         str = vscp_str_format(("-;LSTVAR;%d;%s"),
    //                                  (int)WEBSOCK_ERROR_VARIABLE_UNKNOWN,
    //                                  WEBSOCK_STR_ERROR_VARIABLE_UNKNOWN);

    //         mg_websocket_write(conn,
    //                             MG_WEBSOCKET_OPCODE_TEXT,
    //                             (const char *)str.c_str(),
    //                             str.length());
    //     }

    //     pthread_mutex_unlock(&gpobj->m_variableMutex);

    //     /* TODO
    //             // Send count
    //             strResult = vscp_str_format( ( "+;LISTVAR;%zu" ),
    //        m_gpobj->m_VSCP_Variables.m_listVariable.size() );
    //             mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, ( const
    //             char *
    //        )strResult.c_str() );

    //             listVscpVariable::iterator it;
    //             for( it = m_gpobj->m_VSCP_Variables.m_listVariable.begin();
    //                         it !=
    //        m_gpobj->m_VSCP_Variables.m_listVariable.end();
    //                         ++it ) {

    //                 if ( NULL == ( pvar = *it ) ) continue;

    //                 strResult = ("+;LISTVAR;");
    //                 strResult += vscp_str_format( ("%d;"), i++ );
    //                 strResult += pvar->getName();
    //                 strResult += (";");
    //                 strWork.Printf( ("%d"), pvar->getType() ) ;
    //                 strResult += strWork;
    //                 if ( pvar->isPersistent() ) {
    //                     strResult += (";true;");
    //                 }
    //                 else {
    //                     strResult += (";false;");
    //                 }

    //                 pvar->writeValueToString( strWork );
    //                 strResult += strWork;

    //                 mg_printf_websocket_frame( nc, WEBSOCKET_OP_TEXT, (const
    //        char *)strResult.c_str() );
    //             }

    //             pthread_mutex_unlock(&m_gpobj->m_variableMutex);
    //             */

    // }

    // ------------------------------------------------------------------------
    //                              VERSION
    //-------------------------------------------------------------------------

    else if (vscp_startsWith(strTok, "VERSION")) {

        std::string strvalue;

        std::string strResult = ("+;VERSION;");
        strResult += VSCPD_DISPLAY_VERSION;
        strResult += (";");
        strResult += vscp_str_format(("%d.%d.%d.%d"),
                                     VSCPD_MAJOR_VERSION,
                                     VSCPD_MINOR_VERSION,
                                     VSCPD_RELEASE_VERSION,
                                     VSCPD_BUILD_VERSION);
        // Positive reply
        mg_websocket_write(conn,
                           MG_WEBSOCKET_OPCODE_TEXT,
                           (const char *)strResult.c_str(),
                           strResult.length());

    }

    // ------------------------------------------------------------------------
    //                              COPYRIGHT
    //-------------------------------------------------------------------------

    else if (vscp_startsWith(strTok, "COPYRIGHT")) {

        std::string strvalue;

        std::string strResult = ("+;COPYRIGHT;");
        strResult += VSCPD_COPYRIGHT;

        // Positive reply
        mg_websocket_write(conn,
                           MG_WEBSOCKET_OPCODE_TEXT,
                           (const char *)strResult.c_str(),
                           strResult.length());
    }
}

// ----------------------------------------------------------------------------
//                                  WS2
// ----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
// ws2_connectHandler
//

int
ws2_connectHandler(const struct mg_connection *conn, void *cbdata)
{
    struct mg_context *ctx = mg_get_context(conn);
    int reject             = 1;

    // Check pointers
    if (NULL == conn) return 1;
    if (NULL == ctx) return 1;

    mg_lock_context(ctx);
    websock_session *pSession = websock_new_session(conn);

    if (NULL != pSession) {
        reject = 0;
    }
    mg_unlock_context(ctx);

    syslog(LOG_ERR,
           "[Websocket] Connection: client %s",
           (reject ? "rejected" : "accepted"));

    return reject;
}

////////////////////////////////////////////////////////////////////////////////
// ws2_closeHandler
//

void
ws2_closeHandler(const struct mg_connection *conn, void *cbdata)
{
    struct mg_context *ctx = mg_get_context(conn);
    websock_session *pSession =
      (websock_session *)mg_get_user_connection_data(conn);

    if (NULL == conn) return;
    if (NULL == pSession) return;
    if (pSession->m_conn != conn) return;
    if (pSession->m_conn_state < WEBSOCK_CONN_STATE_CONNECTED) return;

    mg_lock_context(ctx);

    // Record activity
    pSession->lastActiveTime = time(NULL);

    pSession->m_conn_state = WEBSOCK_CONN_STATE_NULL;
    pSession->m_conn       = NULL;
    gpobj->m_clientList.removeClient(pSession->m_pClientItem);
    pSession->m_pClientItem = NULL;

    pthread_mutex_lock(&gpobj->m_websocketSessionMutex);
    /*  TODO - Must remove session - gpobj->m_websocketSessions.DeleteContents(
    true ); if ( !gpobj->m_websocketSessions.DeleteObject( pSession )  ) {
        syslog( LOG_ERR,"[Websocket] Failed to delete session object.");
    }*/
    pthread_mutex_unlock(&gpobj->m_websocketSessionMutex);

    mg_unlock_context(ctx);
}

#define WS2_AUTH0_TEMPLATE                                                     \
    "{"                                                                        \
    "    \"type\" : \"+\", "                                                   \
    "    \"args\" : [\"AUTH0\",\"%s\"]"                                        \
    "}"

////////////////////////////////////////////////////////////////////////////////
// ws2_readyHandler
//

void
ws2_readyHandler(struct mg_connection *conn, void *cbdata)
{
    websock_session *pSession =
      (websock_session *)mg_get_user_connection_data(conn);

    // Check pointers
    if (NULL == conn) return;
    if (NULL == pSession) return;
    if (pSession->m_conn != conn) return;
    if (pSession->m_conn_state < WEBSOCK_CONN_STATE_CONNECTED) return;

    // Record activity
    pSession->lastActiveTime = time(NULL);

    // Start authentication
    /* Auth0 response
        {
            "type" : "+"
            "args" : ["AUTH0","%s"]
        }
    */
    std::string str = vscp_str_format(WS2_AUTH0_TEMPLATE, pSession->m_sid);
    mg_websocket_write(
      conn, MG_WEBSOCKET_OPCODE_TEXT, (const char *)str.c_str(), str.length());

    pSession->m_conn_state = WEBSOCK_CONN_STATE_DATA;
}

////////////////////////////////////////////////////////////////////////////////
// ws2_dataHandler
//

int
ws2_dataHandler(
  struct mg_connection *conn, int bits, char *data, size_t len, void *cbdata)
{
    std::string strWsPkt;
    websock_session *pSession =
      (websock_session *)mg_get_user_connection_data(conn);

    // Check pointers
    if (NULL == conn) return WEB_ERROR;
    if (NULL == pSession) return WEB_ERROR;
    if (pSession->m_conn != conn) return WEB_ERROR;
    if (pSession->m_conn_state < WEBSOCK_CONN_STATE_CONNECTED) return WEB_ERROR;

    // Record activity
    pSession->lastActiveTime = time(NULL);

    switch (((unsigned char)bits) & 0x0F) {

        case MG_WEBSOCKET_OPCODE_CONTINUATION:

            // Save and concatenate mesage
            pSession->m_strConcatenated += std::string(data, len);

            // if last process is
            if (1 & bits) {
                if (!ws1_message(conn, pSession, pSession->m_strConcatenated)) {
                    return WEB_ERROR;
                }
            }
            break;

        // https://developer.mozilla.org/en-US/docs/Web/API/WebSockets_API/Writing_WebSocket_servers
        case MG_WEBSOCKET_OPCODE_TEXT:
            if (1 & bits) {
                strWsPkt = std::string(data, len);
                if (!ws1_message(conn, pSession, strWsPkt)) {
                    return WEB_ERROR;
                }
            } else {
                // Store first part
                pSession->m_strConcatenated = std::string(data, len);
            }
            break;

        case MG_WEBSOCKET_OPCODE_BINARY:
            break;

        case MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE:
            break;

        case MG_WEBSOCKET_OPCODE_PING:
            // fprintf( stdout, "Ping received" );
            break;

        case MG_WEBSOCKET_OPCODE_PONG:
            // fprintf( stdout, "Pong received" );
            break;

        default:
            break;
    }

    return WEB_OK;
}

///////////////////////////////////////////////////////////////////////////////
// ws2_message
//

bool
ws2_message(struct mg_connection *conn,
            websock_session *pSession,
            std::string &strWsPkt)
{
    w2msg msg;
    std::string str;
    json json_obj; // Command obj, event obj etc

    // Check pointer
    if (NULL == conn) return false;
    if (NULL == pSession) return false;

    /*
    {
        "type": "event(E)|command(C)|response(+)|variable(V),
    }
    */
    try {
        json json_pkg = json::parse(strWsPkt.c_str());

        // "type": "event(E)|command(C)|response(+)|variable(V)
        if (json_pkg.find("type") != json_pkg.end()) {

            std::string str = json_pkg.at("type").get<std::string>();
            vscp_trim(str);
            vscp_makeUpper(str);

            // Command
            if ("COMMAND" == str) {
                msg.m_type = MSG_TYPE_COMMAND;
                try {
                    for (auto it = json_pkg.begin(); it != json_pkg.end();
                         ++it) {
                        if ("cmd" == it.key()) {
                            str      = it.value();
                            json_obj = json::parse(str);
                            return ws2_command(conn, pSession, json_obj);
                        }
                    }
                } catch (...) {
                    syslog(LOG_ERR,
                           "Failed to parse ws2 websocket command object %s",
                           strWsPkt.c_str());
                    return false;
                }
            }
            // Event
            else if ("EVENT" == str) {
                msg.m_type = MSG_TYPE_EVENT;
                try {
                    for (auto it = json_pkg.begin(); it != json_pkg.end();
                         ++it) {
                        if ("event" == it.key()) {

                            str      = it.value();
                            json_obj = json::parse(str);

                            // Client must be authorised to send events
                            if ((NULL == pSession->m_pClientItem) ||
                                !pSession->m_pClientItem->bAuthenticated) {

                                str = vscp_str_format(
                                  WS2_NEGATIVE_RESPONSE,
                                  "EVENT",
                                  (int)WEBSOCK_ERROR_NOT_AUTHORISED,
                                  WEBSOCK_STR_ERROR_NOT_AUTHORISED);
                                mg_websocket_write(conn,
                                                   MG_WEBSOCKET_OPCODE_TEXT,
                                                   (const char *)str.c_str(),
                                                   str.length());
                                return false;
                            }

                            vscpEvent ev;
                            if (vscp_convertXMLToEvent(&ev, str)) {

                                // If GUID is all null give it GUID of interface
                                if (vscp_isGUIDEmpty(ev.GUID)) {
                                    pSession->m_pClientItem->m_guid.writeGUID(
                                      ev.GUID);
                                }

                                // Check if this user is allowed to send this
                                // event
                                if (!pSession->m_pClientItem->m_pUserItem
                                       ->isUserAllowedToSendEvent(
                                         ev.vscp_class, ev.vscp_type)) {
                                    syslog(
                                      LOG_ERR,
                                      "websocket] User [%s] not allowed to "
                                      "send event class=%d type=%d.",
                                      pSession->m_pClientItem->m_pUserItem
                                        ->getUserName()
                                        .c_str(),
                                      ev.vscp_class,
                                      ev.vscp_type);
                                    return false;
                                }

                                ev.obid = pSession->m_pClientItem->m_clientID;
                                if (websock_sendevent(conn, pSession, &ev)) {
                                    mg_websocket_write(conn,
                                                       MG_WEBSOCKET_OPCODE_TEXT,
                                                       "+;EVENT",
                                                       7);
                                } else {
                                    str = vscp_str_format(
                                      ("-;%d;%s"),
                                      (int)WEBSOCK_ERROR_TX_BUFFER_FULL,
                                      WEBSOCK_STR_ERROR_TX_BUFFER_FULL);
                                    mg_websocket_write(
                                      conn,
                                      MG_WEBSOCKET_OPCODE_TEXT,
                                      (const char *)str.c_str(),
                                      str.length());
                                    return false;
                                }
                            }
                        }
                    }
                } catch (...) {
                    syslog(LOG_ERR,
                           "Failed to parse ws2 websocket event object %s",
                           strWsPkt.c_str());
                    return false;
                }
            }
            // Positive response
            else if ("+" == str) {
                msg.m_type = MSG_TYPE_RESPONSE_POSITIVE;
                try {
                    for (auto it = json_pkg.begin(); it != json_pkg.end();
                         ++it) {
                        if ("response" == it.key()) {
                            str      = it.value();
                            json_obj = json::parse(str);
                            break;
                        }
                    }
                } catch (...) {
                    syslog(LOG_ERR,
                           "Failed to parse ws2 websocket + response object %s",
                           strWsPkt.c_str());
                    return false;
                }
            }
            // Negative response
            else if ("-" == str) {
                msg.m_type = MSG_TYPE_RESPONSE_NEGATIVE;
                try {
                    for (auto it = json_pkg.begin(); it != json_pkg.end();
                         ++it) {
                        if ("response" == it.key()) {
                            str      = it.value();
                            json_obj = json::parse(str);
                            break;
                        }
                    }
                } catch (...) {
                    syslog(LOG_ERR,
                           "Failed to parse ws2 websocket - response object %s",
                           strWsPkt.c_str());
                    return false;
                }
            }
            // Changed variable
            else if ("VARIABLE" == str) {
                msg.m_type = MSG_TYPE_VARIABLE;
                try {
                    for (auto it = json_pkg.begin(); it != json_pkg.end();
                         ++it) {
                        if ("variable" == it.key()) {
                            str      = it.value();
                            json_obj = json::parse(str);
                            break;
                        }
                    }
                } catch (...) {
                    syslog(LOG_ERR,
                           "Failed to parse ws2 websocket variable object %s",
                           strWsPkt.c_str());
                    return false;
                }
            } else {
                // This is a type we do not recognize
                syslog(
                  LOG_ERR, "Unknown ws2 websocket type %s", strWsPkt.c_str());
                return false;
            }
        }

    } catch (...) {
        syslog(LOG_ERR,
               "Failed to parse ws2 websocket command %s",
               strWsPkt.c_str());
        return false;
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////
// ws2_command
//

bool
ws2_command(struct mg_connection *conn,
            struct websock_session *pSession,
            json &jsonObj)
{
    std::string strCmd;

    // Check pointer
    if (NULL == conn) return false;
    if (NULL == pSession) return false;

    strCmd = jsonObj["command"].get<std::string>();
    vscp_trim(strCmd);
    vscp_makeUpper(strCmd);
    syslog(LOG_DEBUG, "[Websocket ws2] Command = %s", strCmd.c_str());

    // Get arguments
    std::map<std::string, std::string> argmap;
    for (auto it = jsonObj["args"].begin(); it != jsonObj["args"].end(); ++it) {
        argmap[it.key()] = it.value();
    }

    // ------------------------------------------------------------------------
    //                                NOOP
    //-------------------------------------------------------------------------

    if ("NOOP" == strCmd) {

        std::string str = vscp_str_format(WS2_POSITIVE_RESPONSE, "NOOP","null");
        mg_websocket_write(
          conn, MG_WEBSOCKET_OPCODE_TEXT, str.c_str(), str.length());

    }

    // ------------------------------------------------------------------------
    //                               CHALLENGE
    //-------------------------------------------------------------------------

    else if ("CHALLENGE" == strCmd) {

        // Send authentication challenge
        if ((NULL == pSession->m_pClientItem) ||
            !pSession->m_pClientItem->bAuthenticated) {

            // Start authentication
            std::string strSessionId =
              vscp_str_format("{\"sid\": \"%s\"}", pSession->m_sid);
            std::string str =
              vscp_str_format(WS2_POSITIVE_RESPONSE, "AUTH0", strSessionId);
            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());
        }

    }

    // ------------------------------------------------------------------------
    //                                AUTH
    //-------------------------------------------------------------------------

    // AUTH;iv;aes128
    else if ("AUTH" == strCmd) {

        std::string str;
        std::string strUser;
        std::string strIV     = argmap["iv"];
        std::string strCrypto = argmap["crypto"];
        if (websock_authentication(conn, pSession, strIV, strCrypto)) {
            std::string userSettings;
            std::map<std::string, std::string> mapUser;
            pSession->m_pClientItem->m_pUserItem->getAsMap(mapUser);
            std::string strargs                             = "{";
            std::map<std::string, std::string>::iterator it = mapUser.begin();
            while (it != mapUser.end()) {
                strargs +=
                  vscp_str_format(" \"%s\" : \"%s\", ", it->first, it->second);
            }
            strargs += " }";
            str = vscp_str_format(
              WS2_POSITIVE_RESPONSE, "AUTH1", (const char *)strargs.c_str());
            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());
        } else {

            str = vscp_str_format(WS2_NEGATIVE_RESPONSE,
                                  "AUTH",
                                  (int)WEBSOCK_ERROR_NOT_AUTHORISED,
                                  WEBSOCK_STR_ERROR_NOT_AUTHORISED);
            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());
            pSession->m_pClientItem->bAuthenticated = false; // Authenticated
        }
    }

    // ------------------------------------------------------------------------
    //                                OPEN
    //-------------------------------------------------------------------------

    else if ("OPEN" == strCmd) {

        // Must be authorised to do this
        if ((NULL == pSession->m_pClientItem) ||
            !pSession->m_pClientItem->bAuthenticated) {

            std::string str = vscp_str_format(WS2_NEGATIVE_RESPONSE,
                                                "OPEN",
                                              (int)WEBSOCK_ERROR_NOT_AUTHORISED,
                                              WEBSOCK_STR_ERROR_NOT_AUTHORISED);
            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());

            return false; // We still leave channel open
        }

        pSession->m_pClientItem->m_bOpen = true;
        std::string str =
          vscp_str_format(WS2_POSITIVE_RESPONSE, "OPEN","null");
        mg_websocket_write(
          conn, MG_WEBSOCKET_OPCODE_TEXT, (const char *)str.c_str(), str.length());
    }

    // ------------------------------------------------------------------------
    //                                CLOSE
    //-------------------------------------------------------------------------

    else if ("CLOSE" == strCmd) {
        pSession->m_pClientItem->m_bOpen = false;
        std::string str =
          vscp_str_format(WS2_POSITIVE_RESPONSE, "CLOSE","null" );
        mg_websocket_write(
          conn, MG_WEBSOCKET_OPCODE_TEXT, (const char *)str.c_str(), str.length());
    }

    // ------------------------------------------------------------------------
    //                             SETFILTER/SF
    //-------------------------------------------------------------------------

    else if (("SETFILTER" == strCmd) || ("SF" == strCmd)) {

        unsigned char ifGUID[16];
        memset(ifGUID, 0, 16);

        // Must be authorized to do this
        if ((NULL == pSession->m_pClientItem) ||
            !pSession->m_pClientItem->bAuthenticated) {

            std::string str = vscp_str_format(WS2_NEGATIVE_RESPONSE,
                                                "SF",
                                              (int)WEBSOCK_ERROR_NOT_AUTHORISED,
                                              WEBSOCK_STR_ERROR_NOT_AUTHORISED);
            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());

            syslog(LOG_ERR,
                   "[Websocket] User/host not authorised to set a filter.");

            return false; // We still leave channel open
        }

        // Check privilege
        if ((pSession->m_pClientItem->m_pUserItem->getUserRights(0) & 0xf) <
            6) {

            std::string str = vscp_str_format(WS2_NEGATIVE_RESPONSE,
                                                "SF",
                                              (int)WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
                                              WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT);
            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());

            syslog(LOG_ERR,
                   "[Websocket] User [%s] not "
                   "allowed to set a filter.\n",
                   pSession->m_pClientItem->m_pUserItem->getUserName().c_str());
            return false; // We still leave channel open
        }

        // Get filter
        if (!argmap.empty()) {

            std::string strFilter = argmap["filter"];

            pthread_mutex_lock(
              &pSession->m_pClientItem->m_mutexClientInputQueue);
            if (!vscp_readFilterFromString(&pSession->m_pClientItem->m_filter,
                                           strFilter)) {

                std::string str = vscp_str_format(WS2_NEGATIVE_RESPONSE,
                                                "SF",
                                              (int)WEBSOCK_ERROR_SYNTAX_ERROR,
                                              WEBSOCK_STR_ERROR_SYNTAX_ERROR);
            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());

                pthread_mutex_unlock(
                  &pSession->m_pClientItem->m_mutexClientInputQueue);
                return false;
            }

            pthread_mutex_unlock(
              &pSession->m_pClientItem->m_mutexClientInputQueue);
        } else {

            std::string str = vscp_str_format(WS2_NEGATIVE_RESPONSE,
                                                "SF",
                                              (int)WEBSOCK_ERROR_SYNTAX_ERROR,
                                              WEBSOCK_STR_ERROR_SYNTAX_ERROR);
            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());

            return false;
        }

        // Get mask
        if (!argmap.empty()) {

            std::string strMask = argmap["mask"];

            pthread_mutex_lock(
              &pSession->m_pClientItem->m_mutexClientInputQueue);
            if (!vscp_readMaskFromString(&pSession->m_pClientItem->m_filter,
                                         strMask)) {

                std::string str = vscp_str_format(WS2_NEGATIVE_RESPONSE,
                                                "SF",
                                              (int)WEBSOCK_ERROR_SYNTAX_ERROR,
                                              WEBSOCK_STR_ERROR_SYNTAX_ERROR);
            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());

                pthread_mutex_unlock(
                  &pSession->m_pClientItem->m_mutexClientInputQueue);
                return false;
            }

            pthread_mutex_unlock(
              &pSession->m_pClientItem->m_mutexClientInputQueue);

        } else {
            std::string str = vscp_str_format(WS2_NEGATIVE_RESPONSE,
                                                "SF",
                                              (int)WEBSOCK_ERROR_SYNTAX_ERROR,
                                              WEBSOCK_STR_ERROR_SYNTAX_ERROR);
            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());
            return false;
        }

        // Positive response
        std::string str = vscp_str_format(WS2_POSITIVE_RESPONSE,
                                                "SF","null" );
        mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());
    }

    // ------------------------------------------------------------------------
    //                           CLRQ/CLRQUEUE
    //-------------------------------------------------------------------------

    // Clear the event queue
    else if (("CLRQUEUE" == strCmd) || ("CLRQ" == strCmd)) {

        // Must be authorised to do this
        if ((NULL == pSession->m_pClientItem) ||
            !pSession->m_pClientItem->bAuthenticated) {

            std::string str = vscp_str_format(WS2_NEGATIVE_RESPONSE,
                                                "CLRQ",
                                              (int)WEBSOCK_ERROR_NOT_AUTHORISED,
                                              WEBSOCK_STR_ERROR_NOT_AUTHORISED);
            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());

            syslog(LOG_ERR,
                   "[Websocket] User/host not authorised to clear the queue.");

            return false; // We still leave channel open
        }

        // Check privilege
        if ((pSession->m_pClientItem->m_pUserItem->getUserRights(0) & 0xf) <
            1) {

            std::string str = vscp_str_format(WS2_NEGATIVE_RESPONSE,
                                                "CLRQ",
                                              (int)WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,
                                              WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT);
            mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());

            syslog(LOG_ERR,
                   "[Websocket] User [%s] "
                   "not allowed to clear the queue.\n",
                   pSession->m_pClientItem->m_pUserItem->getUserName().c_str());
            return false; // We still leave channel open
        }

        std::deque<vscpEvent *>::iterator it;
        pthread_mutex_lock(&pSession->m_pClientItem->m_mutexClientInputQueue);

        for (it = pSession->m_pClientItem->m_clientInputQueue.begin();
             it != pSession->m_pClientItem->m_clientInputQueue.end();
             ++it) {
            vscpEvent *pEvent =
              pSession->m_pClientItem->m_clientInputQueue.front();
            pSession->m_pClientItem->m_clientInputQueue.pop_front();
            vscp_deleteEvent(pEvent);
        }

        pSession->m_pClientItem->m_clientInputQueue.clear();
        pthread_mutex_unlock(&pSession->m_pClientItem->m_mutexClientInputQueue);

        std::string str = vscp_str_format(WS2_POSITIVE_RESPONSE,
                                                "CLRQ", "null" );
        mg_websocket_write(conn,
                               MG_WEBSOCKET_OPCODE_TEXT,
                               (const char *)str.c_str(),
                               str.length());
    }

    // ------------------------------------------------------------------------
    //                              VERSION
    //-------------------------------------------------------------------------

    else if ("VERSION" == strCmd) {

        std::string strvalue;

        std::string strResult;
        strResult += vscp_str_format(("{ \"version\" : \"%d.%d.%d.%d\" }"),
                                     VSCPD_MAJOR_VERSION,
                                     VSCPD_MINOR_VERSION,
                                     VSCPD_RELEASE_VERSION,
                                     VSCPD_BUILD_VERSION);
        // Positive reply
        std::string str = vscp_str_format(WS2_POSITIVE_RESPONSE,
                                                "VERSION",
                                                strResult.c_str() );
        mg_websocket_write(conn,
                           MG_WEBSOCKET_OPCODE_TEXT,
                           (const char *)strResult.c_str(),
                           strResult.length());
    }

    // ------------------------------------------------------------------------
    //                              COPYRIGHT
    //-------------------------------------------------------------------------

    else if ("COPYRIGHT" == strCmd) {

        std::string strvalue;

        std::string strResult = ("{ \"COPYRIGHT\" : \"");
        strResult += VSCPD_COPYRIGHT;
        strResult += "\" }";

        // Positive reply
        std::string str = vscp_str_format(WS2_POSITIVE_RESPONSE,
                                                "COPYRIGHT",
                                                strResult.c_str() );
        mg_websocket_write(conn,
                           MG_WEBSOCKET_OPCODE_TEXT,
                           (const char *)strResult.c_str(),
                           strResult.length());

    } else {
        std::string str = vscp_str_format(WS2_NEGATIVE_RESPONSE,
                                          strCmd,
                                          (int)WEBSOCK_ERROR_SYNTAX_ERROR,
                                          WEBSOCK_STR_ERROR_SYNTAX_ERROR);
        return false;
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////
// ws2_xcommand
//

void
ws2_xcommand(struct mg_connection *conn,
             struct websock_session *pSession,
             std::string &strCmd)
{
    std::string str; // Worker string
    std::string strTok;

    // Check pointer
    if (NULL == conn) return;
    if (NULL == pSession) return;

    syslog(LOG_ERR, "[Websocket] Command = %s", strCmd.c_str());

    std::deque<std::string> tokens;
    vscp_split(tokens, strCmd, ";");

    // Get command
    if (!tokens.empty()) {
        strTok = tokens.front();
        tokens.pop_front();
        vscp_trim(strTok);
        vscp_makeUpper(strTok);
    } else {
        std::string str = vscp_str_format(("-;%d;%s"),
                                          (int)WEBSOCK_ERROR_SYNTAX_ERROR,
                                          WEBSOCK_STR_ERROR_SYNTAX_ERROR);
        mg_websocket_write(
          conn, MG_WEBSOCKET_OPCODE_TEXT, (const char *)str.c_str(), str.length());
        return;
    }

    // ------------------------------------------------------------------------
    //                                NOOP
    //-------------------------------------------------------------------------

    if (vscp_startsWith(strTok, "NOOP")) {

        mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, "+;NOOP", 6);

        // Send authentication challenge
        if ((NULL == pSession->m_pClientItem) ||
            !pSession->m_pClientItem->bAuthenticated) {
            // TODO
        }
    }
}