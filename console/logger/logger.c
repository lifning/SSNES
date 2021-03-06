/*  SSNES - A frontend for libretro.
 *  Copyright (C) 2010-2012 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2012 - Daniel De Matteis
 *

 * 
 *  SSNES is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  SSNES is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with SSNES.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef __CELLOS_LV2__
#include <netex/net.h>
#include <cell/sysmodule.h>
#include <netex/libnetctl.h>
#include <sys/timer.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#ifdef HW_RVL
#include <network.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "../../netplay_compat.h"

#include "logger.h"

#if !defined(PC_DEVELOPMENT_IP_ADDRESS)
#define PC_DEVELOPMENT_IP_ADDRESS "192.168.1.7"
#endif

#if !defined(PC_DEVELOPMENT_UDP_PORT)
#define PC_DEVELOPMENT_UDP_PORT 3490
#endif

static int g_sid;
static int sock;
static struct sockaddr_in target;
static char sendbuf[4096];

#ifdef HW_RVL
#define sendto(s, msg, len, flags, addr, tolen) net_sendto(s, msg, len, flags, addr, tolen)
#define socket(domain, type, protocol) net_socket(domain, type, protocol)

static int inet_pton(int af, const char *src, void *dst)
{
	if (af != AF_INET)
		return -1;
	return inet_aton (src, dst);
}
#endif

static int if_up_with(int index)
{
	(void)index;
#ifdef __CELLOS_LV2__
	int timeout_count = 10;
	int state;
	int ret;

	ret = cellNetCtlInit();
	if (ret < 0)
	{
		printf("cellNetCtlInit() failed(%x)\n", ret);
		return (-1);
	}

	for (;;)
	{
		ret = cellNetCtlGetState(&state);
		if (ret < 0)
		{
			printf("cellNetCtlGetState() failed(%x)\n", ret);
			return (-1);
		}
		if (state == CELL_NET_CTL_STATE_IPObtained)
			break;
		sys_timer_usleep(500 * 1000);
		timeout_count--;
		if (index && timeout_count < 0)
		{
			printf("if_up_with(%d) timeout\n", index);
			return (0);
		}
	}
#endif

	sock=socket(AF_INET,SOCK_DGRAM ,0);

	target.sin_family = AF_INET;
	target.sin_port = htons(PC_DEVELOPMENT_UDP_PORT);
	inet_pton(AF_INET, PC_DEVELOPMENT_IP_ADDRESS, &target.sin_addr);

	return (0);
}

static int if_down(int sid)
{
	(void)sid;
#ifdef __CELLOS_LV2__
	cellNetCtlTerm();
#endif
	return (0);
}

void logger_init (void)
{
	g_sid = if_up_with(1);
}

void logger_shutdown (void)
{
	if_down(g_sid);
}

void logger_send(const char *__format,...)
{
	va_list args;

	va_start(args,__format);
	vsnprintf(sendbuf,4000,__format, args);
	va_end(args);

	int len=strlen(sendbuf);
	sendto(sock,sendbuf,len,MSG_DONTWAIT,(struct sockaddr*)&target,sizeof(target));
}
