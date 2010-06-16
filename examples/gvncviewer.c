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
		NULL, "[hostname][:display]" },
	{ NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, 0 }
};


static GtkWidget *vnc;

typedef struct {
	GtkWidget *label;
	guint curkeys;
	guint numkeys;
	guint *keysyms;
	gboolean set;
} VncGrabDefs;

static void set_title(VncDisplay *vncdisplay, GtkWidget *window,
	gboolean grabbed)
{
	const gchar *name = vnc_display_get_name(VNC_DISPLAY(vncdisplay));
	VncGrabSequence *seq = vnc_display_get_grab_keys(vncdisplay);
	gchar *seqstr = vnc_grab_sequence_as_string(seq);
	gchar *title;

	if (grabbed)
		title = g_strdup_printf("(Press %s to release pointer) %s - GVncViewer",
					seqstr, name);
	else
		title = g_strdup_printf("%s - GVncViewer",
					name);

	gtk_window_set_title(GTK_WINDOW(window), title);

	g_free(seqstr);
	g_free(title);
}

static gboolean vnc_screenshot(GtkWidget *window G_GNUC_UNUSED,
	GdkEvent *ev, GtkWidget *vncdisplay)
{
	if (ev->key.keyval == GDK_F11) {
		GdkPixbuf *pix = vnc_display_get_pixbuf(VNC_DISPLAY(vncdisplay));
		gdk_pixbuf_save(pix, "gvncviewer.png", "png", NULL, "tEXt::Generator App", "gvncviewer", NULL);
		g_object_unref(pix);
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

static int connected = 0;

static void vnc_connected(GtkWidget *vncdisplay G_GNUC_UNUSED)
{
	printf("Connected to server\n");
	connected = 1;
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
	if(connected)
		printf("Disconnected from server\n");
	else
		printf("Failed to connect to server\n");
	gtk_main_quit();
}

static void send_caf1(GtkWidget *menu G_GNUC_UNUSED, GtkWidget *vncdisplay)
{
	guint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_F1 };
	printf("Sending Ctrl+Alt+F1\n");
	vnc_display_send_keys(VNC_DISPLAY(vncdisplay), keys,
		sizeof(keys)/sizeof(keys[0]));
}

static void send_caf2(GtkWidget *menu G_GNUC_UNUSED, GtkWidget *vncdisplay)
{
	guint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_F2 };
	printf("Sending Ctrl+Alt+F2\n");
	vnc_display_send_keys(VNC_DISPLAY(vncdisplay), keys,
		sizeof(keys)/sizeof(keys[0]));
}

static void send_caf3(GtkWidget *menu G_GNUC_UNUSED, GtkWidget *vncdisplay)
{
	guint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_F3 };
	printf("Sending Ctrl+Alt+F3\n");
	vnc_display_send_keys(VNC_DISPLAY(vncdisplay), keys,
		sizeof(keys)/sizeof(keys[0]));
}

static void send_caf4(GtkWidget *menu G_GNUC_UNUSED, GtkWidget *vncdisplay)
{
	guint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_F4 };
	printf("Sending Ctrl+Alt+F4\n");
	vnc_display_send_keys(VNC_DISPLAY(vncdisplay), keys,
		sizeof(keys)/sizeof(keys[0]));
}

static void send_caf5(GtkWidget *menu G_GNUC_UNUSED, GtkWidget *vncdisplay)
{
	guint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_F5 };
	printf("Sending Ctrl+Alt+F5\n");
	vnc_display_send_keys(VNC_DISPLAY(vncdisplay), keys,
		sizeof(keys)/sizeof(keys[0]));
}

static void send_caf6(GtkWidget *menu G_GNUC_UNUSED, GtkWidget *vncdisplay)
{
	guint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_F6 };
	printf("Sending Ctrl+Alt+F6\n");
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

static void send_caf8(GtkWidget *menu G_GNUC_UNUSED, GtkWidget *vncdisplay)
{
	guint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_F8 };
	printf("Sending Ctrl+Alt+F8\n");
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

static void dialog_update_keysyms(GtkWidget *window, guint *keysyms, guint numsyms)
{
	gchar *keys;
	int i;

	keys = g_strdup("");
	for (i = 0; i < numsyms; i++)
	{
		keys = g_strdup_printf("%s%s%s", keys,
			(strlen(keys) > 0) ? "+" : " ", gdk_keyval_name(keysyms[i]));
	}

	gtk_label_set_text( GTK_LABEL(window), keys);
}

static gboolean dialog_key_ignore(int keyval)
{
	switch (keyval) {
		case GDK_Return:
		case GDK_Escape:
			return TRUE;
	}

	return FALSE;
}

static gboolean dialog_key_press(GtkWidget *window G_GNUC_UNUSED,
        GdkEvent *ev, VncGrabDefs *defs)
{
	gboolean keySymExists;
	int i;

	if (dialog_key_ignore(ev->key.keyval))
		return FALSE;

	if (defs->set && defs->curkeys)
		return FALSE;

	/* Check whether we already have keysym in array - i.e. it was handler by something else */
	keySymExists = FALSE;
	for (i = 0; i < defs->curkeys; i++) {
		if (defs->keysyms[i] == ev->key.keyval)
			keySymExists = TRUE;
	}

	if (!keySymExists) {
		defs->keysyms = g_renew(guint, defs->keysyms, defs->curkeys + 1);
		defs->keysyms[defs->curkeys] = ev->key.keyval;
		defs->curkeys++;
	}

	dialog_update_keysyms(defs->label, defs->keysyms, defs->curkeys);

	if (!ev->key.is_modifier) {
		defs->set = TRUE;
		defs->numkeys = defs->curkeys;
		defs->curkeys--;
	}

	return FALSE;
}

static gboolean dialog_key_release(GtkWidget *window G_GNUC_UNUSED,
        GdkEvent *ev, VncGrabDefs *defs)
{
	int i;

	if (dialog_key_ignore(ev->key.keyval))
		return FALSE;

	if (defs->set) {
		if (defs->curkeys == 0)
			defs->set = FALSE;
		if (defs->curkeys)
			defs->curkeys--;
		return FALSE;
	}

	for (i = 0; i < defs->curkeys; i++)
	{
		if (defs->keysyms[i] == ev->key.keyval)
		{
			defs->keysyms[i] = defs->keysyms[defs->curkeys - 1];
			defs->curkeys--;
			defs->keysyms = g_renew(guint, defs->keysyms, defs->curkeys);
		}
	}

	dialog_update_keysyms(defs->label, defs->keysyms, defs->curkeys);

	return FALSE;
}

static void do_set_grab_keys(GtkWidget *menu G_GNUC_UNUSED, GtkWidget *window)
{
	VncGrabDefs *defs;
	VncGrabSequence *seq;
	GtkWidget *dialog, *content_area, *label;
	gint result;

	dialog = gtk_dialog_new_with_buttons ("Key recorder",
						GTK_WINDOW(window),
						GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
						GTK_STOCK_OK,
						GTK_RESPONSE_ACCEPT,
						GTK_STOCK_CANCEL,
						GTK_RESPONSE_REJECT,
						NULL);

	label = gtk_label_new("Please press desired grab key combination");
	defs = g_new(VncGrabDefs, 1);
	defs->label = label;
	defs->keysyms = 0;
	defs->numkeys = 0;
	defs->curkeys = 0;
	defs->set = FALSE;
	g_signal_connect(dialog, "key-press-event",
			G_CALLBACK(dialog_key_press), defs);
	g_signal_connect(dialog, "key-release-event",
			G_CALLBACK(dialog_key_release), defs);
	gtk_widget_set_size_request(dialog, 300, 100);
	content_area = gtk_dialog_get_content_area( GTK_DIALOG(dialog) );
	gtk_container_add( GTK_CONTAINER(content_area), label);
	gtk_widget_show_all(dialog);

	result = gtk_dialog_run(GTK_DIALOG(dialog));
	if (result == GTK_RESPONSE_ACCEPT) {
		/* Accepted so we make a grab sequence from it */
		seq = vnc_grab_sequence_new(defs->numkeys,
					    defs->keysyms);

		vnc_display_set_grab_keys(VNC_DISPLAY(vnc), seq);
		set_title(VNC_DISPLAY(vnc), window, FALSE);
		vnc_grab_sequence_free(seq);
	}
	g_free(defs);
	gtk_widget_destroy(dialog);
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
				default:
					continue;
				}
				row++;
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
	GtkWidget *sendkey, *view, *settings;
	GtkWidget *submenu;
	GtkWidget *caf1;
	GtkWidget *caf2;
	GtkWidget *caf3;
	GtkWidget *caf4;
	GtkWidget *caf5;
	GtkWidget *caf6;
	GtkWidget *caf7;
	GtkWidget *caf8;
	GtkWidget *cad;
	GtkWidget *cab;
	GtkWidget *fullscreen;
	GtkWidget *scaling;
	GtkWidget *showgrabkeydlg;
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
		fprintf(stderr, "Usage: gvncviewer [hostname][:display]\n%s\n", help_msg);
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
	gtk_menu_shell_append(GTK_MENU_SHELL(menubar), sendkey);

	submenu = gtk_menu_new();

	caf1 = gtk_menu_item_new_with_mnemonic("Ctrl+Alt+F_1");
	caf2 = gtk_menu_item_new_with_mnemonic("Ctrl+Alt+F_2");
	caf3 = gtk_menu_item_new_with_mnemonic("Ctrl+Alt+F_3");
	caf4 = gtk_menu_item_new_with_mnemonic("Ctrl+Alt+F_4");
	caf5 = gtk_menu_item_new_with_mnemonic("Ctrl+Alt+F_5");
	caf6 = gtk_menu_item_new_with_mnemonic("Ctrl+Alt+F_6");
	caf7 = gtk_menu_item_new_with_mnemonic("Ctrl+Alt+F_7");
	caf8 = gtk_menu_item_new_with_mnemonic("Ctrl+Alt+F_8");
	cad = gtk_menu_item_new_with_mnemonic("Ctrl+Alt+_Del");
	cab = gtk_menu_item_new_with_mnemonic("Ctrl+Alt+_Backspace");

	gtk_menu_shell_append(GTK_MENU_SHELL(submenu), caf1);
	gtk_menu_shell_append(GTK_MENU_SHELL(submenu), caf2);
	gtk_menu_shell_append(GTK_MENU_SHELL(submenu), caf3);
	gtk_menu_shell_append(GTK_MENU_SHELL(submenu), caf4);
	gtk_menu_shell_append(GTK_MENU_SHELL(submenu), caf5);
	gtk_menu_shell_append(GTK_MENU_SHELL(submenu), caf6);
	gtk_menu_shell_append(GTK_MENU_SHELL(submenu), caf7);
	gtk_menu_shell_append(GTK_MENU_SHELL(submenu), caf8);
	gtk_menu_shell_append(GTK_MENU_SHELL(submenu), cad);
	gtk_menu_shell_append(GTK_MENU_SHELL(submenu), cab);

	gtk_menu_item_set_submenu(GTK_MENU_ITEM(sendkey), submenu);

	view = gtk_menu_item_new_with_mnemonic("_View");
	gtk_menu_shell_append(GTK_MENU_SHELL(menubar), view);

	submenu = gtk_menu_new();

	fullscreen = gtk_check_menu_item_new_with_mnemonic("_Full Screen");
	scaling = gtk_check_menu_item_new_with_mnemonic("Scaled display");

	gtk_menu_shell_append(GTK_MENU_SHELL(submenu), fullscreen);
	gtk_menu_shell_append(GTK_MENU_SHELL(submenu), scaling);

	gtk_menu_item_set_submenu(GTK_MENU_ITEM(view), submenu);

	settings = gtk_menu_item_new_with_mnemonic("_Settings");
	gtk_menu_shell_append(GTK_MENU_SHELL(menubar), settings);

	submenu = gtk_menu_new();

	showgrabkeydlg = gtk_menu_item_new_with_mnemonic("_Set grab keys");
	gtk_menu_shell_append(GTK_MENU_SHELL(submenu), showgrabkeydlg);

	gtk_menu_item_set_submenu(GTK_MENU_ITEM(settings), submenu);

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

	if (!*hostname) 
		snprintf(hostname, sizeof(hostname), "%s", "127.0.0.1");
	vnc_display_open_host(VNC_DISPLAY(vnc), hostname, port);
	vnc_display_set_keyboard_grab(VNC_DISPLAY(vnc), TRUE);
	vnc_display_set_pointer_grab(VNC_DISPLAY(vnc), TRUE);

	if (!gtk_widget_is_composited(window)) {
		vnc_display_set_scaling(VNC_DISPLAY(vnc), TRUE);
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(scaling), TRUE);
	}

	g_signal_connect(window, "delete-event",
			 G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(vnc, "vnc-connected",
			 G_CALLBACK(vnc_connected), NULL);
	g_signal_connect(vnc, "vnc-initialized",
			 G_CALLBACK(vnc_initialized), window);
	g_signal_connect(vnc, "vnc-disconnected",
			 G_CALLBACK(vnc_disconnected), NULL);
	g_signal_connect(vnc, "vnc-auth-credential",
			 G_CALLBACK(vnc_credential), NULL);
	g_signal_connect(vnc, "vnc-auth-failure",
			 G_CALLBACK(vnc_auth_failure), NULL);

	g_signal_connect(vnc, "vnc-desktop-resize",
			 G_CALLBACK(vnc_desktop_resize), NULL);

	g_signal_connect(vnc, "vnc-pointer-grab",
			 G_CALLBACK(vnc_grab), window);
	g_signal_connect(vnc, "vnc-pointer-ungrab",
			 G_CALLBACK(vnc_ungrab), window);

	g_signal_connect(window, "key-press-event",
			 G_CALLBACK(vnc_screenshot), vnc);

	g_signal_connect(caf1, "activate",
			 G_CALLBACK(send_caf1), vnc);
	g_signal_connect(caf2, "activate",
			 G_CALLBACK(send_caf2), vnc);
	g_signal_connect(caf3, "activate",
			 G_CALLBACK(send_caf3), vnc);
	g_signal_connect(caf4, "activate",
			 G_CALLBACK(send_caf4), vnc);
	g_signal_connect(caf5, "activate",
			 G_CALLBACK(send_caf5), vnc);
	g_signal_connect(caf6, "activate",
			 G_CALLBACK(send_caf6), vnc);
	g_signal_connect(caf7, "activate",
			 G_CALLBACK(send_caf7), vnc);
	g_signal_connect(caf8, "activate",
			 G_CALLBACK(send_caf8), vnc);
	g_signal_connect(cad, "activate",
			 G_CALLBACK(send_cad), vnc);
	g_signal_connect(cab, "activate",
			 G_CALLBACK(send_cab), vnc);
	g_signal_connect(showgrabkeydlg, "activate",
			 G_CALLBACK(do_set_grab_keys), window);
	g_signal_connect(fullscreen, "toggled",
			 G_CALLBACK(do_fullscreen), window);
	g_signal_connect(scaling, "toggled",
			 G_CALLBACK(do_scaling), vnc);
#if WITH_LIBVIEW
	g_signal_connect(window, "window-state-event",
			 G_CALLBACK(window_state_event), layout);
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
