// ControlObject.h: interface for the CControlObject class.
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

#if !defined(CONTROLOBJECT_H__7D80016B_5EFD_40D5_94E3_6FD9C324CC7B__INCLUDED_)
#define CONTROLOBJECT_H__7D80016B_5EFD_40D5_94E3_6FD9C324CC7B__INCLUDED_

#ifdef WIN32
#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000
#endif

#include "wx/wx.h"
#include <wx/thread.h>
#include <wx/filename.h>

#include <mongoose.h>
#include <sqlite3.h>

#include <devicelist.h>
#include <clientlist.h>
#include <interfacelist.h>
#include <userlist.h>
#include <tables.h>
#include <vscp.h>
#include <vscpvariable.h>
#include <vscpautomation.h>
#include <vscptcpipclientthread.h>
#include <vscpudpclientthread.h>
#include <vscpmulticastclientthread.h>
#include <webserver_websocket.h>
#include <daemonvscp.h>
#include <dm.h>
#include <knownnodes.h>
#include <vscp.h>

// Forward declarations
class VSCPClientThread;
class CVSCPAutomation;

// Log level
enum {
    DAEMON_LOGMSG_NONE = 0,
    DAEMON_LOGMSG_NORMAL,
    DAEMON_LOGMSG_DEBUG,
    //DAEMON_LOGMSG_INFO,       // Deprecated from version 12.4.5
    //DAEMON_LOGMSG_NOTICE,     // Deprecated from version 12.4.5
    //DAEMON_LOGMSG_WARNING,    // Deprecated from version 12.4.5
    //DAEMON_LOGMSG_ERROR,      // Deprecated from version 12.4.5
    //DAEMON_LOGMSG_CRITICAL,   // Deprecated from version 12.4.5
    //DAEMON_LOGMSG_ALERT,      // Deprecated from version 12.4.5
    //DAEMON_LOGMSG_EMERGENCY,  // Deprecated from version 12.4.5
};

// Log type
enum {
    DAEMON_LOGTYPE_GENERAL = 0,
    DAEMON_LOGTYPE_SECURITY,
    DAEMON_LOGTYPE_ACCESS,
    DAEMON_LOGTYPE_DM
};

// TTL     Scope
// ----------------------------------------------------------------------
// 0       Restricted to the same host.Won't be output by any interface.
// 1       Restricted to the same subnet.Won't be forwarded by a router.
// <32     Restricted to the same site, organization or department.
// <64     Restricted to the same region.
// <128    Restricted to the same continent.
// <255    Unrestricted in scope.Global.
#define IP_MULTICAST_DEFAULT_TTL    1

// Needed on Linux
#ifndef VSCPMIN
#define VSCPMIN(X,Y) ((X) < (Y) ? (X) : (Y))
#endif

#ifndef VSCPMAX
 #define VSCPMAX(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })
#endif


#define MAX_ITEMS_RECEIVE_QUEUE                 1021
#define MAX_ITEMS_SEND_QUEUE                    1021
#define MAX_ITEMS_CLIENT_RECEIVE_QUEUE          8192

// VSCP daemon defines from vscp.h
#define VSCP_MAX_CLIENTS                        4096    // abs. max. is 0xffff
#define VSCP_MAX_DEVICES                        1024    // abs. max. is 0xffff

///////////////////////////////////////////////////////////////////////
// vscp_md5
//
// Replacement for abandon Cesanta version
//

void vscp_md5( char *digest, const unsigned char *buf, size_t len ) ;


WX_DECLARE_LIST(canalMsg, CanalMsgList);
WX_DECLARE_LIST(vscpEvent, VSCPEventList);

WX_DECLARE_STRING_HASH_MAP( wxString, HashString );


/*!
    This class implement a thread that handles
    client receive events
 */

class clientMsgWorkerThread : public wxThread {
public:

    /// Constructor
    clientMsgWorkerThread();

    /// Destructor
    virtual ~clientMsgWorkerThread();

    /*!
        Thread code entry point
     */
    virtual void *Entry();

    /*!
        called when the thread exits - whether it terminates normally or is
        stopped with Delete() (but not when it is Kill()ed!)
     */
    virtual void OnExit();

    /*!
        Termination control
     */
    bool m_bQuit;

    /*!
        Pointer to control object.
     */
    CControlObject *m_pCtrlObject;

};


/*!
    This is the class that does the main work in the daemon.
*/

class CControlObject {
public:

    /*!
        Constructor
     */
    CControlObject( void );

    /*!
        Destructor
     */
    virtual ~CControlObject( void );
    
    /*!
        Generate a random session key from a string key
        @param pKey Null terminated string key (max 255 characters)
        @param pSid Pointer to 33 byte sid that will receive sid
     */
    bool generateSessionId( const char *pKey, char *pSid );
    
    /*!
        Get server capabilities (64-bit array)
        @param pCapability Pointer to 64 bit capabilities array
        @return True on success.
     */
    bool getVscpCapabilities( uint8_t *pCapability );

    /*!
      Write log message -
    */
    void logMsg( const wxString& wxstr,
                    const uint8_t level = DAEMON_LOGMSG_NORMAL,
                    const uint8_t nType = DAEMON_LOGTYPE_GENERAL );
    
    /*!
     * Get records from log database. The variable vscp.log.database.sql
     * contains the sql expression (without "select * from log")
     * @param sql Search expression
     * @param strResult The search result will be returned in this string.
     * @return True on success.
     */
    bool searchLogDB( const char * sql, wxString& strResult );

    /*!
        General initialisation
     */
    bool init( wxString& strcfgfile, wxString& rootFolder );

    /*!
        Clean up used resources
     */
    bool cleanup( void );

    /*!
        The main worker thread
     */
    bool run( void );

    /*!
        Start worker threads for devices
        @return true on success
     */
    bool startDeviceWorkerThreads( void );

    /*!
        Stop worker threads for devices
        @return true on success
     */
    bool stopDeviceWorkerThreads( void );

    /*!
        Starting daemon worker thread
        @return true on success
     */
    bool startDaemonWorkerThread( void );

    /*!
        Stop daemon worker thread
        @return true on success
     */
    bool stopDaemonWorkerThread( void );


    /*!
        Starting TCP/IP worker thread
        @return true on success
     */
    bool startTcpWorkerThread( void );

    /*!
        Stop the TCP/IP worker thread
        @return true on success
     */
    bool stopTcpWorkerThread( void );

    /*!
        Start the UDP worker thread
    */
    bool startUDPWorkerThread( void );

    /*!
        Stop the UDP Workerthread
    */
    bool stopUDPWorkerThread( void );
    
    /*!
        Start the Multicast worker threads
    */
    bool startMulticastWorkerThreads( void );

    /*!
        Stop the Multicast Workerthreads
    */
    bool stopMulticastWorkerThreads( void );


    /*!
        Starting Client worker thread
        @return true on success
     */
    bool startClientWorkerThread( void );

    /*!
        Stop Client worker thread
        @return true on success
     */
    bool stopClientWorkerThread( void );

    /*!
        Add a new client to the clinet list

        @param Pointer to client that should be added.
        @param Normally not used but can be used to set a special
        client id.
     */
    void addClient( CClientItem *pClientItem, uint32_t id = 0 );

    /*!
        Add a known node
        @param guid Real GUID for node
        @param name Symbolic name for node.
    */
    void addKnownNode( cguid& guid, cguid& ifguid, wxString& name );

    /*!
        Remove a new client from the client list

        @param pClientItem Pointer to client that should be added.
     */
    void removeClient( CClientItem *pClientItem );

    /*!
        Get device address for primary ehernet adapter

        @param guid class
     */
    bool getMacAddress( cguid& guid );


    /*!
        Get the first IP address computer is known under

        @param pGUID Pointer to GUID class
     */
    bool getIPAddress( cguid& guid );
    
    /*!
        Read XML configuration GENEAL data
        @param strcfgfile path to configuration file.
        @return Returns true on success false on failure.
     */
    bool readXMLConfigurationGeneral( wxString& strcfgfile );

    /*!
        Read configuration data
        @param strcfgfile path to configuration file.
        @return Returns true on success false on failure.
     */
    bool readXMLConfiguration( wxString& strcfgfile );

    /*!
         Save configuration data
     */
    bool saveConfiguration( void );


    /*!
        send level II message to all clients
     */
    void sendEventToClient( CClientItem *pClientItem, vscpEvent *pEvent );

    /*!
        Send Level II event to all clients with exception
     */
    void sendEventAllClients( vscpEvent *pEvent, uint32_t excludeID = 0 );
    
    /*!
     * Send event
     * @param pClientItem Client that send the event.
     * @param pEvent Event to send
     * @return True on success false on failure.
     */
    bool sendEvent( CClientItem *pClientItem, vscpEvent *peventToSend );


    /*!
        Get clientmap index from a client id
     */
    uint32_t getClientMapFromId( uint32_t clid );

    /*!
        Get a client id from a client map index
     */
    uint32_t getClientMapFromIndex( uint32_t idx );

    /*!
        Add a client id to the clientmap
     */
    uint32_t addIdToClientMap( uint32_t clid );

    /*!
        Remove a client id to the clientmap
        @return True on success
     */
    bool removeIdFromClientMap( uint32_t clid );


    /*!
        Get the VSCP TCP/IP thread thread
        @return Pointer to the VSCP TCP/IP thread thread
    */
    VSCPClientThread *getTCPIPServer( void ) { return m_pVSCPClientThread; };

    /*!
     * Read configuration data from database.
     * The configuration database record is read after the XML file has
     * been read and will replace duplicate values, if any.
     * @return true on success
     */
     bool dbReadConfiguration( void );
          
     /*!
      * Write configuration datapait to configuration database.
      * 
      * @return true on success
      */
     bool addConfigurationValueToDatabase( const char *pName, 
                                             const char *pValue );
     
     /*!
      * Create configuration table
      * @return true on success
      */
     bool doCreateConfigurationTable( void );
     
     
     /*
      * Update field in settings table
      */
     bool updateConfigurationRecordItem( const wxString& strName, 
                                            const wxString& strValue );
     
     /*!
      * Read in UDP nodes from the database
      */
     bool readUdpNodes( void );
     
     /*!
      * Read in multicast channels from the database
      */
     bool readMulticastChannels( void );
     
    /*!
     * Create log database
     * @return true on success
     */
    bool doCreateLogTable( void );
    
    /*!
     * Create udpnode database
     * @return true on success
     */
    bool doCreateUdpNodeTable( void );
    
    /*!
     * Create multicast database
     * @return true on success
     */
    bool doCreateMulticastTable( void );
    
    /*!
     * Create user table
     */
    bool doCreateUserTable( void );
    
    /*!
     * Create driver table
     */
    bool doCreateDriverTable( void );
    
    /*!
     * Create guid table
     */
    bool doCreateGuidTable( void );
    
    /*!
     * Create location table
     */
    bool doCreateLocationTable( void );
    
    /*!
     * Create mdf table
     */
    bool doCreateMdfCacheTable( void );
    
    /*!
     * Create simpleui table
     */
    bool doCreateSimpleUiTable( void );
    
    /*!
     * Create simpleui item table
     */
    bool doCreateSimpleUiItemTable( void );
    
    /*!
     * Create zone table
     */
    bool doCreateZoneTable( void );
    
    /*!
     * Create subzone table
     */
    bool doCreateSubZoneTable( void );
    
    /*!
     * Create userdef table
     */
    bool doCreateUserdefTableTable( void );
    
    /*!
     * Get current number of records in log database
     */
    long getCountRecordsLogDB( void );
    
    
    /*!
     * Get the system key
     * 
     * @param pKey Buffer that will get 32-byte key. Can be NULL in which
     *              case the key is not copied to the param.
     * @return Pointer to the 32 byte key
     */
    uint8_t *getSystemKey( uint8_t *pKey );
    
    /*!
     * Get MD5 of system key (vscptoken)
     * 
     * @param Reference to string that will receive the MD5 of the key.
     */
    void getSystemKeyMD5( wxString &strKey );
    

    
public:

#ifdef BUILD_VSCPD_SERVICE
    HANDLE m_hEvntSource;
#endif

    wxLocale m_locale;

    // In the configuration database configurations are stored in records.
    // Normally record = 1 (default) is used )
    uint16_t m_nConfiguration;
    
    // This is the root folder for the VSCP daemon, it will look for
    // the configuration database here
    wxString m_rootFolder;
    
    /*!
        true if we should quit
     */
    bool m_bQuit;
    
    /*!
     * Debug flags
     * See vscp_debug.h for possible flags.
     */
    uint32_t m_debugFlags1;
    uint32_t m_debugFlags2;
    uint32_t m_debugFlags3;
    uint32_t m_debugFlags4;
    uint32_t m_debugFlags5;
    uint32_t m_debugFlags6;
    uint32_t m_debugFlags7;
    uint32_t m_debugFlags8;
    
    //**************************************************************************
    //                                 Security
    //**************************************************************************
    
    // Password is MD5 hash over "username:domain:password"
    wxString m_admin_user;          // Defaults to "admin"
    wxString m_admin_password;
    // Default password salt;key
    // E2D453EF99FB3FCD19E67876554A8C27;A4A86F7D7E119BA3F0CD06881E371B989B33B6D606A863B633EF529D64544F8E    
    wxString m_admin_allowfrom;     // Remotes allowed to connect from as admin. Defaults to "*"  
    wxString m_vscptoken;
    // A4A86F7D7E119BA3F0CD06881E371B989B33B6D606A863B633EF529D64544F8E
    // { 0xA4,0xA8,0x6F,0x7D,0x7E,0x11,0x9B,0xA3,0xF0,0xCD,0x06,0x88,0x1E,0x37,0x1B,0x98,
    //   0x9B,0x33,0xB6,0xD6,0x06,0xA8,0x63,0xB6,0x33,0xEF,0x52,0x9D,0x64,0x54,0x4F,0x8E };
    uint8_t m_systemKey[32];

    /*!
        User to run as for Unix
        if not ""
    */
    wxString m_runAsUser;

    /*!
        Maximum number of items in receive queue for clients
     */
    uint32_t m_maxItemsInClientReceiveQueue;

    /*!
        Name of this server
     */
    wxString m_strServerName;

    /*!
        Server GUID
        This is the GUID for the server
    */
    cguid m_guid;

    /*!
        ClientMap
        This structure maps client id's and
        The client map structure
        Maps unsigned log client id's to
        unsigned char id's for the
        GUID
    */
    uint32_t m_clientMap[ VSCP_MAX_CLIENTS ];
    

    //**************************************************************************
    //                             Logging
    //**************************************************************************

    wxMutex m_mutexLogWrite;

    /*!
        Log Level
     */
    uint8_t m_logLevel;

    
    /*!
        Log to syslog
     */
    bool m_bLogToSysLog;
    


    //**************************************************************************
    //                           Communication
    //**************************************************************************

    
    
    /////////////////////////////////////////////////////////
    //                       TCP/IP
    /////////////////////////////////////////////////////////
    
    /// net_skeleton structure
    struct mg_mgr m_mgrTcpIpServer;

    bool m_enableTcpip;
    
    /// Interface(s) used for TCP/IP connection
    wxString m_strTcpInterfaceAddress;

    

    

    
    /////////////////////////////////////////////////////////
    //                        UDP
    /////////////////////////////////////////////////////////
    
    
    /*!
     *  Info fro UDP subsystem
     */
    udpServerInfo m_udpInfo;
    
    /*!
     *  Mutex that protect the UDP info structure
     */
    wxMutex m_mutexUDPInfo;
    
    
    /////////////////////////////////////////////////////////
    //                      MULTICAST
    /////////////////////////////////////////////////////////
    
    // CHANNEL 
    
    /*!
        Multicast channel interface 
    */

    multicastInfo m_multicastInfo;
    
    /*!
     *  Mutex that protect the Multicast info structure
     */
    wxMutex m_mutexMulticastInfo;
    
    // ANNOUNCE
    
    /*!
        Enable Multicast announce interface
    */
    bool m_bMulticastAnounce;

    /// Interface(s) used for multicast announce
    wxString m_strMulticastAnnounceAddress;

    // ttl for multicast announce
    uint8_t m_ttlMultiCastAnnounce;
    
    
    
    //*****************************************************
    //            websocket/webserver interface
    //*****************************************************

    struct web_context *webctx;
    
    // Enable webserver
    bool m_bWebServer;
    
    /*!
        If true web server security is disabled and checks
        will not be performed on user nor remote address.
    */
    bool m_bDisableSecurityWebServer;

    //struct mg_mgr *m_pwebserver;     // Was mg_server

    // Path to web root
    char m_pathWebRoot[ MG_MAX_PATH ];

    // Domain for webserver and other net services
    char m_authDomain[ MG_MAX_PATH ];

    // Path to SSL certificate
    char m_pathCert[ MG_MAX_PATH ];

    /// Extra mime types
    char m_extraMimeTypes[ MG_MAX_PATH ];

    /// Extra mime types on the form "extension1=type1,extension2=type2,..."
    char m_ssi_pattern[ MG_MAX_PATH ];

    // IP ACL. By default, NULL, meaning all IPs are allowed to connect
    char m_ip_acl[ MG_MAX_PATH ];

    // CGI interpreter to use
    char m_cgiInterpreter[ MG_MAX_PATH ];

    // CGI Pattern
    char m_cgiPattern[ MG_MAX_PATH ];

    // Enable directory listing "yes"/"no"
    char m_EnableDirectoryListings[ 5 ];

    // Hide file pattern
    char m_hideFilePatterns[ MG_MAX_PATH ];

    // DAV document root. If NULL, DAV requests are going to fail.
    char m_dav_document_root[ MG_MAX_PATH ];

    // Index files
    char m_indexFiles[ MG_MAX_PATH ];

    // URL rewrites
    char m_urlRewrites[ MG_MAX_PATH ];

    // Leave as NULL to disable authentication.
    // To enable directory protection with authentication, set this to ".htpasswd"
    // Then, creating ".htpasswd" file in any directory automatically protects
    // it with digest authentication.
    // Use `mongoose` web server binary, or `htdigest` Apache utility to
    // create/manipulate passwords file.
    // Make sure `auth_domain` is set to a valid domain name.
    char m_per_directory_auth_file[ MG_MAX_PATH ];

    // Leave as NULL to disable authentication.
    // Normally, only selected directories in the document root are protected.
    // If absolutely every access to the web server needs to be authenticated,
    // regardless of the URI, set this option to the path to the passwords file.
    // Format of that file is the same as ".htpasswd" file. Make sure that file
    // is located outside document root to prevent people fetching it.
    char m_global_auth_file[ MG_MAX_PATH ];

    // webserver port as port "8080" or address + port "127.0.0.1:8080"
    // If only port will bind to all interfaces,
    wxString m_strWebServerInterfaceAddress;   // defaults to "8080"

    // Run as user
    wxString m_runAsUserWeb;

    // * * Websockets * *


    // websocket authentication is needed  (if true)
    bool m_bAuthWebsockets;

    // List of active websocket sessions
    WEBSOCKETSESSIONLIST m_websocketSessions;
         
    
    //**************************************************************************
    //                               VARIABLES
    //**************************************************************************


    /// Hash table for variables
    CVariableStorage m_variables;

    /// Mutex to protect variables
    wxMutex m_variableMutex;


        
    
    
    

    //**************************************************************************
    //                                SQLite3
    //**************************************************************************



    //*****************************************************
    //                    Databases
    //*****************************************************

    wxFileName m_path_db_vscp_daemon;   // Path to the VSCP daemon database
    sqlite3 *m_db_vscp_daemon;
    /// Mutex to protect variables
    wxMutex m_db_vscp_configMutex;      // Mutex for the configuration table

    wxFileName m_path_db_vscp_data;     // Path to the VSCP data database
    sqlite3 *m_db_vscp_data;
    
    wxFileName m_path_db_vscp_log;      // Path to the VSCP log database
    sqlite3 *m_db_vscp_log;
    

    /*!
        Event source for NT event reporting
     */
#ifdef BUILD_VSCPD_SERVICE
    HANDLE m_hEventSource;
#endif


    /*!
        Mutex for client queue
     */
    wxMutex m_wxClientMutex;


    /*!
        Mutex for device queue
     */
    wxMutex m_wxDeviceMutex;



    /// Daemon Decision Matrix Object
    CDM m_dm;


    /// Automation Object
    CVSCPAutomation m_automation;


    /*!
        Username for level II drivers
     */
    wxString m_driverUsername;

    /*!
        Password for Level II drivers
     */
    wxString m_driverPassword;

 
    
    //**************************************************************************
    //                                Lists
    //**************************************************************************
    /*!
        The list with available devices.
     */
    CDeviceList m_deviceList;
    wxMutex m_mutexDeviceList;

    /*!
        The list with active clients.
     */
    CClientList m_clientList;
    wxMutex m_mutexClientList;


    /*!
        The list of users
     */
    CUserList m_userList;
    wxMutex m_mutexUserList;

    /*!
        This is a list with defined tables
    */
    CUserTableObjList m_userTableObjects;
    wxMutex m_mutexUserTables;
    

    /*!
        This is the list with known nodes in the system
    */
    CKnownNodes m_knownNodes;
    wxMutex m_mutexKnownNodes;

    // *************************************************************************


    /*!
        Send queue

        This is the send queue for all clients attached to the system. A client
        place events here and the system distribute it to all other clients.
     */
    VSCPEventList m_clientOutputQueue;

    /*!
        Event object to indicate that there is an event in the client output queue.
     */
    wxSemaphore m_semClientOutputQueue;

    /*!
        Mutex for Level II message send queue
     */
    wxMutex m_mutexClientOutputQueue;

    // *************************************************************************


private:

    //**************************************************************************
    //                          Threads
    //**************************************************************************

    /*!
        controlobject device thread
     */
    clientMsgWorkerThread *m_pclientMsgWorkerThread;
    wxMutex m_mutexclientMsgWorkerThread;

    /*!
        The server thread for the TCP connection interface
     */
    VSCPClientThread *m_pVSCPClientThread;
    wxMutex m_mutexTcpClientListenThread;

    /*!
        The server thread for the VSCP daemon
     */
    daemonVSCPThread *m_pdaemonVSCPThread;
    wxMutex m_mutexdaemonVSCPThread;

    /*!
        The server thread for the VSCP daemon
     */
    VSCPUDPClientThread *m_pVSCPClientUDPThread;
    wxMutex m_mutexVSCPClientnUDPThread;

    /*!
        UDP Worker threads
     */
    VSCPUDPClientThread m_pudpClientThread;
    wxMutex m_mutexudpClientThread;
    

};




#endif // !defined(AFX_CONTROLOBJECT_H__7D80016B_5EFD_40D5_94E3_6FD9C324CC7B__INCLUDED_)
