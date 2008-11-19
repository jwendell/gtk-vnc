/*
 * GTK VNC Widget
 *  
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.0 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "vncdisplay.h"
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "config.h"

#if WITH_LIBVIEW
#include <libview/autoDrawer.h>
#endif

static gchar **args = NULL;
static const GOptionEntry options [] =
{
  { 
    G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_STRING_ARRAY, &args,
    NULL, "hostname[:display]" },
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, 0 }
};


static GtkWidget *vnc;

static void set_title(VncDisplay *vncdisplay, GtkWidget *window,
	gboolean grabbed)
{
	const char *name;
	char title[1024];
	const char *subtitle;

	if (grabbed)
		subtitle = "(Press Ctrl+Alt to release pointer) ";
	else
		subtitle = "";

	name = vnc_display_get_name(VNC_DISPLAY(vncdisplay));
	snprintf(title, sizeof(title), "%s%s - GVncViewer",
		 subtitle, name);

	gtk_window_set_title(GTK_WINDOW(window), title);
}

static gboolean vnc_screenshot(GtkWidget *window G_GNUC_UNUSED,
	GdkEvent *ev, GtkWidget *vncdisplay)
{
	if (ev->key.keyval == GDK_F11) {
		GdkPixbuf *pix = vnc_display_get_pixbuf(VNC_DISPLAY(vncdisplay));
		gdk_pixbuf_save(pix, "gvncviewer.png", "png", NULL, "tEXt::Generator App", "gvncviewer", NULL);
		gdk_pixbuf_unref(pix);
		printf("Screenshot saved to gvncviewer.png\n");
	}
	return FALSE;
}

static void vnc_grab(GtkWidget *vncdisplay, GtkWidget *window)
{
	set_title(VNC_DISPLAY(vncdisplay), window, TRUE);
}

static void vnc_ungrab(GtkWidget *vncdisplay, GtkWidget *window)
{
	set_title(VNC_DISPLAY(vncdisplay), window, FALSE);
}

static void vnc_connected(GtkWidget *vncdisplay G_GNUC_UNUSED)
{
	printf("Connected to server\n");
}

static void vnc_initialized(GtkWidget *vncdisplay, GtkWidget *window)
{
	printf("Connection initialized\n");
	set_title(VNC_DISPLAY(vncdisplay), window, FALSE);
	gtk_widget_show_all(window);
}

static void vnc_auth_failure(GtkWidget *vncdisplay G_GNUC_UNUSED,
	const char *msg)
{
	printf("Authentication failed '%s'\n", msg ? msg : "");
}

static void vnc_desktop_resize(GtkWidget *vncdisplay G_GNUC_UNUSED,
	int width, int height)
{
	printf("Remote desktop size changed to %dx%d\n", width, height);
}

static void vnc_disconnected(GtkWidget *vncdisplay G_GNUC_UNUSED)
{
	printf("Disconnected from server\n");
	gtk_main_quit();
}

static void send_caf1(GtkWidget *menu G_GNUC_UNUSED, GtkWidget *vncdisplay)
{
	guint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_F1 };
	printf("Sending Ctrl+Alt+F1\n");
	vnc_display_send_keys(VNC_DISPLAY(vncdisplay), keys,
		sizeof(keys)/sizeof(keys[0]));
}

static void send_caf7(GtkWidget *menu G_GNUC_UNUSED, GtkWidget *vncdisplay)
{
	guint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_F7 };
	printf("Sending Ctrl+Alt+F7\n");
	vnc_display_send_keys(VNC_DISPLAY(vncdisplay), keys,
		sizeof(keys)/sizeof(keys[0]));
}

static void send_cad(GtkWidget *menu G_GNUC_UNUSED, GtkWidget *vncdisplay)
{
	guint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_Delete };
	printf("Sending Ctrl+Alt+Delete\n");
	vnc_display_send_keys(VNC_DISPLAY(vncdisplay), keys,
		sizeof(keys)/sizeof(keys[0]));
}

static void send_cab(GtkWidget *menu G_GNUC_UNUSED, GtkWidget *vncdisplay)
{
	guint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_BackSpace };
	printf("Sending Ctrl+Alt+Backspace\n");
	vnc_display_send_keys(VNC_DISPLAY(vncdisplay), keys,
		sizeof(keys)/sizeof(keys[0]));
}

static void do_fullscreen(GtkWidget *menu, GtkWidget *window)
{
	if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menu)))
		gtk_window_fullscreen(GTK_WINDOW(window));
	else
		gtk_window_unfullscreen(GTK_WINDOW(window));
}

static void do_scaling(GtkWidget *menu, GtkWidget *vncdisplay)
{
	if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menu)))
		vnc_display_set_scaling(VNC_DISPLAY(vncdisplay), TRUE);
	else
		vnc_display_set_scaling(VNC_DISPLAY(vncdisplay), FALSE);
}

static void vnc_credential(GtkWidget *vncdisplay, GValueArray *credList)
{
	GtkWidget *dialog = NULL;
	int response;
	unsigned int i, prompt = 0;
	const char **data;

	printf("Got credential request for %d credential(s)\n", credList->n_values);

	data = g_new0(const char *, credList->n_values);

	for (i = 0 ; i < credList->n_values ; i++) {
		GValue *cred = g_value_array_get_nth(credList, i);
		switch (g_value_get_enum(cred)) {
		case VNC_DISPLAY_CREDENTIAL_USERNAME:
		case VNC_DISPLAY_CREDENTIAL_PASSWORD:
			prompt++;
			break;
		case VNC_DISPLAY_CREDENTIAL_CLIENTNAME:
			data[i] = "gvncviewer";
		default:
			break;
		}
	}

	if (prompt) {
		GtkWidget **label, **entry, *box, *vbox;
		int row;
		dialog = gtk_dialog_new_with_buttons("Authentication required",
						     NULL,
						     0,
						     GTK_STOCK_CANCEL,
						     GTK_RESPONSE_CANCEL,
						     GTK_STOCK_OK,
						     GTK_RESPONSE_OK,
						     NULL);
		gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

		box = gtk_table_new(credList->n_values, 2, FALSE);
		label = g_new(GtkWidget *, prompt);
		entry = g_new(GtkWidget *, prompt);

		for (i = 0, row =0 ; i < credList->n_values ; i++) {
			GValue *cred = g_value_array_get_nth(credList, i);
			entry[row] = gtk_entry_new();
			switch (g_value_get_enum(cred)) {
			case VNC_DISPLAY_CREDENTIAL_USERNAME:
				label[row] = gtk_label_new("Username:");
				break;
			case VNC_DISPLAY_CREDENTIAL_PASSWORD:
				label[row] = gtk_label_new("Password:");
				gtk_entry_set_activates_default(GTK_ENTRY(entry[row]), TRUE);
				break;
			default:
				continue;
			}
			if (g_value_get_enum (cred) == VNC_DISPLAY_CREDENTIAL_PASSWORD)
				gtk_entry_set_visibility (GTK_ENTRY (entry[row]), FALSE);

			gtk_table_attach(GTK_TABLE(box), label[i], 0, 1, row, row+1, GTK_SHRINK, GTK_SHRINK, 3, 3);
			gtk_table_attach(GTK_TABLE(box), entry[i], 1, 2, row, row+1, GTK_SHRINK, GTK_SHRINK, 3, 3);
			row++;
		}

		vbox = gtk_bin_get_child(GTK_BIN(dialog));
		gtk_container_add(GTK_CONTAINER(vbox), box);

		gtk_widget_show_all(dialog);
		response = gtk_dialog_run(GTK_DIALOG(dialog));
		gtk_widget_hide(GTK_WIDGET(dialog));

		if (response == GTK_RESPONSE_OK) {
			for (i = 0, row = 0 ; i < credList->n_values ; i++) {
				GValue *cred = g_value_array_get_nth(credList, i);
				switch (g_value_get_enum(cred)) {
				case VNC_DISPLAY_CREDENTIAL_USERNAME:
				case VNC_DISPLAY_CREDENTIAL_PASSWORD:
					data[i] = gtk_entry_get_text(GTK_ENTRY(entry[row]));
					break;
				}
			}
		}
	}

	for (i = 0 ; i < credList->n_values ; i++) {
		GValue *cred = g_value_array_get_nth(credList, i);
		if (data[i]) {
			if (vnc_display_set_credential(VNC_DISPLAY(vncdisplay),
						       g_value_get_enum(cred),
						       data[i])) {
				printf("Failed to set credential type %d\n", g_value_get_enum(cred));
				vnc_display_close(VNC_DISPLAY(vncdisplay));
			}
		} else {
			printf("Unsupported credential type %d\n", g_value_get_enum(cred));
			vnc_display_close(VNC_DISPLAY(vncdisplay));
		}
	}

	g_free(data);
	if (dialog)
		gtk_widget_destroy(GTK_WIDGET(dialog));
}

#if WITH_LIBVIEW
static gboolean window_state_event(GtkWidget *widget,
				   GdkEventWindowState *event,
				   gpointer data)
{
	ViewAutoDrawer *drawer = VIEW_AUTODRAWER(data);

	if (event->changed_mask & GDK_WINDOW_STATE_FULLSCREEN) {
		if (event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) {
			vnc_display_force_grab(VNC_DISPLAY(vnc), TRUE);
			ViewAutoDrawer_SetActive(drawer, TRUE);
		} else {
			vnc_display_force_grab(VNC_DISPLAY(vnc), FALSE);
			ViewAutoDrawer_SetActive(drawer, FALSE);
		}
	}

	return FALSE;
}
#endif

int main(int argc, char **argv)
{
	GOptionContext *context;
	GError *error = NULL;
	char port[1024], hostname[1024];
	char *display;
	GtkWidget *window;
	GtkWidget *layout;
	GtkWidget *menubar;
	GtkWidget *sendkey, *view;
	GtkWidget *submenu;
	GtkWidget *caf1;
	GtkWidget *caf7;
	GtkWidget *cad;
	GtkWidget *cab;
	GtkWidget *fullscreen;
	GtkWidget *scaling;
	const char *help_msg = "Run 'gvncviewer --help' to see a full list of available command line options";

	/* Setup command line options */
	context = g_option_context_new ("- Simple VNC Client");
	g_option_context_add_main_entries (context, options, NULL);
	g_option_context_add_group (context, gtk_get_option_group (TRUE));
	g_option_context_add_group (context, vnc_display_get_option_group ());
	g_option_context_parse (context, &argc, &argv, &error);
	if (error) {
		g_print ("%s\n%s\n",
			 error->message,
			help_msg);
		g_error_free (error);
		return 1;
	}
	if (!args || (g_strv_length(args) != 1)) {
		fprintf(stderr, "Usage: gvncviewer hostname[:display]\n%s\n", help_msg);
		return 1;
	}

	vnc = vnc_display_new();

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#if WITH_LIBVIEW
	layout = ViewAutoDrawer_New();
#else
	layout = gtk_vbox_new(FALSE, 0);
#endif
	menubar = gtk_menu_bar_new();

	gtk_window_set_resizable(GTK_WINDOW(window), TRUE);

	sendkey = gtk_menu_item_new_with_mnemonic("_Send Key");
	gtk_menu_bar_append(GTK_MENU_BAR(menubar), sendkey);

	submenu = gtk_menu_new();

	caf1 = gtk_menu_item_new_with_mnemonic("Ctrl+Alt+F_1");
	caf7 = gtk_menu_item_new_with_mnemonic("Ctrl+Alt+F_7");
	cad = gtk_menu_item_new_with_mnemonic("Ctrl+Alt+_Del");
	cab = gtk_menu_item_new_with_mnemonic("Ctrl+Alt+_Backspace");

	gtk_menu_append(GTK_MENU(submenu), caf1);
	gtk_menu_append(GTK_MENU(submenu), caf7);
	gtk_menu_append(GTK_MENU(submenu), cad);
	gtk_menu_append(GTK_MENU(submenu), cab);

	gtk_menu_item_set_submenu(GTK_MENU_ITEM(sendkey), submenu);

	view = gtk_menu_item_new_with_mnemonic("_View");
	gtk_menu_bar_append(GTK_MENU_BAR(menubar), view);

	submenu = gtk_menu_new();

	fullscreen = gtk_check_menu_item_new_with_mnemonic("_Full Screen");
	scaling = gtk_check_menu_item_new_with_mnemonic("Scaled display");

	gtk_menu_append(GTK_MENU(submenu), fullscreen);
	gtk_menu_append(GTK_MENU(submenu), scaling);

	gtk_menu_item_set_submenu(GTK_MENU_ITEM(view), submenu);

#if WITH_LIBVIEW
	ViewAutoDrawer_SetActive(VIEW_AUTODRAWER(layout), FALSE);
	ViewOvBox_SetOver(VIEW_OV_BOX(layout), menubar);
	ViewOvBox_SetUnder(VIEW_OV_BOX(layout), vnc);
#else
	gtk_box_pack_start(GTK_BOX(layout), menubar, FALSE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(layout), vnc, TRUE, TRUE, 0);
#endif
	gtk_container_add(GTK_CONTAINER(window), layout);
	gtk_widget_realize(vnc);

	snprintf(hostname, sizeof(hostname), "%s", args[0]);
	display = strchr(hostname, ':');

	if (display) {
		*display = 0;
		snprintf(port, sizeof(port), "%d", 5900 + atoi(display + 1));
	} else
		snprintf(port, sizeof(port), "%d", 5900);

	vnc_display_open_host(VNC_DISPLAY(vnc), hostname, port);
	vnc_display_set_keyboard_grab(VNC_DISPLAY(vnc), TRUE);
	vnc_display_set_pointer_grab(VNC_DISPLAY(vnc), TRUE);

	if (!gtk_widget_is_composited(window)) {
		vnc_display_set_scaling(VNC_DISPLAY(vnc), TRUE);
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(scaling), TRUE);
	}

	gtk_signal_connect(GTK_OBJECT(window), "delete-event",
			   GTK_SIGNAL_FUNC(gtk_main_quit), NULL);
	gtk_signal_connect(GTK_OBJECT(vnc), "vnc-connected",
			   GTK_SIGNAL_FUNC(vnc_connected), NULL);
	gtk_signal_connect(GTK_OBJECT(vnc), "vnc-initialized",
			   GTK_SIGNAL_FUNC(vnc_initialized), window);
	gtk_signal_connect(GTK_OBJECT(vnc), "vnc-disconnected",
			   GTK_SIGNAL_FUNC(vnc_disconnected), NULL);
	gtk_signal_connect(GTK_OBJECT(vnc), "vnc-auth-credential",
			   GTK_SIGNAL_FUNC(vnc_credential), NULL);
	gtk_signal_connect(GTK_OBJECT(vnc), "vnc-auth-failure",
			   GTK_SIGNAL_FUNC(vnc_auth_failure), NULL);

	gtk_signal_connect(GTK_OBJECT(vnc), "vnc-desktop-resize",
			   GTK_SIGNAL_FUNC(vnc_desktop_resize), NULL);

	gtk_signal_connect(GTK_OBJECT(vnc), "vnc-pointer-grab",
			   GTK_SIGNAL_FUNC(vnc_grab), window);
	gtk_signal_connect(GTK_OBJECT(vnc), "vnc-pointer-ungrab",
			   GTK_SIGNAL_FUNC(vnc_ungrab), window);

	gtk_signal_connect(GTK_OBJECT(window), "key-press-event",
			   GTK_SIGNAL_FUNC(vnc_screenshot), vnc);

	gtk_signal_connect(GTK_OBJECT(caf1), "activate",
			   GTK_SIGNAL_FUNC(send_caf1), vnc);
	gtk_signal_connect(GTK_OBJECT(caf7), "activate",
			   GTK_SIGNAL_FUNC(send_caf7), vnc);
	gtk_signal_connect(GTK_OBJECT(cad), "activate",
			   GTK_SIGNAL_FUNC(send_cad), vnc);
	gtk_signal_connect(GTK_OBJECT(cab), "activate",
			   GTK_SIGNAL_FUNC(send_cab), vnc);
	gtk_signal_connect(GTK_OBJECT(fullscreen), "toggled",
			   GTK_SIGNAL_FUNC(do_fullscreen), window);
	gtk_signal_connect(GTK_OBJECT(scaling), "toggled",
			   GTK_SIGNAL_FUNC(do_scaling), vnc);
#if WITH_LIBVIEW
	gtk_signal_connect(GTK_OBJECT(window), "window-state-event",
			   GTK_SIGNAL_FUNC(window_state_event), layout);
#endif

	gtk_main();

	return 0;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
