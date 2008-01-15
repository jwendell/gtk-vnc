/*
  GTK-VNC-PLUGIN

  By Richard W.M. Jones <rjones@redhat.com>
  Copyright (C) 2008 Red Hat Inc.

  Largely based on DiamondX (http://multimedia.cx/diamondx/), which itself
  is based on Mozilla sources.

  DiamondX copyright notice:

  Example XEmbed-aware Mozilla browser plugin by Adobe.

  Copyright (c) 2007 Adobe Systems Incorporated

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
  LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
  OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <vncdisplay.h>

#include "gtk-vnc-plugin.h"

static void
vnc_connected (GtkWidget *vnc G_GNUC_UNUSED, void *Thisv)
{
  PluginInstance *This = (PluginInstance *) Thisv;

  debug ("vnc_connected, This=%p", This);
}

static void
vnc_disconnected (GtkWidget *vnc G_GNUC_UNUSED, void *Thisv)
{
  PluginInstance *This = (PluginInstance *) Thisv;

  debug ("vnc_disconnected, This=%p", This);
}

static void
vnc_auth_credential (GtkWidget *vnc, GValueArray *creds, void *Thisv)
{
  GtkWidget *dlg = NULL;
  PluginInstance *This = (PluginInstance *) Thisv;
  unsigned i, prompt = 0;
  const char **data;

  debug ("vnc_auth_credential, This=%p", This);

  data = g_new0 (const char *, creds->n_values);

  for (i = 0; i < creds->n_values; ++i) {
    GValue *cred = g_value_array_get_nth (creds, i);
    switch (g_value_get_enum (cred)) {
    case VNC_DISPLAY_CREDENTIAL_USERNAME:
    case VNC_DISPLAY_CREDENTIAL_PASSWORD:
      prompt++;
      break;
    case VNC_DISPLAY_CREDENTIAL_CLIENTNAME:
      data[i] = "gvncviewer";
      break;
    }
  }

  /* Prompt user for username and password. */
  if (prompt) {
    GtkWidget **label, **entry, *box, *vbox;
    int row, response;

    dlg = gtk_dialog_new_with_buttons
      ("Gtk-VNC: Authentication required",
       GTK_WINDOW (This->container),
       GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
       GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
       GTK_STOCK_OK, GTK_RESPONSE_OK,
       NULL);
    gtk_dialog_set_default_response (GTK_DIALOG(dlg), GTK_RESPONSE_OK);

    box = gtk_table_new (creds->n_values, 2, FALSE);
    label = g_new (GtkWidget *, prompt);
    entry = g_new (GtkWidget *, prompt);

    for (i = 0, row = 0; i < creds->n_values; ++i) {
      GValue *cred = g_value_array_get_nth (creds, i);
      switch (g_value_get_enum(cred)) {
      case VNC_DISPLAY_CREDENTIAL_USERNAME:
	label[row] = gtk_label_new("Username:");
	break;
      case VNC_DISPLAY_CREDENTIAL_PASSWORD:
	label[row] = gtk_label_new("Password:");
	break;
      default:
	continue;
      }
      entry[row] = gtk_entry_new();
      if (g_value_get_enum (cred) == VNC_DISPLAY_CREDENTIAL_PASSWORD)
	gtk_entry_set_visibility (GTK_ENTRY (entry[row]), FALSE);

      gtk_table_attach(GTK_TABLE(box), label[i], 0, 1, row, row+1, GTK_SHRINK, GTK_SHRINK, 3, 3);
      gtk_table_attach(GTK_TABLE(box), entry[i], 1, 2, row, row+1, GTK_SHRINK, GTK_SHRINK, 3, 3);
      row++;
    }

    vbox = gtk_bin_get_child(GTK_BIN(dlg));
    gtk_container_add(GTK_CONTAINER(vbox), box);

    gtk_widget_show_all(dlg);
    response = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_hide(GTK_WIDGET(dlg));

    if (response == GTK_RESPONSE_OK) {
      for (i = 0, row = 0 ; i < creds->n_values ; i++) {
	GValue *cred = g_value_array_get_nth(creds, i);
	switch (g_value_get_enum(cred)) {
	case VNC_DISPLAY_CREDENTIAL_USERNAME:
	case VNC_DISPLAY_CREDENTIAL_PASSWORD:
	  data[i] = gtk_entry_get_text(GTK_ENTRY(entry[row]));
	  row++;
	  break;
	}
      }
    }
  }

  /* Set the credentials. */
  for (i = 0 ; i < creds->n_values ; i++) {
    GValue *cred = g_value_array_get_nth(creds, i);
    if (data[i]) {
      if (vnc_display_set_credential(VNC_DISPLAY(vnc),
				     g_value_get_enum(cred),
				     data[i])) {
	debug ("Failed to set credential type %d", g_value_get_enum(cred));
	vnc_display_close(VNC_DISPLAY(vnc));
      }
    } else {
      debug ("Unsupported credential type %d", g_value_get_enum(cred));
      vnc_display_close(VNC_DISPLAY(vnc));
    }
  }

  g_free (data);
  if (dlg)
    gtk_widget_destroy (GTK_WIDGET(dlg));
}

NPError
GtkVNCXSetWindow (NPP instance, NPWindow *window)
{
  PluginInstance *This;
  NPSetWindowCallbackStruct *ws_info;

  if (instance == NULL)
    return NPERR_INVALID_INSTANCE_ERROR;

  This = (PluginInstance*) instance->pdata;

  debug ("GtkVNCXSetWindow, This=%p", This);

  if (This == NULL)
    return NPERR_INVALID_INSTANCE_ERROR;

  ws_info = (NPSetWindowCallbackStruct *)window->ws_info;

  /* Mozilla likes to re-run its greatest hits */
  if (window == This->window &&
      window->x == This->x &&
      window->y == This->y &&
      window->width == This->width &&
      window->height == This->height) {
    debug ("gtk-vnc-plugin: window re-run; returning");
    return NPERR_NO_ERROR;
  }

  This->window = window;
  This->x = window->x;
  This->y = window->y;
  This->width = window->width;
  This->height = window->height;

  /* Create a GtkPlug container and a Gtk-VNC widget inside it. */
  This->container = gtk_plug_new ((GdkNativeWindow)(long)window->window);
  This->vnc = vnc_display_new ();

  /* Make sure the canvas is capable of receiving focus. */
  GTK_WIDGET_SET_FLAGS (GTK_WIDGET(This->vnc), GTK_CAN_FOCUS);

  /* All the events that our canvas wants to receive */
  gtk_widget_add_events
    (This->vnc,
     GDK_BUTTON_PRESS_MASK | 
     GDK_BUTTON_RELEASE_MASK |
     GDK_KEY_PRESS_MASK | 
     GDK_KEY_RELEASE_MASK | 
     GDK_POINTER_MOTION_MASK |
     GDK_SCROLL_MASK |
     GDK_EXPOSURE_MASK |
     GDK_VISIBILITY_NOTIFY_MASK |
     GDK_ENTER_NOTIFY_MASK |
     GDK_LEAVE_NOTIFY_MASK |
     GDK_FOCUS_CHANGE_MASK);

  /* Connect up the signals. */
  g_signal_connect (G_OBJECT(This->vnc), "vnc-connected",
		    G_CALLBACK(vnc_connected), This);
  g_signal_connect (G_OBJECT(This->vnc), "vnc-disconnected",
		    G_CALLBACK(vnc_disconnected), This);
#if 0
  g_signal_connect (G_OBJECT(This->vnc), "vnc-initialized",
		    G_CALLBACK(vnc_initialized), This);
#endif
  g_signal_connect (G_OBJECT(This->vnc), "vnc-auth-credential",
		    G_CALLBACK(vnc_auth_credential), This);
#if 0
  g_signal_connect (G_OBJECT(This->vnc), "vnc-auth-failure",
		    G_CALLBACK(vnc_auth_failure), This);
  g_signal_connect (G_OBJECT(This->vnc), "vnc-desktop-resize",
		    G_CALLBACK(vnc_desktop_resize), This);
  g_signal_connect (G_OBJECT(This->vnc), "vnc-pointer-grab",
		    G_CALLBACK(vnc_pointer_grab), This);
  g_signal_connect (G_OBJECT(This->vnc), "vnc-pointer-ungrab",
		    G_CALLBACK(vnc_pointer_ungrab), This);
  g_signal_connect (G_OBJECT(This->vnc), "key-press-event",
		    G_CALLBACK(vnc_screenshow), This);
#endif

  gtk_widget_show (This->vnc);

  gtk_container_add (GTK_CONTAINER(This->container), This->vnc);

  gtk_widget_show (This->container);

  /*gtk_widget_realize (This->vnc);*/

  /* Start connection to remote host. */
  if (This->host && This->port) {
    debug ("starting connection to %s:%s", This->host, This->port);
    vnc_display_open_host (VNC_DISPLAY(This->vnc), This->host, This->port);
  }

  return NPERR_NO_ERROR;
}

NPError
GtkVNCDestroyWindow (NPP instance)
{
  PluginInstance *This = (PluginInstance*) instance->pdata;

  debug ("GtkVNCDestroyWindow, This=%p", This);

  if (This && This->container) {
    gtk_widget_destroy (This->container);
    This->container = This->vnc = NULL;
  }

  return NPERR_NO_ERROR;
}

static NPWindow windowlessWindow;

int16
GtkVNCXHandleEvent(NPP instance, void *event)
{
  XGraphicsExposeEvent exposeEvent;
  XEvent *nsEvent;

  debug ("GtkVNCXHandleEvent");

  nsEvent = (XEvent *) event;
  exposeEvent = nsEvent->xgraphicsexpose;

  /*printf(" event: x, y, w, h = %d, %d, %d, %d; display @ %p, window/drawable = %d\n",
    exposeEvent.x,
    exposeEvent.y,
    exposeEvent.width,
    exposeEvent.height,
    exposeEvent.display,
    exposeEvent.drawable);*/

  windowlessWindow.window = exposeEvent.display;
  windowlessWindow.x = exposeEvent.x;
  windowlessWindow.y = exposeEvent.y;
  windowlessWindow.width = exposeEvent.width;
  windowlessWindow.height = exposeEvent.height;
  windowlessWindow.ws_info = (void *)exposeEvent.drawable;

  NPP_SetWindow(instance, &windowlessWindow);

  return 0;
}
