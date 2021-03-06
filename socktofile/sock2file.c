/*
    file:   udp2file.c
    Authors:
    Linux code: Denys Fedoryshchenko aka NuclearCat <nuclearcat (at) nuclearcat.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/if_ether.h>
#include <net/ethernet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/select.h>
#include "lib.h"

static void str2ip(const char* ip, uint32_t *retval)
{
	*retval = 0;

	if (strcmp(ip, "0.0.0.0") == 0) {
		return;
	}

	if (ip[0] == 0) {
		return;
	}

	*retval = inet_addr(ip);
	if (*retval != INADDR_NONE) {
		return;
	}
}


int main(int argc, char **argv)
{
	int fd[2], fd_f = 0, proto = 0, num_fd = 0, max, sequence = 0, ret, ret2, c;
	int newline = 0;
	struct sockaddr_in srv_addr, cli_addr;
	socklen_t len;
	char message[65536];
	uint16_t port = 0;
	uint32_t ipaddr = 0, srcipaddr = 0;
	char *directory = NULL;

	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			{ "port",	required_argument,	      NULL,		    'p' },
			{ "proto",	required_argument,	      NULL,		    'x' },
			{ "directory",	required_argument,	      NULL,		    'd' },
			{ "bind",	required_argument,	      NULL,		    'b' },
			{ "sourceonly", required_argument,	      NULL,		    's' },
			{ "newline",	no_argument,		      NULL,		    'n' },
			{ "help",	no_argument,		      NULL,		    'h' },
			{ NULL,		0,			      NULL,		    0	}
		};

		c = getopt_long(argc, argv, "p:x:d:b:s:nh",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'p':
			port = atoi(optarg);
			printf("Port set to %s\n", optarg);
			break;
		case 'n':
			newline = 1;
			break;

		case 'x':
			if (!strcmp(optarg, "tcp"))
				proto = 1;
			printf("Proto set to %s\n", optarg);
			break;

		case 'd':
			directory = optarg;
			printf("Directory set to %s\n", optarg);
			break;

		case 'b':
			printf("Bind set to %s\n", optarg);
			str2ip(optarg, &ipaddr);
			break;

		case 's':
			printf("Source IP restriction set to %s\n", optarg);
			str2ip(optarg, &srcipaddr);
			break;

		case 'h':
			break;

		default:
			printf("?? getopt returned character code 0%o ??\n", c);
			exit(0);
		}
	}


	if (!port || !directory) {
		printf("%s --port N  --directory /some/path [--sourceonly x.x.x.x] [--bind x.x.x.x] [--proto (tcp|udp)] [--newline]\n", argv[0]);
		exit(0);
	}

	if (proto)
		fd[0] = socket(AF_INET, SOCK_STREAM, 0);
	else
		fd[0] = socket(AF_INET, SOCK_DGRAM, 0);

	num_fd++;
	max = fd[0];
	fd[1] = 0;

	memset(&srv_addr, 0x0, sizeof(srv_addr));
	srv_addr.sin_family = AF_INET;
	if (ipaddr)
		srv_addr.sin_addr.s_addr = ipaddr;
	else
		srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	srv_addr.sin_port = htons(port);
	if (bind(fd[0], (struct sockaddr*)&srv_addr, sizeof(srv_addr)) == -1) {
		perror("bind()");
		exit(0);
	}

	if (proto) {
		/* Enable address reuse */
		int on = 1;
		ret = setsockopt( fd[0], SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on) );
		/* Listen */
		ret = listen(fd[0], 1000);
		if (ret == -1) {
			perror("listen");
			exit(1);
		}
	}

	chdir(directory);
	fd_new_date(&fd_f, &sequence);

	daemon(1, 1);

	for (;; ) {

		if (!proto) {
			len = sizeof(cli_addr);
			ret = recvfrom(fd[0], message, sizeof(message) - 1, 0, (struct sockaddr*)&cli_addr, &len);
			ret2 = write(fd_f, message, ret);
			if (ret2 != ret)
				perror("write()");
			if (newline)
				write(fd_f, "\n", 1);
		} else {
			/* Handling tcp */
			fd_set set;
			FD_ZERO(&set);
			FD_SET(fd[0], &set);
			if (fd[1])
				FD_SET(fd[1], &set);
			select(max + 1, &set, NULL, NULL, NULL);
			if (FD_ISSET(fd[0], &set)) {
				len = sizeof(cli_addr);
				int fd_tmp = accept(fd[0], (struct sockaddr*)&cli_addr, &len);
				/* New connection */
				if (srcipaddr && cli_addr.sin_addr.s_addr != srcipaddr) {
					printf("src %d res %d\n", cli_addr.sin_addr.s_addr, srcipaddr);
					close(fd_tmp);
				} else {
					if (fd[1])
						close(fd[1]);
					fd[1] = fd_tmp;
					max = fd[1];
					num_fd = 2;
				}
			}
			if (fd[1] && FD_ISSET(fd[1], &set)) {
				ret = read(fd[1], message, sizeof(message) - 1);
				/* On error */
				if (ret <= 0) {
					/* We close also old file, data maybe corrupted, so new will be clean */
					close(fd[1]);
					close(fd_f);
					fd_f = 0;
					num_fd = 1;
					fd[1] = 0;
					max = fd[0];
				} else {
					/* Writing data */
					ret2 = write(fd_f, message, ret);
					if (ret2 != ret)
						perror("write()");
				}
			}
		}
		fd_new_date(&fd_f, &sequence);
	}
}

