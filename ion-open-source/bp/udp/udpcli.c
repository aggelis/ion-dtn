/*
	udpcli.c:	BP UDP-based convergence-layer input
			daemon, designed to serve as an input
			duct.

	Author: Ted Piotrowski, APL
		Scott Burleigh, JPL

	Copyright (c) 2006, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.
	
									*/
#include "udpcla.h"
#include "ipnfw.h"
#include "dtn2fw.h"

static void	interruptThread()
{
	isignal(SIGTERM, interruptThread);
	ionKillMainThread("udpcli");
}

/*	*	*	Receiver thread functions	*	*	*/

typedef struct
{
	VInduct		*vduct;
	int		ductSocket;
	int		running;
} ReceiverThreadParms;

static void	*handleDatagrams(void *parm)
{
	/*	Main loop for UDP datagram reception and handling.	*/

	ReceiverThreadParms	*rtp = (ReceiverThreadParms *) parm;
	char			*procName = "udpcli";
	AcqWorkArea		*work;
	char			*buffer;
	int			bundleLength;
	struct sockaddr_in	fromAddr;
	unsigned int		hostNbr;
	char			hostName[MAXHOSTNAMELEN + 1];
	char			senderEidBuffer[SDRSTRING_BUFSZ];
	char			*senderEid;

	snooze(1);	/*	Let main thread become interruptable.	*/
	work = bpGetAcqArea(rtp->vduct);
	if (work == NULL)
	{
		putErrmsg("udpcli can't get acquisition work area.", NULL);
		ionKillMainThread(procName);
		return NULL;
	}

	buffer = MTAKE(UDPCLA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("udpcli can't get UDP buffer.", NULL);
		ionKillMainThread(procName);
		return NULL;
	}

	/*	Can now start receiving bundles.  On failure, take
	 *	down the CLI.						*/

	while (rtp->running)
	{	
		bundleLength = receiveBytesByUDP(rtp->ductSocket, &fromAddr,
				buffer, UDPCLA_BUFSZ);
		switch (bundleLength)
		{
		case -1:
		case 0:
			putErrmsg("Can't acquire bundle.", NULL);
			ionKillMainThread(procName);

			/*	Intentional fall-through to next case.	*/

		case 1:				/*	Normal stop.	*/
			rtp->running = 0;
			continue;

		default:
			break;			/*	Out of switch.	*/
		}

		memcpy((char *) &hostNbr,
				(char *) &(fromAddr.sin_addr.s_addr), 4);
		hostNbr = ntohl(hostNbr);
		printDottedString(hostNbr, hostName);
		senderEid = senderEidBuffer;
		getSenderEid(&senderEid, hostName);
		if (bpBeginAcq(work, 0, senderEid) < 0
		|| bpContinueAcq(work, buffer, bundleLength) < 0
		|| bpEndAcq(work) < 0)
		{
			putErrmsg("Can't acquire bundle.", NULL);
			ionKillMainThread(procName);
			rtp->running = 0;
			continue;
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	writeErrmsgMemos();
	writeMemo("[i] udpcli receiver thread has ended.");

	/*	Free resources.						*/

	bpReleaseAcqArea(work);
	MRELEASE(buffer);
	return NULL;
}

/*	*	*	Main thread functions	*	*	*	*/

#if defined (VXWORKS) || defined (RTEMS) || defined (bionic)
int	udpcli(int a1, int a2, int a3, int a4, int a5,
		int a6, int a7, int a8, int a9, int a10)
{
	char	*ductName = (char *) a1;
#else
int	main(int argc, char *argv[])
{
	char	*ductName = (argc > 1 ? argv[1] : NULL);
#endif
	VInduct			*vduct;
	PsmAddress		vductElt;
	Sdr			sdr;
	Induct			duct;
	ClProtocol		protocol;
	char			*hostName;
	unsigned short		portNbr;
	unsigned int		hostNbr;
	struct sockaddr		socketName;
	struct sockaddr_in	*inetName;
	socklen_t		nameLength;
	ReceiverThreadParms	rtp;
	pthread_t		receiverThread;
	int			fd;
	char			quit = 0;

	if (ductName == NULL)
	{
		PUTS("Usage: udpcli <local host name>[:<port number>]");
		return 0;
	}

	if (bpAttach() < 0)
	{
		putErrmsg("udpcli can't attach to BP.", NULL);
		return -1;
	}

	findInduct("udp", ductName, &vduct, &vductElt);
	if (vductElt == 0)
	{
		putErrmsg("No such udp duct.", ductName);
		return -1;
	}

	if (vduct->cliPid != ERROR && vduct->cliPid != sm_TaskIdSelf())
	{
		putErrmsg("CLI task is already started for this duct.",
				itoa(vduct->cliPid));
		return -1;
	}

	/*	All command-line arguments are now validated.		*/

	sdr = getIonsdr();
	CHKZERO(sdr_begin_xn(sdr));
	sdr_read(sdr, (char *) &duct, sdr_list_data(sdr, vduct->inductElt),
			sizeof(Induct));
	sdr_read(sdr, (char *) &protocol, duct.protocol, sizeof(ClProtocol));
	sdr_exit_xn(sdr);
	if (protocol.nominalRate == 0)
	{
		vduct->acqThrottle.nominalRate = DEFAULT_UDP_RATE;
	}
	else
	{
		vduct->acqThrottle.nominalRate = protocol.nominalRate;
	}

	hostName = ductName;
	if (parseSocketSpec(ductName, &portNbr, &hostNbr) != 0)
	{
		putErrmsg("Can't get IP/port for host.", hostName);
		return -1;
	}

	if (portNbr == 0)
	{
		portNbr = BpUdpDefaultPortNbr;
	}

	portNbr = htons(portNbr);
	hostNbr = htonl(hostNbr);
	rtp.vduct = vduct;
	memset((char *) &socketName, 0, sizeof socketName);
	inetName = (struct sockaddr_in *) &socketName;
	inetName->sin_family = AF_INET;
	inetName->sin_port = portNbr;
	memcpy((char *) &(inetName->sin_addr.s_addr), (char *) &hostNbr, 4);
	rtp.ductSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (rtp.ductSocket < 0)
	{
		putSysErrmsg("Can't open UDP socket", NULL);
		return -1;
	}

	nameLength = sizeof(struct sockaddr);
	if (reUseAddress(rtp.ductSocket)
	|| bind(rtp.ductSocket, &socketName, nameLength) < 0
	|| getsockname(rtp.ductSocket, &socketName, &nameLength) < 0)
	{
		closesocket(rtp.ductSocket);
		putSysErrmsg("Can't initialize socket", NULL);
		return -1;
	}

	/*	Initialize sender endpoint ID lookup.			*/

	ipnInit();
	dtn2Init();

	/*	Set up signal handling; SIGTERM is shutdown signal.	*/

	ionNoteMainThread("udpcli");
	isignal(SIGTERM, interruptThread);

	/*	Start the receiver thread.				*/

	rtp.running = 1;
	if (pthread_begin(&receiverThread, NULL, handleDatagrams, &rtp))
	{
		closesocket(rtp.ductSocket);
		putSysErrmsg("udpcli can't create receiver thread", NULL);
		return -1;
	}

	/*	Now sleep until interrupted by SIGTERM, at which point
	 *	it's time to stop the induct.				*/

	{
		char	txt[500];

		isprintf(txt, sizeof(txt),
			"[i] udpcli is running, spec=[%s:%d].", 
			inet_ntoa(inetName->sin_addr), ntohs(portNbr));
		writeMemo(txt);
	}

	ionPauseMainThread(-1);

	/*	Time to shut down.					*/

	rtp.running = 0;

	/*	Wake up the receiver thread by sending it a 1-byte
	 *	datagram.						*/

	fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd >= 0)
	{
		isendto(fd, &quit, 1, 0, &socketName, sizeof(struct sockaddr));
		closesocket(fd);
	}

	pthread_join(receiverThread, NULL);
	closesocket(rtp.ductSocket);
	writeErrmsgMemos();
	writeMemo("[i] udpcli duct has ended.");
	ionDetach();
	return 0;
}
