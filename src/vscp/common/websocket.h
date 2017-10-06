// websocket.h: 
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version
// 2 of the License, or (at your option) any later version.
//
// This file is part of the VSCP (http://www.vscp.org)
//
// Copyright (C) 2000-2017 
// Ake Hedman, Grodans Paradis AB, <akhe@grodansparadis.com>
//
// This file is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this file see the file COPYING.  If not, write to
// the Free Software Foundation, 59 Temple Place - Suite 330,
// Boston, MA 02111-1307, USA.
//

#if !defined(WEBSERVER_WEBSOCKET_H__7D80016B_5EFD_40D5_94E3_6FD9C324CC7B__INCLUDED_)
#define WEBSERVER_WEBSOCKET_H__7D80016B_5EFD_40D5_94E3_6FD9C324CC7B__INCLUDED_

#include <vscp.h>
#include <controlobject.h>

//******************************************************************************
//                                WEBSOCKETS
//******************************************************************************

// websocket types
#define WEBSOCKET_SUBYPE_STANDARD   "vscp-std"      // Original form
#define WEBSOCKET_SUBTYPE_JSON      "vscp-json"     // JSON format

#define MAX_VSCPWS_MESSAGE_QUEUE    (512)

// This is the time it takes for an expired websocket session to be 
// removed by the system.
#define WEBSOCKET_EXPIRE_TIME       (2*60)

// Authentication states
enum {
    WEBSOCK_CONN_STATE_NULL = 0,
    WEBSOCK_CONN_STATE_CONNECTED,
    WEBSOCK_CONN_STATE_DATA
};

// Authentication states
/*enum {
    WEBSOCK_AUTH_STATE_START = 0,
    WEBSOCK_AUTH_STATE_SERVER_HASH,
    WEBSOCK_AUTH_STATE_CLIENT_HASH
};*/


enum {
    WEBSOCK_ERROR_NO_ERROR = 0,                 // Everything is OK.
    WEBSOCK_ERROR_SYNTAX_ERROR = 1,             // Syntax error.
    WEBSOCK_ERROR_UNKNOWN_COMMAND = 2,          // Unknown command.
    WEBSOCK_ERROR_TX_BUFFER_FULL = 3,           // Transmit buffer full.
    WEBSOCK_ERROR_MEMORY_ALLOCATION = 4,        // Problem allocating memory.
    WEBSOCK_ERROR_VARIABLE_DEFINED = 5,         // Variable is already defined.
    WEBSOCK_ERROR_VARIABLE_UNKNOWN = 6,         // Cant find variable
    WEBSOCK_ERROR_VARIABLE_NO_STOCK = 7,        // Cant add stock variable
    WEBSOCK_ERROR_NOT_AUTHORISED = 8,           // Not authorised
    WEBSOCK_ERROR_NOT_ALLOWED_TO_SEND_EVENT = 9,// Not authorized to send events
    WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT = 10,  // Not allowed to do that
    WEBSOCK_ERROR_MUST_HAVE_TABLE_NAME = 11,    // Must have a table name
    WEBSOCK_ERROR_END_DATE_IS_WRONG = 12,       // End date must be later than start date
    WEBSOCK_ERROR_TABLE_NOT_FOUND = 13,         // Table not found
    WEBSOCK_ERROR_TABLE_NO_DATA = 14,           // No data in table
    WEBSOCK_ERROR_TABLE_ERROR_READING = 15      // Error reading table
};

#define	WEBSOCK_STR_ERROR_NO_ERROR                      "Everything is OK"
#define WEBSOCK_STR_ERROR_SYNTAX_ERROR                  "Syntax error"
#define WEBSOCK_STR_ERROR_UNKNOWN_COMMAND               "Unknown command"
#define WEBSOCK_STR_ERROR_TX_BUFFER_FULL                "Transmit buffer full"
#define WEBSOCK_STR_ERROR_MEMORY_ALLOCATION             "Having problems to allocate memory"
#define WEBSOCK_STR_ERROR_VARIABLE_DEFINED              "Variable is already defined"
#define WEBSOCK_STR_ERROR_VARIABLE_UNKNOWN              "Unable to find variable"
#define WEBSOCK_STR_ERROR_VARIABLE_UPDATE               "Unable to update variable"
#define WEBSOCK_STR_ERROR_VARIABLE_NO_STOCK             "Stock variables can't be added/created"
#define WEBSOCK_STR_ERROR_NOT_AUTHORISED                "Not authorised"
#define WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_SEND_EVENT     "Not allowed to send event"
#define WEBSOCK_STR_ERROR_NOT_ALLOWED_TO_DO_THAT        "Not allowed to do that (check privileges)"
#define WEBSOCK_STR_ERROR_MUST_HAVE_TABLE_NAME          "A table name must be given as parameter"
#define WEBSOCK_STR_ERROR_END_DATE_IS_WRONG             "End date must be later than the start date"
#define WEBSOCK_STR_ERROR_TABLE_NOT_FOUND               "Table not found"
#define WEBSOCK_STR_ERROR_TABLE_NO_DATA                 "No data in table"
#define WEBSOCK_STR_ERROR_TABLE_ERROR_READING           "Error reading table"

WX_DECLARE_LIST(vscpEventFilter, TRIGGERLIST);

#define WEBSOCKET_MAINCODE_POSITIVE         "+"
#define WEBSOCKET_MAINCODE_NEGATIVE         "-"

#define WEBSOCKET_MAINCODE_COMMAND          "C"
#define WEBSOCKET_MAINCODE_EVENT            "E"
#define WEBSOCKET_MAINCODE_VARIABLE         "V"

#define WEBSOCKET_SUBCODE_VARIABLE_CHANGED  "C"
#define WEBSOCKET_SUBCODE_VARIABLE_CREATED  "N"
#define WEBSOCKET_SUBCODE_VARIABLE_DELETED  "D"

class websock_session {
    
public:    
       
    websock_session(void);
    ~websock_session(void);
    
    // Connection object
    struct web_connection *m_conn;    
    
    // Connection state (see enums above)
    int m_conn_state;  
        
    // Unique ID for this session. 
    char m_key[33];     // Sec-WebSocket-Key

    // 16 byte iv (SID) for this session
    char m_sid[33];

    // Protocol version
    int m_version;      // Sec-WebSocket-Version

    // Time when this session was last active.
    time_t lastActiveTime;
    
    // Concatenated message receive
    wxString m_strConcatenated;
        
    // Client structure for websocket
    CClientItem *m_pClientItem;

    // Event triggers
    //      Used for acknowledge automatisation
    //      VSCP_TYPE_INFORMATION_ACTION_TRIGGER
    
    bool bEventTrigger;             // True to activate event trigger functionality.
    uint32_t triggerTimeout;        // Time out before trig (or error) must occur.
    TRIGGERLIST listTriggerOK;      // List with positive triggers.
    TRIGGERLIST listTriggerERR;     // List with negative triggers.
    
    // Variable triggers
    bool bVariableTrigger;          // True to activate event trigger functionality.
};

WX_DECLARE_LIST(websock_session, WEBSOCKETSESSIONLIST);

// Public functions 

void  websock_post_incomingEvents( void );

#endif