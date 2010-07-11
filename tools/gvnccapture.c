/*
 * Vnc Image Capture
 *
 * Copyright (C) 2010 Daniel P. Berrange <dan@berrange.com>
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

/*
=head1 NAME

gvnccapture - VNC screenshot capture

=head1 SYNOPSIS

gvnccapture [OPTION]... [HOST][:DISPLAY] FILENAME

=head1 DESCRIPTION

Capture a screenshot of the VNC desktop at HOST:DISPLAY saving to the
image file FILENAME. If HOST is omitted it defaults to "localhost",
if :DISPLAY is omitted, it defaults to ":1". FILENAME must end in a
known image format extension (eg ".png", ".jpeg"). Supported options
are

=over 4

=item --help, -?

Display command line help information

=item --quiet, -q

Do not display information on the console when capturing the screenshot,
with the exception of any password prompt.

=item --debug, -d

Display verbose debugging information on the console

=back

=head1 EXIT STATUS

The exit status is 0 upon successful screen capture, otherwise
it is a non-zero integer

=head1 EXAMPLES

 # gvnccapture localhost:1 desktop.png
 Password: 
 Connected to localhost:1
 Saved display to desktop.png

=head1 AUTHORS

Daniel P. Berrange <dan@berrange.com>

=head1 COPYRIGHT

Copyright (C) 2010 Daniel P. Berrange <dan@berrange.com>.

License LGPLv2+: GNU Lesser GPL version 2 or later <http://gnu.org/licenses/gpl.html>.

This is free software: you are free to change and redistribute it. There is NO WARRANTY, to the extent permitted by law.

=head1 SEE ALSO

vinagre(1)

=cut
 */

#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif
#include <unistd.h>
#include <glib/gi18n.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <vncconnection.h>
#include <vncbaseframebuffer.h>

struct GVncCapture {
	gchar *host;
	int port;

	gboolean quiet;
	gboolean saved;

	VncConnection *conn;
        GMainLoop *loop;
	gboolean connected;
	gchar *output;

	GdkPixbuf *pixbuf;
};

static const guint preferable_auths[] = {
	/*
	 * Both these two provide TLS based auth, and can layer
	 * all the other auth types on top. So these two must
	 * be the first listed
	 */
	VNC_CONNECTION_AUTH_VENCRYPT,
	VNC_CONNECTION_AUTH_TLS,

	/*
	 * Then stackable auth types in order of preference
	 */
	VNC_CONNECTION_AUTH_SASL,
	VNC_CONNECTION_AUTH_MSLOGON,
	VNC_CONNECTION_AUTH_VNC,

	/*
	 * Or nothing at all
	 */
	VNC_CONNECTION_AUTH_NONE
};


static gchar *
do_vnc_get_credential(const gchar *prompt, gboolean doecho)
{
#ifdef HAVE_TERMIOS_H
	struct termios old, new;
#endif
	gchar buf[100];
	gchar *res;
	int n = sizeof(buf);
	ssize_t len;

	printf("%s", prompt);
	fflush(stdout);

#ifdef HAVE_TERMIOS_H
	/* Turn echoing off and fail if we can't. */
	if (!doecho && tcgetattr (fileno (stdin), &old) != 0)
		return NULL;
	new = old;
	new.c_lflag &= ~ECHO;
	if (!doecho && tcsetattr(fileno(stdin), TCSAFLUSH, &new) != 0)
		return NULL;
#else
	doecho = TRUE; /* Avoid unused parameter compile warning */
#endif

	/* Read the password. */
	if ((res = fgets(buf, n, stdin)) != NULL) {
		len = strlen(res);
		if (res[len-1] == '\n')
			res[len-1] = '\0';
	}

#ifdef HAVE_TERMIOS_H
	/* Restore terminal. */
	if (!doecho) {
		printf("\n");
		(void) tcsetattr(fileno (stdin), TCSAFLUSH, &old);
	}
#endif

	return res ? g_strdup(res) : NULL;
}


static void do_vnc_framebuffer_update(VncConnection *conn,
				      guint16 x, guint16 y,
				      guint16 width, guint16 height,
				      gpointer opaque)
{
	struct GVncCapture *capture = opaque;

	if (!capture->pixbuf)
		return;

	/* Crude attempt to see if we've got a complete frame by
	 * checking if we've hit the bottom-right corner. Will
	 * probably break
	 */
	if ((x + width) == vnc_connection_get_width(conn) &&
	    (y + height) == vnc_connection_get_height(conn)) {
		VNC_DEBUG("All done, saving to %s", capture->output);
		gchar *ext = strrchr(capture->output, '.');
		GError *err = NULL;
		if (ext)
			ext++;

		if (!gdk_pixbuf_save(capture->pixbuf,
				     capture->output,
				     ext ? ext : "png",
				     &err,
				     "tEXt::Generator App", "gvnccapture", NULL)) {
			if (!capture->quiet)
				g_print("Unable to save display to %s: %s",
					capture->output, err->message);
		} else {
			capture->saved = TRUE;
			if (!capture->quiet)
				g_print("Saved display to %s\n",
					capture->output);
		}
		vnc_connection_shutdown(conn);
		g_main_quit(capture->loop);
	}
}

static void do_vnc_desktop_resize(VncConnection *conn,
				  int width, int height,
				  gpointer opaque)
{
	struct GVncCapture *capture = opaque;
	const VncPixelFormat *remoteFormat;
	VncPixelFormat localFormat = {
		.bits_per_pixel = 32,
		.depth = 32,
		.byte_order = G_BYTE_ORDER,
		.true_color_flag = TRUE,
		.red_max = 255,
		.green_max = 255,
		.blue_max = 255,
		.red_shift = 0,
		.green_shift = 8,
		.blue_shift = 16,
	};
	VncBaseFramebuffer *fb;

	if (capture->pixbuf) {
		gdk_pixbuf_unref(capture->pixbuf);
		capture->pixbuf = NULL;
	}

	VNC_DEBUG("Resize %dx%d", width, height);
	remoteFormat = vnc_connection_get_pixel_format(conn);

	/* We'll fix our local copy as rgb888 */
	capture->pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB,
					 TRUE, 
					 8,
					 width,
					 height);
	gdk_pixbuf_fill(capture->pixbuf, 0);

	fb = vnc_base_framebuffer_new(gdk_pixbuf_get_pixels(capture->pixbuf),
				      gdk_pixbuf_get_width(capture->pixbuf),
				      gdk_pixbuf_get_height(capture->pixbuf),
				      gdk_pixbuf_get_rowstride(capture->pixbuf),
				      remoteFormat,
				      &localFormat);

	vnc_connection_set_framebuffer(conn, VNC_FRAMEBUFFER(fb));

	g_object_unref(fb);
}


static void do_vnc_initialized(VncConnection *conn,
			       gpointer opaque)
{
	struct GVncCapture *capture = opaque;
	gint32 encodings[] = {  VNC_CONNECTION_ENCODING_DESKTOP_RESIZE,
                                VNC_CONNECTION_ENCODING_ZRLE,
				VNC_CONNECTION_ENCODING_HEXTILE,
				VNC_CONNECTION_ENCODING_RRE,
				VNC_CONNECTION_ENCODING_COPY_RECT,
				VNC_CONNECTION_ENCODING_RAW };
	gint32 *encodingsp;
	int n_encodings;

	do_vnc_desktop_resize(conn,
			      vnc_connection_get_width(conn),
			      vnc_connection_get_height(conn),
			      capture);

	encodingsp = encodings;
	n_encodings = G_N_ELEMENTS(encodings);

	VNC_DEBUG("Sending %d encodings", n_encodings);
	if (!vnc_connection_set_encodings(conn, n_encodings, encodingsp))
		goto error;

	VNC_DEBUG("Requesting first framebuffer update");
	if (!vnc_connection_framebuffer_update_request(capture->conn,
						       0, 0, 0,
						       vnc_connection_get_width(capture->conn),
						       vnc_connection_get_height(capture->conn)))
		vnc_connection_shutdown(capture->conn);

	if (!capture->quiet)
		g_print("Connected to %s:%d\n", capture->host, capture->port - 5900);
	capture->connected = TRUE;
	return;

 error:
	vnc_connection_shutdown(conn);
}

static void do_vnc_disconnected(VncConnection *conn G_GNUC_UNUSED,
				gpointer opaque)
{
	struct GVncCapture *capture = opaque;
	if (!capture->quiet) {
		if (capture->connected)
			g_print("Disconnected from %s:%d\n", capture->host, capture->port - 5900);
		else
			g_print("Unable to connect to %s:%d\n", capture->host, capture->port - 5900);
	}
	g_main_quit(capture->loop);
}

static void do_vnc_auth_credential(VncConnection *conn, GValueArray *credList, gpointer opaque)
{
	struct GVncCapture *capture = opaque;
	guint i;
	char **data;

	data = g_new0(char *, credList->n_values);

	for (i = 0 ; i < credList->n_values ; i++) {
		GValue *cred = g_value_array_get_nth(credList, i);
		switch (g_value_get_enum(cred)) {
		case VNC_CONNECTION_CREDENTIAL_PASSWORD:
			data[i] = do_vnc_get_credential("Password: ", FALSE);
			if (!data[i]) {
				if (!capture->quiet)
					g_print("Failed to read password\n");
				vnc_connection_shutdown(conn);
				goto cleanup;
			}
			break;
		case VNC_CONNECTION_CREDENTIAL_USERNAME:
			data[i] = do_vnc_get_credential("Username: ", TRUE);
			if (!data[i]) {
				if (!capture->quiet)
					g_print("Failed to read username\n");
				vnc_connection_shutdown(conn);
				goto cleanup;
			}
			break;
		case VNC_CONNECTION_CREDENTIAL_CLIENTNAME:
			data[i] = g_strdup("gvnccapture");
			break;
		default:
			break;
		}
	}
	for (i = 0 ; i < credList->n_values ; i++) {
		GValue *cred = g_value_array_get_nth(credList, i);
		if (data[i]) {
			if (!vnc_connection_set_credential(conn,
							   g_value_get_enum(cred),
							   data[i])) {
				g_print("Failed to set credential type %d %s\n", g_value_get_enum(cred), data[i]);
				vnc_connection_shutdown(conn);
			}
		} else {
			if (!capture->quiet)
				g_print("Unsupported credential type %d\n", g_value_get_enum(cred));
			vnc_connection_shutdown(conn);
		}
	}

 cleanup:
	for (i = 0 ; i < credList->n_values ; i++)
		g_free(data[i]);
	g_free(data);
}

static void do_vnc_auth_choose_type(VncConnection *conn,
				    GValueArray *types,
				    gpointer opaque G_GNUC_UNUSED)
{
	guint i, j;

	if (!types->n_values) {
		VNC_DEBUG("No auth types to choose from");
		return;
	}

	for (i = 0 ; i < G_N_ELEMENTS(preferable_auths) ; i++) {
		int pref = preferable_auths[i];

		for (j = 0 ; j < types->n_values ; j++) {
			GValue *type = g_value_array_get_nth(types, j);
			VNC_DEBUG("Compare %d vs %d", pref, g_value_get_enum(type));
			if (pref == g_value_get_enum(type)) {
				VNC_DEBUG("Chosen auth %d", pref);
				vnc_connection_set_auth_type(conn, pref);
				return;
			}
		}
	}

	GValue *type = g_value_array_get_nth(types, 0);
	VNC_DEBUG("Chosen default auth %d", g_value_get_enum(type));
	vnc_connection_set_auth_type(conn, g_value_get_enum(type));
}


static void show_help(const char *binary, const char *error)
{
	if (error)
		g_print("%s\n\n", error);
	g_print("Usage: %s [HOSTNAME][:DISPLAY] FILENAME\n\n", binary);
	g_print("Run '%s --help' to see a full list of available command line options\n",
		binary);
}

static gboolean vnc_debug_option_arg(const gchar *option_name G_GNUC_UNUSED,
				     const gchar *value G_GNUC_UNUSED,
				     gpointer data G_GNUC_UNUSED,
				     GError **error G_GNUC_UNUSED)
{
	vnc_util_set_debug(TRUE);
	return TRUE;
}


int main(int argc, char **argv)
{
	GOptionContext *context;
	GError *error = NULL;
	gchar *display;
	gchar *port;
	gchar **args = NULL;
	int ret;
	gboolean quiet = FALSE;
	const GOptionEntry options [] = {
		{ "debug", 'd', G_OPTION_FLAG_NO_ARG,  G_OPTION_ARG_CALLBACK,
		  vnc_debug_option_arg, N_("Enables debug output"), NULL },
		{ "quiet", 'q', 0, G_OPTION_ARG_NONE,
		  &quiet, N_("Don't print any status to console"), NULL },
		{ G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_STRING_ARRAY, &args,
		  NULL, "HOSTNAME[:DISPLAY] FILENAME" },
		{ NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, 0 }
	};
	struct GVncCapture *capture;

	g_type_init();

	/* Setup command line options */
	context = g_option_context_new("- Vnc Image Capture");
	g_option_context_add_main_entries(context, options, NULL);
	g_option_context_parse(context, &argc, &argv, &error);
	if (error) {
		show_help(argv[0], error->message);
		g_error_free(error);
		return 1;
	}
	if (!args || (g_strv_length(args) != 2)) {
		show_help(argv[0], NULL);
		return 1;
	}

	capture = g_new0(struct GVncCapture, 1);
	capture->quiet = quiet;

	if (args[0][0] == ':') {
		capture->host = g_strdup("localhost");
		display = args[0];
	} else {
		capture->host = g_strdup(args[0]);
		display = strchr(capture->host, ':');
	}
	if (display) {
		*display = 0;
		display++;
		capture->port = 5900 + atoi(display);
	} else {
		capture->port = 5900;
	}
	port = g_strdup_printf("%d", capture->port);

	capture->conn = vnc_connection_new();
	capture->output = g_strdup(args[1]);

	g_signal_connect(capture->conn, "vnc-initialized",
			 G_CALLBACK(do_vnc_initialized), capture);
	g_signal_connect(capture->conn, "vnc-disconnected",
			 G_CALLBACK(do_vnc_disconnected), capture);
	g_signal_connect(capture->conn, "vnc-auth-choose-type",
			 G_CALLBACK(do_vnc_auth_choose_type), capture);
	g_signal_connect(capture->conn, "vnc-auth-choose-subtype",
			 G_CALLBACK(do_vnc_auth_choose_type), capture);
	g_signal_connect(capture->conn, "vnc-auth-credential",
			 G_CALLBACK(do_vnc_auth_credential), capture);
	g_signal_connect(capture->conn, "vnc-desktop-resize",
			 G_CALLBACK(do_vnc_desktop_resize), capture);
	g_signal_connect(capture->conn, "vnc-framebuffer-update",
			 G_CALLBACK(do_vnc_framebuffer_update), capture);

	vnc_connection_open_host(capture->conn, capture->host, port);

	capture->loop = g_main_loop_new(g_main_context_default(), FALSE);

	g_main_loop_run(capture->loop);

	vnc_connection_shutdown(capture->conn);
	g_object_unref(capture->conn);
	if (capture->pixbuf)
		gdk_pixbuf_unref(capture->pixbuf);

	ret = capture->saved ? 0 : 1;

	g_free(capture->host);
	g_free(capture);

	return ret;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
