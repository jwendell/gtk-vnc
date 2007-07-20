#include "vncdisplay.h"
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static void set_title(VncDisplay *vnc, GtkWidget *window, gboolean grabbed)
{
	const char *name;
	char title[1024];
	const char *subtitle;

	if (grabbed)
		subtitle = "(Press Ctrl+Alt to release pointer) ";
	else
		subtitle = "";

	name = vnc_display_get_name(VNC_DISPLAY(vnc));
	snprintf(title, sizeof(title), "%s%s - GVncViewer",
		 subtitle, name);

	gtk_window_set_title(GTK_WINDOW(window), title);
}

static void vnc_grab(GtkWidget *vnc, GtkWidget *window)
{
	set_title(VNC_DISPLAY(vnc), window, TRUE);
}

static void vnc_ungrab(GtkWidget *vnc, GtkWidget *window)
{
	set_title(VNC_DISPLAY(vnc), window, FALSE);
}

static void vnc_connected(GtkWidget *vnc G_GNUC_UNUSED)
{
	printf("Connected to server\n");
}

static void vnc_initialized(GtkWidget *vnc, GtkWidget *window)
{
	printf("Connection initialized\n");
	set_title(VNC_DISPLAY(vnc), window, FALSE);
	gtk_widget_show_all(window);
}

static void vnc_disconnected(GtkWidget *vnc G_GNUC_UNUSED)
{
	printf("Disconnected from server\n");
	gtk_main_quit();
}

static void send_caf1(GtkWidget *menu G_GNUC_UNUSED, GtkWidget *vnc)
{
	guint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_F1 };
	printf("Sending Ctrl+Alt+F1\n");
	vnc_display_send_keys(VNC_DISPLAY(vnc), keys, sizeof(keys)/sizeof(keys[0]));
}

static void send_caf7(GtkWidget *menu G_GNUC_UNUSED, GtkWidget *vnc)
{
	guint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_F7 };
	printf("Sending Ctrl+Alt+F7\n");
	vnc_display_send_keys(VNC_DISPLAY(vnc), keys, sizeof(keys)/sizeof(keys[0]));
}

static void send_cad(GtkWidget *menu G_GNUC_UNUSED, GtkWidget *vnc)
{
	guint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_Delete };
	printf("Sending Ctrl+Alt+Delete\n");
	vnc_display_send_keys(VNC_DISPLAY(vnc), keys, sizeof(keys)/sizeof(keys[0]));
}

static void send_cab(GtkWidget *menu G_GNUC_UNUSED, GtkWidget *vnc)
{
	guint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_BackSpace };
	printf("Sending Ctrl+Alt+Backspace\n");
	vnc_display_send_keys(VNC_DISPLAY(vnc), keys, sizeof(keys)/sizeof(keys[0]));
}

int main(int argc, char **argv)
{
	char port[1024], hostname[1024];
	char *display;
	GtkWidget *window;
	GtkWidget *vnc;
	GtkWidget *layout;
	GtkWidget *menubar;
	GtkWidget *sendkey;
	GtkWidget *submenu;
	GtkWidget *caf1;
	GtkWidget *caf7;
	GtkWidget *cad;
	GtkWidget *cab;

	if (argc != 2 && argc != 3) {
		fprintf(stderr, "Usage: %s hostname[:display] [password]\n",
			argv[0]);
		return 1;
	}

	gtk_init(&argc, &argv);

	vnc = vnc_display_new();
	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	layout = gtk_vbox_new(FALSE, 3);
	menubar = gtk_menu_bar_new();

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

	gtk_container_add(GTK_CONTAINER(window), layout);
	gtk_container_add(GTK_CONTAINER(layout), menubar);
	gtk_container_add(GTK_CONTAINER(layout), vnc);
	gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
	gtk_widget_realize(vnc);

	if (argc == 3)
		vnc_display_set_password(VNC_DISPLAY(vnc), argv[2]);

	snprintf(hostname, sizeof(hostname), "%s", argv[1]);
	display = strchr(hostname, ':');

	if (display) {
		*display = 0;
		snprintf(port, sizeof(port), "%d", 5900 + atoi(display + 1));
	} else
		snprintf(port, sizeof(port), "%d", 5900);

	vnc_display_open_host(VNC_DISPLAY(vnc), hostname, port);
	vnc_display_set_keyboard_grab(VNC_DISPLAY(vnc), TRUE);
	vnc_display_set_pointer_grab(VNC_DISPLAY(vnc), TRUE);
	//vnc_display_set_pointer_local(VNC_DISPLAY(vnc), TRUE);

	gtk_signal_connect(GTK_OBJECT(window), "delete-event",
			   GTK_SIGNAL_FUNC(gtk_main_quit), NULL);
	gtk_signal_connect(GTK_OBJECT(vnc), "vnc-connected",
			   GTK_SIGNAL_FUNC(vnc_connected), NULL);
	gtk_signal_connect(GTK_OBJECT(vnc), "vnc-initialized",
			   GTK_SIGNAL_FUNC(vnc_initialized), window);
	gtk_signal_connect(GTK_OBJECT(vnc), "vnc-disconnected",
			   GTK_SIGNAL_FUNC(vnc_disconnected), NULL);


	gtk_signal_connect(GTK_OBJECT(vnc), "vnc-pointer-grab",
			   GTK_SIGNAL_FUNC(vnc_grab), window);
	gtk_signal_connect(GTK_OBJECT(vnc), "vnc-pointer-ungrab",
			   GTK_SIGNAL_FUNC(vnc_ungrab), window);


	gtk_signal_connect(GTK_OBJECT(caf1), "activate",
			   GTK_SIGNAL_FUNC(send_caf1), vnc);
	gtk_signal_connect(GTK_OBJECT(caf7), "activate",
			   GTK_SIGNAL_FUNC(send_caf7), vnc);
	gtk_signal_connect(GTK_OBJECT(cad), "activate",
			   GTK_SIGNAL_FUNC(send_cad), vnc);
	gtk_signal_connect(GTK_OBJECT(cab), "activate",
			   GTK_SIGNAL_FUNC(send_cab), vnc);

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
