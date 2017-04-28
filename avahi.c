/*
 * Author:  Daniel S. Haischt <me@daniel.stefan.haischt.name>
 * Purpose: Avahi based Zeroconf support
 * Docs:    http://avahi.org/download/doxygen/
 *
 */

#include <config.h>

#if defined(TIVO_SUPPORT) && defined(HAVE_AVAHI)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>

#include <avahi-common/error.h>
#include <avahi-common/thread-watch.h>
#include <avahi-client/publish.h>

#include "upnpglobalvars.h"
#include "log.h"

static struct context {
        /* Avahi stuff */
  AvahiThreadedPoll *threaded_poll;
  AvahiClient       *client;
  AvahiEntryGroup   *group;
} ctx;

/*****************************************************************
 * Private functions
 *****************************************************************/

static void publish_reply(AvahiEntryGroup *g,
                          AvahiEntryGroupState state,
                          void *userdata);

/*
 * This function tries to register the AFP DNS
 * SRV service type.
 */
static void register_stuff(void) {
    char name[128+1];

    assert(ctx.client);

    if (!ctx.group) {
        if (!(ctx.group = avahi_entry_group_new(ctx.client, publish_reply, NULL))) {
            DPRINTF(E_ERROR, L_SSDP, "Failed to create entry group: %s\n",
                avahi_strerror(avahi_client_errno(ctx.client)));
            goto fail;
        }
    }

    if (avahi_entry_group_is_empty(ctx.group)) {
        /* Register our service */

        DPRINTF(E_INFO, L_SSDP, "Registering '%s' with Avahi\n", friendly_name);

	snprintf(name, sizeof(name), "Music on %s", friendly_name);
        if (avahi_entry_group_add_service(ctx.group,
                                          AVAHI_IF_UNSPEC,
                                          AVAHI_PROTO_UNSPEC,
                                          0,
                                          name,
                                          "_tivo-music._tcp",
                                          NULL,
                                          NULL,
                                          runtime_vars.port,
                                          "protocol=http",
                                          "path=/TiVoConnect?Command=QueryContainer&Container=1",
                                          NULL) < 0) {
            DPRINTF(E_ERROR, L_SSDP, "Failed to add service: %s\n",
                avahi_strerror(avahi_client_errno(ctx.client)));
            goto fail;
        }

	snprintf(name, sizeof(name), "Photos on %s", friendly_name);
        if (avahi_entry_group_add_service(ctx.group,
                                          AVAHI_IF_UNSPEC,
                                          AVAHI_PROTO_UNSPEC,
                                          0,
                                          name,
                                          "_tivo-photos._tcp",
                                          NULL,
                                          NULL,
                                          runtime_vars.port,
                                          "protocol=http",
                                          "path=/TiVoConnect?Command=QueryContainer&Container=3",
                                          NULL) < 0) {
            DPRINTF(E_ERROR, L_SSDP, "Failed to add service: %s\n",
                avahi_strerror(avahi_client_errno(ctx.client)));
            goto fail;
        }

	snprintf(name, sizeof(name), "Videos on %s", friendly_name);
        if (avahi_entry_group_add_service(ctx.group,
                                          AVAHI_IF_UNSPEC,
                                          AVAHI_PROTO_UNSPEC,
                                          0,
                                          name,
                                          "_tivo-videos._tcp",
                                          NULL,
                                          NULL,
                                          runtime_vars.port,
                                          "platform=pc/readydlna",
                                          "protocol=http",
                                          "path=/TiVoConnect?Command=QueryContainer&Container=2",
                                          NULL) < 0) {
            DPRINTF(E_ERROR, L_SSDP, "Failed to add service: %s\n",
                avahi_strerror(avahi_client_errno(ctx.client)));
            goto fail;
        }

        if (avahi_entry_group_commit(ctx.group) < 0) {
            DPRINTF(E_ERROR, L_SSDP, "Failed to commit entry group: %s\n",
                avahi_strerror(avahi_client_errno(ctx.client)));
            goto fail;
        }

    }	/* if avahi_entry_group_is_empty*/

    return;

fail:
    time(NULL);
}

/* Called when publishing of service data completes */
static void publish_reply(AvahiEntryGroup *g,
                          AvahiEntryGroupState state,
                          AVAHI_GCC_UNUSED void *userdata)
{
	assert(ctx.group == NULL || g == ctx.group);

	switch (state) {
	case AVAHI_ENTRY_GROUP_ESTABLISHED :
		/* The entry group has been established successfully */
		DPRINTF(E_MAXDEBUG, L_SSDP, "publish_reply: AVAHI_ENTRY_GROUP_ESTABLISHED\n");
		break;
	case AVAHI_ENTRY_GROUP_COLLISION:
		/* With multiple names there's no way to know which one collided */
		DPRINTF(E_ERROR, L_SSDP, "publish_reply: AVAHI_ENTRY_GROUP_COLLISION: %s\n",
				avahi_strerror(avahi_client_errno(ctx.client)));
		avahi_threaded_poll_quit(ctx.threaded_poll);
		break;
	case AVAHI_ENTRY_GROUP_FAILURE:
		DPRINTF(E_ERROR, L_SSDP, "Failed to register service: %s\n",
				avahi_strerror(avahi_client_errno(ctx.client)));
		avahi_threaded_poll_quit(ctx.threaded_poll);
		break;
	case AVAHI_ENTRY_GROUP_UNCOMMITED:
		break;
	case AVAHI_ENTRY_GROUP_REGISTERING:
		break;
	}
}

static void client_callback(AvahiClient *client,
                            AvahiClientState state,
                            void *userdata)
{
    ctx.client = client;

    switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
        /* The server has startup successfully and registered its host
         * name on the network, so it's time to create our services */
        if (!ctx.group)
            register_stuff();
        break;

    case AVAHI_CLIENT_S_COLLISION:
        if (ctx.group)
            avahi_entry_group_reset(ctx.group);
        break;

    case AVAHI_CLIENT_FAILURE: {
        if (avahi_client_errno(client) == AVAHI_ERR_DISCONNECTED) {
            int error;

            avahi_client_free(ctx.client);
            ctx.client = NULL;
            ctx.group = NULL;

            /* Reconnect to the server */
            if (!(ctx.client = avahi_client_new(avahi_threaded_poll_get(ctx.threaded_poll),
                                                 AVAHI_CLIENT_NO_FAIL,
                                                 client_callback,
                                                 &ctx,
                                                 &error))) {

                DPRINTF(E_ERROR, L_SSDP, "Failed to contact server: %s\n",
                    avahi_strerror(error));

                avahi_threaded_poll_quit(ctx.threaded_poll);
            }

        } else {
            DPRINTF(E_ERROR, L_SSDP, "Client failure: %s\n",
                avahi_strerror(avahi_client_errno(client)));
            avahi_threaded_poll_quit(ctx.threaded_poll);
        }
        break;
    }

    case AVAHI_CLIENT_S_REGISTERING:
        break;
    case AVAHI_CLIENT_CONNECTING:
        break;
    }
}

/************************************************************************
 * Public funcions
 ************************************************************************/

/*
 * Tries to shutdown this loop impl.
 * Call this function from inside this thread.
 */
void tivo_bonjour_unregister(void)
{
	DPRINTF(E_DEBUG, L_SSDP, "tivo_bonjour_unregister\n");

	if (ctx.threaded_poll)
		avahi_threaded_poll_stop(ctx.threaded_poll);
	if (ctx.client)
		avahi_client_free(ctx.client);
	if (ctx.threaded_poll)
		avahi_threaded_poll_free(ctx.threaded_poll);
}

/*
 * Tries to setup the Zeroconf thread and any
 * neccessary config setting.
 */
void tivo_bonjour_register(void)
{
	int error;

	/* first of all we need to initialize our threading env */
	ctx.threaded_poll = avahi_threaded_poll_new();
	if (!ctx.threaded_poll)
		return;

	/* now we need to acquire a client */
	ctx.client = avahi_client_new(avahi_threaded_poll_get(ctx.threaded_poll),
			AVAHI_CLIENT_NO_FAIL, client_callback, NULL, &error);
	if (!ctx.client)
	{
		DPRINTF(E_ERROR, L_SSDP, "Failed to create client object: %s\n",
					avahi_strerror(error));
		tivo_bonjour_unregister();
		return;
	}

	if (avahi_threaded_poll_start(ctx.threaded_poll) < 0)
	{
		DPRINTF(E_ERROR, L_SSDP, "Failed to create thread: %s\n",
					avahi_strerror(avahi_client_errno(ctx.client)));
		tivo_bonjour_unregister();
	}
	else
		DPRINTF(E_INFO, L_SSDP, "Successfully started avahi loop.\n");
}

#endif /* HAVE_AVAHI */
