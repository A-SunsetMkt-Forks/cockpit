/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2013 Red Hat, Inc.
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
 * along with Cockpit; If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "cockpithandlers.h"

#include "cockpitbranding.h"
#include "cockpitchannelresponse.h"
#include "cockpitchannelsocket.h"
#include "cockpitwebservice.h"
#include "cockpitws.h"

#include "common/cockpitconf.h"
#include "common/cockpitjson.h"
#include "common/cockpitwebcertificate.h"
#include "common/cockpitwebinject.h"

#include "websocket/websocket.h"

#include <json-glib/json-glib.h>

#include <gio/gio.h>

#include <string.h>

/* For overriding during tests */
const gchar *cockpit_ws_shell_component = "/shell/index.html";

static gchar *
locate_selfsign_ca (void)
{
  g_autofree gchar *cert_path = NULL;
  gchar *ca_path = NULL;
  gchar *error = NULL;

  cert_path = cockpit_certificate_locate (true, &error);
  if (cert_path && g_str_has_suffix (cert_path, "/0-self-signed.cert"))
    {
      g_autofree gchar *dir = g_path_get_dirname (cert_path);
      ca_path = g_build_filename (dir, "0-self-signed-ca.pem", NULL);
      if (!g_file_test (ca_path, G_FILE_TEST_EXISTS))
        {
          g_free (ca_path);
          ca_path = NULL;
        }
    }

  return ca_path;
}

static void
on_web_socket_noauth (WebSocketConnection *connection,
                      gpointer data)
{
  GBytes *payload;
  GBytes *prefix;

  g_debug ("closing unauthenticated web socket");

  payload = cockpit_transport_build_control ("command", "init", "problem", "no-session", NULL);
  prefix = g_bytes_new_static ("\n", 1);

  web_socket_connection_send (connection, WEB_SOCKET_DATA_TEXT, prefix, payload);
  web_socket_connection_close (connection, WEB_SOCKET_CLOSE_GOING_AWAY, "no-session");

  g_bytes_unref (prefix);
  g_bytes_unref (payload);
}

static void
handle_noauth_socket (CockpitWebRequest *request)
{
  WebSocketConnection *connection;

  connection = cockpit_web_service_create_socket (NULL, request);

  g_signal_connect (connection, "open", G_CALLBACK (on_web_socket_noauth), NULL);

  /* Unreferences connection when it closes */
  g_signal_connect (connection, "close", G_CALLBACK (g_object_unref), NULL);
}

/* Called by @server when handling HTTP requests to /cockpit/socket */
gboolean
cockpit_handler_socket (CockpitWebServer *server,
                        CockpitWebRequest *request,
                        CockpitHandlerData *ws)
{
  const gchar *path = cockpit_web_request_get_path (request);
  const gchar *method = cockpit_web_request_get_method (request);
  GHashTable *headers = cockpit_web_request_get_headers (request);

  CockpitWebService *service = NULL;
  const gchar *segment = NULL;

  /*
   * Socket requests should come in on /cockpit/socket or /cockpit+app/socket.
   * However older javascript may connect on /socket, so we continue to support that.
   */

  if (path && path[0])
    segment = strchr (path + 1, '/');
  if (!segment)
    segment = path;

  if (!segment || !g_str_equal (segment, "/socket"))
    return FALSE;

  /* don't support HEAD on a socket, it makes little sense */
  if (g_strcmp0 (method, "GET") != 0)
      return FALSE;

  if (headers && ws)
    service = cockpit_auth_check_cookie (ws->auth, request);
  if (service)
    {
      cockpit_web_service_socket (service, request);
    }
  else
    {
      handle_noauth_socket (request);
    }

  return TRUE;
}

gboolean
cockpit_handler_external (CockpitWebServer *server,
                          CockpitWebRequest *request,
                          CockpitHandlerData *ws)
{
  const gchar *path = cockpit_web_request_get_path (request);
  GHashTable *headers = cockpit_web_request_get_headers (request);

  CockpitWebResponse *response = NULL;
  CockpitWebService *service = NULL;
  const gchar *segment = NULL;
  JsonObject *open = NULL;
  CockpitCreds *creds;
  const gchar *expected;
  const gchar *upgrade;
  guchar *decoded;
  GBytes *bytes;
  gsize length;

  /* The path must start with /cockpit+xxx/channel/csrftoken? or similar */
  if (path && path[0])
    segment = strchr (path + 1, '/');
  if (!segment)
    return FALSE;
  if (!g_str_has_prefix (segment, "/channel/"))
    return FALSE;
  segment += 9;

  /* Make sure we are authenticated, otherwise 404 */
  service = cockpit_auth_check_cookie (ws->auth, request);
  if (!service)
    return FALSE;

  creds = cockpit_web_service_get_creds (service);
  g_return_val_if_fail (creds != NULL, FALSE);

  expected = cockpit_creds_get_csrf_token (creds);
  g_return_val_if_fail (expected != NULL, FALSE);

  /* No such path is valid */
  if (!g_str_equal (segment, expected))
    {
      g_message ("invalid csrf token");
      return FALSE;
    }

  decoded = g_base64_decode (cockpit_web_request_get_query (request), &length);
  if (decoded)
    {
      bytes = g_bytes_new_take (decoded, length);
      if (!cockpit_transport_parse_command (bytes, NULL, NULL, &open))
        {
          open = NULL;
          g_message ("invalid external channel query");
        }
      g_bytes_unref (bytes);
    }

  if (!open)
    {
      response = cockpit_web_request_respond (request);
      cockpit_web_response_error (response, 400, NULL, NULL);
      g_object_unref (response);
    }
  else
    {
      upgrade = g_hash_table_lookup (headers, "Upgrade");
      if (upgrade && g_ascii_strcasecmp (upgrade, "websocket") == 0)
        {
          cockpit_channel_socket_open (service, open, request);
        }
      else
        {
          cockpit_channel_response_open (service, request, open);
        }
      json_object_unref (open);
    }

  g_object_unref (service);

  return TRUE;
}


static void
add_oauth_to_environment (JsonObject *environment)
{
  static const gchar *url;
  JsonObject *object;

  url = cockpit_conf_string ("OAuth", "URL");

  if (url)
    {
      object = json_object_new ();
      json_object_set_string_member (object, "URL", url);
      json_object_set_string_member (object, "ErrorParam",
                                     cockpit_conf_string ("oauth", "ErrorParam"));
      json_object_set_string_member (object, "TokenParam",
                                     cockpit_conf_string ("oauth", "TokenParam"));
      json_object_set_object_member (environment, "OAuth", object);
  }
}

static bool have_command (const char *name)
{
    gint status;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *command = g_strdup_printf ("command -v %s", name);
    if (g_spawn_sync (NULL,
                    (gchar * []){ "/bin/sh", "-ec", command, NULL },
                    NULL,
                    G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                    NULL, NULL, NULL, NULL, &status, &error))
      return status == 0;
    else
      {
        g_warning ("Failed to check for %s: %s", name, error->message);
        return FALSE;
      }
}

static void
add_page_to_environment (JsonObject *object,
                         gboolean    is_cockpit_client)
{
  static gint page_login_to = -1;
  gboolean require_host = FALSE;
  gboolean allow_multihost;
  JsonObject *page;
  const gchar *value;

  page = json_object_new ();

  value = cockpit_conf_string ("WebService", "LoginTitle");
  if (value)
    json_object_set_string_member (page, "title", value);

  if (page_login_to < 0)
    {
      /* cockpit.beiboot is part of cockpit-bridge package */
      gboolean have_ssh = have_command ("ssh") && have_command ("cockpit-bridge");
      if (!have_ssh)
        g_info ("cockpit-bridge or ssh are not available, disabling remote logins");
      page_login_to = cockpit_conf_bool ("WebService", "LoginTo", have_ssh);
    }

  require_host = is_cockpit_client || cockpit_conf_bool ("WebService", "RequireHost", FALSE);
  allow_multihost = cockpit_conf_bool ("WebService", "AllowMultiHost", ALLOW_MULTIHOST_DEFAULT);

  json_object_set_boolean_member (page, "connect", page_login_to);
  json_object_set_boolean_member (page, "require_host", require_host);
  json_object_set_boolean_member (page, "allow_multihost", allow_multihost);
  json_object_set_object_member (object, "page", page);
}

static void
add_logged_into_to_environment (JsonObject *object,
                                CockpitAuth *auth,
                                GHashTable *request_headers)
{
  JsonArray *logged_into = json_array_new ();

  gchar *h = g_hash_table_lookup (request_headers, "Cookie");
  while (h && *h) {
    const gchar *start = h;
    while (*h && *h != '=')
      h++;
    const gchar *equal = h;
    while (*h && *h != ';')
      h++;
    const gchar *end = h;
    if (*h)
      h++;
    while (*h && *h == ' ')
      h++;

    if (*equal != '=')
      continue;

    g_autofree gchar *value = g_strndup (equal + 1, end - equal - 1);

    if (!cockpit_auth_is_valid_cookie_value (auth, value))
      continue;

    g_autofree gchar *name = g_strndup (start, equal - start);
    if (g_str_equal (name, "cockpit"))
      json_array_add_string_element(logged_into, ".");
    else if (g_str_has_prefix (name, "machine-cockpit+"))
      json_array_add_string_element(logged_into, name + strlen("machine-cockpit+"));
  }

  json_object_set_array_member (object, "logged_into", logged_into);
}

static GBytes *
build_environment (CockpitAuth *auth, GHashTable *request_headers)
{
  static const gchar *prefix = "\n    <script>\nvar environment = ";
  static const gchar *suffix = ";\n    </script>";

  GByteArray *buffer;
  GBytes *bytes;
  JsonObject *object;
  gchar *hostname;

  object = json_object_new ();

  gboolean is_cockpit_client = cockpit_conf_bool ("WebService", "X-For-CockpitClient", FALSE);
  json_object_set_boolean_member (object, "is_cockpit_client", is_cockpit_client);

  add_page_to_environment (object, is_cockpit_client);
  add_logged_into_to_environment (object, auth, request_headers);

  hostname = g_malloc0 (HOST_NAME_MAX + 1);
  gethostname (hostname, HOST_NAME_MAX);
  hostname[HOST_NAME_MAX] = '\0';
  json_object_set_string_member (object, "hostname", hostname);
  g_free (hostname);

  add_oauth_to_environment (object);

  g_autofree gchar *ca_path = locate_selfsign_ca ();
  if (ca_path)
    json_object_set_string_member (object, "CACertUrl", "/ca.cer");

  g_autofree gchar *contents = NULL;
  g_autoptr(GError) error = NULL;
  gsize len;

  const gchar *banner = cockpit_conf_string ("Session", "Banner");
  if (banner)
    {
      // TODO: parse macros (see `man agetty` for possible macros)
      g_file_get_contents (banner, &contents, &len, &error);
      if (error)
        g_message ("error loading contents of banner: %s", error->message);
      else
        json_object_set_string_member (object, "banner", contents);
    }

  bytes = cockpit_json_write_bytes (object);
  json_object_unref (object);

  buffer = g_bytes_unref_to_array (bytes);
  g_byte_array_prepend (buffer, (const guint8 *)prefix, strlen (prefix));
  g_byte_array_append (buffer, (const guint8 *)suffix, strlen (suffix));
  return g_byte_array_free_to_bytes (buffer);
}

static void
send_login_html (CockpitWebResponse *response,
                 CockpitHandlerData *ws,
                 const gchar *path,
                 GHashTable *headers)
{
  static const gchar *marker = "<meta insert=\"dynamic_content_here\" />";
  static const gchar *po_marker = "/*insert_translations_here*/";

  CockpitWebFilter *filter;
  GBytes *environment;
  GError *error = NULL;
  GBytes *bytes;

  GBytes *url_bytes = NULL;
  CockpitWebFilter *filter2 = NULL;
  const gchar *url_root = NULL;
  const gchar *accept = NULL;
  gchar *content_security_policy = NULL;
  gchar *cookie_line = NULL;
  gchar *base;

  gchar *language = NULL;
  gchar **languages = NULL;
  GBytes *po_bytes;
  CockpitWebFilter *filter3 = NULL;

  environment = build_environment (ws->auth, headers);
  filter = cockpit_web_inject_new (marker, environment, 1);
  g_bytes_unref (environment);
  cockpit_web_response_add_filter (response, filter);
  g_object_unref (filter);

  url_root = cockpit_web_response_get_url_root (response);
  if (url_root)
    base = g_strdup_printf ("<base href=\"%s/\">", url_root);
  else
    base = g_strdup ("<base href=\"/\">");

  url_bytes = g_bytes_new_take (base, strlen(base));
  filter2 = cockpit_web_inject_new (marker, url_bytes, 1);
  g_bytes_unref (url_bytes);
  cockpit_web_response_add_filter (response, filter2);
  g_object_unref (filter2);

  cockpit_web_response_set_cache_type (response, COCKPIT_WEB_RESPONSE_NO_CACHE);

  if (ws->login_po_js)
    {
      language = cockpit_web_server_parse_cookie (headers, "CockpitLang");
      if (!language)
        {
          accept = g_hash_table_lookup (headers, "Accept-Language");
          languages = cockpit_web_server_parse_accept_list (accept, NULL);
          language = languages[0];
        }

      po_bytes = cockpit_web_response_negotiation (ws->login_po_js, NULL, language, NULL, NULL, &error);
      if (error)
        {
          g_message ("%s", error->message);
          g_clear_error (&error);
        }
      else if (po_bytes)
        {
          filter3 = cockpit_web_inject_new (po_marker, po_bytes, 1);
          g_bytes_unref (po_bytes);
          cockpit_web_response_add_filter (response, filter3);
          g_object_unref (filter3);
        }
    }

  bytes = cockpit_web_response_negotiation (ws->login_html, NULL, NULL, NULL, NULL, &error);
  if (error)
    {
      g_message ("%s", error->message);
      cockpit_web_response_error (response, 500, NULL, NULL);
      g_error_free (error);
    }
  else if (!bytes)
    {
      cockpit_web_response_error (response, 404, NULL, NULL);
    }
  else
    {
      /* The login Content-Security-Policy allows the page to have inline <script> and <style> tags. */
      gboolean secure = g_strcmp0 (cockpit_web_response_get_protocol (response), "https") == 0;
      cookie_line = cockpit_auth_empty_cookie_value (path, secure);
      content_security_policy = cockpit_web_response_security_policy ("default-src 'self' 'unsafe-inline'",
                                                                      cockpit_web_response_get_origin (response));

      cockpit_web_response_headers (response, 200, "OK", -1,
                                    "Content-Type", "text/html",
                                    "Content-Security-Policy", content_security_policy,
                                    "Set-Cookie", cookie_line,
                                    NULL);
      cockpit_web_response_set_cache_type (response, COCKPIT_WEB_RESPONSE_NO_CACHE);
      if (cockpit_web_response_queue (response, bytes))
        cockpit_web_response_complete (response);

      g_bytes_unref (bytes);
    }

  g_free (cookie_line);
  g_free (content_security_policy);
  g_strfreev (languages);
}

static void
send_login_response (CockpitWebResponse *response,
                     JsonObject *object,
                     GHashTable *headers)
{
  GBytes *content;

  content = cockpit_json_write_bytes (object);

  g_hash_table_replace (headers, g_strdup ("Content-Type"), g_strdup ("application/json"));
  cockpit_web_response_content (response, headers, content, NULL);
  g_bytes_unref (content);
}

static void
on_login_complete (GObject *object,
                   GAsyncResult *result,
                   gpointer user_data)
{
  // consumes the reference on user_data
  g_autoptr(CockpitWebResponse) response = g_steal_pointer (&user_data);

  /* Never cache a login response */
  g_autoptr(GHashTable) headers = cockpit_web_server_new_table ();
  cockpit_web_response_set_cache_type (response, COCKPIT_WEB_RESPONSE_NO_CACHE);

  g_autoptr(GError) error = NULL;
  g_autoptr(JsonObject) response_data = cockpit_auth_login_finish (COCKPIT_AUTH (object), result,
                                                                   cockpit_web_response_get_stream (response),
                                                                   headers, &error);

  if (error)
    {
      g_autoptr(GBytes) body = NULL;

      if (response_data)
        {
          g_hash_table_insert (headers, g_strdup ("Content-Type"), g_strdup ("application/json"));
          body = cockpit_json_write_bytes (response_data);
        }

      cockpit_web_response_gerror (response, headers, body, error);
    }
  else
    {
      send_login_response (response, response_data, headers);
    }
}

static void
handle_login (CockpitHandlerData *data,
              CockpitWebService *service,
              CockpitWebRequest *request,
              CockpitWebResponse *response)
{
  GHashTable *out_headers;
  CockpitCreds *creds;
  JsonObject *creds_json = NULL;

  if (service)
    {
      out_headers = cockpit_web_server_new_table ();
      creds = cockpit_web_service_get_creds (service);
      creds_json = cockpit_creds_to_json (creds);
      send_login_response (response, creds_json, out_headers);
      g_hash_table_unref (out_headers);
      json_object_unref (creds_json);
      return;
    }

  cockpit_auth_login_async (data->auth, request, on_login_complete, g_object_ref (response));
}

static void
handle_resource (CockpitHandlerData *data,
                 CockpitWebService *service,
                 const gchar *path,
                 GHashTable *headers,
                 CockpitWebResponse *response)
{
  gchar *where;

  where = cockpit_web_response_pop_path (response);
  if (where && (where[0] == '@' || where[0] == '$') && where[1] != '\0')
    {
      if (service)
        {
          cockpit_channel_response_serve (service, headers, response, where,
                                          cockpit_web_response_get_path (response));
        }
      else if (g_str_has_suffix (path, ".html"))
        {
          send_login_html (response, data, path, headers);
        }
      else
        {
          cockpit_web_response_error (response, 401, NULL, NULL);
        }
    }
  else
    {
      cockpit_web_response_error (response, 404, NULL, NULL);
    }

  g_free (where);
}

static void
handle_shell (CockpitHandlerData *data,
              CockpitWebService *service,
              const gchar *path,
              GHashTable *headers,
              CockpitWebResponse *response)
{
  gboolean valid;
  const gchar *shell_path;

  /* Check if a valid path for a shell to be served at */
  valid = g_str_equal (path, "/") ||
          g_str_has_prefix (path, "/@") ||
          g_str_has_prefix (path, "/=") ||
          strspn (path + 1, COCKPIT_RESOURCE_PACKAGE_VALID) == strcspn (path + 1, "/");

  if (g_str_has_prefix (path, "/=/") ||
      g_str_has_prefix (path, "/@/") ||
      g_str_has_prefix (path, "//"))
    {
      valid = FALSE;
    }

  if (!valid)
    {
      cockpit_web_response_error (response, 404, NULL, NULL);
    }
  else if (service)
    {
      shell_path = cockpit_conf_string ("WebService", "Shell");
      cockpit_channel_response_serve (service, headers, response, NULL,
                                      shell_path ? shell_path : cockpit_ws_shell_component);
      cockpit_web_response_set_cache_type (response, COCKPIT_WEB_RESPONSE_NO_CACHE);
    }
  else
    {
      send_login_html (response, data, path, headers);
    }
}

gboolean
cockpit_handler_default (CockpitWebServer *server,
                         CockpitWebRequest *request,
                         const gchar *path,
                         GHashTable *headers,
                         CockpitWebResponse *response,
                         CockpitHandlerData *data)
{
  CockpitWebService *service;
  const gchar *remainder = NULL;
  gboolean resource;

  path = cockpit_web_response_get_path (response);
  g_return_val_if_fail (path != NULL, FALSE);

  /* robots.txt is unauthorized and works on any directory */
  if (g_str_has_suffix (path, "/robots.txt"))
    {
      g_autoptr(GHashTable) out_headers = cockpit_web_server_new_table ();
      g_hash_table_insert (out_headers, g_strdup ("Content-Type"), g_strdup ("text/plain"));
      const char *body ="User-agent: *\nDisallow: /\n";
      g_autoptr(GBytes) content = g_bytes_new_static (body, strlen (body));
      cockpit_web_response_content (response, out_headers, content, NULL);
      return TRUE;
    }

  resource = g_str_has_prefix (path, "/cockpit/") ||
             g_str_has_prefix (path, "/cockpit+") ||
             g_str_equal (path, "/cockpit");

  // Check for auth
  service = cockpit_auth_check_cookie (data->auth, request);

  /* Stuff in /cockpit or /cockpit+xxx */
  if (resource)
    {
      g_assert (cockpit_web_response_skip_path (response));
      remainder = cockpit_web_response_get_path (response);

      if (!remainder)
        {
          cockpit_web_response_error (response, 404, NULL, NULL);
          return TRUE;
        }
      else if (g_str_has_prefix (remainder, "/static/"))
        {
          cockpit_branding_serve (service, response, path, remainder + 8,
                                  data->os_release, data->branding_roots);
          return TRUE;
        }
    }

  if (resource)
    {
      if (g_str_equal (remainder, "/login"))
        {
          handle_login (data, service, request, response);
        }
      else
        {
          handle_resource (data, service, path, headers, response);
        }
    }
  else
    {
      handle_shell (data, service, path, headers, response);
    }

  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

gboolean
cockpit_handler_root (CockpitWebServer *server,
                      CockpitWebRequest *request,
                      const gchar *path,
                      GHashTable *headers,
                      CockpitWebResponse *response,
                      CockpitHandlerData *ws)
{
  /* Don't cache forever */
  cockpit_web_response_file (response, path, ws->branding_roots);
  return TRUE;
}

gboolean
cockpit_handler_ping (CockpitWebServer *server,
                      CockpitWebRequest *request,
                      const gchar *path,
                      GHashTable *headers,
                      CockpitWebResponse *response,
                      CockpitHandlerData *ws)
{
  GHashTable *out_headers;
  const gchar *body;
  GBytes *content;

  out_headers = cockpit_web_server_new_table ();

  /*
   * The /ping request has unrestricted CORS enabled on it. This allows javascript
   * in the browser on embedding websites to check if Cockpit is available. These
   * websites could do this in another way (such as loading an image from Cockpit)
   * but this does it in the correct manner.
   *
   * See: http://www.w3.org/TR/cors/
   */
  g_hash_table_insert (out_headers, g_strdup ("Access-Control-Allow-Origin"), g_strdup ("*"));

  g_hash_table_insert (out_headers, g_strdup ("Content-Type"), g_strdup ("application/json"));
  body ="{ \"service\": \"cockpit\" }";
  content = g_bytes_new_static (body, strlen (body));

  cockpit_web_response_content (response, out_headers, content, NULL);

  g_bytes_unref (content);
  g_hash_table_unref (out_headers);

  return TRUE;
}

gboolean
cockpit_handler_ca_cert (CockpitWebServer *server,
                         CockpitWebRequest *request,
                         const gchar *path,
                         GHashTable *headers,
                         CockpitWebResponse *response,
                         CockpitHandlerData *ws)
{
  g_autofree gchar *ca_path = NULL;

  ca_path = locate_selfsign_ca ();
  if (ca_path == NULL) {
    cockpit_web_response_error (response, 404, NULL, "CA certificate not found");
    return TRUE;
  }

  const gchar *root_dir[] = { "/", NULL };
  cockpit_web_response_file (response, ca_path, root_dir);
  return TRUE;
}
