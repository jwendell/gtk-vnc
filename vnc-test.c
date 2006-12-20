#include "vncdisplay.h"
#include <gtk/gtk.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

int main(int argc, char **argv)
{
	int s;
	struct sockaddr_in addr;
	GtkWidget *window, *vnc;

	gtk_init(&argc, &argv);

	s = socket(PF_INET, SOCK_STREAM, 0);
	inet_aton("localhost", (struct in_addr *)&addr.sin_addr);
	addr.sin_port = htons(5901);
	addr.sin_family = AF_INET;

	connect(s, (struct sockaddr *)&addr, sizeof(addr));


	vnc = vnc_display_new();
	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	gtk_container_add(GTK_CONTAINER(window), vnc);
	gtk_widget_show_all(window);

	vnc_display_open(VNC_DISPLAY(vnc), s);

	gtk_signal_connect(GTK_OBJECT(window), "delete-event", 
			   GTK_SIGNAL_FUNC(gtk_main_quit), NULL);

	gtk_main();

	return 0;
}
