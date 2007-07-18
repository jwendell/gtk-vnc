#include "vncdisplay.h"
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static GtkWidget *window;
static GtkWidget *vnc;


void vnc_grab(GtkWidget *vnc)
{
	gtk_window_set_title(GTK_WINDOW(window), "Press Ctrl+Alt to release pointer. GVncViewer");
}


void vnc_ungrab(GtkWidget *vnc)
{
	gtk_window_set_title(GTK_WINDOW(window), "GVncViewer");
}

void send_caf1(GtkWidget *button)
{
	gint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_F1 };
	printf("Sending Ctrl+Alt+F1\n");
	vnc_display_send_keys(VNC_DISPLAY(vnc), keys, sizeof(keys)/sizeof(keys[0]));
}
void send_caf7(GtkWidget *button)
{
	gint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_F7 };
	printf("Sending Ctrl+Alt+F7\n");
	vnc_display_send_keys(VNC_DISPLAY(vnc), keys, sizeof(keys)/sizeof(keys[0]));
}
void send_cad(GtkWidget *button)
{
	gint keys[] = { GDK_Control_L, GDK_Alt_L, GDK_Delete };
	printf("Sending Ctrl+Alt+Delete\n");
	vnc_display_send_keys(VNC_DISPLAY(vnc), keys, sizeof(keys)/sizeof(keys[0]));
}

int main(int argc, char **argv)
{
	char *ret = NULL;
	GtkWidget *layout;
	GtkWidget *buttons;
	GtkWidget *caf1;
	GtkWidget *caf7;
	GtkWidget *cad;

	if (argc != 3 && argc != 4) {
		fprintf(stderr, "syntax: vnc-test ipaddress port [password]\n");
		return 1;
	}

	gtk_init(&argc, &argv);

	vnc = vnc_display_new();
	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(window), "GVncViewer");
	layout = gtk_vbox_new(FALSE, 3);
	buttons = gtk_hbox_new(FALSE, 3);
	caf1 = gtk_button_new_with_label("Ctrl+Alt+F1");
	caf7 = gtk_button_new_with_label("Ctrl+Alt+F7");
	cad = gtk_button_new_with_label("Ctrl+Alt+Del");

	gtk_container_add(GTK_CONTAINER(window), layout);
	gtk_container_add(GTK_CONTAINER(layout), buttons);
	gtk_container_add(GTK_CONTAINER(layout), vnc);
	gtk_container_add(GTK_CONTAINER(buttons), caf1);
	gtk_container_add(GTK_CONTAINER(buttons), caf7);
	gtk_container_add(GTK_CONTAINER(buttons), cad);
	gtk_widget_show_all(window);

	if (argc == 4)
		vnc_display_set_password(VNC_DISPLAY(vnc), argv[3]);
	vnc_display_open_name(VNC_DISPLAY(vnc), argv[1], argv[2]);
	vnc_display_set_keyboard_grab(VNC_DISPLAY(vnc), TRUE);
	vnc_display_set_pointer_grab(VNC_DISPLAY(vnc), TRUE);
	//vnc_display_set_pointer_local(VNC_DISPLAY(vnc), TRUE);

	gtk_signal_connect(GTK_OBJECT(window), "delete-event",
			   GTK_SIGNAL_FUNC(gtk_main_quit), NULL);
	gtk_signal_connect(GTK_OBJECT(vnc), "vnc-pointer-grab",
			   GTK_SIGNAL_FUNC(vnc_grab), NULL);
	gtk_signal_connect(GTK_OBJECT(vnc), "vnc-pointer-ungrab",
			   GTK_SIGNAL_FUNC(vnc_ungrab), NULL);


	gtk_signal_connect(GTK_OBJECT(caf1), "clicked",
			   GTK_SIGNAL_FUNC(send_caf1), NULL);
	gtk_signal_connect(GTK_OBJECT(caf7), "clicked",
			   GTK_SIGNAL_FUNC(send_caf7), NULL);
	gtk_signal_connect(GTK_OBJECT(cad), "clicked",
			   GTK_SIGNAL_FUNC(send_cad), NULL);

	gtk_signal_connect(GTK_OBJECT(vnc), "vnc-disconnected",
			   GTK_SIGNAL_FUNC(gtk_main_quit), NULL);

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
