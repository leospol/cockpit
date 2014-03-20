/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "cockpitpipe.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gunixsocketaddress.h>

#include <sys/uio.h>
#include <string.h>

/* Can't use g_test_skip() yet */
static void
test_skip (const gchar *reason)
{
  if (g_test_verbose ())
    g_print ("GTest: skipping: %s\n", reason);
  else
    g_print ("SKIP: %s ", reason);
}

/* ----------------------------------------------------------------------------
 * Mock
 */

static GType mock_echo_pipe_get_type (void) G_GNUC_CONST;

typedef struct {
  CockpitPipe parent;
  GByteArray *received;
  gboolean closed;
  gchar *problem;
} MockEchoPipe;

typedef CockpitPipeClass MockEchoPipeClass;

G_DEFINE_TYPE (MockEchoPipe, mock_echo_pipe, COCKPIT_TYPE_PIPE);

static void
mock_echo_pipe_read (CockpitPipe *pipe,
                     GByteArray *buffer,
                     gboolean end_of_data)
{
  MockEchoPipe *self = (MockEchoPipe *)pipe;
  g_byte_array_append (self->received, buffer->data, buffer->len);
  g_byte_array_set_size (buffer, 0);
}

static void
mock_echo_pipe_closed (CockpitPipe *pipe,
                       const gchar *problem)
{
  MockEchoPipe *self = (MockEchoPipe *)pipe;
  g_assert (!self->closed);
  self->closed = TRUE;
  self->problem = g_strdup (problem);
}

static void
mock_echo_pipe_init (MockEchoPipe *self)
{
  self->received = g_byte_array_new ();
}

static void
mock_echo_pipe_finalize (GObject *object)
{
  MockEchoPipe *self = (MockEchoPipe *)object;

  g_byte_array_free (self->received, TRUE);
  g_free (self->problem);

  G_OBJECT_CLASS (mock_echo_pipe_parent_class)->finalize (object);
}
static void
mock_echo_pipe_class_init (MockEchoPipeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  CockpitPipeClass *pipe_class = COCKPIT_PIPE_CLASS (klass);

  object_class->finalize = mock_echo_pipe_finalize;

  pipe_class->read = mock_echo_pipe_read;
  pipe_class->closed = mock_echo_pipe_closed;
}

/* ----------------------------------------------------------------------------
 * Testing
 */

typedef struct {
  CockpitPipe *pipe;
} TestCase;

static void
setup_echo (TestCase *tc,
            gconstpointer data)
{
  int fds[2];

  if (pipe (fds) < 0)
    g_assert_not_reached ();

  tc->pipe = g_object_new (g_type_from_name (data),
                           "name", "test",
                           "in-fd", fds[0],
                           "out-fd", fds[1],
                           NULL);
}

static void
teardown (TestCase *tc,
          gconstpointer data)
{
  g_object_add_weak_pointer (G_OBJECT (tc->pipe),
                             (gpointer *)&tc->pipe);
  g_object_unref (tc->pipe);

  /* If this asserts, outstanding references to transport */
  g_assert (tc->pipe == NULL);
}

static void
test_echo_and_close (TestCase *tc,
                     gconstpointer data)
{
  MockEchoPipe *echo_pipe = (MockEchoPipe *)tc->pipe;
  GBytes *sent, *bytes;

  sent = g_bytes_new_static ("the message", 11);
  cockpit_pipe_write (tc->pipe, sent);

  while (echo_pipe->received->len < 11)
    g_main_context_iteration (NULL, TRUE);

  g_byte_array_ref (echo_pipe->received);
  bytes = g_byte_array_free_to_bytes (echo_pipe->received);
  g_assert (g_bytes_equal (bytes, sent));
  g_bytes_unref (sent);
  g_bytes_unref (bytes);

  cockpit_pipe_close (tc->pipe, NULL);

  while (!echo_pipe->closed)
    g_main_context_iteration (NULL, TRUE);
}

static void
test_echo_queue (TestCase *tc,
                 gconstpointer data)
{
  MockEchoPipe *echo_pipe = (MockEchoPipe *)tc->pipe;
  GBytes *sent;

  sent = g_bytes_new_static ("one", 3);
  cockpit_pipe_write (tc->pipe, sent);
  g_bytes_unref (sent);
  sent = g_bytes_new_static ("two", 3);
  cockpit_pipe_write (tc->pipe, sent);
  g_bytes_unref (sent);

  /* Only closes after above are sent */
  cockpit_pipe_close (tc->pipe, NULL);

  while (!echo_pipe->closed)
    g_main_context_iteration (NULL, TRUE);

  g_assert_cmpint (echo_pipe->received->len, ==, 6);
  g_assert (memcmp (echo_pipe->received->data, "onetwo", 6) == 0);
}

static void
test_echo_large (TestCase *tc,
                 gconstpointer data)
{
  MockEchoPipe *echo_pipe = (MockEchoPipe *)tc->pipe;
  GBytes *sent;

  /* Medium length */
  sent = g_bytes_new_take (g_strnfill (1020, '!'), 1020);
  cockpit_pipe_write (tc->pipe, sent);
  while (echo_pipe->received->len < g_bytes_get_size (sent))
    g_main_context_iteration (NULL, TRUE);
  g_assert_cmpint (echo_pipe->received->len, ==, g_bytes_get_size (sent));
  g_assert (memcmp (echo_pipe->received->data, g_bytes_get_data (sent, NULL), g_bytes_get_size (sent)) == 0);
  g_bytes_unref (sent);

  g_byte_array_set_size (echo_pipe->received, 0);

  /* Extra large */
  sent = g_bytes_new_take (g_strnfill (10 * 1000 * 1000, '?'), 10 * 1000 * 1000);
  cockpit_pipe_write (tc->pipe, sent);
  while (echo_pipe->received->len < g_bytes_get_size (sent))
    g_main_context_iteration (NULL, TRUE);
  g_assert_cmpint (echo_pipe->received->len, ==, g_bytes_get_size (sent));
  g_assert (memcmp (echo_pipe->received->data, g_bytes_get_data (sent, NULL), g_bytes_get_size (sent)) == 0);
  g_bytes_unref (sent);

  g_byte_array_set_size (echo_pipe->received, 0);

  /* Double check that didn't csrew things up */
  sent = g_bytes_new_static ("yello", 5);
  cockpit_pipe_write (tc->pipe, sent);
  while (echo_pipe->received->len < g_bytes_get_size (sent))
    g_main_context_iteration (NULL, TRUE);
  g_assert_cmpint (echo_pipe->received->len, ==, g_bytes_get_size (sent));
  g_assert (memcmp (echo_pipe->received->data, g_bytes_get_data (sent, NULL), g_bytes_get_size (sent)) == 0);
  g_bytes_unref (sent);
}

static void
test_close_problem (TestCase *tc,
                    gconstpointer data)
{
  MockEchoPipe *echo_pipe = (MockEchoPipe *)tc->pipe;

  cockpit_pipe_close (tc->pipe, "right now");

  while (!echo_pipe->closed)
    g_main_context_iteration (NULL, TRUE);

  g_assert_cmpstr (echo_pipe->problem, ==, "right now");
}

static void
test_buffer (TestCase *tc,
             gconstpointer data)
{
  GByteArray *buffer;
  GBytes *sent;

  buffer = cockpit_pipe_get_buffer (tc->pipe);
  g_assert (buffer != NULL);
  g_assert_cmpuint (buffer->len, ==, 0);

  /* Including null terminator */
  sent = g_bytes_new_static ("blahdeedoo", 11);
  cockpit_pipe_write (tc->pipe, sent);
  g_bytes_unref (sent);

  while (buffer->len == 0)
    g_main_context_iteration (NULL, TRUE);

  g_assert_cmpint (buffer->len, ==, 11);
  g_assert_cmpstr ((gchar *)buffer->data, ==, "blahdeedoo");
}

static gboolean
on_log_ignore_warnings (const gchar *log_domain,
                        GLogLevelFlags log_level,
                        const gchar *message,
                        gpointer user_data)
{
  switch (log_level & G_LOG_LEVEL_MASK)
    {
    case G_LOG_LEVEL_WARNING:
    case G_LOG_LEVEL_MESSAGE:
    case G_LOG_LEVEL_INFO:
    case G_LOG_LEVEL_DEBUG:
      return FALSE;
    default:
      return TRUE;
    }
}

static void
test_read_error (void)
{
  MockEchoPipe *echo_pipe;
  int out;

  /* Assuming FD 1000 is not taken */
  g_assert (write (1000, "1", 1) < 0);

  out = dup (2);
  g_assert (out >= 0);

  /* Below we cause a warning, and g_test_expect_message() is broken */
  g_test_log_set_fatal_handler (on_log_ignore_warnings, NULL);

  /* Pass in a bad read descriptor */
  echo_pipe = g_object_new (mock_echo_pipe_get_type (),
                            "name", "test",
                            "in-fd", 1000,
                            "out-fd", out,
                            NULL);

  while (!echo_pipe->closed)
    g_main_context_iteration (NULL, TRUE);

  g_assert_cmpstr (echo_pipe->problem, ==, "internal-error");

  g_object_unref (echo_pipe);
}

static void
test_write_error (void)
{
  MockEchoPipe *echo_pipe;
  GBytes *sent;
  int fds[2];

  /* Just used so we have a valid fd */
  if (pipe(fds) < 0)
    g_assert_not_reached ();

  /* Below we cause a warning, and g_test_expect_message() is broken */
  g_test_log_set_fatal_handler (on_log_ignore_warnings, NULL);

  /* Pass in a bad write descriptor */
  echo_pipe = g_object_new (mock_echo_pipe_get_type (),
                            "name", "test",
                            "in-fd", fds[0],
                            "out-fd", 1000,
                            NULL);

  sent = g_bytes_new ("test", 4);
  cockpit_pipe_write (COCKPIT_PIPE (echo_pipe), sent);
  g_bytes_unref (sent);

  while (!echo_pipe->closed)
    g_main_context_iteration (NULL, TRUE);

  g_assert_cmpstr (echo_pipe->problem, ==, "internal-error");

  close (fds[1]);

  g_object_unref (echo_pipe);
}

static void
test_read_combined (void)
{
  MockEchoPipe *echo_pipe;
  struct iovec iov[4];
  gint fds[2];
  int out;

  if (pipe(fds) < 0)
    g_assert_not_reached ();

  out = dup (2);
  g_assert (out >= 0);

  /* Pass in a read end of the pipe */
  echo_pipe = g_object_new (mock_echo_pipe_get_type (),
                            "name", "test",
                            "in-fd", fds[0],
                            "out-fd", out,
                            NULL);

  /* Write two messages to the pipe at once */
  iov[0].iov_base = "one";
  iov[0].iov_len = 3;
  iov[1].iov_base = "two";
  iov[1].iov_len = 3;
  iov[2].iov_base = "three";
  iov[2].iov_len = 5;
  iov[3].iov_base = "\0";
  iov[3].iov_len = 1;
  g_assert_cmpint (writev (fds[1], iov, 4), ==, 12);

  while (echo_pipe->received->len < 12)
    g_main_context_iteration (NULL, TRUE);

  g_assert_cmpint (echo_pipe->received->len, ==, 12);
  g_assert_cmpstr ((gchar *)echo_pipe->received->data, ==, "onetwothree");

  close (fds[1]);
  g_object_unref (echo_pipe);
}

static void
test_consume_entire (void)
{
  GByteArray *buffer;
  GBytes *bytes;

  buffer = g_byte_array_new ();
  g_byte_array_append (buffer, (guint8 *)"Marmaalaaaade!", 15);

  bytes = cockpit_pipe_consume (buffer, 0, 15);
  g_assert_cmpuint (buffer->len, ==, 0);
  g_byte_array_free (buffer, TRUE);

  g_assert_cmpuint (g_bytes_get_size (bytes), ==, 15);
  g_assert_cmpstr (g_bytes_get_data (bytes, NULL), ==, "Marmaalaaaade!");
  g_bytes_unref (bytes);
}

static void
test_consume_partial (void)
{
  GByteArray *buffer;
  GBytes *bytes;

  buffer = g_byte_array_new ();
  g_byte_array_append (buffer, (guint8 *)"Marmaalaaaade!", 15);

  bytes = cockpit_pipe_consume (buffer, 0, 7);
  g_assert_cmpuint (buffer->len, ==, 8);
  g_assert_cmpstr ((gchar *)buffer->data, ==, "aaaade!");
  g_byte_array_free (buffer, TRUE);

  g_assert_cmpuint (g_bytes_get_size (bytes), ==, 7);
  g_assert (memcmp (g_bytes_get_data (bytes, NULL), "Marmaal", 7) == 0);
  g_bytes_unref (bytes);
}

static void
test_consume_skip (void)
{
  GByteArray *buffer;
  GBytes *bytes;

  buffer = g_byte_array_new ();
  g_byte_array_append (buffer, (guint8 *)"Marmaalaaaade!", 15);

  bytes = cockpit_pipe_consume (buffer, 7, 8);
  g_assert_cmpuint (buffer->len, ==, 0);
  g_byte_array_free (buffer, TRUE);

  g_assert_cmpuint (g_bytes_get_size (bytes), ==, 8);
  g_assert_cmpstr (g_bytes_get_data (bytes, NULL), ==,  "aaaade!");
  g_bytes_unref (bytes);
}

static void
test_buffer_skip (void)
{
  GByteArray *buffer;

  buffer = g_byte_array_new ();
  g_byte_array_append (buffer, (guint8 *)"Marmaalaaaade!", 15);

  cockpit_pipe_skip (buffer, 7);
  g_assert_cmpuint (buffer->len, ==, 8);

  g_assert_cmpstr ((char *)buffer->data, ==,  "aaaade!");
  g_byte_array_free (buffer, TRUE);
}

static void
test_properties (void)
{
  CockpitPipe *tpipe;
  gchar *name;
  gint in;
  gint out;
  int fds[2];

  if (pipe(fds) < 0)
    g_assert_not_reached ();

  tpipe = g_object_new (mock_echo_pipe_get_type (),
                       "name", "testo",
                       "in-fd", fds[0],
                       "out-fd", fds[1],
                       NULL);

  g_object_get (tpipe, "name", &name, "in-fd", &in, "out-fd", &out, NULL);
  g_assert_cmpstr (name, ==, "testo");
  g_free (name);
  g_assert_cmpint (in, ==, fds[0]);
  g_assert_cmpint (out, ==, fds[1]);

  g_object_unref (tpipe);
}

typedef struct {
  GSocket *listen_sock;
  GSource *listen_source;
  GSocket *conn_sock;
  GSource *conn_source;
  GSocketAddress *address;
} TestConnect;

static gboolean
on_socket_input (GSocket *socket,
                 GIOCondition cond,
                 gpointer user_data)
{
  gchar buffer[1024];
  GError *error = NULL;
  gssize ret, wret;

  ret = g_socket_receive (socket, buffer, sizeof (buffer), NULL, &error);
  g_assert_no_error (error);

  if (ret == 0)
    {
      g_socket_shutdown (socket, FALSE, TRUE, &error);
      g_assert_no_error (error);
      return FALSE;
    }

  g_assert (ret > 0);
  wret = g_socket_send (socket, buffer, ret, NULL, &error);
  g_assert_no_error (error);
  g_assert (wret == ret);
  return TRUE;
}

static gboolean
on_socket_connection (GSocket *socket,
                      GIOCondition cond,
                      gpointer user_data)
{
  TestConnect *tc = user_data;
  GError *error = NULL;

  g_assert (tc->conn_source == NULL);
  tc->conn_sock = g_socket_accept (tc->listen_sock, NULL, &error);
  g_assert_no_error (error);

  tc->conn_source = g_socket_create_source (tc->conn_sock, G_IO_IN, NULL);
  g_source_set_callback (tc->conn_source, (GSourceFunc)on_socket_input, tc, NULL);
  g_source_attach (tc->conn_source, NULL);

  /* Only one connection */
  return FALSE;
}

static void
setup_connect (TestConnect *tc,
               gconstpointer data)
{
  GError *error = NULL;
  GInetAddress *inet;
  GSocketAddress *address;

  inet = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
  address = g_inet_socket_address_new (inet, 0);
  g_object_unref (inet);

  tc->listen_sock = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
                                  G_SOCKET_PROTOCOL_DEFAULT, &error);
  g_assert_no_error (error);

  g_socket_bind (tc->listen_sock, address, TRUE, &error);
  g_object_unref (address);
  g_assert_no_error (error);

  tc->address = g_socket_get_local_address (tc->listen_sock, &error);
  g_assert_no_error (error);

  g_socket_listen (tc->listen_sock, &error);
  g_assert_no_error (error);

  tc->listen_source = g_socket_create_source (tc->listen_sock, G_IO_IN, NULL);
  g_source_set_callback (tc->listen_source, (GSourceFunc)on_socket_connection, tc, NULL);
  g_source_attach (tc->listen_source, NULL);
}

static void
teardown_connect (TestConnect *tc,
                  gconstpointer data)
{
  g_object_unref (tc->address);
  if (tc->conn_source)
    {
      g_source_destroy (tc->conn_source);
      g_source_unref (tc->conn_source);
    }
  if (tc->listen_source)
    {
      g_source_destroy (tc->listen_source);
      g_source_unref (tc->listen_source);
    }
  g_clear_object (&tc->listen_sock);
  g_clear_object (&tc->conn_sock);
}

static void
test_connect_and_read (TestConnect *tc,
                       gconstpointer user_data)
{
  CockpitPipe *pipe;
  GError *error = NULL;
  GByteArray *buffer;

  pipe = cockpit_pipe_connect ("broooo", tc->address);
  g_assert (pipe != NULL);

  while (tc->conn_sock == NULL)
    g_main_context_iteration (NULL, TRUE);

  /* Send the null terminator */
  g_assert_cmpint (g_socket_send (tc->conn_sock, "eier", 5, NULL, &error), ==, 5);
  g_assert_no_error (error);

  buffer = cockpit_pipe_get_buffer (pipe);
  while (buffer->len == 0)
    g_main_context_iteration (NULL, TRUE);

  g_assert_cmpuint (buffer->len, ==, 5);
  g_assert_cmpstr ((gchar *)buffer->data, ==, "eier");

  g_object_unref (pipe);
}

static void
test_connect_and_write (TestConnect *tc,
                        gconstpointer user_data)
{
  gchar buffer[8];
  CockpitPipe *pipe;
  GError *error = NULL;
  GBytes *sent;

  pipe = cockpit_pipe_connect ("broooo", tc->address);
  g_assert (pipe != NULL);

  /* Sending on the pipe before actually connected */
  sent = g_bytes_new_static ("jola", 5);
  cockpit_pipe_write (pipe, sent);
  g_bytes_unref (sent);
  g_assert (tc->conn_sock == NULL);

  /* Now we connect in main loop */
  while (tc->conn_sock == NULL)
    g_main_context_iteration (NULL, TRUE);

  /* Read from the socket */
  g_assert_cmpint (g_socket_receive (tc->conn_sock, buffer, sizeof (buffer), NULL, &error), ==, 5);
  g_assert_no_error (error);

  g_assert_cmpstr (buffer, ==, "jola");
  g_object_unref (pipe);
}

static void
on_closed_get_problem (CockpitPipe *pipe,
                       const gchar *problem,
                       gpointer user_data)
{
  gchar **retval = user_data;
  g_assert (retval != NULL && *retval == NULL);
  *retval = g_strdup (problem ? problem : "");
}

static void
test_fail_not_found (void)
{
  CockpitPipe *pipe;
  GSocketAddress *address;
  gchar *problem = NULL;

  address = g_unix_socket_address_new ("/non-existent");
  pipe = cockpit_pipe_connect ("bad", address);
  g_object_unref (address);

  /* Should not have closed at this point */
  g_assert (pipe != NULL);
  g_signal_connect (pipe, "closed", G_CALLBACK (on_closed_get_problem), &problem);

  /* closes in main loop */
  while (problem == NULL)
    g_main_context_iteration (NULL, TRUE);

  g_assert_cmpstr (problem, ==, "not-found");
  g_free (problem);
  g_object_unref (pipe);
}

static void
test_fail_not_authorized (void)
{
  CockpitPipe *pipe;
  GSocketAddress *address;
  gchar *unix_path;
  gchar *problem = NULL;
  gint fd;

  if (geteuid () == 0)
    {
      test_skip ("running as root");
      return;
    }

  unix_path = g_strdup ("/tmp/cockpit-test-XXXXXX.sock");
  fd = g_mkstemp (unix_path);
  g_assert_cmpint (fd, >=, 0);

  /* Take away all permissions from the file */
  g_assert_cmpint (fchmod (fd, 0000), ==, 0);

  address = g_unix_socket_address_new (unix_path);
  pipe = cockpit_pipe_connect ("bad", address);
  g_object_unref (address);

  /* Should not have closed at this point */
  g_assert (pipe != NULL);
  g_signal_connect (pipe, "closed", G_CALLBACK (on_closed_get_problem), &problem);

  /* closes in main loop */
  while (problem == NULL)
    g_main_context_iteration (NULL, TRUE);

  g_assert_cmpstr (problem, ==, "not-authorized");
  g_free (unix_path);
  g_free (problem);
  g_object_unref (pipe);
}


int
main (int argc,
      char *argv[])
{
  g_type_init ();

  g_set_prgname ("test-pipe");
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/pipe/buffer/consume-entire", test_consume_entire);
  g_test_add_func ("/pipe/buffer/consume-partial", test_consume_partial);
  g_test_add_func ("/pipe/buffer/consume-skip", test_consume_skip);
  g_test_add_func ("/pipe/buffer/skip", test_buffer_skip);

  g_test_add_func ("/pipe/properties", test_properties);

  /*
   * Fixture data is the GType name of the pipe class
   * so register these types here.
   */
  g_type_class_ref (mock_echo_pipe_get_type ());
  g_type_class_ref (cockpit_pipe_get_type ());

  g_test_add ("/pipe/echo-message", TestCase, "MockEchoPipe",
              setup_echo, test_echo_and_close, teardown);
  g_test_add ("/pipe/echo-queue", TestCase, "MockEchoPipe",
              setup_echo, test_echo_queue, teardown);
  g_test_add ("/pipe/echo-large", TestCase, "MockEchoPipe",
              setup_echo, test_echo_large, teardown);
  g_test_add ("/pipe/close-problem", TestCase, "MockEchoPipe",
              setup_echo, test_close_problem, teardown);
  g_test_add ("/pipe/buffer", TestCase, "CockpitPipe",
              setup_echo, test_buffer, teardown);

  g_test_add_func ("/pipe/read-error", test_read_error);
  g_test_add_func ("/pipe/write-error", test_write_error);
  g_test_add_func ("/pipe/read-combined", test_read_combined);

  g_test_add ("/pipe/connect/and-read", TestConnect, NULL,
              setup_connect, test_connect_and_read, teardown_connect);
  g_test_add ("/pipe/connect/and-write", TestConnect, NULL,
              setup_connect, test_connect_and_write, teardown_connect);

  g_test_add_func ("/test-stream/connect/not-found", test_fail_not_found);
  g_test_add_func ("/test-stream/connect/not-authorized", test_fail_not_authorized);

  return g_test_run ();
}