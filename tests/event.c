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


int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/event/redaction/top-level", test_event_redaction_top_level);
  g_test_add_func ("/event/replace/encrypted", test_event_replace_encrypted);

  return g_test_run ();
}
