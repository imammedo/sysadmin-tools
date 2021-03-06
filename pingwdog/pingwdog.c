/*
    file:   pingwdog.c
    Authors: 
    Denys Fedoryshchenko aka NuclearCat <nuclearcat (at) nuclearcat.com>

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

#include <pcap.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/if_ether.h>
#include <net/ethernet.h>
#ifdef __linux__
#include <netinet/ether.h>
#include <linux/if_tun.h>
#endif
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#ifdef __FreeBSD__
#include <net/if_tap.h>
#endif
#include <net/if.h>
#include <poll.h>
#include <unistd.h>
#include <err.h>
#include <signal.h>

#include <curl/curl.h>
#include <sys/mman.h>

#include <getopt.h>
#include <netinet/ip_icmp.h>
#include <netdb.h>
#include <sys/select.h>
#include <assert.h>
#include <syslog.h>

//TODO: remove global variable, make global struct
int background=1;

unsigned short
in_cksum(unsigned short *addr, int len)
{
    int nleft = len;
    int sum = 0;
    unsigned short *w = addr;
    unsigned short answer = 0;

    while (nleft > 1) {
        sum += *w++;
        nleft -= 2;
    }

    if (nleft == 1) {
        *(unsigned char *)(&answer) = *(unsigned char *)w;
        sum += answer;
    }

    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    answer = ~sum;
    return (answer);
}

long timevaldiff(struct timeval *starttime, struct timeval *finishtime)
{
  long msec;
  msec=(finishtime->tv_sec-starttime->tv_sec)*1000;
  msec+=(finishtime->tv_usec-starttime->tv_usec)/1000;
  return msec;
}

void exec_detached(char *string, int good) {

    pid_t pid = fork();
    if (pid)
	return;

    setsid();
    execlp(string,(good == 1 ? "good" : "bad"),NULL);
    exit(0);
}


static int ping4(char *dsthost, long maxdelay, char *bindaddr)
{
	int s,retval,icmp_len,rcvd,seq,stilldelay = maxdelay;
	int on = 1;
	struct hostent *hp;
	char buf[100];
        struct ip *ip = (struct ip *)buf;
        struct icmp *icmp = (struct icmp *)(ip + 1);
        struct sockaddr_in dst,src;
	fd_set rfds;
	struct timeval tv;
	struct timeval tvent, tvcur; // entrance time, current time
	long timediff;
	size_t dstsize;

	gettimeofday(&tvent,NULL);

        FD_ZERO(&rfds);
	memset(icmp,0x0,sizeof(struct icmp));



       if ((s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)) < 0) {
                perror("socket");
                exit(1);
        }
	if (bindaddr != NULL) {
	    struct sockaddr_in serv_addr;
	    serv_addr.sin_family = AF_INET;
	    if (!inet_pton(AF_INET, bindaddr, (struct in_addr *)&serv_addr.sin_addr.s_addr)) {
		perror("bind address invalid");
		exit(-1);
	    }
	    serv_addr.sin_port = 0;
	    if (bind(s, (struct sockaddr *) &serv_addr,
		    sizeof(serv_addr)) < 0)
	    {
		perror("bind error");
		exit(-1);
	    }
	}

        if (setsockopt(s, IPPROTO_IP, IP_HDRINCL, &on, sizeof(on)) < 0) {
                perror("IP_HDRINCL");
                exit(1);
        }

       if ((hp = gethostbyname(dsthost)) == NULL) {
                if ((ip->ip_dst.s_addr = inet_addr(dsthost)) == -1) {
                        fprintf(stderr, "%s: unknown host\n", dsthost);
                }
        } else {
                memcpy(&ip->ip_dst.s_addr, hp->h_addr_list[0], hp->h_length);
        }

        ip->ip_v = 4;
        ip->ip_hl = sizeof *ip >> 2;
        ip->ip_tos = 0;
        ip->ip_len = htons(sizeof(buf));
        ip->ip_id = 0;
        ip->ip_off = 0;
        ip->ip_ttl = 255;
        ip->ip_p = 1;
        ip->ip_sum = 0;                 /* kernel fills in */
        ip->ip_src.s_addr = 0;          /* kernel fills in */

	dst.sin_addr = ip->ip_dst;
        dst.sin_family = AF_INET;

        icmp->icmp_type = ICMP_ECHO;
        icmp->icmp_code = 0;
	icmp->icmp_hun.ih_idseq.icd_id = getpid();
	icmp->icmp_hun.ih_idseq.icd_seq = seq = htons(rand());

	icmp_len = sizeof(buf) - sizeof(struct iphdr);
        icmp->icmp_cksum = in_cksum((unsigned short *)icmp, icmp_len);

	if (sendto(s, buf, sizeof(buf), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0)
                        perror("sendto");

        FD_SET(s, &rfds);

        tv.tv_sec = maxdelay/1000;
        tv.tv_usec = (maxdelay%1000)*1000;
	
	while(1) {
	    retval = select(s+1, &rfds, NULL, NULL, &tv);
	    gettimeofday(&tvcur,NULL);
	    if (retval) {
		dstsize = sizeof(dst);
		rcvd = recvfrom(s,buf,sizeof(buf),0, (struct sockaddr *)&src, &dstsize);
		if (rcvd >= ( 8 + ip->ip_hl * 4)) {
		    if (!background) {
			printf("Got something %d == %d, %d == %d code %d == %d and cmp %d(%d)\n",icmp->icmp_hun.ih_idseq.icd_seq,seq,icmp->icmp_hun.ih_idseq.icd_id,getpid(),icmp->icmp_type,ICMP_ECHOREPLY,memcmp(&src.sin_addr,&dst.sin_addr,sizeof(src.sin_addr)),dstsize);
			printf("src %s\n", inet_ntoa(src.sin_addr));
			printf("dst %s\n", inet_ntoa(dst.sin_addr));
		    }
		    if (icmp->icmp_type == ICMP_ECHOREPLY && !memcmp(&src.sin_addr,&dst.sin_addr,sizeof(src.sin_addr)) && icmp->icmp_hun.ih_idseq.icd_id == getpid() && icmp->icmp_hun.ih_idseq.icd_seq == seq) {
			if (!background)
			    printf("VALID PACKET\n");
			close(s);
			return(1);
		    }
		} 
//		else {
//		    printf("Too short packet\n");
//		}
	    }
	    timediff = timevaldiff(&tvent,&tvcur);

	    if (retval != 1 && timediff >= maxdelay) {
		if (!background)
		    printf("No valid packet, expired\n");
		close(s);
		return(0);
	    } else {
		stilldelay -= timediff;
    		tv.tv_sec = stilldelay/1000;
    		tv.tv_usec = (stilldelay%1000)*1000;
	    }
	}
	close(s);
	return(retval);	
	
}


int main(int argc,char **argv)
{
  int interval = 5, maxfail = 5,gracetime = 60,timeout=1000,num=1,maxloss=1;
  char *onfail = NULL,*onrestore = NULL,*dsthost=NULL,*bindaddr=NULL;
  int len,c,cntbatchfails = 0,failtrigger = 2,failcount = 0,i,syslogswitch=0;
  struct sigaction sa;

  sa.sa_handler = SIG_IGN;
  sa.sa_flags = SA_NOCLDWAIT;
  if (sigaction(SIGCHLD, &sa, NULL) == -1) {
    perror("sigaction");
    exit(1);
  }

  while (1)
         {

	    static struct option long_options[] =
             {
               /* These options don't set a flag.
                  We distinguish them by their indices. */
               {"dst",  required_argument, 0, 'd'},
               {"interval",  required_argument, 0, 'i'},
               {"timeout",  required_argument, 0, 't'},
               {"num",  required_argument, 0, 'n'},
               {"loss",  required_argument, 0, 'l'},
               {"maxfail",  required_argument, 0, 'm'},
               {"gracetime",  required_argument, 0, 'g'},
               {"onfail",  required_argument, 0, 'f'},
               {"onrestore",  required_argument, 0, 'r'},
               {"bind",  required_argument, 0, 'b'},
               {"foreground",  no_argument, 0, '1'},
               {"syslog",  no_argument, 0, 's'},
               {"help",  no_argument, 0, 'h'},
               {0, 0, 0, 0}
             };
           /* getopt_long stores the option index here. */
	    int option_index = 0;
     
	    c = getopt_long (argc, argv, "d:b:p:g:f:r:b:n:lsh", long_options, &option_index);
          /* Detect the end of the options. */
           if (c == -1)
             break;
     
           switch (c)
             {
             case 'i':
		interval = atoi(optarg);
               break;

             case 'm':
		maxfail = atoi(optarg);
               break;

             case 'g':
		gracetime = atoi(optarg);
               break;

             case 't':
		timeout = atoi(optarg);
               break;

             case 'n':
		num = atoi(optarg);
               break;

             case 'l':
		maxloss = atoi(optarg);
               break;
     
             case 'b':
		len = strlen(optarg) + 1;
		bindaddr = malloc(len);
		strncpy(bindaddr,optarg,len);
               break;

             case 'f':
		len = strlen(optarg) + 1;
		onfail = malloc(len);
		strncpy(onfail,optarg,len);
               break;

             case 'r':
		len = strlen(optarg) + 1;
		onrestore = malloc(len);
		strncpy(onrestore,optarg,len);
               break;

             case 'd':
		len = strlen(optarg) + 1;
		dsthost = malloc(len);
		strncpy(dsthost,optarg,len);
               break;

             case '1':
		background = 0;
               break;

             case 's':
		syslogswitch = 1;
               break;
     
             case 'h':
		printf("Available options:\n");
		printf("--dst 		- Destination host\n");
		printf("--interval 	- Interval between pings (seconds)\n");
		printf("--num 		- Number of pings\n");
		printf("--loss		- If more or equal than this ping failed in batch (%% loss), consider batch as FAIL\n");
		printf("--maxfail 	- How much batches need to fail in row to trigger FAIL state\n");
		printf("--timeout 	- Ping timeout (milliseconds)\n");
		printf("--gracetime 	- Delay before script start actually monitoring (e.g. STP on switches takes up to 60 sec), default 60 sec\n");
		printf("--onfail 	- Script to run on failure\n");
		printf("--onrestore 	- Script to run on restore\n");
		printf("--bind	 	- Bind to IP\n");
		printf("--foreground 	- Fork to foreground\n");
               /* getopt_long already printed an error message. */
		exit(1);
               break;
     
             default:
               abort ();
             }
         }                            

    if ((onfail == NULL && onrestore == NULL) || dsthost == NULL) {
	printf("Please specify at least onfail or onrestore script, also dst must be specified\n");
	printf("Try %s --help\n",argv[0]);
	exit(1);
    }

    if (num < maxloss) {
	printf("Number of pings less than required maxloss, dropping maxloss number to num\n");
	maxloss = num;
    }

    if (background)
	i = daemon(0,1);

    if (syslogswitch)
	syslog(LOG_WARNING,"pingwdog start");

    while(1) {
	failcount = 0;
	for (i=0;i<num;i++) {
	    if (!ping4(dsthost,timeout,bindaddr))
		failcount++;
	}

	if (failcount >= maxloss) {
	    cntbatchfails++;
	} else {
	    cntbatchfails=0;
	}

	if (cntbatchfails > maxfail && failtrigger != 1) {
	    exec_detached(onfail,0);
	    if (syslogswitch)
		syslog(LOG_WARNING,"pingwdog, %s from %s is BAD",dsthost,(bindaddr == NULL ? "default" : bindaddr));
	    failtrigger = 1;
	}

	if (cntbatchfails == 0 && failtrigger != 0) {
	    exec_detached(onrestore,1);
	    if (syslogswitch)
		syslog(LOG_WARNING,"pingwdog, %s from %s is GOOD",dsthost,(bindaddr == NULL ? "default" : bindaddr));

	    failtrigger = 0;
	}
	sleep(interval);

    }

    return(0);
}

