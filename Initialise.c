/*
 * Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *    Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 *    Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 *    Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <c6x.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ti/sysbios/knl/Idle.h>
#include <ti/ndk/inc/netmain.h>
#include <xdc/runtime/Memory.h>
#include <xdc/runtime/System.h>
#include <ti/ndk/inc/_stack.h>
#include <ti/ndk/inc/tools/console.h>
#include <ti/ndk/inc/tools/servers.h>

#include <xdc/std.h>
#include <xdc/runtime/Error.h>
#include <xdc/runtime/System.h>
#include <xdc/runtime/IHeap.h>
#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Task.h>
#include <xdc/runtime/knl/Thread.h>
#include <xdc/cfg/global.h>
#include <ti/omp/omp.h>

#include "ti/platform/platform.h"
#include "ti/platform/resource_mgr.h"

#include "header/Config.h"
#include "header/Controller.h"

int g_network_ready = 0;
SOCKET stcp;
SOCKET client_sock;
HANDLE hCfg;

void InitialiseConnection();
void CloseConnection();

extern int32_t res_mgr_init_qmss_global(uint32_t max_num_desc);

/**************************************************************************
 ** NDK static configuration
 ****************************************************************************/

char HostName[CFG_HOSTNAME_MAX] = { 0 };
char *LocalIPAddr = "0.0.0.0"; /* Set to "0.0.0.0" for DHCP client option */
char *PCStaticIP = "192.168.2.101"; /* Static IP address for host PC */
char *EVMStaticIP = "192.168.2.100"; /*    "   IP     "   for EVM */
char *LocalIPMask = "255.255.254.0"; /* Mask for DHCP Server option */
char *GatewayIP = "192.168.2.101"; /* Not used when using DHCP */
char *DomainName = "demo.net"; /* Not used when using DHCP */
char *DNSServer = "0.0.0.0"; /* Used when set to anything but zero */

/**************************************************************************
 ** IP Stack - NDK Routines
 ***************************************************************************/

/* Our NETCTRL callback functions */
static void NetworkOpen();
static void NetworkClose();
static void NetworkIPAddr(IPN IPAddr, uint IfIdx, uint fAdd);

/*  Reporting function - IP stack calls it to give us updates */
static void ServiceReport(uint Item, uint Status, uint Report, HANDLE hCfgEntry);

#define MAX_NUM_DESC_OTHER 1024

void lld_init(void) {
	QMSS_CFG_T qmss_cfg;

	/* Initialize QMSS */
	if (platform_get_coreid() == 0) {
		qmss_cfg.master_core = 1;
	} else {
		return;
	}

	res_mgr_init_qmss_global(MAX_NUM_DESC_OTHER + MAX_NUM_DESC);

	qmss_cfg.max_num_desc = MAX_NUM_DESC;
	qmss_cfg.desc_size = MAX_DESC_SIZE;
	qmss_cfg.mem_region = Qmss_MemRegion_MEMORY_REGION_NOT_SPECIFIED;
	if (res_mgr_init_qmss(&qmss_cfg) != 0) {
		goto close_n_exit;
	}

	close_n_exit: return;
}

int master_main(void) {
	CI_SERVICE_DHCPC dhcpservice; /* Configuration data for dhcp client including handle */
	uint8_t dhcp_options[] = { DHCPOPT_SERVER_IDENTIFIER, DHCPOPT_ROUTER };
	Int status;
	//QMSS_CFG_T      qmss_cfg;
	CPPI_CFG_T cppi_cfg;

	platform_uart_init();
	platform_uart_set_baudrate(115200);
	platform_write_configure(PLATFORM_WRITE_ALL);

	platform_write("\n\nMCSDK IMAGE PROCESSING DEMONSTRATION\n\n");

	/* Initialize the components required to run this application:
	 *  (1) CPPI
	 *  (2) Packet Accelerator
	 */

	/* Initialize CPPI */
	if (platform_get_coreid() == 0) {
		cppi_cfg.master_core = 1;
	} else {
		cppi_cfg.master_core = 0;
	}
	cppi_cfg.dma_num = Cppi_CpDma_PASS_CPDMA;
	cppi_cfg.num_tx_queues = NUM_PA_TX_QUEUES;
	cppi_cfg.num_rx_channels = NUM_PA_RX_CHANNELS;
	if (res_mgr_init_cppi(&cppi_cfg) != 0) {
		//		goto close_n_exit;
	}

	if (res_mgr_init_pass() != 0) {
		platform_write("Failed to initialize the Packet Accelerator \n");
		//		goto close_n_exit;
	} else {
		platform_write("PA successfully initialized \n");
	}

	status = NC_SystemOpen(NC_PRIORITY_LOW, NC_OPMODE_INTERRUPT);
	if (status != NC_OPEN_SUCCESS) {
		platform_write("NC_SystemOpen Failed (%d)\n", status);
		//		goto close_n_exit;
	}

	/* Create a new configuration */
	hCfg = CfgNew();
	if (!hCfg) {
		platform_write("Unable to create configuration\n");
		//		goto close_n_exit;
	}

	/* Validate the length of the supplied names */
	if (strlen(DomainName) >= CFG_DOMAIN_MAX
			|| strlen(HostName) >= CFG_HOSTNAME_MAX) {
		platform_write("Domain or Host Name too long\n");
		//		goto close_n_exit;
	}

	/* Add our global hostname to hCfg (to be claimed in all connected domains) */
	CfgAddEntry(hCfg, CFGTAG_SYSINFO, CFGITEM_DHCP_HOSTNAME, 0,
			strlen(HostName), (UINT8 *) HostName, 0);

	/*
	 ** Read User SW 1
	 ** If user SW 1 = OFF position: static IP mode (default), SW 1 = ON: client mode
	 */
	if (!platform_get_switch_state(1)) {
		CI_IPNET NA;
		CI_ROUTE RT;
		IPN IPTmp;

		/* Setup an IP address to this EVM */
		bzero(&NA, sizeof(NA));
		NA.IPAddr = inet_addr(EVMStaticIP);
		NA.IPMask = inet_addr(LocalIPMask);
		strcpy(NA.Domain, DomainName);

		/* Add the address to interface 1 */
		CfgAddEntry(hCfg, CFGTAG_IPNET, 1, 0, sizeof(CI_IPNET), (UINT8 *) &NA,
				0);

		/* Add the default gateway (back to user PC) */
		bzero(&RT, sizeof(RT));
		RT.IPDestAddr = inet_addr(PCStaticIP);
		RT.IPDestMask = inet_addr(LocalIPMask);
		RT.IPGateAddr = inet_addr(GatewayIP);

		/* Add the route */
		CfgAddEntry(hCfg, CFGTAG_ROUTE, 0, 0, sizeof(CI_ROUTE), (UINT8 *) &RT,
				0);

		/* Manually add the DNS server when specified */
		IPTmp = inet_addr(DNSServer);
		if (IPTmp)
			CfgAddEntry(hCfg, CFGTAG_SYSINFO, CFGITEM_DHCP_DOMAINNAMESERVER, 0,
					sizeof(IPTmp), (UINT8 *) &IPTmp, 0);

		platform_write("EVM in StaticIP mode at %s\n", EVMStaticIP);
		platform_write("Set IP address of PC to %s\n", PCStaticIP);
	} else {
		platform_write("Configuring DHCP client\n");

		bzero(&dhcpservice, sizeof(dhcpservice));
		dhcpservice.cisargs.Mode = CIS_FLG_IFIDXVALID;
		dhcpservice.cisargs.IfIdx = 1;
		dhcpservice.cisargs.pCbSrv = &ServiceReport;
		dhcpservice.param.pOptions = dhcp_options;
		dhcpservice.param.len = 2;
		CfgAddEntry(hCfg, CFGTAG_SERVICE, CFGITEM_SERVICE_DHCPCLIENT, 0,
				sizeof(dhcpservice), (UINT8 *) &dhcpservice,
				&(dhcpservice.cisargs.hService));
	}

	/*
	 ** Configure IPStack/OS Options
	 */

	/* Set debug message level */
	status = DBG_WARN;
	CfgAddEntry(hCfg, CFGTAG_OS, CFGITEM_OS_DBGPRINTLEVEL, CFG_ADDMODE_UNIQUE,
			sizeof(uint), (UINT8 *) &status, 0);

	// TCP Receive limit
	status = 8192;
	CfgAddEntry(hCfg, CFGTAG_IP, CFGITEM_IP_SOCKTCPRXLIMIT, CFG_ADDMODE_UNIQUE,
			sizeof(uint), (UINT8 *) &status, 0);

	/*
	 ** Boot the system using this configuration
	 **
	 ** We keep booting until the function returns 0. This allows
	 ** us to have a "reboot" command.
	 */

	do {
		status = NC_NetStart(hCfg, NetworkOpen, NetworkClose, NetworkIPAddr);
		printf("here\n");
	} while (status > 0);
	//
	//	platform_write("Shutting things down\n");
	//
	//	/* Delete Configuration */
	//	CfgFree(hCfg);
	//	NC_SystemClose();
	return 0;
}

/*
 * Main Entry Point
 */
int main() {
	Task_Handle task;
	Task_Params task_params;
	Error_Block eb;

	if (DNUM == 0) {
		Error_init(&eb);
		Task_Params_init(&task_params);
		task_params.priority = 5;
		task = Task_create((ti_sysbios_knl_Task_FuncPtr)master_main, &task_params, &eb);
		if (task == NULL) {
			System_printf("Task_create() failed!\n");
			return 0;
		}
	}

	while (1) {
		if(g_network_ready == 1){
			InitialiseConnection();
			Controller(client_sock);
		}
	}
}

void InitialiseConnection() {

	client_sock = INVALID_SOCKET;
	stcp = INVALID_SOCKET;
	struct sockaddr_in sin1, client;
	int *clientlen = malloc(sizeof(struct sockaddr));

	/* Allocate the file environment for this task */
	fdOpenSession(TaskSelf());

	/* Create the main TCP listen socket */
	stcp = socket(AF_INET, SOCK_STREAMNC, IPPROTO_TCP);
	if (stcp == INVALID_SOCKET) {
		fprintf(stderr, "socket failed\n");
		//		goto leave;
	}

	/* Set Port = 7, leaving IP address = Any */
	bzero( &sin1, sizeof(struct sockaddr_in));
	sin1.sin_family = AF_INET;
	sin1.sin_len = sizeof(sin1);
	sin1.sin_port = htons(7);
	sin1.sin_addr.s_addr = inet_addr("192.168.2.100");

	/* Bind socket */
	if (bind(stcp, (PSA) &sin1, sizeof(sin1)) < 0) {
		fprintf(stderr, "bind failed\n");
	}

	/* Start listening */
	if (listen(stcp, 1) < 0) {
		fprintf(stderr, "listen failed\n");
	}

	client_sock = accept(stcp, (PSA) &client, clientlen);
	if (client_sock == NULL) {
		fprintf(stderr, "accept failed\n");
	}

//	//	/* We only get here on an error - close the sockets */
//	//	if (stcp != INVALID_SOCKET)
//	//		fdClose(stcp);
//	if (client_sock != INVALID_SOCKET)
//		fdClose(client_sock);
	//
	//	DbgPrintf(DBG_INFO, "EchoSrv Fatal Error\n");

	/* This task is killed by the system - here, we block */
	//	TaskBlock(TaskSelf());
}

void CloseConnection() {
	if (client_sock != INVALID_SOCKET)
		fdClose(client_sock);
	printf("== End TCP Echo Client Test ==\n\n");
	// Free the file descriptor environment for this Task
	fdCloseSession((HANDLE) Task_self());
	Task_exit();
	/* Delete Configuration */
	CfgFree(hCfg);
	NC_SystemClose();
}


/*************************************************************************
 *  @b EVM_init()
 *
 *  @n
 *
 *  Initializes the platform hardware. This routine is configured to start in
 *     the evm.cfg configuration file. It is the first routine that BIOS
 *     calls and is executed before Main is called. If you are debugging within
 *  CCS the default option in your target configuration file may be to execute
 *  all code up until Main as the image loads. To debug this you should disable
 *  that option.
 *
 *  @param[in]  None
 *
 *  @retval
 *      None
 ************************************************************************/
void EVM_init(void) {
	platform_init_flags sFlags;
	platform_init_config sConfig;
	/* Status of the call to initialize the platform */
	Int32 pform_status;
	/* Platform Information - we will read it form the Platform Library */
	platform_info sPlatformInfo;

	int i, j;

	if (platform_get_coreid() != 0) {
		return;
	}

	/*
	 * You can choose what to initialize on the platform by setting the following
	 * flags. We will initialize everything.
	 */
	memset((void *) &sFlags, 0, sizeof(platform_init_flags));
	memset((void *) &sConfig, 0, sizeof(platform_init_config));

	sFlags.pll = 0; /* PLLs for clocking      */
	sFlags.ddr = 0; /* External memory         */
	sFlags.tcsl = 1; /* Time stamp counter     */
	sFlags.phy = 1; /* Ethernet             */
	sFlags.ecc = 0; /* Memory ECC             */

	sConfig.pllm = 0; /* Use libraries default clock divisor */

	pform_status = platform_init(&sFlags, &sConfig);

	/* If we initialized the platform okay */
	if (pform_status == Platform_EOK) {
		/* Get information about the platform so we can use it in various places */
		memset((void *) &sPlatformInfo, 0, sizeof(platform_info));
		platform_get_info(&sPlatformInfo);
		//MultiProc_setLocalId((Uint16) platform_get_coreid());

		/* Create our host name: Its board name + last 6 digits of the serial number.
		 * Since the serial number is in I2C it can be altered or even not there so
		 * we have to take into account that it may not be what we expect.
		 */
		strcpy(HostName, "tidemo-");
		i = strlen(HostName);
		j = strlen(sPlatformInfo.serial_nbr);

		if (j > 0) {
			if (j > 6) {
				memcpy(&HostName[i], &sPlatformInfo.serial_nbr[j - 6], 6);
				HostName[i + 7] = '\0';
			} else {
				memcpy(&HostName[i], sPlatformInfo.serial_nbr, j);
				HostName[i + j + 1] = '\0';
			}
		}
	} else {
		/* Initialization of the platform failed... die */
		platform_write("Platform failed to initialize. Error code %d \n",
				pform_status);
		platform_write("We will die in an infinite loop... \n");
		while (1) {
			(void) platform_led(1, PLATFORM_LED_ON, PLATFORM_SYSTEM_LED_CLASS);
			(void) platform_delay(50000);
			(void) platform_led(1, PLATFORM_LED_OFF, PLATFORM_SYSTEM_LED_CLASS);
			(void) platform_delay(50000);
		};
	}

	return;
}

/*************************************************************************
 *  @b NetworkOpen()
 *
 *  @n
 *
 *  This function is called after the Network stack has started..
 *
 *  @param[in]  None
 *
 *  @retval
 *      None
 ************************************************************************/
static void NetworkOpen() {
	g_network_ready = 1;
	return;
}

/*************************************************************************
 *  @b NetworkClose()
 *
 *  @n
 *
 *  This function is called when the network is shutting down,
 *     or when it no longer has any IP addresses assigned to it.
 *
 *  @param[in]  None
 *
 *  @retval
 *      None
 ************************************************************************/
static void NetworkClose() {
	return;
}

/*************************************************************************
 *  @b NetworkIPAddr( IPN IPAddr, uint IfIdx, uint fAdd )
 *
 *  @n
 *
 *  This function is called whenever an IP address binding is
 *  added or removed from the system.
 *
 *  @param[in]
 *     IPAddr - The IP address we are adding or removing.
 *
 *  @param[in]
 *     IfIdx - Interface index (number). Used for multicast.
 *
 *  @param[in]
 *     fAdd -  True if we added the interface, false if its being removed.
 *
 *  @retval
 *      None
 ************************************************************************/

static void NetworkIPAddr(IPN IPAddr, uint IfIdx, uint fAdd) {
	static uint fAddGroups = 0;
	IPN IPTmp;

	if (fAdd)
		platform_write("Network Added: ");
	else
		platform_write("Network Removed: ");

	/* Print a message */
	IPTmp = ntohl(IPAddr);
	platform_write("If-%d:%d.%d.%d.%d \n", IfIdx, (UINT8) (IPTmp >> 24) & 0xFF,
			(UINT8) (IPTmp >> 16) & 0xFF, (UINT8) (IPTmp >> 8) & 0xFF,
			(UINT8) IPTmp & 0xFF);

	/* This is a good time to join any multicast group we require */
	if (fAdd && !fAddGroups) {
		fAddGroups = 1;
		/*      IGMPJoinHostGroup( inet_addr("224.1.2.3"), IfIdx ); */
	}

	return;
}

/*************************************************************************
 *  @b DHCP_reset( uint IfIdx, uint fOwnTask )
 *
 *  @n
 *
 *  This function is called whenever an IP address binding is
 *  added or removed from the system.
 *
 *  @param[in]
 *     IfIdx - Interface index (number) that is using DHCP.
 *
 *  @param[in]
 *     fOwnTask -  Set when called on a new task thread (via TaskCreate()).
 *
 *  @retval
 *      None
 ************************************************************************/
void DHCP_reset(uint IfIdx, uint fOwnTask) {
	CI_SERVICE_DHCPC dhcpc;
	HANDLE h;
	int rc, tmp;
	uint idx;

	/* If we were called from a newly created task thread, allow
	 the entity that created us to complete */
	if (fOwnTask) {
		TaskSleep(500);
	}

	/* Find DHCP on the supplied interface */
	for (idx = 1;; idx++) {
		/* Find a DHCP entry */
		rc = CfgGetEntry(0, CFGTAG_SERVICE, CFGITEM_SERVICE_DHCPCLIENT, idx,
				&h);
		if (rc != 1)
			goto RESET_EXIT;

		/* Get DHCP entry data */
		tmp = sizeof(dhcpc);
		rc = CfgEntryGetData(h, &tmp, (UINT8 *) &dhcpc);

		/* If not the right entry, continue */
		if ((rc <= 0) || dhcpc.cisargs.IfIdx != IfIdx) {
			CfgEntryDeRef(h);
			h = 0;
			continue;
		}

		/* This is the entry we want! */

		/* Remove the current DHCP service */
		CfgRemoveEntry(0, h);

		/* Specify DHCP Service on specified IF */
		bzero(&dhcpc, sizeof(dhcpc));
		dhcpc.cisargs.Mode = CIS_FLG_IFIDXVALID;
		dhcpc.cisargs.IfIdx = IfIdx;
		dhcpc.cisargs.pCbSrv = &ServiceReport;
		CfgAddEntry(0, CFGTAG_SERVICE, CFGITEM_SERVICE_DHCPCLIENT, 0,
				sizeof(dhcpc), (UINT8 *) &dhcpc, 0);
		break;
	}

	RESET_EXIT:
	/* If we are a function, return, otherwise, call TaskExit() */
	if (fOwnTask)
		TaskExit();

	return;
}

/*************************************************************************
 *  @b ServiceReport( uint Item, uint Status, uint Report, HANDLE h )
 *
 *  @n
 *
 *  Here's a quick example of using service status updates from the IP
 *  Stack. Lets store the states of the services so we can refrence them
 *  elsehwere (e.g. the information Page).
 *  The defines for the services are in the NDK header file netcfg.h
 *
 *  @param[in]
 *     Item - The service that is reporting (ie Telnet, HTTP, DHCP, etc).
 *
 *  @param[in]
 *     Status - Overall status of that service.
 *
 *  @param[in]
 *     Report - What its reporting.
 *
 *  @param[in]
 *     Handle - Handle to  the Service.
 *
 *  @retval
 *      None
 ************************************************************************/
void CheckDHCPOptions();

/*
 *  Defines for dealing with IP services so we can report on the state of them.
 * See netcfg.h in the NDK and callback in hpdspua.c.
 */
typedef struct _service_state {
	char name[10];
	uint report;
	uint status;
} Service_state_s;

/* These arrays are order dependent based on defines in the NDK header files */
char *ReportStr[] = { "", "Running", "Updated", "Complete", "Fault" };
char *StatusStr[] = { "Disabled", "Waiting", "IPTerm", "Failed", "Enabled" };

Service_state_s ServiceStatus[CFGITEM_SERVICE_MAX] = { { "Telnet", 0, 0 }, {
		"HTTP", 0, 0 }, { "NAT", 0, 0 }, { "DHCPS", 0, 0 }, { "DHCPC", 0, 0 }, {
				"DNS", 0, 0 } };

static void ServiceReport(uint Item, uint Status, uint Report, HANDLE h) {

	/* Save off the status */
	ServiceStatus[Item - 1].status = Status;
	ServiceStatus[Item - 1].report = Report;

	platform_write("Service Status: %-9s: %-9s: %-9s: %03d\n",
			ServiceStatus[Item - 1].name,
			StatusStr[ServiceStatus[Item - 1].status],
			ReportStr[ServiceStatus[Item - 1].report / 256], Report & 0xFF);

	/*
	 // Example of adding to the DHCP configuration space
	 //
	 // When using the DHCP client, the client has full control over access
	 // to the first 256 entries in the CFGTAG_SYSINFO space.
	 //
	 // Note that the DHCP client will erase all CFGTAG_SYSINFO tags except
	 // CFGITEM_DHCP_HOSTNAME. If the application needs to keep manual
	 // entries in the DHCP tag range, then the code to maintain them should
	 // be placed here.
	 //
	 // Here, we want to manually add a DNS server to the configuration, but
	 // we can only do it once DHCP has finished its programming.
	 */
	if (Item == CFGITEM_SERVICE_DHCPCLIENT && Status == CIS_SRV_STATUS_ENABLED
			&& (Report == (NETTOOLS_STAT_RUNNING | DHCPCODE_IPADD)
					|| Report == (NETTOOLS_STAT_RUNNING | DHCPCODE_IPRENEW))) {
		IPN IPTmp;

		/* Manually add the DNS server when specified */
		IPTmp = inet_addr(DNSServer);
		if (IPTmp)
			CfgAddEntry(0, CFGTAG_SYSINFO, CFGITEM_DHCP_DOMAINNAMESERVER, 0,
					sizeof(IPTmp), (UINT8 *) &IPTmp, 0);
#if 0
		/* We can now check on what the DHCP server supplied in
		 response to our DHCP option tags. */
		CheckDHCPOptions();
#endif

	}

	/* Reset DHCP client service on failure */
	if (Item == CFGITEM_SERVICE_DHCPCLIENT
			&& (Report & ~0xFF) == NETTOOLS_STAT_FAULT) {
		CI_SERVICE_DHCPC dhcpc;
		int tmp;

		/* Get DHCP entry data (for index to pass to DHCP_reset). */
		tmp = sizeof(dhcpc);
		CfgEntryGetData(h, &tmp, (UINT8 *) &dhcpc);

		/* Create the task to reset DHCP on its designated IF
		 We must use TaskCreate instead of just calling the function as
		 we are in a callback function. */
		TaskCreate(DHCP_reset, "DHCPreset", OS_TASKPRINORM, 0x1000,
				dhcpc.cisargs.IfIdx, 1, 0);
	}

	return;
}

/*************************************************************************
 *  @b CheckDHCPOptions()
 *
 *  @n
 *
 *  Checks the DHCP Options and configures them.
 *
 *  @param[in]
 *     None
 *
 *  @retval
 *      None
 ************************************************************************/
void CheckDHCPOptions() {
	char IPString[16];
	IPN IPAddr;
	int i, rc;

	/*
	 *  Now scan for DHCPOPT_SERVER_IDENTIFIER via configuration
	 */
	platform_write("\nDHCP Server ID:\n");
	for (i = 1;; i++) {
		/* Try and get a DNS server */
		rc = CfgGetImmediate(0, CFGTAG_SYSINFO, DHCPOPT_SERVER_IDENTIFIER, i, 4,
				(UINT8 *) &IPAddr);
		if (rc != 4)
			break;

		/* We got something */

		/* Convert IP to a string */
		NtIPN2Str(IPAddr, IPString);
		platform_write("DHCP Server %d = '%s'\n", i, IPString);
	}
	if (i == 1)
		platform_write("None\n\n");
	else
		platform_write("\n");

	/*  Now scan for DHCPOPT_ROUTER via the configuration */
	platform_write("Router Information:\n");
	for (i = 1;; i++) {
		/* Try and get a DNS server */
		rc = CfgGetImmediate(0, CFGTAG_SYSINFO, DHCPOPT_ROUTER, i, 4,
				(UINT8 *) &IPAddr);
		if (rc != 4)
			break;

		/* We got something */

		/* Convert IP to a string */
		NtIPN2Str(IPAddr, IPString);
		platform_write("Router %d = '%s'\n", i, IPString);
	}
	if (i == 1)
		platform_write("None\n\n");
	else
		platform_write("\n");

	return;
}
