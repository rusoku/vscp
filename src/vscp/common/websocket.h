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

#if !defined(WEBSOCKET_H__INCLUDED_)
#define WEBSOCKET_H__INCLUDED_

#include <vscp.h>
//#include <controlobject.h>

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

enum {
    WEBSOCK_ERROR_NO_ERROR,                     // Everything is OK.
    WEBSOCK_ERROR_SYNTAX_ERROR,                 // Syntax error.
    WEBSOCK_ERROR_UNKNOWN_COMMAND,              // Unknown command.
    WEBSOCK_ERROR_TX_BUFFER_FULL,               // Transmit buffer full.
    WEBSOCK_ERROR_MEMORY_ALLOCATION,            // Problem allocating memory.
    WEBSOCK_ERROR_VARIABLE_DEFINED,             // Variable is already defined.
    WEBSOCK_ERROR_VARIABLE_UNKNOWN,             // Cant find variable
    WEBSOCK_ERROR_VARIABLE_NO_STOCK,            // Cant add stock variable
    WEBSOCK_ERROR_NOT_AUTHORISED,               // Not authorised
    WEBSOCK_ERROR_NOT_ALLOWED_TO_SEND_EVENT,    // Not authorized to send events
    WEBSOCK_ERROR_NOT_ALLOWED_TO_DO_THAT,       // Not allowed to do that
    WEBSOCK_ERROR_MUST_HAVE_TABLE_NAME,         // Must have a table name
    WEBSOCK_ERROR_END_DATE_IS_WRONG,            // End date must be later than start date
    WEBSOCK_ERROR_INVALID_DATE,                 // Invalid date
    WEBSOCK_ERROR_TABLE_NOT_FOUND,              // Table not found
    WEBSOCK_ERROR_TABLE_NO_DATA,                // No data in table
    WEBSOCK_ERROR_TABLE_ERROR_READING,          // Error reading table
    WEBSOCK_ERROR_TABLE_CREATE_FORMAT,          // Table create formats was wrong
    WEBSOCK_ERROR_TABLE_DELETE_FAILED,          // Table delete failed
    WEBSOCK_ERROR_TABLE_LIST_FAILED,            // Table list failed        
    WEBSOCK_ERROR_TABLE_FAILED_TO_GET,          // Failed to get table
    WEBSOCK_ERROR_TABLE_FAILED_GET_DATA,        // Failed to get table data
    WEBSOCK_ERROR_TABLE_FAILED_CLEAR,           // Failed to clear table
    WEBSOCK_ERROR_TABLE_LOG_MISSING_VALUE,      // Missing value 
    WEBSOCK_ERROR_TABLE_LOG_FAILED,             // Failed to log data
    WEBSOCK_ERROR_TABLE_NEED_SQL,               // Missing SQL expression   
    WEBSOCK_ERROR_TABLE_FAILD_COMMAND_RECORDS,  // Failed to get # records 
    WEBSOCK_ERROR_TABLE_FAILD_COMMAND_FIRSTDATE,// Failed to get first date  
    WEBSOCK_ERROR_TABLE_FAILD_COMMAND_LASTDATE, // Failed to get last date
    WEBSOCK_ERROR_TABLE_FAILD_COMMAND_SUM,      // Failed to get sum
    WEBSOCK_ERROR_TABLE_FAILD_COMMAND_MIN,      // Failed to get min
    WEBSOCK_ERROR_TABLE_FAILD_COMMAND_MAX,      // Failed to get max
    WEBSOCK_ERROR_TABLE_FAILD_COMMAND_AVERAGE,  // Failed to get average
    WEBSOCK_ERROR_TABLE_FAILD_COMMAND_MEDIAN,   // Failed to get median
    WEBSOCK_ERROR_TABLE_FAILD_COMMAND_STDDEV,   // Failed to get stddev
    WEBSOCK_ERROR_TABLE_FAILD_COMMAND_VARIANCE, // Failed to get variance
    WEBSOCK_ERROR_TABLE_FAILD_COMMAND_MODE,     // Failed to get mode
    WEBSOCK_ERROR_TABLE_FAILD_COMMAND_UPPERQ,   // Failed to get upperq
    WEBSOCK_ERROR_TABLE_FAILD_COMMAND_LOWERQ,   // Failed to get lowerq
    WEBSOCK_ERROR_TABLE_FAILD_COMMAND_CLEAR     // Failed to clear entries
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
#define WEBSOCK_STR_ERROR_INVALID_DATE                  "Invalid date"
#define WEBSOCK_STR_ERROR_TABLE_NOT_FOUND               "Table not found"
#define WEBSOCK_STR_ERROR_TABLE_NO_DATA                 "No data in table"
#define WEBSOCK_STR_ERROR_TABLE_ERROR_READING           "Error reading table"
#define WEBSOCK_STR_ERROR_TABLE_CREATE_FORMAT           "Table create format was wrong"
#define WEBSOCK_STR_ERROR_TABLE_DELETE_FAILED           "Table delete faild"
#define WEBSOCK_STR_ERROR_TABLE_LIST_FAILED             "Table list faild"
#define WEBSOCK_STR_ERROR_TABLE_FAILED_TO_GET           "Failed to get table (is it available?)"
#define WEBSOCK_STR_ERROR_TABLE_FAILED_GET_DATA         "Failed to get table data"
#define WEBSOCK_STR_ERROR_TABLE_FAILED_CLEAR            "Failed to clear table"
#define WEBSOCK_STR_ERROR_TABLE_LOG_MISSING_VALUE       "A value is needed"
#define WEBSOCK_STR_ERROR_TABLE_LOG_FAILED              "Failed to log data"
#define WEBSOCK_STR_ERROR_TABLE_NEED_SQL                "Missing SQL expression"
#define WEBSOCK_STR_ERROR_TABLE_FAILD_COMMAND_RECORDS   "Faild to get number of records"
#define WEBSOCK_STR_ERROR_TABLE_FAILD_COMMAND_FIRSTDATE "Faild to get first date"
#define WEBSOCK_STR_ERROR_TABLE_FAILD_COMMAND_LASTDATE  "Faild to get last date"
#define WEBSOCK_STR_ERROR_TABLE_FAILD_COMMAND_SUM       "Faild to get sum"
#define WEBSOCK_STR_ERROR_TABLE_FAILD_COMMAND_MIN       "Faild to get min"
#define WEBSOCK_STR_ERROR_TABLE_FAILD_COMMAND_MAX       "Faild to get max"
#define WEBSOCK_STR_ERROR_TABLE_FAILD_COMMAND_AVERAGE   "Faild to get average"
#define WEBSOCK_STR_ERROR_TABLE_FAILD_COMMAND_MEDIAN    "Faild to get median"
#define WEBSOCK_STR_ERROR_TABLE_FAILD_COMMAND_STDDEV    "Faild to get stddev"
#define WEBSOCK_STR_ERROR_TABLE_FAILD_COMMAND_VARIANCE  "Faild to get variance"
#define WEBSOCK_STR_ERROR_TABLE_FAILD_COMMAND_MODE      "Faild to get mode"
#define WEBSOCK_STR_ERROR_TABLE_FAILD_COMMAND_UPPERQ    "Faild to get upperq"
#define WEBSOCK_STR_ERROR_TABLE_FAILD_COMMAND_LOWERQ    "Faild to get lowerq"
#define WEBSOCK_STR_ERROR_TABLE_FAILD_COMMAND_CLEAR     "Faild to clear table enteries"

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
    char m_websocket_key[33];     // Sec-WebSocket-Key 

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