/*
 * mcwm, a small window manager for the X Window System using the X
 * protocol C Binding libraries.
 *
 * For 'user' configurable stuff, see config.h.
 *
 * MC, mc at the domain hack.org
 * http://hack.org/mc/
 *
 * Copyright (c) 2010,2011 Michael Cardell Widerkrantz, mc at the
 * domain hack.org.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <string.h>
#include <signal.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_atom.h>
#include <xcb/xcb_icccm.h>

#include <X11/keysym.h>

#ifdef DEBUG
#include "events.h"
#endif

#include "list.h"

/* Check here for user configurable parts: */
#include "config.h"

#ifdef DMALLOC
#include "dmalloc.h"
#endif

#ifdef DEBUG
#define PDEBUG(Args...) \
  do { fprintf(stderr, "mcwm: "); fprintf(stderr, ##Args); } while(0)
#define D(x) x
#else
#define PDEBUG(Args...)
#define D(x)
#endif


/* Internal Constants. */

/* We're currently moving a window with the mouse. */
#define MCWM_MOVE 2

/* We're currently resizing a window with the mouse. */
#define MCWM_RESIZE 3

/*
 * We're currently tabbing around the window list, looking for a new
 * window to focus on.
 */
#define MCWM_TABBING 4

/* Our highest workspace. */
#define WORKSPACE_MAX 9

/* Value in WM hint which means this window is fixed on all workspaces. */
#define NET_WM_FIXED 0xffffffff

/* This means we didn't get any window hint at all. */
#define MCWM_NOWS 0xfffffffe


/* Types. */

/* All our key shortcuts. */
typedef enum {
    KEY_F,
    KEY_H,
    KEY_J,
    KEY_K,
    KEY_L,
    KEY_M,
    KEY_R,
    KEY_RET,
    KEY_X,
    KEY_TAB,
    KEY_1,
    KEY_2,
    KEY_3,
    KEY_4,
    KEY_5,
    KEY_6,
    KEY_7,
    KEY_8,
    KEY_9,
    KEY_0,
    KEY_Y,
    KEY_U,
    KEY_B,
    KEY_N,
    KEY_END,
    KEY_MAX
} key_enum_t;

/* Everything we know about a window. */
struct client
{
    xcb_drawable_t id;          /* ID of this window. */
    bool usercoord;             /* X,Y was set by -geom. */
    uint32_t x;                 /* X coordinate. Only updated when maxed. */
    uint32_t y;                 /* Y coordinate. Ditto.  */
    uint16_t width;             /* Width in pixels. Ditto. */
    uint16_t height;            /* Height in pixels. Ditto. */
    uint16_t min_width, min_height; /* Hints from application. */
    uint16_t max_width, max_height;
    int32_t width_inc, height_inc;
    int32_t base_width, base_height;
    bool vertmaxed;             /* Vertically maximized? */
    bool maxed;                 /* Totally maximized? */
    bool fixed;           /* Visible on all workspaces? */
    struct item *winitem; /* Pointer to our place in global windows list. */
    struct item *wsitem[WORKSPACE_MAX + 1]; /* Pointer to our place in every
                                             * workspace window list. */
};
    

/* Globals */

int sigcode;                    /* Signal code. Non-zero if we've been
                                 * interruped by a signal. */
xcb_connection_t *conn;         /* Connection to X server. */
xcb_screen_t *screen;           /* Our current screen.  */
uint32_t curws = 0;             /* Current workspace. */
struct client *focuswin;        /* Current focus window. */
struct client *lastfocuswin;        /* Last focused window. NOTE! Only
                                     * used to communicate between
                                     * start and end of tabbing
                                     * mode. */
struct item *winlist = NULL;    /* Global list of all client windows. */
int mode = 0;                   /* Internal mode, such as move or resize */

/*
 * Workspace list: Every workspace has a list of all visible
 * windows.
 */
struct item *wslist[WORKSPACE_MAX + 1] =
{
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

/* Shortcut key type and initializiation. */
struct keys
{
    xcb_keysym_t keysym;
    xcb_keycode_t keycode;
} keys[KEY_MAX] =
{
    { USERKEY_FIX, 0 },
    { USERKEY_MOVE_LEFT, 0 },
    { USERKEY_MOVE_DOWN, 0 },
    { USERKEY_MOVE_UP, 0 },
    { USERKEY_MOVE_RIGHT, 0 },
    { USERKEY_MAXVERT, 0 },
    { USERKEY_RAISE, 0 },
    { USERKEY_TERMINAL, 0 },
    { USERKEY_MAX, 0 },
    { USERKEY_CHANGE, 0 },
    { USERKEY_WS1, 0 },
    { USERKEY_WS2, 0 },
    { USERKEY_WS3, 0 },
    { USERKEY_WS4, 0 },
    { USERKEY_WS5, 0 },
    { USERKEY_WS6, 0 },
    { USERKEY_WS7, 0 },
    { USERKEY_WS8, 0 },
    { USERKEY_WS9, 0 },
    { USERKEY_WS10, 0 },
    { USERKEY_TOPLEFT, 0 },
    { USERKEY_TOPRIGHT, 0 },
    { USERKEY_BOTLEFT, 0 },
    { USERKEY_BOTRIGHT, 0 },
    { USERKEY_DELETE, 0 },
};    

/* All keycodes generating our MODKEY mask. */
struct modkeycodes
{
    xcb_keycode_t *keycodes;
    unsigned len;
} modkeys =
{
    NULL,
    0
};

/* Global configuration. */
struct conf
{
    bool borders;               /* Do we draw borders? */
    char *terminal;             /* Path to terminal to start. */
    uint32_t focuscol;          /* Focused border colour. */
    uint32_t unfocuscol;        /* Unfocused border colour.  */
    uint32_t fixedcol;          /* Fixed windows border colour. */
} conf;

xcb_atom_t atom_desktop;        /*
                                 * EWMH _NET_WM_DESKTOP hint that says
                                 * what workspace a window should be
                                 * on.
                                 */

xcb_atom_t wm_delete_window;    /* WM_DELETE_WINDOW event to close windows.  */
xcb_atom_t wm_protocols;        /* WM_PROTOCOLS.  */


/* Functions declerations. */

void finishtabbing(void);
struct modkeycodes getmodkeys(xcb_mod_mask_t modmask);
void cleanup(int code);
void arrangewindows(uint16_t rootwidth, uint16_t rootheight);
void setwmdesktop(xcb_drawable_t win, uint32_t ws);
int32_t getwmdesktop(xcb_drawable_t win);
void addtoworkspace(struct client *client, uint32_t ws);
void delfromworkspace(struct client *client, uint32_t ws);
void changeworkspace(uint32_t ws);
void fixwindow(struct client *client, bool setcolour);
uint32_t getcolor(const char *colstr);
void forgetclient(struct client *client);
void forgetwin(xcb_window_t win);
void newwin(xcb_window_t win);
struct client *setupwin(xcb_window_t win);
xcb_keycode_t keysymtokeycode(xcb_keysym_t keysym, xcb_key_symbols_t *keysyms);
int setupkeys(void);
int setupscreen(void);
void raisewindow(xcb_drawable_t win);
void raiseorlower(struct client *client);
void movewindow(xcb_drawable_t win, uint16_t x, uint16_t y);
struct client *findclient(xcb_drawable_t win);
void focusnext(void);
void setunfocus(xcb_drawable_t win);
void setfocus(struct client *client);
int start_terminal(void);
void resize(xcb_drawable_t win, uint16_t width, uint16_t height);
void resizestep(struct client *client, char direction);
void mousemove(xcb_drawable_t win, int rel_x, int rel_y);
void mouseresize(struct client *client, int rel_x, int rel_y);
void movestep(struct client *client, char direction);
void unmax(struct client *client);
void maximize(struct client *client);
void maxvert(struct client *client);
bool getpointer(xcb_drawable_t win, int16_t *x, int16_t *y);
bool getgeom(xcb_drawable_t win, int16_t *x, int16_t *y, uint16_t *width,
             uint16_t *height);
void topleft(void);
void topright(void);
void botleft(void);
void botright(void);
void deletewin(void);
void handle_keypress(xcb_key_press_event_t *ev);
void printhelp(void);
void sigcatch(int sig);


/* Function bodies. */

/*
 * MODKEY was released after tabbing around the
 * workspace window ring. This means this mode is
 * finished and we have found a new focus window.
 *
 * We need to move first the window we used to focus
 * on to the head of the window list and then move the
 * new focus to the head of the list as well. The list
 * should always start with the window we're focusing
 * on.
 */
void finishtabbing(void)
{
                                        
    mode = 0;
    
    if (NULL != lastfocuswin)
    {
        movetohead(&wslist[curws], lastfocuswin->wsitem[curws]);
        lastfocuswin = NULL;             
    }
                
    movetohead(&wslist[curws], focuswin->wsitem[curws]);
}

/*
 * Find out what keycode modmask is bound to. Returns a struct. If the
 * len in the struct is 0 something went wrong.
 */
struct modkeycodes getmodkeys(xcb_mod_mask_t modmask)
{
    xcb_get_modifier_mapping_cookie_t cookie;
    xcb_get_modifier_mapping_reply_t *reply;
    xcb_keycode_t *modmap;
    struct modkeycodes keycodes = {
        NULL,
        0
    };
    int mask;
    unsigned i;
    const xcb_mod_mask_t masks[8] = { XCB_MOD_MASK_SHIFT,
                                      XCB_MOD_MASK_LOCK,
                                      XCB_MOD_MASK_CONTROL,
                                      XCB_MOD_MASK_1,
                                      XCB_MOD_MASK_2,
                                      XCB_MOD_MASK_3,
                                      XCB_MOD_MASK_4,
                                      XCB_MOD_MASK_5 };

    cookie = xcb_get_modifier_mapping_unchecked(conn);

    if ((reply = xcb_get_modifier_mapping_reply(conn, cookie, NULL)) == NULL)
    {
        return keycodes;
    }

    if (NULL == (keycodes.keycodes = calloc(reply->keycodes_per_modifier,
                                            sizeof (xcb_keycode_t))))
    {
        PDEBUG("Out of memory.\n");
        return keycodes;
    }
    
    modmap = xcb_get_modifier_mapping_keycodes(reply);

    /*
     * The modmap now contains keycodes.
     *
     * The number of keycodes in the list is 8 *
     * keycodes_per_modifier. The keycodes are divided into eight
     * sets, with each set containing keycodes_per_modifier elements.
     *
     * Each set corresponds to a modifier in masks[] in the order
     * specified above.
     *
     * The keycodes_per_modifier value is chosen arbitrarily by the
     * server. Zeroes are used to fill in unused elements within each
     * set.
     */
    for (mask = 0; mask < 8; mask ++)
    {
        if (masks[mask] == modmask)
        {
            for (i = 0; i < reply->keycodes_per_modifier; i ++)
            {
                if (0 != modmap[mask * reply->keycodes_per_modifier + i])
                {
                    keycodes.keycodes[i]
                        = modmap[mask * reply->keycodes_per_modifier + i];
                    keycodes.len ++;
                }
            }
            
            PDEBUG("Got %d keycodes.\n", keycodes.len);
        }
    } /* for mask */
    
    free(reply);
    
    return keycodes;
}

/*
 * Map all windows we know about. Set keyboard focus to be wherever
 * the mouse pointer is. Then exit.
 */
void cleanup(int code)
{
    struct item *item;
    struct client *client;

    xcb_set_input_focus(conn, XCB_NONE,
                        XCB_INPUT_FOCUS_POINTER_ROOT,
                        XCB_CURRENT_TIME);
    
    for (item = winlist; item != NULL; item = item->next)
    {
        client = item->data;
        xcb_map_window(conn, client->id);
    }

    xcb_flush(conn);

    if (SIGSEGV == code)
    {
        abort();
    }

    xcb_disconnect(conn);
    
    exit(code);
}

/*
 *
 * Rearrange windows to fit new screen size rootwidth x rootheight.
 */ 
void arrangewindows(uint16_t rootwidth, uint16_t rootheight)
{
    uint32_t mask = 0;
    uint32_t values[5];
    bool changed;
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    struct item *item;
    struct client *client;
    
    PDEBUG("Rearranging all windows to fit new screen size %d x %d.\n",
           rootwidth, rootheight);

    /*
     * Go through all windows we care about and look at their
     * coordinates and geometry. If they don't fit on the new screen,
     * move them around and resize them as necessary.
     */
    for (item = winlist; item != NULL; item = item->next)
    {
        client = item->data;

        changed = false;
    
        if (!getgeom(client->id, &x, &y, &width, &height))
        {
            return;
        }

        PDEBUG("Win %d at %d,%d %d x %d\n", client->id, x, y, width, height);

        if (width > rootwidth)
        {
            width = rootwidth - BORDERWIDTH * 2;
            changed = true;
        }

        if (height > rootheight)
        {
            height = rootheight - BORDERWIDTH * 2;
            changed = true;
        }

        /* If x or y + geometry is outside of screen, move window. */

        if (x + width > rootwidth)
        {
            x = rootwidth - (width + BORDERWIDTH * 2);
            changed = true;
        }

        if (y + height > rootheight)
        {
            y = rootheight - (height + BORDERWIDTH * 2);;
            changed = true;
        }

        /* Reset sense of maximized. */
        client->vertmaxed = false;

        if (client->maxed)
        {
            client->maxed = false;
                
            /* Set borders again. */
            values[0] = BORDERWIDTH;

            mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH;
    
            xcb_configure_window(conn, client->id, mask, &values[0]);
            xcb_flush(conn);            
        }
        
        if (changed)
        {
            PDEBUG("--- Win %d going to %d,%d %d x %d\n", client->id,
                   x, y, width, height);

            mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
                | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
            values[0] = x;
            values[1] = y;
            values[2] = width;
            values[3] = height;
            
            xcb_configure_window(conn, client->id, mask, values);
            xcb_flush(conn);
        }
    } /* for */
}

/* Set the EWMH hint that window win belongs on workspace ws. */
void setwmdesktop(xcb_drawable_t win, uint32_t ws)
{
    PDEBUG("Changing _NET_WM_DESKTOP on window %d to %d\n", win, ws);
    
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win,
                        atom_desktop, CARDINAL, 32, 1,
                        &ws);
}

/*
 * Get EWWM hint so we might know what workspace window win should be
 * visible on.
 *
 * Returns either workspace, NET_WM_FIXED if this window should be
 * visible on all workspaces or MCWM_NOWS if we didn't find any hints.
 */
int32_t getwmdesktop(xcb_drawable_t win)
{
    xcb_get_property_reply_t *reply;
    xcb_get_property_cookie_t cookie;
    uint32_t *wsp;
    uint32_t ws;
    
    cookie = xcb_get_any_property(conn, false, win, atom_desktop,
                                  sizeof (int32_t));

    reply = xcb_get_property_reply(conn, cookie, NULL);
    if (NULL == reply)
    {
        fprintf(stderr, "mcwm: Couldn't get properties for win %d\n", win);
        return MCWM_NOWS;
    }

    /* Length is 0 if we didn't find it. */
    if (0 == xcb_get_property_value_length(reply))
    {
        PDEBUG("_NET_WM_DESKTOP reply was 0 length.\n");
        goto bad;
    }
        
    wsp = xcb_get_property_value(reply);

    ws = *wsp;

    PDEBUG("got _NET_WM_DESKTOP: %d stored at %p.\n", ws, (void *)wsp);
    
    free(reply);

    return ws;

bad:
    free(reply);
    return MCWM_NOWS;
}

/* Add a window, specified by client, to workspace ws. */
void addtoworkspace(struct client *client, uint32_t ws)
{
    struct item *item;
    
    item = additem(&wslist[ws]);
    if (NULL == item)
    {
        PDEBUG("addtoworkspace: Out of memory.\n");
        return;
    }

    /* Remember our place in the workspace window list. */
    client->wsitem[ws] = item;

    /* Remember the data. */
    item->data = client;

    /*
     * Set window hint property so we can survive a crash.
     * 
     * Fixed windows have their own special WM hint. We don't want to
     * mess with that.
     */
    if (!client->fixed)
    {
        setwmdesktop(client->id, ws);
    }
}

/* Delete window client from workspace ws. */
void delfromworkspace(struct client *client, uint32_t ws)
{
    delitem(&wslist[ws], client->wsitem[ws]);

    /* Reset our place in the workspace window list. */
    client->wsitem[ws] = NULL;
}

/* Change current workspace to ws. */
void changeworkspace(uint32_t ws)
{
    struct item *item;
    struct client *client;
    
    if (ws == curws)
    {
        PDEBUG("Changing to same workspace!\n");
        return;
    }

    PDEBUG("Changing from workspace #%d to #%d\n", curws, ws);

    /*
     * We lose our focus if the window we focus isn't fixed. An
     * EnterNotify event will set focus later.
     */
    if (NULL != focuswin && !focuswin->fixed)
    {
        setunfocus(focuswin->id);
        focuswin = NULL;
    }
    
    /* Go through list of current ws. Unmap everything that isn't fixed. */
    for (item = wslist[curws]; item != NULL; )
    {
        client = item->data;

        PDEBUG("changeworkspace. unmap phase. ws #%d, client-fixed: %d\n",
               curws, client->fixed);
      
        if (client->fixed)
        {
            /* Add the fixed window to the new workspace window list. */
            addtoworkspace(client, ws);

            /*
             * Remove the fixed window from the current workspace
             * list.
             *  
             * NB! Before deleting this item, we need to save the
             * address to next item so we can continue through the
             * list.
             */      
            item = item->next;

            delfromworkspace(client, curws);
        }
        else
        {
            /*
             * This is an ordinary window. Just unmap it. Note that
             * this will generate an unnecessary UnmapNotify event
             * which we will try to handle later.
             */
            xcb_unmap_window(conn, client->id);

            item = item->next;
        }

    } /* for */
    
    /* Go through list of new ws. Map everything that isn't fixed. */
    for (item = wslist[ws]; item != NULL; item = item->next)
    {
        client = item->data;

        PDEBUG("changeworkspace. map phase. ws #%d, client-fixed: %d\n",
               ws, client->fixed);

        /* Fixed windows are already mapped. Map everything else. */
        if (!client->fixed)
        {
            xcb_map_window(conn, client->id);
        }
    }

    xcb_flush(conn);

    curws = ws;
}

/*
 * Fix or unfix a window client from all workspaces. If setcolour is
 * set, also change back to ordinary focus colour when unfixing.
 */
void fixwindow(struct client *client, bool setcolour)
{
    uint32_t values[1];

    if (NULL == client)
    {
        return;
    }
    
    if (client->fixed)
    {
        client->fixed = false;
        setwmdesktop(client->id, curws);

        if (setcolour)
        {
            /* Set border color to ordinary focus colour. */
            values[0] = conf.focuscol;
            xcb_change_window_attributes(conn, client->id, XCB_CW_BORDER_PIXEL,
                                         values);
        }
        
    }
    else
    {
        /*
         * First raise the window. If we're going to another desktop
         * we don't want this fixed window to be occluded behind
         * something else.
         */
        raisewindow(client->id);
        
        client->fixed = true;
        setwmdesktop(client->id, NET_WM_FIXED);

        if (setcolour)
        {
            /* Set border color to fixed colour. */
            values[0] = conf.fixedcol;
            xcb_change_window_attributes(conn, client->id, XCB_CW_BORDER_PIXEL,
                                         values);
        }
    }

    xcb_flush(conn);    
}

/*
 * Get the pixel values of a named colour colstr.
 *
 * Returns pixel values.
 * */
uint32_t getcolor(const char *colstr)
{
    xcb_alloc_named_color_reply_t *col_reply;    
    xcb_colormap_t colormap; 
    xcb_generic_error_t *error;
    xcb_alloc_named_color_cookie_t colcookie;

    colormap = screen->default_colormap;

    colcookie = xcb_alloc_named_color(conn, colormap, strlen(colstr), colstr);
    
    col_reply = xcb_alloc_named_color_reply(conn, colcookie, &error);
    if (NULL != error)
    {
        fprintf(stderr, "mcwm: Couldn't get pixel value for colour %s. "
                "Exiting.\n", colstr);

        xcb_disconnect(conn);
        exit(1);
    }

    return col_reply->pixel;
}

/* Forget everything about client client. */
void forgetclient(struct client *client)
{
    if (NULL == client)
    {
        PDEBUG("forgetclient: client was NULL\n");
        return;
    }
    
    /* Delete window from workspace list. */
    delfromworkspace(client, curws);

    free(client->winitem->data);    

    delitem(&winlist, client->winitem);
}

/* Forget everything about a client with client->id win. */
void forgetwin(xcb_window_t win)
{
    struct item *item;
    struct client *client;
    uint32_t ws;
    
    /* Find this window in the global window list. */
    for (item = winlist; item != NULL; item = item->next)
    {
        client = item->data;

        /*
         * Forget about it completely and free allocated data.
         *
         * Note that it might already be freed by handling an
         * UnmapNotify, so it isn't necessarily an error if we don't
         * find it.
         */
        PDEBUG("Win %d == client ID %d\n", win, client->id);
        if (win == client->id)
        {
            /* Found it. */
            PDEBUG("Found it. Forgetting...\n");

            /*
             * Delete window from whatever workspace lists it belonged
             * to. Note that it's OK to be on several workspaces at
             * once.
             */
            for (ws = 0; ws != WORKSPACE_MAX; ws ++)
            {
                PDEBUG("Looking in ws #%d.\n", ws);
                if (NULL == client->wsitem[ws])
                {
                    PDEBUG("  but it wasn't there.\n");
                }
                else
                {
                    PDEBUG("  found it here. Deleting!\n");
                    delfromworkspace(client, ws);                    
                }
            }
            
            free(item->data);

            delitem(&winlist, item);

            return;
        }
    }
}

/*
 * Set position, geometry and attributes of a new window and show it
 * on the screen.
 */
void newwin(xcb_window_t win)
{
    int16_t pointx;
    int16_t pointy;
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    struct client *client;

    if (NULL != findclient(win))
    {
        /*
         * We know this window from before. It's trying to map itself
         * on the current workspace, but since it's unmapped it
         * probably belongs on another workspace. We don't like that.
         * Silently ignore.
         */
        return;
    }

    /* Get pointer position so we can move the window to the cursor. */

    if (!getpointer(screen->root, &pointx, &pointy))
    {
        pointx = 0;
        pointy = 0;
    }
    
    /*
     * Set up stuff, like borders, add the window to the client list,
     * et cetera.
     */
    
    client = setupwin(win);
    if (NULL == client)
    {
        fprintf(stderr, "mcwm: Couldn't set up window. Out of memory.\n");
        return;
    }

    /* Add this window to the current workspace. */
    addtoworkspace(client, curws);

    if (!getgeom(win, &x, &y, &width, &height))
    {
        PDEBUG("Couldn't get geometry\n");
        return;
    }

    /*
     * If the client says the user specified the coordinates, we
     * override the pointer position and place the window where the
     * client specifies instead.
     */
    if (client->usercoord)
    {
        pointx = x;
        pointy = y;
    }
    
    /*
     * If the window is larger than our screen, just place it in the
     * corner and resize.
     */
    if (width > screen->width_in_pixels)
    {
        pointx = 0;
        width = screen->width_in_pixels - BORDERWIDTH * 2;;
        resize(win, width, height);
    }
    else if (pointx + width + BORDERWIDTH * 2 > screen->width_in_pixels)
    {
        pointx = screen->width_in_pixels - (width + BORDERWIDTH * 2);
    }

    if (height > screen->height_in_pixels)
    {
        pointy = 0;
        height = screen->height_in_pixels - BORDERWIDTH * 2;
        resize(win, width, height);
    }
    else if (pointy + height + BORDERWIDTH * 2 > screen->height_in_pixels)
    {
        pointy = screen->height_in_pixels - (height + BORDERWIDTH * 2);
    }
    
    /* Move the window to cursor position. */
    movewindow(win, pointx, pointy);
    
    /* Show window on screen. */
    xcb_map_window(conn, win);

    /*
     * Move cursor into the middle of the window so we don't lose the
     * pointer to another window.
     */
    xcb_warp_pointer(conn, XCB_NONE, win, 0, 0, 0, 0,
                     width / 2, height / 2);
    
    xcb_flush(conn);
}

/* Set border colour, width and event mask for window. */
struct client *setupwin(xcb_window_t win)
{
    uint32_t mask = 0;    
    uint32_t values[2];
    struct item *item;
    struct client *client;
    xcb_size_hints_t hints;
    uint32_t ws;
    
    if (conf.borders)
    {
        /* Set border color. */
        values[0] = conf.unfocuscol;
        xcb_change_window_attributes(conn, win, XCB_CW_BORDER_PIXEL, values);

        /* Set border width. */
        values[0] = BORDERWIDTH;
        mask = XCB_CONFIG_WINDOW_BORDER_WIDTH;
        xcb_configure_window(conn, win, mask, values);
    }
    
    mask = XCB_CW_EVENT_MASK;
    values[0] = XCB_EVENT_MASK_ENTER_WINDOW;
    xcb_change_window_attributes_checked(conn, win, mask, values);

    xcb_flush(conn);
    
    /* Remember window and store a few things about it. */
    
    item = additem(&winlist);
    
    if (NULL == item)
    {
        PDEBUG("newwin: Out of memory.\n");
        return NULL;
    }

    client = malloc(sizeof (struct client));
    if (NULL == client)
    {
        PDEBUG("newwin: Out of memory.\n");
        return NULL;
    }

    item->data = client;

    /* Initialize client. */
    client->id = win;
    client->usercoord = false;
    client->x = 0;
    client->y = 0;
    client->width = 0;
    client->height = 0;
    client->min_width = 0;
    client->min_height = 0;
    client->max_width = screen->width_in_pixels;
    client->max_height = screen->height_in_pixels;
    client->base_width = 0;
    client->base_height = 0;
    client->width_inc = 1;
    client->height_inc = 1;
    client->vertmaxed = false;
    client->maxed = false;
    client->fixed = false;
    client->winitem = item;

    for (ws = 0; ws != WORKSPACE_MAX; ws ++)
    {
        client->wsitem[ws] = NULL;
    }
    
    PDEBUG("Adding window %d\n", client->id);

    /*
     * Get the window's incremental size step, if any.
     */
    if (!xcb_get_wm_normal_hints_reply(
            conn, xcb_get_wm_normal_hints_unchecked(
                conn, win), &hints, NULL))
    {
        PDEBUG("Couldn't get size hints.");
    }

    /*
     * The user specified the position coordinates. Remember that so
     * we can use geometry later.
     */
    if (hints.flags & XCB_SIZE_HINT_US_POSITION)
    {
        client->usercoord = true;
    }

    if (hints.flags & XCB_SIZE_HINT_P_MIN_SIZE)
    {
        client->min_width = hints.min_width;
        client->min_height = hints.min_height;
    }
    
    if (hints.flags & XCB_SIZE_HINT_P_MAX_SIZE)
    {
        
        client->max_width = hints.max_width;
        client->max_height = hints.max_height;
    }
    
    if (hints.flags & XCB_SIZE_HINT_P_RESIZE_INC)
    {
        client->width_inc = hints.width_inc;
        client->height_inc = hints.height_inc;

        PDEBUG("widht_inc %d\nheight_inc %d\n", client->width_inc,
               client->height_inc);
    }

    if (hints.flags & XCB_SIZE_HINT_BASE_SIZE)
    {
        client->base_width = hints.base_width;
        client->base_height = hints.base_height;
    }

    return client;
}

/*
 * Get a keycode from a keysym.
 *
 * Returns keycode value. 
 */
xcb_keycode_t keysymtokeycode(xcb_keysym_t keysym, xcb_key_symbols_t *keysyms)
{
    xcb_keycode_t *keyp;
    xcb_keycode_t key;

    /* We only use the first keysymbol, even if there are more. */
    keyp = xcb_key_symbols_get_keycode(keysyms, keysym);
    if (NULL == keyp)
    {
        fprintf(stderr, "mcwm: Couldn't look up key. Exiting.\n");
        exit(1);
        return 0;
    }

    key = *keyp;
    free(keyp);
    
    return key;
}

/*
 * Set up all shortcut keys.
 *
 * Returns 0 on success, non-zero otherwise. 
 */
int setupkeys(void)
{
    xcb_key_symbols_t *keysyms;
    unsigned i;
    
    /* Get all the keysymbols. */
    keysyms = xcb_key_symbols_alloc(conn);

    /*
     * Find out what keys generates our MODKEY mask. Unfortunately it
     * might be several keys.
     */
    if (NULL != modkeys.keycodes)
    {
        free(modkeys.keycodes);
    }
    modkeys = getmodkeys(MODKEY);

    if (0 == modkeys.len)
    {
        fprintf(stderr, "We couldn't find any keycodes to our main modifier "
                "key!\n");
        return -1;
    }

    for (i = 0; i < modkeys.len; i ++)
    {
        /*
         * Grab the keys that are bound to MODKEY mask with any other
         * modifier.
         */
        xcb_grab_key(conn, 1, screen->root, XCB_MOD_MASK_ANY,
                     modkeys.keycodes[i],
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);    
    }
    
    /* Now grab the rest of the keys with the MODKEY modifier. */
    for (i = KEY_F; i < KEY_MAX; i ++)
    {
        keys[i].keycode = keysymtokeycode(keys[i].keysym, keysyms);        
        if (0 == keys[i].keycode)
        {
            /* Couldn't set up keys! */
    
            /* Get rid of key symbols. */
            xcb_key_symbols_free(keysyms);

            return -1;
        }
            
        /* Grab other keys with a modifier mask. */
        xcb_grab_key(conn, 1, screen->root, MODKEY, keys[i].keycode,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

        /*
         * XXX Also grab it's shifted counterpart. A bit ugly here
         * because we grab all of them not just the ones we want.
         */
        xcb_grab_key(conn, 1, screen->root, MODKEY | SHIFTMOD,
                     keys[i].keycode,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    } /* for */

    /* Need this to take effect NOW! */
    xcb_flush(conn);
    
    /* Get rid of the key symbols table. */
    xcb_key_symbols_free(keysyms);
    
    return 0;
}

/*
 * Walk through all existing windows and set them up.
 *
 * Returns 0 on success. 
 */
int setupscreen(void)
{
    xcb_query_tree_reply_t *reply;
    xcb_query_pointer_reply_t *pointer;
    int i;
    int len;
    xcb_window_t *children;
    xcb_get_window_attributes_reply_t *attr;
    struct client *client;
    uint32_t ws;
    
    /* Get all children. */
    reply = xcb_query_tree_reply(conn,
                                 xcb_query_tree(conn, screen->root), 0);
    if (NULL == reply)
    {
        return -1;
    }

    len = xcb_query_tree_children_length(reply);    
    children = xcb_query_tree_children(reply);
    
    /* Set up all windows on this root. */
    for (i = 0; i < len; i ++)
    {
        attr = xcb_get_window_attributes_reply(
            conn, xcb_get_window_attributes(conn, children[i]), NULL);

        if (!attr)
        {
            fprintf(stderr, "Couldn't get attributes for window %d.",
                    children[i]);
            continue;
        }

        /*
         * Don't set up or even bother windows in override redirect
         * mode.
         *
         * This mode means they wouldn't have been reported to us
         * with a MapRequest if we had been running, so in the
         * normal case we wouldn't have seen them.
         *
         * Only handle visible windows. 
         */    
        if (!attr->override_redirect
            && attr->map_state == XCB_MAP_STATE_VIEWABLE)
        {
            client = setupwin(children[i]);
            if (NULL != client)
            {
                /*
                 * Check if this window has a workspace set already as
                 * a WM hint.
                 *
                 */
                ws = getwmdesktop(children[i]);

                if (ws == NET_WM_FIXED)
                {
                    fixwindow(client, false);
                    addtoworkspace(client, curws);
                }
                else if (MCWM_NOWS != ws && ws < WORKSPACE_MAX)
                {
                    addtoworkspace(client, ws);
                    /* If it's not our current workspace, hide it. */
                    if (ws != curws)
                    {
                        xcb_unmap_window(conn, client->id);
                    }
                }
                else
                {
                    /*
                     * No workspace hint at all. Just add it to our
                     * current workspace.
                     */
                    addtoworkspace(client, curws);
                }
            }
        }
        
        free(attr);
    } /* for */

    changeworkspace(0);
        
    /*
     * Get pointer position so we can set focus on any window which
     * might be under it.
     */
    pointer = xcb_query_pointer_reply(
        conn, xcb_query_pointer(conn, screen->root), 0);

    if (NULL == pointer)
    {
        focuswin = NULL;
    }
    else
    {
        setfocus(findclient(pointer->child));
        free(pointer);
    }
    
    xcb_flush(conn);
    
    free(reply);
    
    return 0;
}

/* Raise window win to top of stack. */
void raisewindow(xcb_drawable_t win)
{
    uint32_t values[] = { XCB_STACK_MODE_ABOVE };

    if (screen->root == win || 0 == win)
    {
        return;
    }
    
    xcb_configure_window(conn, win,
                         XCB_CONFIG_WINDOW_STACK_MODE,
                         values);
    xcb_flush(conn);
}

/*
 * Set window client to either top or bottom of stack depending on
 * where it is now.
 */
void raiseorlower(struct client *client)
{
    uint32_t values[] = { XCB_STACK_MODE_OPPOSITE };
    xcb_drawable_t win;
    
    if (NULL == client)
    {
        return;
    }

    win = client->id;
    
    xcb_configure_window(conn, win,
                         XCB_CONFIG_WINDOW_STACK_MODE,
                         values);
    xcb_flush(conn);
}

/* Move window win to root coordinates x,y. */
void movewindow(xcb_drawable_t win, uint16_t x, uint16_t y)
{
    uint32_t values[2];

    if (screen->root == win || 0 == win)
    {
        /* Can't move root. */
        return;
    }
    
    values[0] = x;
    values[1] = y;

    xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_X
                         | XCB_CONFIG_WINDOW_Y, values);
    
    xcb_flush(conn);

}

/* Change focus to next in window ring. */
void focusnext(void)
{
    struct client *client = NULL;
    
#if DEBUG
    if (NULL != focuswin)
    {
        PDEBUG("Focus now in win %d\n", focuswin->id);
    }
#endif

    if (NULL == wslist[curws])
    {
        PDEBUG("No windows to focus on in this workspace.\n");
        return;
    }
    
    if (MCWM_TABBING != mode)
    {
        /*
         * Remember what we last focused on. We need this when the
         * MODKEY is released and we move the last focused window in
         * the tabbing order list.
         */        
        lastfocuswin = focuswin;
        mode = MCWM_TABBING;

        PDEBUG("Began tabbing.\n");        
    }
    
    /* If we currently have no focus focus first in list. */
    if (NULL == focuswin || NULL == focuswin->wsitem[curws])
    {
        PDEBUG("Focusing first in list: %p\n", wslist[curws]);
        client = wslist[curws]->data;

        if (NULL != focuswin && NULL == focuswin->wsitem[curws])
        {
            PDEBUG("XXX Our focused window %d isn't on this workspace!\n",
                   focuswin->id);
        }
    }
    else
    {
        if (NULL == focuswin->wsitem[curws]->next)
        {
            /*
             * We were at the end of list. Focusing on first window in
             * list unless we were already there.
             */
            if (focuswin->wsitem[curws] != wslist[curws]->data)
            {
                PDEBUG("End of list. Focusing first in list: %p\n",
                       wslist[curws]);
                client = wslist[curws]->data;
            }
        }
        else
        {
            /* Otherwise, focus the next in list. */
            PDEBUG("Tabbing. Focusing next: %p.\n",
                   focuswin->wsitem[curws]->next);            
            client = focuswin->wsitem[curws]->next->data;
        }
    } /* if NULL focuswin */

    if (NULL != client)
    {
        /*
         * Raise window if it's occluded, then warp pointer into it and
         * set keyboard focus to it.
         */
        uint32_t values[] = { XCB_STACK_MODE_TOP_IF };    

        xcb_configure_window(conn, client->id, XCB_CONFIG_WINDOW_STACK_MODE,
                             values);
        xcb_warp_pointer(conn, XCB_NONE, client->id, 0, 0, 0, 0, 0, 0);
        setfocus(client);
    }
}

/* Mark window win as unfocused. */
void setunfocus(xcb_drawable_t win)
{
    uint32_t values[1];

    if (NULL == focuswin)
    {
        return;
    }
    
    if (focuswin->id == screen->root || !conf.borders)
    {
        return;
    }

    /* Set new border colour. */
    values[0] = conf.unfocuscol;
    xcb_change_window_attributes(conn, win, XCB_CW_BORDER_PIXEL, values);

    xcb_flush(conn);
}

/*
 * Find client with client->id win in global window list.
 *
 * Returns client pointer or NULL if not found.
 */
struct client *findclient(xcb_drawable_t win)
{
    struct item *item;
    struct client *client;

    for (item = winlist; item != NULL; item = item->next)
    {
        client = item->data;
        if (win == client->id)
        {
            PDEBUG("findclient: Found it. Win: %d\n", client->id);
            return client;
        }
    }

    return NULL;
}

/* Set focus on window client. */
void setfocus(struct client *client)
{
    uint32_t values[1];

    /*
     * If client is NULL, we focus on whatever the pointer is on.
     *
     * This is a pathological case, but it will make the poor user
     * able to focus on windows anyway, even though this window
     * manager might be buggy.
     */
    if (NULL == client)
    {
        PDEBUG("setfocus: client was NULL!\n");

        focuswin = NULL;

        xcb_set_input_focus(conn, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT,
                            XCB_CURRENT_TIME);
        xcb_flush(conn);
        
        return;
    }
    
    /*
     * Don't bother focusing on the root window or on the same window
     * that already has focus.
     */
    if (client->id == screen->root || client == focuswin)
    {
        return;
    }

    if (conf.borders)
    {
        /* Set new border colour. */
        if (client->fixed)
        {
            values[0] = conf.fixedcol;            
        }
        else
        {
            values[0] = conf.focuscol;
        }

        xcb_change_window_attributes(conn, client->id, XCB_CW_BORDER_PIXEL,
                                     values);

        /* Unset last focus. */
        if (NULL != focuswin)
        {
            setunfocus(focuswin->id);
        }
    }

    /* Set new input focus. */

    xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, client->id,
                        XCB_CURRENT_TIME);

    xcb_flush(conn);
    
    /* Remember the new window as the current focused window. */
    focuswin = client;
}

/*
 * Start a program specified in conf.terminal.
 *
 * Returns 0 on success. 
 */
int start_terminal(void)
{
    pid_t pid;

    pid = fork();
    if (-1 == pid)
    {
        perror("fork");
        return -1;
    }
    else if (0 == pid)
    {
        pid_t termpid;

        /* In our first child. */

        /*
         * Make this process a new process leader, otherwise the
         * terminal will die when the wm dies. Also, this makes any
         * SIGCHLD go to this process when we fork again.
         */
        if (-1 == setsid())
        {
            perror("setsid");
            exit(1);
        }
        
        /*
         * Fork again for the terminal process. This way, the wm won't
         * know anything about it.
         */
        termpid = fork();
        if (-1 == termpid)
        {
            perror("fork");
            exit(1);
        }
        else if (0 == termpid)
        {
            char *argv[2];
        
            /* In the second child, now starting terminal. */
        
            argv[0] = conf.terminal;
            argv[1] = NULL;

            if (-1 == execvp(conf.terminal, argv))
            {
                perror("execve");            
                exit(1);
            }
        } /* second child */

        /* Exit our first child so the wm can pick up and continue. */
        exit(0);
    } /* first child */
    else
    {
        /* Wait for the first forked process to exit. */
        waitpid(pid, NULL, 0);
    }

    return 0;
}

/* Resize window win to width,height. */
void resize(xcb_drawable_t win, uint16_t width, uint16_t height)
{
    uint32_t values[2];

    if (screen->root == win || 0 == win)
    {
        /* Can't resize root. */
        return;
    }
    
    values[0] = width;
    values[1] = height;
                                        
    xcb_configure_window(conn, win,
                         XCB_CONFIG_WINDOW_WIDTH
                         | XCB_CONFIG_WINDOW_HEIGHT, values);
    xcb_flush(conn);
}

/*
 * Resize window client in direction direction. Direction is:
 *
 * h = left, that is decrease width.
 *
 * j = down, that is, increase height.
 *
 * k = up, that is, decrease height.
 *
 * l = right, that is, increase width.
 */
void resizestep(struct client *client, char direction)
{
    int16_t start_x;
    int16_t start_y;
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    uint16_t origwidth;
    uint16_t origheight;    
    int step_x = MOVE_STEP;
    int step_y = MOVE_STEP;
    xcb_drawable_t win;
    bool warp = false;
    
    if (NULL == client)
    {
        return;
    }

    if (client->maxed)
    {
        /* Can't resize a fully maximized window. */
        return;
    }
    
    win = client->id;

    /* Save pointer position so we can warp back later, if necessary. */
    if (!getpointer(win, &start_x, &start_y))
    {
        return;
    }
    
    raisewindow(win);

    /* Get window geometry. */
    if (!getgeom(client->id, &x, &y, &width, &height))
    {
        return;
    }

    origwidth = width;
    origheight = height;

    if (client->width_inc > 1)
    {
        step_x = client->width_inc;
    }
    else
    {
        step_x = MOVE_STEP;
    }

    if (client->height_inc > 1)
    {
        step_y = client->height_inc;
    }
    else
    {
        step_y = MOVE_STEP;        
    }
    
    switch (direction)
    {
    case 'h':
        if (step_x >= width)
        {
            return;
        }

        width = width - step_x;
        height = height;

        break;

    case 'j':
        width = width;
        height = height + step_y;
        if (height + y > screen->height_in_pixels)
        {
            return;
        }
        break;

    case 'k':
        if (step_y >= height)
        {
            return;
        }
        height = height - step_y;
        break;

    case 'l':
        width = width + step_x;
        height = height;
        if (width + x > screen->width_in_pixels)
        {
            return;
        }
        break;

    default:
        PDEBUG("resizestep in unknown direction.\n");
        break;
    } /* switch direction */

    /* Is it smaller than it wants to be? */
    if (0 != client->min_height && height < client->min_height)
    {
        height = client->min_height;
    }

    if (0 != client->min_width && width < client->min_width)
    {
        width = client->min_width;
    }
    
    PDEBUG("Resizing to %dx%d\n", width, height);
    resize(win, width, height);

    /* If this window was vertically maximized, remember that it isn't now. */
    if (client->vertmaxed)
    {
        client->vertmaxed = false;
    }
    
    /*
     * We might need to warp the pointer to keep the focus.
     *
     * Don't do anything if the pointer was outside the window when we
     * began resizing.
     *
     * If the pointer was inside the window when we began and it still
     * is, don't do anything. However, if we're about to lose the
     * pointer, move in.
     */    
    if (start_x > 0 - BORDERWIDTH && start_x < origwidth + BORDERWIDTH
        && start_y > 0 - BORDERWIDTH && start_y < origheight + BORDERWIDTH )
    {
        x = start_x;
        y = start_y;

        if (start_x > width - step_x)
        {
            x = width / 2;
            if (0 == x)
            {
                x = 1;
            }
            warp = true;
        }

        if (start_y > height - step_y)
        {
            y = height / 2;
            if (0 == y)
            {
                y = 1;
            }
            warp = true;        
        }

        if (warp)
        {
            xcb_warp_pointer(conn, XCB_NONE, win, 0, 0, 0, 0,
                             x, y);
            xcb_flush(conn);
        }
    }
}

/*
 * Move window win as a result of pointer motion to coordinates
 * rel_x,rel_y.
 */
void mousemove(xcb_drawable_t win, int rel_x, int rel_y)
{
    xcb_get_geometry_reply_t *geom;    
    int x;
    int y;
    
    /* Get window geometry. */

    geom = xcb_get_geometry_reply(conn,
                                  xcb_get_geometry(conn, win),
                                  NULL);
    if (NULL == geom)
    {
        return;
    }
    
    x = rel_x;
    y = rel_y;

    if (x < 0)
    {
        x = 0;
    }
    if (y < 0)
    {
        y = 0;
    }
    if (y + geom->height + BORDERWIDTH * 2 > screen->height_in_pixels)
    {
        y = screen->height_in_pixels - (geom->height + BORDERWIDTH * 2);
    }
    if (x + geom->width + BORDERWIDTH * 2 > screen->width_in_pixels)
    {
        x = screen->width_in_pixels - (geom->width + BORDERWIDTH * 2);
    }
    
    movewindow(win, x, y);

    free(geom);
}

void mouseresize(struct client *client, int rel_x, int rel_y)
{
    uint16_t width;
    uint16_t height;
    int16_t x;
    int16_t y;
    
    /* Get window geometry. We throw away width and height values. */
    if (!getgeom(client->id, &x, &y, &width, &height))
    {
        return;
    }

    /*
     * Calculate new width and height. If we have WM hints, we use
     * them. Otherwise these are set to 1 pixel when initializing
     * client.
     *
     * Note that we need to take the absolute of the difference since
     * we're dealing with unsigned integers. This has the interesting
     * side effect that we resize the window even if the mouse pointer
     * is at the other side of the window.
     */

    width = abs(rel_x - x);
    height = abs(rel_y - y);

    width -= (width - client->base_width) % client->width_inc;
    height -= (height - client->base_height) % client->height_inc;
    
    /* Is it smaller than it wants to  be? */
    if (0 != client->min_height && height < client->min_height)
    {
        height = client->min_height;
    }

    if (0 != client->min_width && width < client->min_width)
    {
        width = client->min_width;
    }
    
    /* Check if the window fits on screen. */
    if (x + width > screen->width_in_pixels - BORDERWIDTH * 2)
    {
        width = screen->width_in_pixels - (x + BORDERWIDTH * 2);
    }
        
    if (y + height > screen->height_in_pixels - BORDERWIDTH * 2)
    {
        height = screen->height_in_pixels - (y + BORDERWIDTH * 2);
    }
    
    PDEBUG("Resizing to %dx%d (%dx%d)\n", width, height,
           (width - client->base_width) / client->width_inc,
           (height - client->base_height) / client->height_inc);
    
    resize(client->id, width, height);

    /* If this window was vertically maximized, remember that it isn't now. */
    if (client->vertmaxed)
    {
        client->vertmaxed = false;
    }
}

void movestep(struct client *client, char direction)
{
    int16_t start_x;
    int16_t start_y;
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    xcb_drawable_t win;
    
    if (NULL == client)
    {
        return;
    }

    if (client->maxed)
    {
        /* We can't move a fully maximized window. */
        return;
    }

    win = client->id;

    /* Save pointer position so we can warp pointer here later. */
    if (!getpointer(win, &start_x, &start_y))
    {
        return;
    }

    if (!getgeom(win, &x, &y, &width, &height))
    {
        return;
    }

    width = width + BORDERWIDTH * 2;
    height = height + BORDERWIDTH * 2;

    raisewindow(win);
        
    switch (direction)
    {
    case 'h':
        x = x - MOVE_STEP;
        if (x < 0)
        {
            x = 0;
        }

        movewindow(win, x, y);
        break;

    case 'j':
        y = y + MOVE_STEP;
        if (y + height > screen->height_in_pixels)
        {
            y = screen->height_in_pixels - height;
        }
        movewindow(win, x, y);
        break;

    case 'k':
        y = y - MOVE_STEP;
        if (y < 0)
        {
            y = 0;
        }
        
        movewindow(win, x, y);
        break;

    case 'l':
        x = x + MOVE_STEP;
        if (x + width > screen->width_in_pixels)
        {
            x = screen->width_in_pixels - width;
        }

        movewindow(win, x, y);
        break;

    default:
        PDEBUG("movestep: Moving in unknown direction.\n");
        break;
    } /* switch direction */

    /*
     * If the pointer was inside the window to begin with, move
     * pointer back to where it was, relative to the window.
     */
    if (start_x > 0 - BORDERWIDTH && start_x < width + BORDERWIDTH
        && start_y > 0 - BORDERWIDTH && start_y < height + BORDERWIDTH )
    {
        xcb_warp_pointer(conn, XCB_NONE, win, 0, 0, 0, 0,
                         start_x, start_y);
        xcb_flush(conn);        
    }
}

void unmax(struct client *client)
{
    uint32_t values[5];
    uint32_t mask = 0;

    if (NULL == client)
    {
        PDEBUG("unmax: client was NULL!\n");
        return;
    }
    
    /* Restore geometry. */
    if (client->maxed)
    {

        values[0] = client->x;
        values[1] = client->y;        
        values[2] = client->width;
        values[3] = client->height;

        /* Set borders again. */
        values[4] = BORDERWIDTH;
        
        mask =
            XCB_CONFIG_WINDOW_X
            | XCB_CONFIG_WINDOW_Y
            | XCB_CONFIG_WINDOW_WIDTH
            | XCB_CONFIG_WINDOW_HEIGHT
            | XCB_CONFIG_WINDOW_BORDER_WIDTH;
    }
    else
    {
        values[0] = client->y;
        values[1] = client->width;
        values[2] = client->height;

        mask = XCB_CONFIG_WINDOW_Y
            | XCB_CONFIG_WINDOW_WIDTH
            | XCB_CONFIG_WINDOW_HEIGHT;
    }

    xcb_configure_window(conn, client->id, mask, values);

    /* Warp pointer to window or we might lose it. */
    xcb_warp_pointer(conn, XCB_NONE, client->id, 0, 0, 0, 0,
                     1, 1);

    xcb_flush(conn);
}

void maximize(struct client *client)
{
    xcb_get_geometry_reply_t *geom;
    uint32_t values[4];
    uint32_t mask = 0;    
    
    if (NULL == client)
    {
        PDEBUG("maximize: client was NULL!\n");
        return;
    }

    /*
     * Check if maximized already. If so, revert to stored
     * geometry.
     */
    if (client->maxed)
    {
        unmax(client);
        client->maxed = false;
        return;
    }
    
    /* Get window geometry. */
    geom = xcb_get_geometry_reply(conn,
                                  xcb_get_geometry(conn, client->id),
                                  NULL);
    if (NULL == geom)
    {
        return;
    }

    /* Raise first. Pretty silly to maximize below something else. */
    raisewindow(client->id);
    
    /* FIXME: Store original geom in property as well? */
    client->x = geom->x;
    client->y = geom->y;
    client->width = geom->width;
    client->height = geom->height;

    /* Remove borders. */
    values[0] = 0;
    mask = XCB_CONFIG_WINDOW_BORDER_WIDTH;
    xcb_configure_window(conn, client->id, mask, values);
    
    /* Move to top left and resize. */
    values[0] = 0;
    values[1] = 0;
    values[2] = screen->width_in_pixels;
    values[3] = screen->height_in_pixels;
    xcb_configure_window(conn, client->id, XCB_CONFIG_WINDOW_X
                         | XCB_CONFIG_WINDOW_Y
                         | XCB_CONFIG_WINDOW_WIDTH
                         | XCB_CONFIG_WINDOW_HEIGHT, values);

    xcb_flush(conn);

    client->maxed = true;
    
    free(geom);    
}

void maxvert(struct client *client)
{
    uint32_t values[2];
    uint16_t width;
    uint16_t height;
    int16_t x;
    int16_t y;
    
    if (NULL == client)
    {
        PDEBUG("maxvert: client was NULL\n");
        return;
    }

    /*
     * Check if maximized already. If so, revert to stored geometry.
     */
    if (client->vertmaxed)
    {
        unmax(client);
        client->vertmaxed = false;
        return;
    }

    /* Raise first. Pretty silly to maximize below something else. */
    raisewindow(client->id);

    /* Get window geometry. */
    if (!getgeom(client->id, &x, &y, &width, &height))
    {
        return;
    }
    
    /*
     * Store original coordinates and geometry.
     * FIXME: Store in property as well?
     */
    client->x = x;
    client->y = y;
    client->width = width;
    client->height = height;

    /* Compute new height considering height increments and screen height. */
    height = screen->height_in_pixels - BORDERWIDTH * 2;
    height -= (height - client->base_height) % client->height_inc;

    /* Move to top of screen and resize. */
    values[0] = 0;
    values[1] = height;
    
    xcb_configure_window(conn, client->id, XCB_CONFIG_WINDOW_Y
                         | XCB_CONFIG_WINDOW_HEIGHT, values);
    xcb_flush(conn);

    /* Remember that this client is vertically maximized. */
    client->vertmaxed = true;    
}

bool getpointer(xcb_drawable_t win, int16_t *x, int16_t *y)
{
    xcb_query_pointer_reply_t *pointer;
    
    pointer = xcb_query_pointer_reply(
        conn, xcb_query_pointer(conn, win), 0);

    if (NULL == pointer)
    {
        return false;
    }

    *x = pointer->win_x;
    *y = pointer->win_y;

    free(pointer);

    return true;
}

bool getgeom(xcb_drawable_t win, int16_t *x, int16_t *y, uint16_t *width,
             uint16_t *height)
{
    xcb_get_geometry_reply_t *geom;
    
    geom = xcb_get_geometry_reply(conn,
                                  xcb_get_geometry(conn, win), NULL);
    
    if (NULL == geom)
    {
        return false;
    }

    *x = geom->x;
    *y = geom->y;
    *width = geom->width;
    *height = geom->height;
    
    free(geom);

    return true;
}

void topleft(void)
{
    int16_t pointx;
    int16_t pointy;

    if (NULL == focuswin)
    {
        return;
    }

    raisewindow(focuswin->id);
    
    if (!getpointer(focuswin->id, &pointx, &pointy))
    {
        return;
    }
    
    movewindow(focuswin->id, 0, 0);
    xcb_warp_pointer(conn, XCB_NONE, focuswin->id, 0, 0, 0, 0,
                     pointx, pointy);
    xcb_flush(conn);
}

void topright(void)
{
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    int16_t pointx;
    int16_t pointy;

    if (NULL == focuswin)
    {
        return;
    }

    raisewindow(focuswin->id);
    
    if (!getpointer(focuswin->id, &pointx, &pointy))
    {
        return;
    }
    
    if (!getgeom(focuswin->id, &x, &y, &width, &height))
    {
        return;
    }

    movewindow(focuswin->id, screen->width_in_pixels
               - (width + BORDERWIDTH * 2), 0);

    xcb_warp_pointer(conn, XCB_NONE, focuswin->id, 0, 0, 0, 0,
                     pointx, pointy);
    xcb_flush(conn);
}


void botleft(void)
{
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    int16_t pointx;
    int16_t pointy;

    if (NULL == focuswin)
    {
        return;
    }

    raisewindow(focuswin->id);
    
    if (!getpointer(focuswin->id, &pointx, &pointy))
    {
        return;
    }
    
    if (!getgeom(focuswin->id, &x, &y, &width, &height))
    {
        return;
    }
    
    movewindow(focuswin->id, 0, screen->height_in_pixels
               - (height + BORDERWIDTH * 2));

    xcb_warp_pointer(conn, XCB_NONE, focuswin->id, 0, 0, 0, 0,
                     pointx, pointy);
    xcb_flush(conn);
}

void botright(void)
{
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    int16_t pointx;
    int16_t pointy;

    if (NULL == focuswin)
    {
        return;
    }

    raisewindow(focuswin->id);
    
    if (!getpointer(focuswin->id, &pointx, &pointy))
    {
        return;
    }
    
    if (!getgeom(focuswin->id, &x, &y, &width, &height))
    {
        return;
    }
    
    movewindow(focuswin->id,
               screen->width_in_pixels
               - (width + BORDERWIDTH * 2),
               screen->height_in_pixels
               - (height + BORDERWIDTH * 2));

    xcb_warp_pointer(conn, XCB_NONE, focuswin->id, 0, 0, 0, 0,
                     pointx, pointy);
    xcb_flush(conn);
}

void deletewin(void)
{
    xcb_get_property_cookie_t cookie;
    xcb_get_wm_protocols_reply_t protocols;
    bool use_delete = false;
    uint32_t i;

    if (NULL == focuswin)
    {
        return;
    }

    /* Check if WM_DELETE is supported.  */
    cookie = xcb_get_wm_protocols_unchecked(conn, focuswin->id, wm_protocols);
    if (xcb_get_wm_protocols_reply(conn, cookie, &protocols, NULL) == 1) {
        for (i = 0; i < protocols.atoms_len; i++)
            if (protocols.atoms[i] == wm_delete_window)
                 use_delete = true;
    }

    xcb_get_wm_protocols_reply_wipe(&protocols);

    if (use_delete)
    {
        xcb_client_message_event_t ev = {
          .response_type = XCB_CLIENT_MESSAGE,
          .format = 32,
          .sequence = 0,
          .window = focuswin->id,
          .type = wm_protocols,
          .data.data32 = { wm_delete_window, XCB_CURRENT_TIME }
        };

        xcb_send_event(conn, false, focuswin->id,
                       XCB_EVENT_MASK_NO_EVENT, (char *) &ev);
    }
    else
    {
        xcb_kill_client(conn, focuswin->id);
    }

    xcb_flush(conn);    
}

void handle_keypress(xcb_key_press_event_t *ev)
{
    int i;
    key_enum_t key;
    
    for (key = KEY_MAX, i = KEY_F; i < KEY_MAX; i ++)
    {
        if (ev->detail == keys[i].keycode)
        {
            key = i;
        }
    }
    if (key == KEY_MAX)
    {
        PDEBUG("Unknown key pressed.\n");

        /*
         * We don't know what to do with this key. Send this key press
         * event to the focused window.
         */
        xcb_send_event(conn, false, XCB_SEND_EVENT_DEST_ITEM_FOCUS,
                       XCB_EVENT_MASK_NO_EVENT, (char *) ev);
        xcb_flush(conn);
        return;
    }

    if (MCWM_TABBING == mode && key != KEY_TAB)
    {
        /* First finish tabbing around. Then deal with the next key. */
        finishtabbing();
    }
    
    /* Is it shifted? */
    if (ev->state & SHIFTMOD)
    {
        switch (key)
        {
        case KEY_H: /* h */
            resizestep(focuswin, 'h');
            break;

        case KEY_J: /* j */
            resizestep(focuswin, 'j');
            break;

        case KEY_K: /* k */
            resizestep(focuswin, 'k');
            break;

        case KEY_L: /* l */
            resizestep(focuswin, 'l');
            break;

        default:
            /* Ignore other shifted keys. */
            break;
        }
    }
    else
    {
        switch (key)
        {
        case KEY_RET: /* return */
            start_terminal();
            break;

        case KEY_F: /* f */
            fixwindow(focuswin, true);
            break;
            
        case KEY_H: /* h */
            movestep(focuswin, 'h');
            break;

        case KEY_J: /* j */
            movestep(focuswin, 'j');
            break;

        case KEY_K: /* k */
            movestep(focuswin, 'k');
            break;

        case KEY_L: /* l */
            movestep(focuswin, 'l');
            break;

        case KEY_TAB: /* tab */
            focusnext();
            break;

        case KEY_M: /* m */
            maxvert(focuswin);
            break;

        case KEY_R: /* r*/
            raiseorlower(focuswin);
            break;
                    
        case KEY_X: /* x */
            maximize(focuswin);
            break;

        case KEY_1:
            changeworkspace(0);
            break;
            
        case KEY_2:
            changeworkspace(1);            
            break;

        case KEY_3:
            changeworkspace(2);            
            break;

        case KEY_4:
            changeworkspace(3);            
            break;

        case KEY_5:
            changeworkspace(4);            
            break;

        case KEY_6:
            changeworkspace(5);            
            break;

        case KEY_7:
            changeworkspace(6);            
            break;

        case KEY_8:
            changeworkspace(7);            
            break;

        case KEY_9:
            changeworkspace(8);            
            break;

        case KEY_0:
            changeworkspace(9);            
            break;

        case KEY_Y:
            topleft();
            break;

        case KEY_U:
            topright();
            break;

        case KEY_B:
            botleft();
            break;

        case KEY_N:
            botright();
            break;

        case KEY_END:
            deletewin();
            break;
            
        default:
            /* Ignore other keys. */
            break;            
        } /* switch unshifted */
    }
} /* handle_keypress() */

void events(void)
{
    xcb_generic_event_t *ev;

    int16_t mode_x = 0;             /* X coord when in special mode */
    int16_t mode_y = 0;             /* Y coord when in special mode */
    int fd;                         /* Our X file descriptor */
    fd_set in;                      /* For select */
    int found;                      /* Ditto. */

    /* Get the file descriptor so we can do select() on it. */
    fd = xcb_get_file_descriptor(conn);

    for (sigcode = 0; 0 == sigcode;)
    {
        /* Prepare for select(). */
        FD_ZERO(&in);
        FD_SET(fd, &in);

        /*
         * Check for events, again and again. When poll returns NULL,
         * we block on select() until the event file descriptor gets
         * readable again.
         *
         * We do it this way instead of xcb_wait_for_event() since
         * select() will return if we we're interrupted by a signal.
         * We like that.
         */
        ev = xcb_poll_for_event(conn);
        if (NULL == ev)
        {
            found = select(fd + 1, &in, NULL, NULL, NULL);
            if (-1 == found)
            {
                if (EINTR == errno)
                {
                    /* We received a signal. Break out of loop. */
                    break;
                }
                else
                {
                    /* Something was seriously wrong with select(). */
                    fprintf(stderr, "mcwm: select failed.");
                    cleanup(0);
                    exit(1);
                }
            }
            else
            {
                /* We found more events. Goto start of loop. */
                continue;
            }
        }

#ifdef DEBUG
        if (ev->response_type <= MAXEVENTS)
        {
            PDEBUG("Event: %s\n", evnames[ev->response_type]);
        }
        else
        {
            PDEBUG("Event: #%d. Not known.\n", ev->response_type);
        }
#endif

        switch (ev->response_type & ~0x80)
        {
        case XCB_MAP_REQUEST:
        {
            xcb_map_request_event_t *e;

            PDEBUG("event: Map request.\n");
            e = (xcb_map_request_event_t *) ev;
            newwin(e->window);
        }
        break;
        
        case XCB_DESTROY_NOTIFY:
        {
            xcb_destroy_notify_event_t *e;

            e = (xcb_destroy_notify_event_t *) ev;

            /*
             * If we had focus or our last focus in this window,
             * forget about the focus.
             *
             * We will get an EnterNotify if there's another window
             * under the pointer so we can set the focus proper later.
             */
            if (NULL != focuswin)
            {
                if (focuswin->id == e->window)
                {
                    focuswin = NULL;
                }
            }
            if (NULL != lastfocuswin)
            {
                if (lastfocuswin->id == e->window)
                {
                    lastfocuswin = NULL;
                }
            }
            
            /*
             * Find this window in list of clients and forget about
             * it.
             */
            forgetwin(e->window);
        }
        break;
            
        case XCB_BUTTON_PRESS:
        {
            xcb_button_press_event_t *e;
            int16_t x;
            int16_t y;
            uint16_t width;
            uint16_t height;

            e = (xcb_button_press_event_t *) ev;
            PDEBUG("Button %d pressed in window %ld, subwindow %d "
                    "coordinates (%d,%d)\n",
                   e->detail, (long)e->event, e->child, e->event_x,
                   e->event_y);

            if (e->child != 0)
            {
                /*
                 * If middle button was pressed, raise window or lower
                 * it if it was already on top.
                 */
                if (2 == e->detail)
                {
                    raiseorlower(focuswin);
                }
                else
                {
                    int16_t pointx;
                    int16_t pointy;

                    /* We're moving or resizing. */

                    /*
                     * Get and save pointer position inside the window
                     * so we can go back to it when we're done moving
                     * or resizing.
                     */
                    if (!getpointer(e->child, &pointx, &pointy))
                    {
                        break;
                    }

                    mode_x = pointx;
                    mode_y = pointy;

                    /* Raise window. */
                    raisewindow(e->child);

                    /* Get window geometry. */
                    if (!getgeom(e->child, &x, &y, &width, &height))
                    {
                        break;
                    }

                    /* Mouse button 1 was pressed. */
                    if (1 == e->detail)
                    {
                        mode = MCWM_MOVE;

                        /*
                         * Warp pointer to upper left of window before
                         * starting move.
                         */
                        xcb_warp_pointer(conn, XCB_NONE, e->child, 0, 0, 0, 0,
                                         1, 1);
                    }
                    else
                    {
                        /* Mouse button 3 was pressed. */

                        mode = MCWM_RESIZE;

                        /* Warp pointer to lower right. */
                        xcb_warp_pointer(conn, XCB_NONE, e->child, 0, 0, 0, 0,
                                         width, height);
                    }

                    /*
                     * Take control of the pointer in the root window
                     * and confine it to root.
                     *
                     * Give us events when the key is released or if
                     * any motion occurs with the key held down, but
                     * give us only hints about movement. We ask for
                     * the position ourselves later.
                     *
                     * Keep updating everything else.
                     *
                     * Don't use any new cursor.
                     */
                    xcb_grab_pointer(conn, 0, screen->root,
                                     XCB_EVENT_MASK_BUTTON_RELEASE
                                     | XCB_EVENT_MASK_BUTTON_MOTION,
                                     XCB_GRAB_MODE_ASYNC,
                                     XCB_GRAB_MODE_ASYNC,
                                     screen->root,
                                     XCB_NONE,
                                     XCB_CURRENT_TIME);

                    xcb_flush(conn);

                    PDEBUG("mode now : %d\n", mode);
                }
            } /* subwindow */
            else
            {
                /*
                 * Do something on the root when mouse buttons are
                 * pressed?
                 */
            }
        }
        break;

        case XCB_MOTION_NOTIFY:
        {
            xcb_motion_notify_event_t *e;

            e = (xcb_motion_notify_event_t *) ev;

            /*
             * Our pointer is moving and since we even get this event
             * we're either resizing or moving a window.
             *
             * We don't bother actually doing anything if we don't
             * have a focused window or if the focused window is
             * totally maximized.
             */
            if (mode == MCWM_MOVE)
            {
                if (NULL != focuswin && !focuswin->maxed)
                {
                    mousemove(focuswin->id, e->root_x, e->root_y);
                }
            }
            else if (mode == MCWM_RESIZE)
            {
                /* Resize. */
                if (NULL != focuswin && !focuswin->maxed)
                {
                    mouseresize(focuswin, e->root_x, e->root_y);
                }
            }
            else
            {
                PDEBUG("Motion event when we're not moving our resizing!\n");
            }
        }
        break;

        case XCB_BUTTON_RELEASE:
            PDEBUG("Mouse button released! mode = %d\n", mode);

            if (0 == mode)
            {
                /*
                 * Mouse button released, but not in a saved mode. Do
                 * nothing.
                 */
                break;
            }
            else
            {
                int16_t x;
                int16_t y;
                uint16_t width;
                uint16_t height;
                
                /* We're finished moving or resizing. */

                if (NULL == focuswin)
                {
                    /*
                     * We don't seem to have a focused window! Just
                     * ungrab and reset the mode.
                     */
                    PDEBUG("No focused window when finished moving or "
                           "resizing!");
                
                    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
                    xcb_flush(conn); /* Important! */
                    
                    mode = 0;
                    break;
                }
                
                /*
                 * We will get an EnterNotify and focus another window
                 * if the pointer just happens to be on top of another
                 * window when we ungrab the pointer, so we have to
                 * warp the pointer before to prevent this.
                 */
                if (!getgeom(focuswin->id, &x, &y, &width, &height))
                {
                    break;
                }

                /*
                 * Move to saved position within window or if that
                 * position is now outside current window, move inside
                 * window.
                 */
                if (mode_x > width)
                {
                    x = width / 2;
                    if (0 == x)
                    {
                        x = 1;
                    }
                    
                }
                else
                {
                    x = mode_x;
                }

                if (mode_y > height)
                {
                    y = height / 2;
                    if (0 == y)
                    {
                        y = 1;
                    }
                }
                else
                {
                    y = mode_y;
                }

                xcb_warp_pointer(conn, XCB_NONE, focuswin->id, 0, 0, 0, 0,
                                 x, y);
                
                xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
                xcb_flush(conn); /* Important! */
                
                mode = 0;
                PDEBUG("mode now = %d\n", mode);
            }
        break;
                
        case XCB_KEY_PRESS:
        {
            xcb_key_press_event_t *e = (xcb_key_press_event_t *)ev;
            
            PDEBUG("Key %d pressed\n", e->detail);

            handle_keypress(e);
        }
        break;

        case XCB_KEY_RELEASE:
        {
            xcb_key_release_event_t *e = (xcb_key_release_event_t *)ev;
            unsigned i;
            
            PDEBUG("Key %d released.\n", e->detail);

            if (MCWM_TABBING == mode)
            {
                /*
                 * Check if it's the that was released was a key
                 * generating the MODKEY mask.
                 */
                for (i = 0; i < modkeys.len; i ++)
                {
                    PDEBUG("Is it %d?\n", modkeys.keycodes[i]);
                    
                    if (e->detail == modkeys.keycodes[i])
                    {
                        finishtabbing();

                        /* Get out of for... */
                        break;
                    }
                } /* for keycodes. */
            } /* if tabbing. */
        }
        break;
            
        case XCB_ENTER_NOTIFY:
        {
            xcb_enter_notify_event_t *e = (xcb_enter_notify_event_t *)ev;
            struct client *client;
            
            PDEBUG("event: Enter notify eventwin %d, child %d, detail %d.\n",
                   e->event,
                   e->child,
                   e->detail);

            /*
             * If this isn't a normal enter notify, don't bother.
             *
             * We also need ungrab events, since these will be
             * generated on button and key grabs and if the user for
             * some reason presses a button on the root and then moves
             * the pointer to our window and releases the button, we
             * get an Ungrab EnterNotify.
             * 
             * The other cases means the pointer is grabbed and that
             * either means someone is using it for menu selections or
             * that we're moving or resizing. We don't want to change
             * focus in those cases.
             */
            if (e->mode == XCB_NOTIFY_MODE_NORMAL
                || e->mode == XCB_NOTIFY_MODE_UNGRAB)
            {
                /*
                 * If we're entering the same window we focus now,
                 * then don't bother focusing.
                 */
                if (NULL == focuswin || e->event != focuswin->id)
                {
                    /*
                     * Otherwise, set focus to the window we just
                     * entered if we can find it among the windows we
                     * know about. If not, just keep focus in the old
                     * window.
                     */
                    client = findclient(e->event);
                    if (NULL != client)
                    {
                        if (MCWM_TABBING != mode)
                        {
                            /*
                             * We are focusing on a new window. Since
                             * we're not currently tabbing around the
                             * window ring, we need to update the
                             * current workspace window list: Move
                             * first the old focus to the head of the
                             * list and then the new focus to the head
                             * of the list.
                             */
                            if (NULL != focuswin)
                            {
                                movetohead(&wslist[curws],
                                           focuswin->wsitem[curws]);
                                lastfocuswin = NULL;                 
                            }

                            movetohead(&wslist[curws], client->wsitem[curws]);
                        } /* if not tabbing */

                        setfocus(client);
                    }
                }
            }

        }
        break;        
        
        case XCB_CONFIGURE_NOTIFY:
        {
            xcb_configure_notify_event_t *e
                = (xcb_configure_notify_event_t *)ev;
            
            if (e->window == screen->root)
            {
                /*
                 * When using RANDR, the root can change geometry when
                 * the user adds a new screen, tilts their screen 90
                 * degrees or whatnot. We might need to rearrange
                 * windows to be visible.
                 *
                 * We might get notified for several reasons, not just
                 * if the geometry changed. If the geometry is
                 * unchanged, do nothing.
                 * 
                 */
                PDEBUG("Notify event for root!\n");
                PDEBUG("Possibly a new root geometry: %dx%d\n",
                       e->width, e->height);

                if (e->width == screen->width_in_pixels
                    && e->height == screen->height_in_pixels)
                {
                    /* Root geometry is really unchanged. Do nothing. */
                    PDEBUG("Hey! Geometry didn't change.\n");
                }
                else
                {
                    arrangewindows(e->width, e->height);
                    screen->width_in_pixels = e->width;
                    screen->height_in_pixels = e->height;
                }
            }
        }
        break;
        
        case XCB_CONFIGURE_REQUEST:
        {
            xcb_configure_request_event_t *e
                = (xcb_configure_request_event_t *)ev;
            uint32_t mask = 0;
            uint32_t values[7];
            int i = -1;
            
            PDEBUG("event: Configure request. mask = %d\n", e->value_mask);

            /*
             * We ignore border width configurations, but handle all
             * others.
             */

            if (e->value_mask & XCB_CONFIG_WINDOW_X)
            {
                PDEBUG("Changing X coordinate to %d\n", e->x);
                mask |= XCB_CONFIG_WINDOW_X;
                i ++;                
                values[i] = e->x;
            }

            if (e->value_mask & XCB_CONFIG_WINDOW_Y)
            {
                PDEBUG("Changing Y coordinate to %d.\n", e->y);
                mask |= XCB_CONFIG_WINDOW_Y;
                i ++;                
                values[i] = e->y;
            }
            
            if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH)
            {
                PDEBUG("Changing width to %d.\n", e->width);
                mask |= XCB_CONFIG_WINDOW_WIDTH;
                i ++;                
                values[i] = e->width;
            }
            
            if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
            {
                PDEBUG("Changing height to %d.\n", e->height);
                mask |= XCB_CONFIG_WINDOW_HEIGHT;
                i ++;                
                values[i] = e->height;
            }

            if (e->value_mask & XCB_CONFIG_WINDOW_SIBLING)
            {
                mask |= XCB_CONFIG_WINDOW_SIBLING;
                i ++;                
                values[i] = e->sibling;
            }

            if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
            {
                PDEBUG("Changing stack order.\n");
                mask |= XCB_CONFIG_WINDOW_STACK_MODE;
                i ++;                
                values[i] = e->stack_mode;
            }

            if (-1 != i)
            {
                xcb_configure_window(conn, e->window, mask, values);
                xcb_flush(conn);
            }
        }
        break;

        case XCB_CIRCULATE_REQUEST:
        {
            xcb_circulate_request_event_t *e
                = (xcb_circulate_request_event_t *)ev;

            /*
             * Subwindow e->window to parent e->event is about to be
             * restacked.
             *
             * Just do what was requested, e->place is either
             * XCB_PLACE_ON_TOP or _ON_BOTTOM. We don't care.
             */
            xcb_circulate_window(conn, e->window, e->place);
        }
        break;

        case XCB_MAPPING_NOTIFY:
        {
            xcb_mapping_notify_event_t *e
                = (xcb_mapping_notify_event_t *)ev;

            /*
             * XXX Gah! We get a new notify message for *every* key!
             * We want to know when the entire keyboard is finished.
             * Impossible? Better handling somehow?
             */

            /*
             * We're only interested in keys and modifiers, not
             * pointer mappings, for instance.
             */
            if (e->request != XCB_MAPPING_MODIFIER
                && e->request != XCB_MAPPING_KEYBOARD)
            {
                break;
            }
            
            /* Forget old key bindings. */
            xcb_ungrab_key(conn, XCB_GRAB_ANY, screen->root, XCB_MOD_MASK_ANY);

            /* Use the new ones. */
            setupkeys();
        }
        break;
        
        case XCB_UNMAP_NOTIFY:
        {
            xcb_unmap_notify_event_t *e =
                (xcb_unmap_notify_event_t *)ev;
            struct item *item;
            struct client *client;

            /*
             * Find the window in our *current* workspace list, then
             * forget about it. If it gets mapped, we add it to our
             * lists again then.
             *
             * Note that we might not know about the window we got the
             * UnmapNotify event for. It might be a window we just
             * unmapped on *another* workspace when changing
             * workspaces, for instance, or it might be a window with
             * override redirect set. This is not an error.
             *
             * XXX We might need to look in the global window list,
             * after all. Consider if a window is unmapped on our last
             * workspace while changing workspaces... If we do this,
             * we need to keep track of our own windows and ignore
             * UnmapNotify on them.
             */
            for (item = wslist[curws]; item != NULL; item = item->next)
            {
                client = item->data;
                
                if (client->id == e->window)
                {
                    PDEBUG("Forgetting about %d\n", e->window);
                    forgetclient(client);
                    break;
                }
            } /* for */
        } 
        break;
            
        } /* switch */

        /* Forget about this event. */
        free(ev);
    }
}

void printhelp(void)
{
    printf("mcwm: Usage: mcwm [-b] [-t terminal-program] [-f colour] "
           "[-u colour] [-x colour] \n");
    printf("  -b means draw no borders\n");
    printf("  -t urxvt will start urxvt when MODKEY + Return is pressed\n");
    printf("  -f colour sets colour for focused window borders of focused "
           "to a named color.\n");
    printf("  -u colour sets colour for unfocused window borders.");
    printf("  -x color sets colour for fixed window borders.");    
}

void sigcatch(int sig)
{
    sigcode = sig;
}

int main(int argc, char **argv)
{
    uint32_t mask = 0;
    uint32_t values[2];
    char ch;                    /* Option character */
    xcb_void_cookie_t cookie;
    xcb_generic_error_t *error;
    xcb_drawable_t root;
    char *focuscol;
    char *unfocuscol;
    char *fixedcol;    

    /* Install signal handlers. */

    if (SIG_ERR == signal(SIGINT, sigcatch))
    {
        perror("mcwm: signal");
        exit(1);
    }

    if (SIG_ERR == signal(SIGSEGV, sigcatch))
    {
        perror("mcwm: signal");
        exit(1);
    }

    if (SIG_ERR == signal(SIGTERM, sigcatch))
    {
        perror("mcwm: signal");
        exit(1);
    }
    
    /* Set up defaults. */
    
    conf.borders = true;
    conf.terminal = TERMINAL;
    focuscol = FOCUSCOL;
    unfocuscol = UNFOCUSCOL;
    fixedcol = FIXEDCOL;
    
    while (1)
    {
        ch = getopt(argc, argv, "bt:f:u:x:");
        if (-1 == ch)
        {
                
            /* No more options, break out of while loop. */
            break;
        }
        
        switch (ch)
        {
        case 'b':
            /* No borders. */
            conf.borders = false;
            break;

        case 't':
            conf.terminal = optarg;
            break;

        case 'f':
            focuscol = optarg;
            break;

        case 'u':
            unfocuscol = optarg;
            break;

        case 'x':
            fixedcol = optarg;
            break;
            
        default:
            printhelp();
            exit(0);
        } /* switch */
    }
    
    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn))
    {
        perror("xcb_connect");
        exit(1);
    }
    
    /* Get the first screen */
    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    root = screen->root;
    
    PDEBUG("Screen size: %dx%d\nRoot window: %d\n", screen->width_in_pixels,
           screen->height_in_pixels, screen->root);
    
    /* Get some colours. */
    conf.focuscol = getcolor(focuscol);
    conf.unfocuscol = getcolor(unfocuscol);
    conf.fixedcol = getcolor(fixedcol);
    
    /* Get some atoms. */
    atom_desktop = xcb_atom_get(conn, "_NET_WM_DESKTOP");
    wm_delete_window = xcb_atom_get(conn, "WM_DELETE_WINDOW");
    wm_protocols = xcb_atom_get(conn, "WM_PROTOCOLS");
    
    /* Loop over all clients and set up stuff. */
    if (0 != setupscreen())
    {
        fprintf(stderr, "Failed to initialize windows. Exiting.\n");
        exit(1);
    }

    /* Set up key bindings. */
    if (0 != setupkeys())
    {
        fprintf(stderr, "mcwm: Couldn't set up keycodes. Exiting.");
        exit(1);
    }

    /* Grab mouse buttons. */

    xcb_grab_button(conn, 0, root, XCB_EVENT_MASK_BUTTON_PRESS
                    | XCB_EVENT_MASK_BUTTON_RELEASE,
                    XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, root, XCB_NONE,
                    1 /* left mouse button */,
                    MOUSEMODKEY);

    xcb_grab_button(conn, 0, root, XCB_EVENT_MASK_BUTTON_PRESS
                    | XCB_EVENT_MASK_BUTTON_RELEASE,
                    XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, root, XCB_NONE,
                    2 /* middle mouse button */,
                    MOUSEMODKEY);

    xcb_grab_button(conn, 0, root, XCB_EVENT_MASK_BUTTON_PRESS
                    | XCB_EVENT_MASK_BUTTON_RELEASE,
                    XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, root, XCB_NONE,
                    3 /* right mouse button */,
                    MOUSEMODKEY);

    /* Subscribe to events. */
    mask = XCB_CW_EVENT_MASK;

    values[0] = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
        | XCB_EVENT_MASK_STRUCTURE_NOTIFY
        | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;

    cookie =
        xcb_change_window_attributes_checked(conn, root, mask, values);
    error = xcb_request_check(conn, cookie);

    xcb_flush(conn);
    
    if (NULL != error)
    {
        fprintf(stderr, "mcwm: Can't get SUBSTRUCTURE REDIRECT. "
                "Error code: %d\n"
                "Another window manager running? Exiting.\n",
                error->error_code);

        xcb_disconnect(conn);
        
        exit(1);
    }

    /* Loop over events. */
    events();

    /* Die gracefully. */
    cleanup(sigcode);

    exit(0);
}
