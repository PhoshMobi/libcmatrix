/*
 * Copyright (C) 2026 Phosh.mobi e.V.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#undef NDEBUG
#undef G_DISABLE_ASSERT
#undef G_DISABLE_CHECKS
#undef G_DISABLE_CAST_CHECKS
#undef G_LOG_DOMAIN

#include "events/cm-event.c"

static JsonObject *
parse (const char *json)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) err = NULL;

  json_parser_load_from_data (parser, json, -1, &err);
  g_assert_no_error (err);

  return json_node_dup_object (json_parser_get_root (parser));
}


static void
test_event_redaction_top_level (void)
{
  g_autoptr (JsonObject) root = parse ("{"
                                       "\"type\": \"m.room.redaction\","
                                       "\"event_id\": \"$r\","
                                       "\"redacts\": \"$target\","
                                       "\"content\": {}}");
  g_autoptr (CmEvent) event = cm_event_new_from_json (root, NULL);

  g_assert_cmpstr (cm_event_get_replaces_id (event), ==, "$target");
}


static void
test_event_redaction_in_content (void)
{
  /* Room version 11 and later, see MSC2174 */
  g_autoptr (JsonObject) root = parse ("{"
                                       "\"type\": \"m.room.redaction\","
                                       "\"event_id\": \"$r\","
                                       "\"content\": {\"redacts\": \"$target\"}}");
  g_autoptr (CmEvent) event = cm_event_new_from_json (root, NULL);

  g_assert_cmpstr (cm_event_get_replaces_id (event), ==, "$target");
}


static void
test_event_replace_encrypted (void)
{
  /* related_to can show up in inner and outer */
  const char *rel = "\"m.relates_to\": {\"rel_type\": \"m.replace\", \"event_id\": \"$orig\"}";
  g_autofree char *outer = g_strdup_printf ("{\"type\": \"m.room.encrypted\","
                                            "\"event_id\": \"$e\", \"content\": {%s}}", rel);
  g_autofree char *inner = g_strdup_printf ("{\"type\": \"m.room.message\","
                                            "\"event_id\": \"$e\", \"content\": {%s}}", rel);
  g_autoptr(JsonObject) enc = parse (outer);
  g_autoptr(JsonObject) root = parse (inner);
  g_autoptr(CmEvent) event = cm_event_new_from_json (root, enc);

  g_assert_cmpstr (cm_event_get_replaces_id (event), ==, "$orig");
}


static void
test_event_redacted (void)
{
  g_autoptr (JsonObject) plain = parse ("{"
    "\"type\": \"m.room.message\", \"event_id\": \"$a\","
    "\"content\": {\"msgtype\": \"m.text\", \"body\": \"hi\"}}");
  /* The server strips the content and adds the redaction event */
  g_autoptr (JsonObject) by_server = parse ("{"
    "\"type\": \"m.room.message\", \"event_id\": \"$b\", \"content\": {},"
    "\"unsigned\": {\"redacted_because\": {\"type\": \"m.room.redaction\","
    "\"event_id\": \"$r\", \"redacts\": \"$b\"}}}");
  g_autoptr (JsonObject) enc = parse ("{"
    "\"type\": \"m.room.encrypted\", \"event_id\": \"$c\", \"content\": {},"
    "\"unsigned\": {\"redacted_because\": {}}}");
  g_autoptr (JsonObject) dec = parse ("{"
    "\"type\": \"m.room.message\", \"event_id\": \"$c\", \"content\": {}}");
  g_autoptr (CmEvent) event = cm_event_new_from_json (plain, NULL);
  g_autoptr (CmEvent) event_server = cm_event_new_from_json (by_server, NULL);
  g_autoptr (CmEvent) event_enc = cm_event_new_from_json (dec, enc);
  guint updated = 0;

  g_assert_false (cm_event_is_redacted (event));
  g_assert_true (cm_event_is_redacted (event_server));
  g_assert_true (cm_event_is_redacted (event_enc));

  /* Marking it ourselves notifies once */
  g_signal_connect_swapped (event, "updated", G_CALLBACK (g_atomic_int_inc), &updated);
  cm_event_set_redacted (event);
  cm_event_set_redacted (event);
  g_assert_true (cm_event_is_redacted (event));
  g_assert_cmpuint (updated, ==, 1);
}


int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/event/redaction/top-level", test_event_redaction_top_level);
  g_test_add_func ("/event/redaction/in-content", test_event_redaction_in_content);
  g_test_add_func ("/event/replace/encrypted", test_event_replace_encrypted);
  g_test_add_func ("/event/redacted", test_event_redacted);

  return g_test_run ();
}
