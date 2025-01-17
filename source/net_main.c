/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// net_main.c

#include "quakedef.h"
#include "net_vcr.h"

qsocket_t	*net_activeSockets = NULL;
qsocket_t	*net_freeSockets = NULL;
int			net_numsockets = 0;

qboolean	serialAvailable = false;
qboolean	ipxAvailable = false;
qboolean	tcpipAvailable = false;
#ifdef PSP_NETWORKING_CODE
qboolean	tcpipAdhoc = false;
#endif

int			net_hostport;
int			DEFAULTnet_hostport = 26000;

char		my_ipx_address[NET_NAMELEN];
char		my_tcpip_address[NET_NAMELEN];

void (*GetComPortConfig) (int portNumber, int *port, int *irq, int *baud, qboolean *useModem);
void (*SetComPortConfig) (int portNumber, int port, int irq, int baud, qboolean useModem);
void (*GetModemConfig) (int portNumber, char *dialType, char *clear, char *init, char *hangup);
void (*SetModemConfig) (int portNumber, char *dialType, char *clear, char *init, char *hangup);

static qboolean	listening = false;

qboolean	slistInProgress = false;
qboolean	slistSilent = false;
qboolean	slistLocal = true;
static double	slistStartTime;
static int		slistLastShown;

static void Slist_Send(void);
static void Slist_Poll(void);
PollProcedure	slistSendProcedure = {NULL, 0.0, Slist_Send};
PollProcedure	slistPollProcedure = {NULL, 0.0, Slist_Poll};


sizebuf_t		net_message;
int				net_activeconnections = 0;

int messagesSent = 0;
int messagesReceived = 0;
int unreliableMessagesSent = 0;
int unreliableMessagesReceived = 0;

cvar_t	net_messagetimeout = {"net_messagetimeout","300"};
cvar_t	net_connecttimeout = {"net_connecttimeout","10"};	// JPG 2.01 - qkick/qflood protection
cvar_t	hostname = {"hostname", "UNNAMED"};
#ifdef PROQUAKE_EXTENSION
cvar_t	pq_password = {"pq_password", ""};					// JPG 3.00 - password protection
#endif
cvar_t	rcon_password = {"rcon_password", ""};				// JPG 3.00 - rcon password
cvar_t	rcon_server = {"rcon_server", ""};					// JPG 3.00 - rcon server
char	server_name[MAX_QPATH];								// JPG 3.50 - use the current server if rcon_server is not set

// JPG 3.00 - rcon
#define RCON_BUFF_SIZE	8192
char		rcon_buff[RCON_BUFF_SIZE];
sizebuf_t	rcon_message = {false, false, rcon_buff, RCON_BUFF_SIZE, 0};
qboolean	rcon_active = false;

qboolean	configRestored = false;
cvar_t	config_com_port = {"_config_com_port", "0x3f8", true};
cvar_t	config_com_irq = {"_config_com_irq", "4", true};
cvar_t	config_com_baud = {"_config_com_baud", "57600", true};
cvar_t	config_com_modem = {"_config_com_modem", "1", true};
cvar_t	config_modem_dialtype = {"_config_modem_dialtype", "T", true};
cvar_t	config_modem_clear = {"_config_modem_clear", "ATZ", true};
cvar_t	config_modem_init = {"_config_modem_init", "", true};
cvar_t	config_modem_hangup = {"_config_modem_hangup", "AT H", true};

#ifdef IDGODS
cvar_t	idgods = {"idgods", "0"};
#endif

int	vcrFile = -1;
qboolean recording = false;

// these two macros are to make the code more readable
#define sfunc	net_drivers[sock->driver]
#define dfunc	net_drivers[net_driverlevel]

int	net_driverlevel;


double			net_time;

double SetNetTime(void)
{
	net_time = Sys_DoubleTime();
	return net_time;
}

#ifdef PROQUAKE_EXTENSION
// JPG 3.00 - need this for linux build
#ifndef _WIN32
unsigned _lrotl (unsigned x, int s)
{
	s &= 31;
	return (x << s) | (x >> (32 - s));
}
unsigned _lrotr (unsigned x, int s)
{
	s &= 31;
	return (x >> s) | (x << (32 - s));
}
#endif
#endif

/*
===================
NET_NewQSocket

Called by drivers when a new communications endpoint is required
The sequence and buffer fields will be filled in properly
===================
*/
qsocket_t *NET_NewQSocket (void)
{
	qsocket_t	*sock;

	if (net_freeSockets == NULL)
		return NULL;

	if (net_activeconnections >= svs.maxclients)
		return NULL;

	// get one from free list
	sock = net_freeSockets;
	net_freeSockets = sock->next;

	// add it to active list
	sock->next = net_activeSockets;
	net_activeSockets = sock;

	sock->disconnected = false;
	sock->connecttime = net_time;
	strcpy (sock->address,"UNSET ADDRESS");
	sock->driver = net_driverlevel;
	sock->socket = 0;
	sock->driverdata = NULL;
	sock->canSend = true;
	sock->sendNext = false;
	sock->lastMessageTime = net_time;
	sock->ackSequence = 0;
	sock->sendSequence = 0;
	sock->unreliableSendSequence = 0;
	sock->sendMessageLength = 0;
	sock->receiveSequence = 0;
	sock->unreliableReceiveSequence = 0;
	sock->receiveMessageLength = 0;

	return sock;
}


void NET_FreeQSocket(qsocket_t *sock)
{
	qsocket_t	*s;

	// remove it from active list
	if (sock == net_activeSockets)
		net_activeSockets = net_activeSockets->next;
	else
	{
		for (s = net_activeSockets; s; s = s->next)
			if (s->next == sock)
			{
				s->next = sock->next;
				break;
			}
		if (!s)
			Sys_Error ("NET_FreeQSocket: not active\n");
	}

	// add it to free list
	sock->next = net_freeSockets;
	net_freeSockets = sock;
	sock->disconnected = true;
}


static void NET_Listen_f (void)
{
	if (Cmd_Argc () != 2)
	{
		Con_Printf ("\"listen\" is \"%u\"\n", listening ? 1 : 0);
		return;
	}

	listening = atoi(Cmd_Argv(1)) ? true : false;

	for (net_driverlevel=0 ; net_driverlevel<net_numdrivers; net_driverlevel++)
	{
		if (net_drivers[net_driverlevel].initialized == false)
			continue;
		dfunc.Listen (listening);
	}
}


static void MaxPlayers_f (void)
{
	int 	n;

	if (Cmd_Argc () != 2)
	{
		Con_Printf ("\"maxplayers\" is \"%u\"\n", svs.maxclients);
		return;
	}

	if (sv.active)
	{
		Con_Printf ("maxplayers can not be changed while a server is running.\n");
		return;
	}

	n = atoi(Cmd_Argv(1));
	if (n < 1)
		n = 1;
	if (n > svs.maxclientslimit)
	{
		n = svs.maxclientslimit;
		Con_Printf ("\"maxplayers\" set to \"%u\"\n", n);
	}

	if ((n == 1) && listening)
		Cbuf_AddText ("listen 0\n");

	if ((n > 1) && (!listening))
		Cbuf_AddText ("listen 1\n");

	svs.maxclients = n;
	if (n == 1)
	{
		Cvar_SetValueByRef (&deathmatch, 0);
#ifdef SUPPORTS_KUROK
		Cvar_SetValueByRef (&coop, 0);
#endif
	}
	else
	{
#ifdef SUPPORTS_KUROK
		if (coop.value)
			Cvar_SetValueByRef (&deathmatch, 0);
		else
            if (deathmatch.value > 1)
                Cvar_SetValueByRef (&deathmatch, deathmatch.value);
            else
#endif
                Cvar_SetValueByRef (&deathmatch, 1);
	}
}


static void NET_Port_f (void)
{
	int 	n;

	if (Cmd_Argc () != 2)
	{
		Con_Printf ("\"port\" is \"%u\"\n", net_hostport);
		return;
	}

	n = atoi(Cmd_Argv(1));
	if (n < 1 || n > 65534)
	{
		Con_Printf ("Bad value, must be between 1 and 65534\n");
		return;
	}

	DEFAULTnet_hostport = n;
	net_hostport = n;

	if (listening)
	{
		// force a change to the new port
		Cbuf_AddText ("listen 0\n");
		Cbuf_AddText ("listen 1\n");
	}
}


static void PrintSlistHeader(void)
{
	Con_Printf("Server          Map             Users\n");
	Con_Printf("--------------- --------------- -----\n");
	slistLastShown = 0;
}


static void PrintSlist(void)
{
	int n;

	for (n = slistLastShown; n < hostCacheCount; n++)
	{
		if (hostcache[n].maxusers)
			Con_Printf("%-15.15s %-15.15s %2u/%2u\n", hostcache[n].name, hostcache[n].map, hostcache[n].users, hostcache[n].maxusers);
		else
			Con_Printf("%-15.15s %-15.15s\n", hostcache[n].name, hostcache[n].map);
	}
	slistLastShown = n;
}


static void PrintSlistTrailer(void)
{
	if (hostCacheCount)
		Con_Printf("== end list ==\n\n");
	else
		Con_Printf("No Quake servers found.\n\n");
}


void NET_Slist_f (void)
{
	if (slistInProgress)
		return;

	if (! slistSilent)
	{
#ifdef SUPPORTS_KUROK
        if (IS_KUROK)
		    Con_Printf("Looking for Kurok servers...\n");
		else
#endif
		    Con_Printf("Looking for Quake servers...\n");
		PrintSlistHeader();
	}

	slistInProgress = true;
	slistStartTime = Sys_DoubleTime();

	SchedulePollProcedure(&slistSendProcedure, 0.0);
	SchedulePollProcedure(&slistPollProcedure, 0.1);

	hostCacheCount = 0;
}


static void Slist_Send(void)
{
	for (net_driverlevel=0; net_driverlevel < net_numdrivers; net_driverlevel++)
	{
		if (!slistLocal && net_driverlevel == 0)
			continue;
		if (net_drivers[net_driverlevel].initialized == false)
			continue;
		dfunc.SearchForHosts (true);
	}

	if ((Sys_DoubleTime() - slistStartTime) < 0.5)
		SchedulePollProcedure(&slistSendProcedure, 0.75);
}


static void Slist_Poll(void)
{
	for (net_driverlevel=0; net_driverlevel < net_numdrivers; net_driverlevel++)
	{
		if (!slistLocal && net_driverlevel == 0)
			continue;
		if (net_drivers[net_driverlevel].initialized == false)
			continue;
		dfunc.SearchForHosts (false);
	}

	if (! slistSilent)
		PrintSlist();

	if ((Sys_DoubleTime() - slistStartTime) < 1.5)
	{
		SchedulePollProcedure(&slistPollProcedure, 0.1);
		return;
	}

	if (! slistSilent)
		PrintSlistTrailer();
	slistInProgress = false;
	slistSilent = false;
	slistLocal = true;
}


/*
===================
NET_Connect
===================
*/

int hostCacheCount = 0;
hostcache_t hostcache[HOSTCACHESIZE];

qsocket_t *NET_Connect (char *host)
{
	qsocket_t		*ret;
	int				n, numdrivers = net_numdrivers;

	SetNetTime();

	if (host && *host == 0)
		host = NULL;

	if (host)
	{
		if (strcasecmp (host, "local") == 0)
		{
			numdrivers = 1;
			goto JustDoIt;
		}

		if (hostCacheCount)
		{
			for (n = 0; n < hostCacheCount; n++)
				if (strcasecmp (host, hostcache[n].name) == 0)
				{
					host = hostcache[n].cname;
					break;
				}
			if (n < hostCacheCount)
				goto JustDoIt;
		}
	}

	slistSilent = host ? true : false;
	NET_Slist_f ();

	while(slistInProgress)
		NET_Poll();

	if (host == NULL)
	{
		if (hostCacheCount != 1)
			return NULL;
		host = hostcache[0].cname;
		Con_Printf("Connecting to...\n%s @ %s\n\n", hostcache[0].name, host);
	}

	if (hostCacheCount)
		for (n = 0; n < hostCacheCount; n++)
			if (strcasecmp (host, hostcache[n].name) == 0)
			{
				host = hostcache[n].cname;
				break;
			}

JustDoIt:
	for (net_driverlevel=0 ; net_driverlevel<numdrivers; net_driverlevel++)
	{
		if (net_drivers[net_driverlevel].initialized == false)
			continue;
		ret = dfunc.Connect (host);
#ifdef SUPPORTS_CHEATFREE_MODE
		if (!sv.active && pq_cheatfree)
			Security_SetSeed(_lrotr(net_seed, 17), argv[0]);
#endif
		if (ret)
			return ret;
	}

	/* JPG 3.20 - this has always annoyed me so I commented it out
	if (host)
	{
		Con_Printf("\n");
		PrintSlistHeader();
		PrintSlist();
		PrintSlistTrailer();
	}
	*/

	return NULL;
}


/*
===================
NET_CheckNewConnections
===================
*/

struct
{
	double	time;
	int		op;
	long	session;
} vcrConnect;

qsocket_t *NET_CheckNewConnections (void)
{
	qsocket_t	*ret;

	SetNetTime();

	for (net_driverlevel=0 ; net_driverlevel<net_numdrivers; net_driverlevel++)
	{
		if (net_drivers[net_driverlevel].initialized == false)
			continue;
		if (net_driverlevel && listening == false)
			continue;
		ret = dfunc.CheckNewConnections ();
		if (ret)
		{
			if (recording)
			{
				vcrConnect.time = host_time;
				vcrConnect.op = VCR_OP_CONNECT;
				vcrConnect.session = (long)ret;
				Sys_FileWrite (vcrFile, &vcrConnect, sizeof(vcrConnect));
				Sys_FileWrite (vcrFile, ret->address, NET_NAMELEN);
			}

			return ret;
		}
	}

	if (recording)
	{
		vcrConnect.time = host_time;
		vcrConnect.op = VCR_OP_CONNECT;
		vcrConnect.session = 0;
		Sys_FileWrite (vcrFile, &vcrConnect, sizeof(vcrConnect));
	}

	return NULL;
}

/*
===================
NET_Close
===================
*/
void NET_Close (qsocket_t *sock)
{
	if (!sock)
		return;

	if (sock->disconnected)
		return;

	SetNetTime();

	// call the driver_Close function
	sfunc.Close (sock);

	NET_FreeQSocket(sock);
}


/*
=================
NET_GetMessage

If there is a complete message, return it in net_message

returns 0 if no data is waiting
returns 1 if a message was received
returns -1 if connection is invalid
=================
*/

struct
{
	double	time;
	int		op;
	long	session;
	int		ret;
	int		len;
} vcrGetMessage;

extern void PrintStats(qsocket_t *s);

int	NET_GetMessage (qsocket_t *sock)
{
	int ret;

	if (!sock)
		return -1;

	if (sock->disconnected)
	{
		Con_Printf("NET_GetMessage: disconnected socket\n");
		return -1;
	}

	SetNetTime();

	ret = sfunc.QGetMessage(sock);

	// see if this connection has timed out
	if (ret == 0 && sock->driver)
	{
		if (net_time - sock->lastMessageTime > net_messagetimeout.value)
		{
			NET_Close(sock);
			return -1;
		}

		// JPG 2.01 - qflood/qkick protection
		if (net_time - sock->lastMessageTime > net_connecttimeout.value && sv.active &&
			host_client && sock == host_client->netconnection && !strcmp(host_client->name, "unconnected"))
		{
			NET_Close(sock);
			return -1;
		}
	}

	if (ret > 0)
	{
#ifdef PROQUAKE_EXTENSION
		// JPG 3.20 - cheat free
		if (pq_cheatfree && sock->mod != MOD_QSMACK && (sock->mod_version < 35 || sock->encrypt))
		{
			// Con_Printf("NET_Decrypt\n");
			Security_Decode(net_message.data, net_message.data, net_message.cursize, sock->client_port);
		}
#endif

		if (sock->driver)
		{
			sock->lastMessageTime = net_time;
			if (ret == 1)
				messagesReceived++;
			else if (ret == 2)
				unreliableMessagesReceived++;
		}

		if (recording)
		{
			vcrGetMessage.time = host_time;
			vcrGetMessage.op = VCR_OP_GETMESSAGE;
			vcrGetMessage.session = (long)sock;
			vcrGetMessage.ret = ret;
			vcrGetMessage.len = net_message.cursize;
			Sys_FileWrite (vcrFile, &vcrGetMessage, 24);
			Sys_FileWrite (vcrFile, net_message.data, net_message.cursize);
		}
	}
	else
	{
		if (recording)
		{
			vcrGetMessage.time = host_time;
			vcrGetMessage.op = VCR_OP_GETMESSAGE;
			vcrGetMessage.session = (long)sock;
			vcrGetMessage.ret = ret;
			Sys_FileWrite (vcrFile, &vcrGetMessage, 20);
		}
	}

	return ret;
}


/*
==================
NET_SendMessage

Try to send a complete length+message unit over the reliable stream.
returns 0 if the message cannot be delivered reliably, but the connection
		is still considered valid
returns 1 if the message was sent properly
returns -1 if the connection died
==================
*/
struct
{
	double	time;
	int		op;
	long	session;
	int		r;
} vcrSendMessage;

#ifdef PROQUAKE_EXTENSION
// JPG 3.20 - cheat free
byte buff[NET_MAXMESSAGE];
sizebuf_t newdata;
#endif

int NET_SendMessage (qsocket_t *sock, sizebuf_t *data)
{
	int		r;

	if (!sock)
		return -1;

	if (sock->disconnected)
	{
		Con_Printf("NET_SendMessage: disconnected socket\n");
		return -1;
	}
#ifdef PROQUAKE_EXTENSION
	// JPG 3.20 - cheat free
	if (pq_cheatfree && sock->mod != MOD_QSMACK)
	{
		if (sock->mod_version < 35 || sock->encrypt == 1 || sock->encrypt == 2)	// JPG 3.50
		{
			// Con_Printf("NET_Encrypt\n");
			Security_Encode(data->data, buff, data->cursize, sock->client_port);
			newdata.data = buff;
			newdata.cursize = data->cursize;
			data = &newdata;
		}
		if (sock->encrypt == 1)
			sock->encrypt = 0;
		else if (sock->encrypt == 3)
			sock->encrypt = 2;
	}
#endif

	SetNetTime();
	r = sfunc.QSendMessage(sock, data);
	if (r == 1 && sock->driver)
		messagesSent++;

	if (recording)
	{
		vcrSendMessage.time = host_time;
		vcrSendMessage.op = VCR_OP_SENDMESSAGE;
		vcrSendMessage.session = (long)sock;
		vcrSendMessage.r = r;
		Sys_FileWrite (vcrFile, &vcrSendMessage, 20);
	}

	return r;
}


int NET_SendUnreliableMessage (qsocket_t *sock, sizebuf_t *data)
{
	int		r;

	if (!sock)
		return -1;

	if (sock->disconnected)
	{
		Con_Printf("NET_SendMessage: disconnected socket\n");
		return -1;
	}
#ifdef PROQUAKE_EXTENSION
	// JPG 3.20 - cheat free
	if (pq_cheatfree && sock->mod != MOD_QSMACK)
	{
		if (sock->mod_version < 35 || sock->encrypt == 1 || sock->encrypt == 2)	// JPG 3.50
		{
			// Con_Printf("NET_EncryptUnreliable\n");
			Security_Encode(data->data, buff, data->cursize, sock->client_port);
			newdata.data = buff;
			newdata.cursize = data->cursize;
			data = &newdata;
		}
		if (sock->encrypt == 1)
			sock->encrypt = 0;
		else if (sock->encrypt == 3)
			sock->encrypt = 2;
	}
#endif

	SetNetTime();
	r = sfunc.SendUnreliableMessage(sock, data);
	if (r == 1 && sock->driver)
		unreliableMessagesSent++;

	if (recording)
	{
		vcrSendMessage.time = host_time;
		vcrSendMessage.op = VCR_OP_SENDMESSAGE;
		vcrSendMessage.session = (long)sock;
		vcrSendMessage.r = r;
		Sys_FileWrite (vcrFile, &vcrSendMessage, 20);
	}

	return r;
}


/*
==================
NET_CanSendMessage

Returns true or false if the given qsocket can currently accept a
message to be transmitted.
==================
*/
qboolean NET_CanSendMessage (qsocket_t *sock)
{
	int		r;

	if (!sock)
		return false;

	if (sock->disconnected)
		return false;

	SetNetTime();

	r = sfunc.CanSendMessage(sock);

	if (recording)
	{
		vcrSendMessage.time = host_time;
		vcrSendMessage.op = VCR_OP_CANSENDMESSAGE;
		vcrSendMessage.session = (long)sock;
		vcrSendMessage.r = r;
		Sys_FileWrite (vcrFile, &vcrSendMessage, 20);
	}

	return r;
}


int NET_SendToAll(sizebuf_t *data, int blocktime)
{
	double		start;
	int		i, count = 0;
	qboolean	state1 [MAX_SCOREBOARD];
	qboolean	state2 [MAX_SCOREBOARD];

	for (i=0, host_client = svs.clients ; i<svs.maxclients ; i++, host_client++)
	{
		if (!host_client->netconnection)
			continue;
		if (host_client->active)
		{
			if (host_client->netconnection->driver == 0)
			{
				NET_SendMessage(host_client->netconnection, data);
				state1[i] = true;
				state2[i] = true;
				continue;
			}
			count++;
			state1[i] = false;
			state2[i] = false;
		}
		else
		{
			state1[i] = true;
			state2[i] = true;
		}
	}

	start = Sys_DoubleTime();
	while (count)
	{
		count = 0;
		for (i=0, host_client = svs.clients ; i<svs.maxclients ; i++, host_client++)
		{
			if (! state1[i])
			{
				if (NET_CanSendMessage (host_client->netconnection))
				{
					state1[i] = true;
					NET_SendMessage(host_client->netconnection, data);
				}
				else
				{
					NET_GetMessage (host_client->netconnection);
				}
				count++;
				continue;
			}

			if (! state2[i])
			{
				if (NET_CanSendMessage (host_client->netconnection))
					state2[i] = true;
				else
					NET_GetMessage (host_client->netconnection);

				count++;
				continue;
			}
		}
		if ((Sys_DoubleTime() - start) > blocktime)
			break;
	}
	return count;
}


//=============================================================================

/*
====================
NET_Init
====================
*/

void NET_Init (void)
{
	int		i, controlSocket;
	qsocket_t	*s;

	if (COM_CheckParm("-playback"))
	{
		net_numdrivers = 1;
		net_drivers[0].Init = VCR_Init;
	}

	if (COM_CheckParm("-record"))
		recording = true;

	i = COM_CheckParm ("-port");
	if (!i)
		i = COM_CheckParm ("-udpport");
	if (!i)
		i = COM_CheckParm ("-ipxport");

	if (i)
	{
		if (i < com_argc-1)
			DEFAULTnet_hostport = atoi (com_argv[i+1]);
		else
			Sys_Error ("NET_Init: you must specify a number after -port");
	}
	net_hostport = DEFAULTnet_hostport;

	if (COM_CheckParm("-listen") || cls.state == ca_dedicated)
		listening = true;
	net_numsockets = svs.maxclientslimit;
	if (cls.state != ca_dedicated)
		net_numsockets++;

	SetNetTime();

	for (i = 0; i < net_numsockets; i++)
	{
		s = (qsocket_t *)Hunk_AllocName(sizeof(qsocket_t), "qsocket");
		s->next = net_freeSockets;
		net_freeSockets = s;
		s->disconnected = true;
	}

	// allocate space for network message buffer
	SZ_Alloc (&net_message, NET_MAXMESSAGE);

	Cvar_RegisterVariable (&net_messagetimeout, NULL);
	Cvar_RegisterVariable (&net_connecttimeout, NULL);	// JPG 2.01 - qkick/qflood protection
	Cvar_RegisterVariable (&hostname, NULL);
#ifdef PROQUAKE_EXTENSION
	Cvar_RegisterVariable (&pq_password, NULL);			// JPG 3.00 - password protection
#endif
	Cvar_RegisterVariable (&rcon_password, NULL);			// JPG 3.00 - rcon password
	Cvar_RegisterVariable (&rcon_server, NULL);			// JPG 3.00 - rcon server
	Cvar_RegisterVariable (&config_com_port, NULL);
	Cvar_RegisterVariable (&config_com_irq, NULL);
	Cvar_RegisterVariable (&config_com_baud, NULL);
	Cvar_RegisterVariable (&config_com_modem, NULL);
	Cvar_RegisterVariable (&config_modem_dialtype, NULL);
	Cvar_RegisterVariable (&config_modem_clear, NULL);
	Cvar_RegisterVariable (&config_modem_init, NULL);
	Cvar_RegisterVariable (&config_modem_hangup, NULL);

#ifdef PSP_NETWORKING_CODE
	if(!host_initialized)
#endif
	{
		Cmd_AddCommand ("slist", NET_Slist_f);
		Cmd_AddCommand ("listen", NET_Listen_f);
		Cmd_AddCommand ("maxplayers", MaxPlayers_f);
		Cmd_AddCommand ("port", NET_Port_f);
	}

	// initialize all the drivers
	for (net_driverlevel=0 ; net_driverlevel<net_numdrivers ; net_driverlevel++)
		{
		controlSocket = net_drivers[net_driverlevel].Init();
		if (controlSocket == -1)
			continue;
		net_drivers[net_driverlevel].initialized = true;
		net_drivers[net_driverlevel].controlSock = controlSocket;
		if (listening)
			net_drivers[net_driverlevel].Listen (true);
		}

	if (*my_ipx_address)
		Con_DPrintf("IPX address %s\n", my_ipx_address);
	if (*my_tcpip_address)
		Con_DPrintf("TCP/IP address %s\n", my_tcpip_address);
#ifdef PROQUAKE_EXTENSION
			// JPG 3.20 - cheat free
	if (pq_cheatfreeEnabled)
	{
		net_seed = rand() ^ (rand() << 10) ^ (rand() << 20);
		net_seed &= 0x7fffffff;
		if (net_seed == 0x7fffffff)
			net_seed = 0;
		net_seed |= 1;
		if (net_seed <= 1)
			net_seed = 0x34719;
#ifdef SUPPORTS_CHEATFREE_MODE
		Security_SetSeed(net_seed, argv[0]);
#endif
	}
#endif
}

/*
====================
NET_Shutdown
====================
*/

void		NET_Shutdown (void)
{
	qsocket_t	*sock;

	SetNetTime();

	for (sock = net_activeSockets; sock; sock = sock->next)
		NET_Close(sock);

// shutdown the drivers
	for (net_driverlevel = 0; net_driverlevel < net_numdrivers; net_driverlevel++)
	{
		if (net_drivers[net_driverlevel].initialized == true)
		{
			net_drivers[net_driverlevel].Shutdown ();
			net_drivers[net_driverlevel].initialized = false;
		}
	}

	if (vcrFile != -1)
	{
		Con_Printf ("Closing vcrfile.\n");
		Sys_FileClose(vcrFile);
	}
}


static PollProcedure *pollProcedureList = NULL;

void NET_Poll(void)
{
	PollProcedure *pp;
	qboolean	useModem;

	if (!configRestored)
	{
		if (serialAvailable)
		{
			if (config_com_modem.value != 0) // Baker 3.99: changed from == 1.0 to != 0
				useModem = true;
			else
				useModem = false;
			SetComPortConfig (0, (int)config_com_port.value, (int)config_com_irq.value, (int)config_com_baud.value, useModem);
			SetModemConfig (0, config_modem_dialtype.string, config_modem_clear.string, config_modem_init.string, config_modem_hangup.string);
		}
		configRestored = true;
	}

	SetNetTime();

	for (pp = pollProcedureList; pp; pp = pp->next)
	{
		if (pp->nextTime > net_time)
			break;
		pollProcedureList = pp->next;
		pp->procedure(pp->arg);
	}
}


void SchedulePollProcedure(PollProcedure *proc, double timeOffset)
{
	PollProcedure *pp, *prev;

	proc->nextTime = Sys_DoubleTime() + timeOffset;
	for (pp = pollProcedureList, prev = NULL; pp; pp = pp->next)
	{
		if (pp->nextTime >= proc->nextTime)
			break;
		prev = pp;
	}

	if (prev == NULL)
	{
		proc->next = pollProcedureList;
		pollProcedureList = proc;
		return;
	}

	proc->next = pp;
	prev->next = proc;
}


#ifdef IDGODS
#define IDNET	0xc0f62800

qboolean IsID(struct qsockaddr *addr)
{
	if (idgods.value == 0.0)
		return false;

	if (addr->sa_family != 2)
		return false;

	if ((BigLong(*(int *)&addr->sa_data[2]) & 0xffffff00) == IDNET)
		return true;
	return false;
}
#endif
