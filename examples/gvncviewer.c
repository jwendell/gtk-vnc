#include "vncdisplay.h"
#include <gtk/gtk.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>


int main(int argc, char **argv)
{
	GtkWidget *window;
	GtkWidget *vnc;
	char *ret = NULL;

	if (argc != 3 && argc != 4) {
	  fprintf(stderr, "syntax: vnc-test ipaddress port [password]\n");
	  return 1;
	}

	gtk_init(&argc, &argv);

	vnc = vnc_display_new();
	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	gtk_container_add(GTK_CONTAINER(window), vnc);
	gtk_widget_show_all(window);

	if (argc == 4)
	  vnc_display_set_password(VNC_DISPLAY(vnc), argv[3]);
	vnc_display_open_name(VNC_DISPLAY(vnc), argv[1], argv[2]);

	gtk_signal_connect(GTK_OBJECT(window), "delete-event",
			   GTK_SIGNAL_FUNC(gtk_main_quit), NULL);

	gtk_main();

	return 0;
}
