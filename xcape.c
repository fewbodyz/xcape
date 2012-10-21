/************************************************************************
 * xcape.c
 *
 * Copyright 2012 Albin Olsson
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 ***********************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/record.h>
#include <X11/extensions/XTest.h>
#include <X11/XKBlib.h>


/************************************************************************
 * Internal data types
 ***********************************************************************/
typedef struct _Key_t
{
    KeyCode key;
    struct _Key_t *next;
} Key_t;

typedef struct _KeyMap_t
{
    KeySym from;
    Key_t *to_keys;
    Bool used;
    Bool pressed;
    Bool mouse;
    int  fake;
    struct timeval down_at;
    struct _KeyMap_t *next;
} KeyMap_t;

typedef struct _XCape_t
{
    Display *data_conn;
    Display *ctrl_conn;
    XRecordContext record_ctx;
    pthread_t sigwait_thread;
    sigset_t sigset;
    Bool debug;
    KeyMap_t *map;
} XCape_t;

/************************************************************************
 * Internal function declarations
 ***********************************************************************/
void *sig_handler (void *user_data);

void intercept (XPointer user_data, XRecordInterceptData *data);

KeyMap_t *parse_mapping (Display *ctrl_conn, char *mapping);

/************************************************************************
 * Main function
 ***********************************************************************/
int main (int argc, char **argv)
{
    XCape_t *self = malloc (sizeof (XCape_t));
    int dummy, ch;
    static char default_mapping[] = "Control_L=Escape";
    char *mapping = default_mapping;

    self->debug = False;
    while ((ch = getopt (argc, argv, "de:")) != -1)
    {
        switch (ch)
        {
        case 'd':
            self->debug = True;
            break;
        case 'e':
            mapping = optarg;
            break;
        default:
            fprintf (stdout, "Usage: %s [-d] [-e <mapping>]\n", argv[0]);
            fprintf (stdout,
                    "Runs as a daemon unless -d flag is set\n");
            return EXIT_SUCCESS;
        }
    }

    if (self->debug != True)
        daemon (0, 0);

    self->data_conn = XOpenDisplay (NULL);
    self->ctrl_conn = XOpenDisplay (NULL);

    if (!XQueryExtension (self->ctrl_conn,
                "XTEST", &dummy, &dummy, &dummy))
    {
        fprintf (stderr, "Xtst extension missing\n");
        exit (EXIT_FAILURE);
    }
    if (!XRecordQueryVersion (self->ctrl_conn, &dummy, &dummy))
    {
        fprintf (stderr, "Failed to obtain xrecord version\n");
        exit (EXIT_FAILURE);
    }
    if (!XkbQueryExtension (self->ctrl_conn, &dummy, &dummy, 
            &dummy, &dummy, &dummy))
    {
        fprintf (stderr, "Failed to obtain xkb version\n");
        exit (EXIT_FAILURE);
    }

    self->map = parse_mapping (self->ctrl_conn, mapping);

    sigemptyset (&self->sigset);
    sigaddset (&self->sigset, SIGINT);
    sigaddset (&self->sigset, SIGTERM);
    pthread_sigmask (SIG_BLOCK, &self->sigset, NULL);

    pthread_create (&self->sigwait_thread,
            NULL, sig_handler, self);

    XRecordRange *rec_range = XRecordAllocRange();
    rec_range->device_events.first = KeyPress;
    rec_range->device_events.last = ButtonRelease;
    XRecordClientSpec client_spec = XRecordAllClients;

    self->record_ctx = XRecordCreateContext (self->ctrl_conn,
            0, &client_spec, 1, &rec_range, 1);

    if (self->record_ctx == 0)
    {
        fprintf (stderr, "Failed to create xrecord context\n");
        exit (EXIT_FAILURE);
    }

    XSync (self->ctrl_conn, False);

    if (!XRecordEnableContext (self->data_conn,
                self->record_ctx, intercept, (XPointer)self))
    {
        fprintf (stderr, "Failed to enable xrecord context\n");
        exit (EXIT_FAILURE);
    }

    if (!XRecordFreeContext (self->ctrl_conn, self->record_ctx))
    {
        fprintf (stderr, "Failed to free xrecord context\n");
    }

    XCloseDisplay (self->ctrl_conn);
    XCloseDisplay (self->data_conn);

    if (self->debug) fprintf (stdout, "main exiting\n");

    return EXIT_SUCCESS;
}


/************************************************************************
 * Internal functions
 ***********************************************************************/
void *sig_handler (void *user_data)
{
    XCape_t *self = (XCape_t*)user_data;
    int sig;

    if (self->debug) fprintf (stdout, "sig_handler running...\n");

    sigwait(&self->sigset, &sig);

    if (self->debug) fprintf (stdout, "Caught signal %d!\n", sig);

    if (!XRecordDisableContext (self->ctrl_conn,
                self->record_ctx))
    {
        fprintf (stderr, "Failed to disable xrecord context\n");
        exit(EXIT_FAILURE);
    }

    XSync (self->ctrl_conn, False);

    if (self->debug) fprintf (stdout, "sig_handler exiting...\n");

    return NULL;
}

void handle_key (XCape_t *self, KeyMap_t *key,
        Bool mouse_pressed, int key_event)
{
    Key_t *k;
    KeyCode kk;

    if (key->fake)
    {
        key->fake--;
        return;
    }

    kk = XKeysymToKeycode (self->ctrl_conn, key->from);
    if (key_event == KeyPress)
    {
        if (self->debug) fprintf (stdout, "Key pressed!\n");

        key->pressed = True;
        gettimeofday (&key->down_at, NULL);

        if (mouse_pressed)
        {
            key->used = True;
        }
    }
    else
    {
        if (self->debug) fprintf (stdout, "Key released!\n");
        if (key->used == False)
        {
            struct timeval timev, ms650 = {
                .tv_sec = 0,
                .tv_usec = 650000
            };
            gettimeofday (&timev, NULL);
            timersub (&timev, &key->down_at, &timev);

            if (timercmp (&timev, &ms650, <))
            {
                if (self->debug) fprintf (stdout,
                        "Generating ESC!\n");

                for (k = key->to_keys; k != NULL; k = k->next)
                {
                    XTestFakeKeyEvent (self->ctrl_conn,
                            k->key, True, 0);
                    if (kk == k->key)
                        key->fake += 2;
                }
                for (k = key->to_keys; k != NULL; k = k->next)
                {
                    XTestFakeKeyEvent (self->ctrl_conn,
                            k->key, False, 0);
                }
                XFlush (self->ctrl_conn);
            }
        }
        key->used = False;
        key->pressed = False;
    }
}

void intercept (XPointer user_data, XRecordInterceptData *data)
{
    XCape_t *self = (XCape_t*)user_data;
    static Bool mouse_pressed = False;
    KeyMap_t *km;

    if (data->category == XRecordFromServer)
    {
        int     key_event = data->data[0];
        KeyCode key_code  = data->data[1];

        if (self->debug) fprintf (stdout,
                "Intercepted key event %d, key code %d\n",
                key_event, key_code);

        if (key_event == ButtonPress)
        {
            mouse_pressed = True;
        }
        else if (key_event == ButtonRelease)
        {
            mouse_pressed = False;
        }
        else
        {
            for (km = self->map; km != NULL; km = km->next)
            {
                if (XkbKeycodeToKeysym (self->ctrl_conn, key_code, 0, 0)
                        == km->from)
                {
                    handle_key (self, km, mouse_pressed, key_event);
                }
                else if (km->pressed && key_event == KeyPress)
                {
                    km->used = True;
                }
            }
        }
    }

    XRecordFreeData (data);
}

KeyMap_t* parse_token (Display *dpy, char *token)
{
    KeyMap_t *km = NULL;
    Key_t    *k, *nk;
    KeySym    ks;
    char      *from, *to, *key;

    to = token;
    from = strsep (&to, "=");
    if (from != NULL)
    {
        km = calloc (1, sizeof (KeyMap_t));

        if ((ks = XStringToKeysym (from)) == NoSymbol)
        {
            fprintf (stderr, "Cannot parse %s\n", token);
            return NULL;
        }

        km->from    = ks;
        km->to_keys = k = NULL;

        for(;;)
        {
            key = strsep (&to, "|");
            if (key == NULL)
                break;

            if ((ks = XStringToKeysym (key)) == NoSymbol)
            {
                fprintf (stderr, "Cannot parse %s\n", to);
                return NULL;
            }

            nk = calloc (1, sizeof (Key_t));
            nk->key = XKeysymToKeycode (dpy, ks);

            if (k == NULL)
            {
                km->to_keys = k = nk;
            }
            else
            {
                k->next = nk;
                k = nk;
            }
        }
    }

    return km;
}

KeyMap_t *parse_mapping (Display *ctrl_conn, char *mapping)
{
    char     *token;
    KeyMap_t *rval, *km, *nkm;

    rval = km = NULL;

    for(;;)
    {
        token = strsep (&mapping, ";");
        if (token == NULL)
            break;

        nkm = parse_token (ctrl_conn, token);

        if (nkm != NULL)
        {
            if (km == NULL)
                rval = km = nkm;
            else
            {
                km->next = nkm;
                km = nkm;
            }
        }
    }

    return rval;
}
