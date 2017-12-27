/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

#include <sys/types.h>
#include <sys/select.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "event.h"
#include "log.h"

static event_module_init_t select_init;
static event_module_fini_t select_fini;
static event_module_add_t select_add;
static event_module_add_t select_del;
static event_module_process_t select_process;

static fd_set master_read_fd_set;
static fd_set master_write_fd_set;
static fd_set work_read_fd_set;
static fd_set work_write_fd_set;

static struct event **events;
static int nevents;
static int max_fd;

struct event_module event_module = {
	.add =		select_add,
	.del =		select_del,
	.enable =	select_add,
	.disable =	select_del,
	.process =	select_process,
	.init = 	select_init,
	.fini =		select_fini,
};

static int
select_init(void)
{

	events = calloc(FD_SETSIZE, sizeof(struct event *));
	if (events == NULL)
		return (ENOMEM);

	FD_ZERO(&master_read_fd_set);
	FD_ZERO(&master_write_fd_set);
	max_fd = 0;
	nevents = 0;

	return (0);
}


static void
select_fini(void)
{

	free(events);
	events = NULL;
}

static int
select_add(struct event *ev)
{

	assert(ev->fd <= FD_SETSIZE);

	switch (ev->rdwr) {
	case EVENT_READ:
		FD_SET(ev->fd, &master_read_fd_set);
		break;
	case EVENT_WRITE:
		FD_SET(ev->fd, &master_write_fd_set);
		break;
	case EVENT_RDWR:
		FD_SET(ev->fd, &master_read_fd_set);
		FD_SET(ev->fd, &master_write_fd_set);
		break;
	}

	if (max_fd != -1 && max_fd < ev->fd)
		max_fd = ev->fd;

	events[nevents] = ev;
	ev->index = nevents++;

	return (0);
}

static int
select_del(struct event *ev)
{

	assert(ev->fd <= FD_SETSIZE);

	switch (ev->rdwr) {
	case EVENT_READ:
		FD_CLR(ev->fd, &master_read_fd_set);
		break;
	case EVENT_WRITE:
		FD_CLR(ev->fd, &master_write_fd_set);
		break;
	case EVENT_RDWR:
		FD_CLR(ev->fd, &master_read_fd_set);
		FD_CLR(ev->fd, &master_write_fd_set);
		break;
	}

	if (max_fd == ev->fd)
		max_fd = -1;

	if (ev->index < --nevents) {
		struct event *ev0;

		ev0 = events[nevents];
		events[ev->index] = ev0;
		ev0->index = ev->index;
	}
	ev->index = -1;

	return (0);
}

static int
select_process(u_long msec)
{
	struct timeval tv, *tp;
	struct event *ev;
	int ready;

	/* Need to rescan for max_fd. */
	if (max_fd == -1)
		for (int i = 0; i < nevents; i++) {
			if (max_fd < events[i]->fd)
				max_fd = events[i]->fd;
		}

	tv.tv_sec = (long) (msec / 1000);
	tv.tv_usec = (long) ((msec % 1000) * 1000);
	tp = &tv;

	work_read_fd_set = master_read_fd_set;
	work_write_fd_set = master_write_fd_set;

	ready = select(max_fd + 1, &work_read_fd_set, &work_write_fd_set, NULL, tp);

	if (ready == -1) {
		if (errno == EINTR)
			return (errno);
		DPRINTF(E_ERROR, L_GENERAL, "select(all): %s\n", strerror(errno));
		DPRINTF(E_FATAL, L_GENERAL, "Failed to select open sockets. EXITING\n");
	}

	if (ready == 0)
		return (0);

	for (int i = 0; i < nevents; i++) {
		ev = events[i];

		switch (ev->rdwr) {
		case EVENT_READ:
			if (FD_ISSET(ev->fd, &work_read_fd_set))
				ev->process(ev);
			break;
		case EVENT_WRITE:
			if (FD_ISSET(ev->fd, &work_write_fd_set))
				ev->process(ev);
			break;
		case EVENT_RDWR:
			if (FD_ISSET(ev->fd, &work_read_fd_set) ||
			    FD_ISSET(ev->fd, &work_write_fd_set))
				ev->process(ev);
			break;
		}
	}

	return (0);
}
