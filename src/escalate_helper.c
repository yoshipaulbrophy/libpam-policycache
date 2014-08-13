/**
 * Copyright 2014 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "escalate_helper.h"
#include "escalate_message.h"

#include <pwd.h>
#include <security/pam_ext.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>


EscalateHelper *EscalateHelperNew(int stdin_fd, int stdout_fd) {
  EscalateHelper *self = g_new0(EscalateHelper, 1);
  self->reader = g_io_channel_unix_new(stdin_fd);
  self->writer = g_io_channel_unix_new(stdout_fd);
  self->conv.conv = EscalateHelperConversation;
  self->conv.appdata_ptr = self;
  self->result = PAM_SYSTEM_ERR;
  return self;
}


void EscalateHelperFree(EscalateHelper *self) {
  if (self->pamh) {
    int pam_status = pam_end(self->pamh, self->result);
    if (pam_status != PAM_SUCCESS) {
      g_error("pam_end returned unexpected code %d: %s", pam_status,
              pam_strerror(self->pamh, pam_status));
    }
  }
  g_io_channel_unref(self->reader);
  g_io_channel_unref(self->writer);
  g_free(self->username);
  g_free(self);
}


static gboolean EscalateHelperIsSafeItem(int item_type) {
  switch (item_type) {
    case PAM_TTY:
    case PAM_RUSER:
    case PAM_RHOST:
    case PAM_XDISPLAY:
    case PAM_AUTHTOK_TYPE:
      return TRUE;
    default:
      return FALSE;
  }
}


static EscalateMessage *EscalateHelperRecv(EscalateHelper *self,
                                           EscalateMessageType type,
                                           GError **error) {
  EscalateMessage *message = EscalateMessageRead(self->reader, error);
  if (!message)
    return NULL;

  if (EscalateMessageGetType(message) == type) {
    return message;
  } else {
    g_set_error(error, ESCALATE_HELPER_ERROR,
                ESCALATE_HELPER_ERROR_UNEXPECTED_MESSAGE_TYPE,
                "Expected message type %d but got %d instead", type,
                EscalateMessageGetType(message));
    EscalateMessageUnref(message);
    return NULL;
  }
}


static gboolean EscalateHelperCheckUsername(EscalateHelper *self,
                                            GError **error) {
  struct passwd *user = NULL;
  g_assert(self->username);

  if (getuid() == 0)
    return TRUE;

  user = getpwnam(self->username);
  if (!user) {
    g_set_error(error, ESCALATE_HELPER_ERROR,
                ESCALATE_HELPER_ERROR_UNKNOWN_USERNAME,
                "Failed to find uid for user '%s'", self->username);
    return FALSE;
  }

  if (user->pw_uid != getuid()) {
    g_set_error(error, ESCALATE_HELPER_ERROR,
                ESCALATE_HELPER_ERROR_PRIVILEGE_ERROR,
                "Can't use escalate for user '%s' (uid=%d) when running as"
                " another user (uid=%d)", self->username, user->pw_uid,
                getuid());
    return FALSE;
  }

  return TRUE;
}


gboolean EscalateHelperHandleStart(EscalateHelper *self, GError **error) {
  EscalateMessage *message = NULL;
  GVariantIter *items = NULL;
  int pam_status = PAM_SYSTEM_ERR;
  int item_type = -1;
  const gchar *item_value = NULL;
  gboolean result = FALSE;

  message = EscalateHelperRecv(self, ESCALATE_MESSAGE_TYPE_START, error);
  if (!message)
    goto done;

  EscalateMessageGetValues(message, &self->action, &self->flags,
                           &self->username, &items);

  if (!EscalateHelperCheckUsername(self, error))
    goto done;

  pam_status = pam_start(ESCALATE_SERVICE_NAME, self->username, &self->conv,
                         &self->pamh);
  if (pam_status != PAM_SUCCESS) {
    g_set_error(error, ESCALATE_HELPER_ERROR,
                ESCALATE_HELPER_ERROR_START_FAILED,
                "Failed to start PAM session: %s",
                pam_strerror(self->pamh, pam_status));
    goto done;
  }

  while (g_variant_iter_loop(items, "{im&s}", &item_type, &item_value)) {
    if (!EscalateHelperIsSafeItem(item_type)) {
      g_set_error(error, ESCALATE_HELPER_ERROR,
                  ESCALATE_HELPER_ERROR_UNSUPPORTED_ITEM,
                  "Item type %d is not supported", item_type);
      goto done;
    }
    pam_status = pam_set_item(self->pamh, item_type, item_value);
    if (pam_status != PAM_SUCCESS) {
      g_set_error(error, ESCALATE_HELPER_ERROR,
                  ESCALATE_HELPER_ERROR_SET_ITEM_FAILED,
                  "Failed to set item type %d to '%s'", item_type, item_value);
      goto done;
    }
  }

  result = TRUE;

done:
  if (items)
    g_variant_iter_free(items);
  if (message)
    EscalateMessageUnref(message);
  return result;
}


gboolean EscalateHelperDoAction(EscalateHelper *self, GError **error) {
  EscalateMessage *message = NULL;

  switch (self->action) {
    case ESCALATE_MESSAGE_ACTION_AUTHENTICATE:
      self->result = pam_authenticate(self->pamh, self->flags);
      break;
    default:
      g_error("Unsupported action %d", self->action);
  }

  message = EscalateMessageNew(ESCALATE_MESSAGE_TYPE_FINISH, self->result);
  return EscalateMessageWrite(message, self->writer, error);
}


static int EscalateHelperPrompt(EscalateHelper *self,
                                const struct pam_message *conv_request,
                                struct pam_response *conv_response) {
  EscalateMessage *request = NULL;
  GError *error = NULL;
  EscalateMessage *response = NULL;
  gchar *response_msg = NULL;
  int response_retcode = 0;
  int result = PAM_CONV_ERR;

  request = EscalateMessageNew(ESCALATE_MESSAGE_TYPE_CONV_MESSAGE,
                               conv_request->msg_style, conv_request->msg);

  if (!EscalateMessageWrite(request, self->writer, &error)) {
    pam_syslog(self->pamh, LOG_WARNING,
               "Failed to write conversation request: %s", error->message);
    g_clear_error(&error);
    goto done;
  }

  response = EscalateHelperRecv(self, ESCALATE_MESSAGE_TYPE_CONV_RESPONSE,
                                &error);
  if (!response) {
    pam_syslog(self->pamh, LOG_WARNING,
               "Failed to read conversation response: %s", error->message);
    g_clear_error(&error);
    goto done;
  }

  EscalateMessageGetValues(response, &response_msg, &response_retcode);

  conv_response->resp = strdup(response_msg);
  conv_response->resp_retcode = response_retcode;
  result = PAM_SUCCESS;

done:
  if (request)
    EscalateMessageUnref(request);
  if (response)
    EscalateMessageUnref(response);
  g_free(response_msg);
  return result;
}


int EscalateHelperConversation(int conv_len,
                               const struct pam_message **conv_requests,
                               struct pam_response **conv_responses,
                               void *user_data) {
  EscalateHelper *self = (EscalateHelper *) user_data;
  struct pam_response *tmp_conv_responses = NULL;
  int result = PAM_SUCCESS;

  if (conv_len == 0) {
    pam_syslog(self->pamh, LOG_WARNING,
               "Conversation function called with no messages");
    return PAM_CONV_ERR;
  }

  tmp_conv_responses = malloc(sizeof(struct pam_response) * conv_len);
  g_assert(tmp_conv_responses);
  memset(tmp_conv_responses, 0, sizeof(struct pam_response) * conv_len);

  for (guint i = 0; i < conv_len && result == PAM_SUCCESS; i++) {
    g_assert(conv_requests[i]);
    result = EscalateHelperPrompt(self, conv_requests[i],
                                  &tmp_conv_responses[i]);
  }

  if (result == PAM_SUCCESS) {
    *conv_responses = tmp_conv_responses;
  } else {
    for (guint i = 0; i < conv_len; i++) {
      if (tmp_conv_responses[i].resp) {
        free(tmp_conv_responses[i].resp);
      }
    }
    free(tmp_conv_responses);
  }

  return result;
}


int main(int argc, char **argv) {
  GError *error = NULL;
  GOptionContext *context = NULL;
  EscalateHelper *helper = NULL;
  int exit_code = 2;

  clearenv();
  umask(077);

  context = g_option_context_new("- helper for pam_escalate.so");
  if (!g_option_context_parse(context, &argc, &argv, &error)) {
    goto done;
  }

  if (argc > 1) {
    g_set_error(&error, ESCALATE_HELPER_ERROR, ESCALATE_HELPER_ERROR_EXTRA_ARGS,
                "Non-flag arguments are not accepted");
    goto done;
  }

  helper = EscalateHelperNew(STDIN_FILENO, STDOUT_FILENO);

  if (!EscalateHelperHandleStart(helper, &error)) {
    goto done;
  }

  if (EscalateHelperDoAction(helper, &error)) {
    exit_code = 0;
  } else {
    exit_code = 1;
  }

done:
  if (error) {
    g_printerr("Caught error: %s\n", error->message);
    g_error_free(error);
  }
  EscalateHelperFree(helper);
  return exit_code;
}


GQuark _EscalateHelperErrorQuark() {
  return g_quark_from_string("escalate-helper-error-quark");
}