/*
 * Copyright (C) 2010, Robert Oostenveld
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/
 *
 */

#include <sys/types.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include "peer.h"
#include "extern.h"
#include "platform_includes.h"

/* The original behaviour was to use multicast to announce the
   presence of a peer. In the presence of a firewall or if no network
   is configured, the multicast does not work. In the case someone
   only wants to use multiple peers on a single computer, an additional
   localhost packet is sent which communicates the presence of multiple
   peers to each other.
 */

#define USE_MULTICAST
#define USE_LOCALHOST

float frand(float min, float max) {
		float val;
		val = rand();				/* ranging from 0 to RAND_MAX */
		val = val/RAND_MAX;			/* ranging from 0 to 1 */
		val = min + val*(max-min);	/* ranging from min to max */
		return val;
}

typedef struct {
		void *message;
		int fd;
} threadlocal_t;

void cleanup_announce(void *arg) {
		threadlocal_t *threadlocal;
		threadlocal = (threadlocal_t *)arg;

		DEBUG(LOG_DEBUG, "cleanup_announce()");

		if (threadlocal && threadlocal->message) {
				FREE(threadlocal->message);
		}

		if (threadlocal && threadlocal->fd>0) {
				closesocket(threadlocal->fd);
				threadlocal->fd = -1;
		}

		pthread_mutex_lock(&mutexstatus);
		announceStatus = 0;
		pthread_mutex_unlock(&mutexstatus);

		pthread_mutex_lock(&mutexthreadcount);
		threadcount--;
		pthread_mutex_unlock(&mutexthreadcount);
}

void *announce(void *arg) {
		int fd = 0;
		int verbose = 0;
		struct sockaddr_in multicastAddr, localhostAddr;
		hostdef_t *message = NULL;
		unsigned char ttl = 3;
		unsigned char one = 1;
		UINT64_T memavail;

		threadlocal_t threadlocal;
		threadlocal.message = NULL;
		threadlocal.fd = -1;

		DEBUG(LOG_NOTICE, "announce()");

		/* this is for debugging */
		pthread_mutex_lock(&mutexthreadcount);
		threadcount++;
		pthread_mutex_unlock(&mutexthreadcount);

		pthread_cleanup_push(cleanup_announce, &threadlocal);

		/* the status contains the thread id when running, or zero when not running */
		pthread_mutex_lock(&mutexstatus);
		if (announceStatus==0) {
				announceStatus = 1;
				pthread_mutex_unlock(&mutexstatus);
		}
		else {
				pthread_mutex_unlock(&mutexstatus);
				goto cleanup;
		}

		/* allocate memory for the message */
		if ((message = (hostdef_t *)malloc(sizeof(hostdef_t))) == NULL) {
				perror("announce malloc");
				DEBUG(LOG_ERR, "error: announce malloc");
				goto cleanup;
		}

		/* this will be deallocated at cleanup */
		threadlocal.message = message;

		DEBUG(LOG_DEBUG, "announce: threadcount = %d", threadcount);

		pthread_mutex_lock(&mutexhost);
		DEBUG(LOG_INFO, "announce: host.name = %s", host->name);
		DEBUG(LOG_INFO, "announce: host.port = %u", host->port);
		DEBUG(LOG_INFO, "announce: host.id   = %u", host->id);
		pthread_mutex_unlock(&mutexhost);

		/* create what looks like an ordinary UDP socket */
		if ((fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP)) < 0) {
				perror("announce socket");
				DEBUG(LOG_ERR, "error: announce socket");
				goto cleanup;
		}

		/* this will be closed at cleanup */
		threadlocal.fd = fd;

		/* set up destination address for localhost announce packet */
		memset(&localhostAddr,0,sizeof(localhostAddr));
		localhostAddr.sin_family      = AF_INET;
		localhostAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
		localhostAddr.sin_port        = htons(ANNOUNCE_PORT);

		/* set up destination address for multicast announce packet */
		memset(&multicastAddr,0,sizeof(multicastAddr));
		multicastAddr.sin_family      = AF_INET;
		multicastAddr.sin_addr.s_addr = inet_addr(ANNOUNCE_GROUP);
		multicastAddr.sin_port        = htons(ANNOUNCE_PORT);

		/* now just sendto() our destination */
		while (1) {

				/* this should be done while the mutexhost and the mutexpeerlist are unlocked */
				smartmem_update();
				smartcpu_update();

				pthread_mutex_lock(&mutexhost);

				/* the host details can change over time */
				memcpy(message, host, sizeof(hostdef_t));

				if (strncasecmp(host->name, "localhost", STRLEN)==0)
						ttl = 0;
				else
						ttl = 1;

				/*  the TTL (time to live/hop count) for the send can be
				 *  ----------------------------------------------------------------------
				 *  0      Restricted to the same host. Won't be output by any interface.
				 *  1      Restricted to the same subnet. Won't be forwarded by a router.
				 *  <32    Restricted to the same site, organization or department.
				 *  <64    Restricted to the same region.
				 *  <128   Restricted to the same continent.
				 *  <255   Unrestricted in scope. Global.
				 */

				if ((setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, (void*)&ttl, sizeof(ttl))) < 0) {
						perror("setsockopt");
						DEBUG(LOG_ERR, "error: setsockopt");
				}

				pthread_mutex_unlock(&mutexhost);

#ifdef USE_MULTICAST
				/* note that this is a thread cancelation point */
				if (sendto(fd,message,sizeof(hostdef_t),0,(struct sockaddr *) &multicastAddr,sizeof(multicastAddr)) < 0) {
						perror("announce sendto (multicast)");
						DEBUG(LOG_ERR, "error: announce sendto (multicast)");
						goto cleanup;
				}
#endif

#ifdef USE_LOCALHOST
				/* note that this is a thread cancelation point */
				if (sendto(fd,message,sizeof(hostdef_t),0,(struct sockaddr *) &localhostAddr,sizeof(localhostAddr)) < 0) {
						perror("announce sendto (localhost)");
						DEBUG(LOG_ERR, "error: announce sendto (localhost)");
						goto cleanup;
				}
#endif

				/* note that this is a thread cancelation point */
				pthread_testcancel();
				/* avoid peers from perfectly synchronizing their announce packets */
				usleep(ANNOUNCESLEEP + frand(-ANNOUNCEJITTER, ANNOUNCEJITTER));
		}

cleanup:
		printf(""); /* otherwise the pthread_cleanup_pop won't compile */
		pthread_cleanup_pop(1);
		return NULL;
}


/* the following is a subset of the announce function, with the
   difference that it does not loop, does not sleep, and does not use
   a local copy of the host structure */

int announce_once(void) {
		int fd = 0;
		int verbose = 0;
		struct sockaddr_in multicastAddr, localhostAddr;
		hostdef_t *message = NULL;
		unsigned char ttl = 3;
		unsigned char one = 1;

		/* create what looks like an ordinary UDP socket */
		if ((fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP)) < 0) {
				perror("announce_once socket");
				DEBUG(LOG_ERR, "error: announce_once socket");
				goto cleanup;
		}

		/* allocate memory for the message */
		if ((message = (hostdef_t *)malloc(sizeof(hostdef_t))) == NULL) {
				perror("announce_once malloc");
				DEBUG(LOG_ERR, "error: announce_once malloc");
				goto cleanup;
		}

		/* set up destination address for localhost announce packet */
		memset(&localhostAddr,0,sizeof(localhostAddr));
		localhostAddr.sin_family      = AF_INET;
		localhostAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
		localhostAddr.sin_port        = htons(ANNOUNCE_PORT);

		/* set up destination address for multicast announce packet */
		memset(&multicastAddr,0,sizeof(multicastAddr));
		multicastAddr.sin_family      = AF_INET;
		multicastAddr.sin_addr.s_addr = inet_addr(ANNOUNCE_GROUP);
		multicastAddr.sin_port        = htons(ANNOUNCE_PORT);

		pthread_mutex_lock(&mutexhost);

		/* the host details can change over time */
		memcpy(message, host, sizeof(hostdef_t));

		pthread_mutex_unlock(&mutexhost);

		if (strncasecmp(host->name, "localhost", STRLEN)==0)
				ttl = 0;
		else
				ttl = 1;

		/*  the TTL (time to live/hop count) for the send can be
		 *  ----------------------------------------------------------------------
		 *  0      Restricted to the same host. Won't be output by any interface.
		 *  1      Restricted to the same subnet. Won't be forwarded by a router.
		 *  <32    Restricted to the same site, organization or department.
		 *  <64    Restricted to the same region.
		 *  <128   Restricted to the same continent.
		 *  <255   Unrestricted in scope. Global.
		 */

		if ((setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, (void*)&ttl, sizeof(ttl))) < 0) {
				perror("announce_once setsockopt");
				DEBUG(LOG_ERR, "error: announce_once setsockopt");
		}

#ifdef USE_MULTICAST
		if (sendto(fd,message,sizeof(hostdef_t),0,(struct sockaddr *) &multicastAddr,sizeof(multicastAddr)) < 0) {
				perror("announce_once sendto (multicast)");
				DEBUG(LOG_ERR, "error: announce_once sendto (multicast)");
				goto cleanup;
		}
#endif

#ifdef USE_LOCALHOST
		if (sendto(fd,message,sizeof(hostdef_t),0,(struct sockaddr *) &localhostAddr,sizeof(localhostAddr)) < 0) {
				perror("announce_once sendto (localhost)");
				DEBUG(LOG_ERR, "error: announce_once sendto (localhost)");
				goto cleanup;
		}
#endif

cleanup:
		FREE(message);
		closesocket(fd);
		return 0;
}

