/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* room.c
 *
 * Copyright 2022 Purism SPC
 *           2026 Phosh.mobi e.V.
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#undef NDEBUG
#undef G_DISABLE_ASSERT
#undef G_DISABLE_CHECKS
#undef G_DISABLE_CAST_CHECKS
#undef G_LOG_DOMAIN

#include "cm-room.c"

#include <libsoup/soup.h>
#include <glib/gstdio.h>


static void
test_room_new (void)
{
  CmRoom *room;

  room = cm_room_new ("some-room-id");
  g_assert (CM_IS_ROOM (room));

  g_assert_cmpstr (cm_room_get_id (room), ==, "some-room-id");

  g_assert_false (cm_room_is_encrypted (room));

  g_assert_finalize_object (room);
}

static void
test_room_new_from_json (void)
{
  g_autoptr(JsonObject) root = NULL;
  CmRoom *room;

  root = json_object_new ();
  json_object_set_object_member (root, "local", json_object_new ());
  json_object_set_string_member (json_object_get_object_member (root, "local"),
                                 "past_alias", "past");
  json_object_set_string_member (json_object_get_object_member (root, "local"),
                                 "encryption", "encrypted");

  room = cm_room_new_from_json ("some-room-id", json_object_ref (root), NULL);
  cm_room_set_prev_batch (room, "s1_2_3");

  g_assert_true (cm_room_is_encrypted (room));

  g_assert_finalize_object (room);
}


typedef struct
{
  SoupServer *server;
  char       *homeserver;
  guint       send_status;
  const char *send_body;
  guint       n_send;
} TestServer;


typedef struct
{
  gboolean  done;
  char     *event_id;
  GError   *error;
} TestResult;


static void
on_request (SoupServer        *server,
            SoupServerMessage *msg,
            const char        *path,
            GHashTable        *query,
            gpointer           user_data)
{
  TestServer *ts = user_data;
  const char *body = "{}";
  guint status = SOUP_STATUS_OK;

  if (strstr (path, "/send/")) {
    ts->n_send++;
    status = ts->send_status;
    body = ts->send_body;
  } else {
    status = SOUP_STATUS_NOT_FOUND;
  }

  soup_server_message_set_status (msg, status, NULL);
  soup_server_message_set_response (msg, "application/json",
                                    SOUP_MEMORY_COPY, body, strlen (body));
}


static void
test_server_start (TestServer *ts)
{
  g_autoptr (GError) err = NULL;
  g_autoslist (GUri) uris = NULL;

  ts->send_status = SOUP_STATUS_OK;
  ts->send_body = "{\"event_id\":\"$ev1\"}";

  ts->server = soup_server_new (NULL, NULL);
  soup_server_add_handler (ts->server, NULL, on_request, ts, NULL);
  soup_server_listen_local (ts->server, 0, 0, &err);
  g_assert_no_error (err);

  uris = soup_server_get_uris (ts->server);
  g_assert_nonnull (uris);
  ts->homeserver = g_uri_to_string (uris->data);
}


static void
test_server_stop (TestServer *ts)
{
  soup_server_disconnect (ts->server);
  g_clear_object (&ts->server);
  g_clear_pointer (&ts->homeserver, g_free);
}


static void
on_db_open (GObject *object, GAsyncResult *result, gpointer user_data)
{
  GTask *task = user_data;
  gboolean success = g_task_propagate_boolean (G_TASK (result), NULL);

  g_task_return_boolean (task, success);
}


static CmRoom *
create_room_with_client (TestServer *ts)
{
  g_autoptr(CmDb) db = NULL;
  g_autoptr(CmClient) client = NULL;
  g_autoptr(GTask) task = NULL;
  CmRoom *room;

  g_unlink (g_test_get_filename (G_TEST_BUILT, "test-room.db", NULL));
  db = cm_db_new ();
  task = g_task_new (NULL, NULL, NULL, NULL);
  cm_db_open_async (db, g_strdup (g_test_get_dir (G_TEST_BUILT)),
                    "test-room.db", on_db_open, task);
  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);
  g_assert_true (g_task_propagate_boolean (task, NULL));

  client = cm_client_new ();
  /* Don't require keyring for secrets */
  g_object_set_data (G_OBJECT (client), "no-save", GINT_TO_POINTER (TRUE));
  cm_client_set_db (client, db);
  g_assert_true (cm_client_set_homeserver (client, ts->homeserver));
  cm_client_set_user_id (client, "@foo:example.org");
  cm_client_set_access_token (client, "token");

  room = cm_room_new ("!room1:example.org");
  cm_room_set_client (room, client);

  return room;
}


static void
send_done_cb (GObject *object, GAsyncResult *result, gpointer user_data)
{
  TestResult *res = user_data;

  res->event_id = cm_room_send_text_finish (CM_ROOM (object), result, &res->error);
  res->done = TRUE;
}


static void
wait_for_results (TestResult *res, guint n)
{
  gint64 end = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;

  for (;;) {
    guint done = 0;

    for (guint i = 0; i < n; i++)
      done += res[i].done;
    if (done == n || g_get_monotonic_time () > end)
      break;
    g_main_context_iteration (NULL, FALSE);
    g_usleep (500);
  }

  /* All done? */
  for (guint i = 0; i < n; i++)
    g_assert_true (res[i].done);
}


static void
on_room_gone (gpointer  data, GObject *where_the_object_was)
{
  *(gboolean *)data = TRUE;
}


static void
release_room (CmRoom *room)
{
  gint64 end = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  gboolean gone = FALSE;

  g_object_weak_ref (G_OBJECT (room), on_room_gone, &gone);
  /* Drop our reference */
  g_object_unref (room);

  /* Wait for all room to be released */
  while (!gone && g_get_monotonic_time () < end) {
    g_main_context_iteration (NULL, FALSE);
    g_usleep (500);
  }

  g_assert_true (gone);
}


static void
test_result_clear (TestResult *res)
{
  g_clear_pointer (&res->event_id, g_free);
  g_clear_error (&res->error);
}


static void
test_room_send_text (void)
{
  TestServer ts = { 0 };
  TestResult res[2] = { 0 };
  CmRoom *room;

  test_server_start (&ts);
  room = create_room_with_client (&ts);

  cm_room_send_text_async (room, "one", NULL, send_done_cb, &res[0]);
  cm_room_send_text_async (room, "two", NULL, send_done_cb, &res[1]);
  wait_for_results (res, G_N_ELEMENTS (res));

  for (guint i = 0; i < G_N_ELEMENTS (res); i++) {
    g_assert_no_error (res[i].error);
    g_assert_cmpstr (res[i].event_id, ==, "$ev1");
    test_result_clear (&res[i]);
  }
  g_assert_cmpuint (ts.n_send, ==, G_N_ELEMENTS (res));

  release_room (room);
  test_server_stop (&ts);
}


int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/room/new", test_room_new);
  g_test_add_func ("/room/new-from-json", test_room_new_from_json);

  g_test_add_func ("/room/send/text", test_room_send_text);

  return g_test_run ();
}
