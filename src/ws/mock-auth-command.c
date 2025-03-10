/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2016 Red Hat, Inc.
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

#include "common/cockpitauthorize.h"
#include "common/cockpitframe.h"

#include <security/pam_appl.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "src/session/session-utils.h"

#define DEBUG 0
#define EX 127

static const char *auth_prefix = "\n{\"command\":\"authorize\",\"cookie\":\"xxx\"";
static const char *auth_suffix = "\"}";

static void
write_authorize_challenge (const char *data)
{
  char *message = NULL;
  if (asprintf (&message, "%s,\"challenge\":\"%s%s", auth_prefix, data, auth_suffix) < 0)
    errx (EX, "out of memory writing string");
#if DEBUG
  fprintf (stderr, "mock-auth-command > %s\n", message);
#endif
  if (cockpit_frame_write (STDOUT_FILENO, (unsigned char *)message, strlen (message)) < 0)
    err (EX, "couldn't write auth request");
  free (message);
}

static void
write_message (const char *message)
{
  if (cockpit_frame_write (STDOUT_FILENO, (unsigned char *)message, strlen (message)) < 0)
    err (EX, "coludn't write message");
}

static void
write_init_message (const char *data)
{
  char *message = NULL;
  if (asprintf (&message, "\n{\"command\":\"init\",%s,\"version\":1}", data) < 0)
    errx (EX, "out of memory writing string");
#if DEBUG
  fprintf (stderr, "mock-auth-command > %s\n", message);
#endif
  write_message (message);
  free (message);
}

int
main (int argc,
      char **argv)
{
  int success = 0;
  int launch_bridge = 0;
  const char *data = NULL;
  char *type;

  write_authorize_challenge ("*");

  char *message = read_authorize_response ("authorization");
  char *response = get_authorize_key (message, "response", true);
  data = cockpit_authorize_type (response, &type);
  assert (data != NULL);

  if (strcmp (data, "") == 0)
    {
      write_init_message ("\"problem\":\"authentication-failed\"");
    }
  if (strcmp (data, "no-cookie") == 0)
    {
      write_message ("\n{\"command\":\"authorize\",\"response\": \"user me\"}");
      free (message);
      free (response);
      write_authorize_challenge ("*");
      message = read_authorize_response ("no-cookie");
      response = get_authorize_key (message, "response", true);
      if (!response || strcmp (response, "user me") != 0)
        {
          write_init_message ("\"problem\": \"authentication-failed\"");
        }
      else
        {
          write_init_message ("\"user\": \"me\"");
          success = 1;
        }
    }
  else if (strcmp (data, "failslow") == 0)
    {
      sleep (2);
      write_init_message ("\"problem\":\"authentication-failed\"");
    }
  else if (strcmp (data, "fail") == 0)
    {
      write_init_message ("\"problem\":\"authentication-failed\"");
    }
  else if (strcmp (data, "not-supported") == 0)
    {
      write_init_message ("\"problem\": \"authentication-not-supported\", \"auth-method-results\": {}");
    }
  else if (strcmp (data, "ssh-fail") == 0)
    {
      write_init_message ("\"problem\": \"authentication-failed\", \"auth-method-results\": { \"password\": \"denied\"}");
    }
  else if (strcmp (data, "denied") == 0)
    {
      write_init_message ("\"problem\": \"access-denied\"");
    }
  else if (strcmp (data, "success") == 0)
    {
      write_init_message ("\"user\": \"me\"");
      success = 1;
    }
  else if (strcmp (data, "ssh-remote-switch") == 0 &&
           strcmp (argv[1], "machine") == 0)
    {
      write_init_message ("\"user\": \"me\"");
      success = 1;
    }
  else if (strcmp (data, "ssh-alt-machine") == 0 &&
           strcmp (argv[1], "machine") == 0)
    {
      write_init_message ("\"user\": \"me\"");
      success = 1;
    }
  else if (strcmp (data, "ssh-alt-default") == 0 &&
           strcmp (argv[1], "default-host") == 0)
    {
      write_init_message ("\"user\": \"me\"");
      success = 1;
    }
  else if (type && strcmp (type, "basic") == 0 &&
           strcmp (argv[1], "127.0.0.1") == 0)
    {
      if (strcmp (data, "bWU6dGhpcyBpcyB0aGUgcGFzc3dvcmQ=") == 0)
        {
          write_init_message ("\"user\": \"me\"");
          success = 1;
        }
      else if (strcmp (data, "YnJpZGdlLXVzZXI6dGhpcyBpcyB0aGUgcGFzc3dvcmQ=") == 0)
        {
          launch_bridge = 1;
          success = 1;
        }
      else
        {
          write_init_message ("\"problem\": \"authentication-failed\", \"auth-method-results\": { \"password\": \"denied\"}");
        }
    }
  else if (type && strcmp (type, "basic") == 0 &&
           strcmp (argv[1], "machine") == 0)
    {
      if (strcmp (data, "cmVtb3RlLXVzZXI6dGhpcyBpcyB0aGUgbWFjaGluZSBwYXNzd29yZA==") == 0)
        {
          write_init_message ("\"user\": \"remote-user\"");
          success = 1;
        }
      else if (strcmp (data, "YnJpZGdlLXVzZXI6dGhpcyBpcyB0aGUgcGFzc3dvcmQ=") == 0)
        {
          launch_bridge = 1;
          success = 1;
        }
      else
        {
          write_init_message ("\"problem\": \"authentication-failed\", \"auth-method-results\": { \"password\": \"denied\"}");
        }
    }
  else if (type && strcmp (type, "basic") == 0)
    {
      if (strcmp (data, "bWU6dGhpcyBpcyB0aGUgcGFzc3dvcmQ=") == 0)
        {
          write_init_message ("\"user\": \"me\"");
          success = 1;
        }
      else if (strcmp (data, "YnJpZGdlLXVzZXI6dGhpcyBpcyB0aGUgcGFzc3dvcmQ=") == 0)
        {
          launch_bridge = 1;
          success = 1;
        }
      else
        {
          write_init_message ("\"problem\": \"authentication-failed\"");
        }
    }
  else if (strcmp (data, "data-then-success") == 0)
    {
      write_message ("\n{\"command\":\"authorize\",\"challenge\":\"x-login-data\",\"cookie\":\"blah\",\"login-data\":{ \"login\": \"data\"}}");
      write_init_message ("\"user\": \"me\"");
      success = 1;
    }
  else if (strcmp (data, "two-step") == 0)
    {
      write_authorize_challenge ("X-Conversation conv dHlwZSB0d28=");
      free (message);
      free (response);
      message = read_authorize_response ("two-step");
      response = get_authorize_key (message, "response", true);
      data = cockpit_authorize_type (response, NULL);
      if (!data || strcmp (data, "conv dHdv") != 0)
        {
          write_init_message ("\"problem\": \"authentication-failed\"");
        }
      else
        {
          write_init_message ("\"user\": \"me\"");
          success = 1;
        }
    }
  else if (strcmp (data, "three-step") == 0)
    {
      write_authorize_challenge ("X-Conversation conv dHlwZSB0d28=");
      free (message);
      free (response);
      message = read_authorize_response ("three-step");
      response = get_authorize_key (message, "response", true);
      data = cockpit_authorize_type (response, NULL);
      if (!data || strcmp (data, "conv dHdv") != 0)
        {
          write_init_message ("\"problem\": \"authentication-failed\"");
          goto out;
        }

      write_authorize_challenge ("X-Conversation conv dHlwZSB0aHJlZQ==");
      free (message);
      free (response);
      message = read_authorize_response ("three-step");
      response = get_authorize_key (message, "response", true);
      data = cockpit_authorize_type (response, NULL);
      if (!data || strcmp (data, "conv dGhyZWU=") != 0)
        {
          write_init_message ("\"problem\": \"authentication-failed\"");
        }
      else
        {
          write_init_message ("\"user\": \"me\"");
          success = 1;
        }
    }
  else if (strcmp (data, "success-bad-data") == 0)
    {
      write_init_message ("\"user\": \"me\", \"login-data\": \"bad\"");
      success = 1;
    }
  else if (strcmp (data, "no-user") == 0)
    {
      write_init_message ("\"other\":1");
    }
  else if (strcmp (data, "with-error") == 0)
    {
      write_init_message ("\"problem\": \"unknown\", \"message\": \"detail for error\"");
    }
  else if (strcmp (data, "with-error") == 0)
    {
      write_init_message ("\"problem\": \"unknown\", \"message\": \"detail for error\"");
    }
  else if (strcmp (data, "too-slow") == 0)
    {
      sleep (10);
      write_init_message ("\"user\": \"me\", \"login-data\": { \"login\": \"data\"}");
      success = 1;
    }

out:
  free (response);
  free (message);
  if (success)
    {
      if (launch_bridge)
        execlp (BUILDDIR "/cockpit-bridge", BUILDDIR "/cockpit-bridge", NULL);
      else
        execlp ("cat", "cat", NULL);
    }
  exit (PAM_AUTH_ERR);
}
