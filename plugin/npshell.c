/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Stephen Mak <smak@sun.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

/*
 * npshell.c
 *
 * Netscape Client Plugin API
 * - Function that need to be implemented by plugin developers
 *
 * This file defines a "shell" plugin that plugin developers can use
 * as the basis for a real plugin.  This shell just provides empty
 * implementations of all functions that the plugin can implement
 * that will be called by Netscape (the NPP_xxx methods defined in
 * npapi.h).
 *
 * dp Suresh <dp@netscape.com>
 * updated 5/1998 <pollmann@netscape.com>
 * updated 9/2000 <smak@sun.com>
 *
 */


/*
The contents of this file are subject to the Mozilla Public License

Version 1.1 (the "License"); you may not use this file except in compliance
with the License. You may obtain a copy of the License at http://www.mozilla.org/MPL/

Software distributed under the License is distributed on an "AS IS" basis,
WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License for
the specific language governing rights and limitations under the License.

The Original Code is stub code that defines the binary interface to a Mozilla
plugin.

The Initial Developer of the Original Code is Mozilla.

Portions created by Adobe Systems Incorporated are Copyright (C) 2007. All Rights Reserved.

Contributor(s): Adobe Systems Incorporated.
*/


#include <stdio.h>
#include <string.h>

#include <npapi.h>
#include <npupp.h>

#include "gtk-vnc-plugin.h"

/***********************************************************************
 *
 * Implementations of plugin API functions
 *
 ***********************************************************************/

char *
NPP_GetMIMEDescription(void)
{
  return (char *) MIME_TYPES_HANDLED;
}

NPError
NPP_GetValue(NPP instance G_GNUC_UNUSED, NPPVariable variable, void *value)
{
  NPError err = NPERR_NO_ERROR;

  debug ("NPP_GetValue %d", variable);

  switch (variable) {
  case NPPVpluginNameString:
    *((const char **)value) = PLUGIN_NAME;
    break;
  case NPPVpluginDescriptionString:
    *((const char **)value) = PLUGIN_DESCRIPTION;
    break;
  case NPPVpluginNeedsXEmbed:
    *((PRBool *)value) = PR_TRUE;
    break;
  default:
    err = NPERR_GENERIC_ERROR;
  }
  return err;
}

NPError
NPP_Initialize(void)
{
  debug ("NPP_Initialize");

  gtk_init(0, 0);

  return NPERR_NO_ERROR;
}

#ifdef OJI
jref
NPP_GetJavaClass()
{
  return NULL;
}
#endif

void
NPP_Shutdown(void)
{
  debug ("NPP_Shutdown");
}

NPError
NPP_New(NPMIMEType pluginType G_GNUC_UNUSED,
        NPP instance,
        uint16 mode,
        int16 argc,
        char* argn[],
        char* argv[],
        NPSavedData *saved G_GNUC_UNUSED)
{
  PluginInstance *This;
  NPError err = NPERR_NO_ERROR;
  PRBool supportsXEmbed = PR_FALSE;
  NPNToolkitType toolkit = 0;
  int i;
  char *key, *value;

  debug ("NPP_New");

  if (instance == NULL)
    return NPERR_INVALID_INSTANCE_ERROR;

  /* http://developer.mozilla.org/en/docs/XEmbed_Extension_for_Mozilla_Plugins
   * Check for XEmbed and Gtk toolkit.
   */
  err = NPN_GetValue (instance,
                      NPNVSupportsXEmbedBool,
                      (void *)&supportsXEmbed);
  if (err != NPERR_NO_ERROR || supportsXEmbed != PR_TRUE)
    return NPERR_INCOMPATIBLE_VERSION_ERROR;

#if 1
  err = NPN_GetValue (instance,
                      NPNVToolkit,
                      (void *)&toolkit);
  if (err != NPERR_NO_ERROR || toolkit != NPNVGtk2)
    return NPERR_INCOMPATIBLE_VERSION_ERROR;
#endif

  instance->pdata = NPN_MemAlloc(sizeof(PluginInstance));

  This = (PluginInstance*) instance->pdata;

  if (This == NULL) {
    return NPERR_OUT_OF_MEMORY_ERROR;
  }

  memset(This, 0, sizeof(PluginInstance));

  /* Mode is NP_EMBED, NP_FULL, or NP_BACKGROUND (see npapi.h). */
  This->mode = mode;
  This->instance = instance;
  This->host = This->port = NULL;

  /* Read the parameters passed to the plugin. */
  for (i = 0; i < argc; i++)
    {
      key = argn[i];
      value = argv[i];

      if (strcmp (argn[i], "host") == 0)
        This->host = strdup (argv[i]);
      else if (strcmp (argn[i], "port") == 0)
        This->port = strdup (argv[i]);
    }

  return NPERR_NO_ERROR;
}

NPError
NPP_Destroy(NPP instance, NPSavedData** save G_GNUC_UNUSED)
{
  PluginInstance* This;

  debug ("NPP_Destroy");

  if (instance == NULL)
    return NPERR_INVALID_INSTANCE_ERROR;

  This = (PluginInstance*) instance->pdata;

  if (This != NULL)
    {
      (void) GtkVNCDestroyWindow (instance);
      free (This->host);
      free (This->port);
      NPN_MemFree(instance->pdata);
      instance->pdata = NULL;
    }

  return NPERR_NO_ERROR;
}


NPError
NPP_SetWindow(NPP instance, NPWindow* window)
{
  debug ("NPP_SetWindow");

  return GtkVNCXSetWindow(instance, window);
}

int32
NPP_WriteReady(NPP instance, NPStream *stream)
{
  /*printf("NPP_WriteReady()\n");*/
  if (instance == NULL)
    return NPERR_INVALID_INSTANCE_ERROR;

  /* We don't want any data, kill the stream */
  NPN_DestroyStream(instance, stream, NPRES_DONE);

  /* Number of bytes ready to accept in NPP_Write() */
  return -1L;   /* don't accept any bytes in NPP_Write() */
}

int32
NPP_Write(NPP instance, NPStream *stream,
          int32 offset G_GNUC_UNUSED, int32 len G_GNUC_UNUSED,
          void *buffer G_GNUC_UNUSED)
{
  /*printf("NPP_Write()\n");*/
  if (instance == NULL)
    return NPERR_INVALID_INSTANCE_ERROR;

  /* We don't want any data, kill the stream */
  NPN_DestroyStream(instance, stream, NPRES_DONE);

  return -1L;   /* don't accept any bytes in NPP_Write() */
}

NPError
NPP_DestroyStream(NPP instance, NPStream *stream G_GNUC_UNUSED,
                  NPError reason G_GNUC_UNUSED)
{
  /*printf("NPP_DestroyStream()\n");*/
  if (instance == NULL)
    return NPERR_INVALID_INSTANCE_ERROR;

    /***** Insert NPP_DestroyStream code here *****\
    PluginInstance* This;
    This = (PluginInstance*) instance->pdata;
    \**********************************************/

    return NPERR_NO_ERROR;
}

void
NPP_StreamAsFile(NPP instance G_GNUC_UNUSED, NPStream *stream G_GNUC_UNUSED,
                 const char* fname G_GNUC_UNUSED)
{
    /*printf("NPP_StreamAsFile()\n");*/
    /***** Insert NPP_StreamAsFile code here *****\
    PluginInstance* This;
    if (instance != NULL)
        This = (PluginInstance*) instance->pdata;
    \*********************************************/
}

void
NPP_URLNotify(NPP instance G_GNUC_UNUSED, const char* url G_GNUC_UNUSED,
              NPReason reason G_GNUC_UNUSED, void* notifyData G_GNUC_UNUSED)
{
    /*printf("NPP_URLNotify()\n");*/
    /***** Insert NPP_URLNotify code here *****\
    PluginInstance* This;
    if (instance != NULL)
        This = (PluginInstance*) instance->pdata;
    \*********************************************/
}


void
NPP_Print(NPP instance, NPPrint* printInfo)
{
    /*printf("NPP_Print()\n");*/
    if(printInfo == NULL)
        return;

    if (instance != NULL) {
    /***** Insert NPP_Print code here *****\
        PluginInstance* This = (PluginInstance*) instance->pdata;
    \**************************************/

        if (printInfo->mode == NP_FULL) {
            /*
             * PLUGIN DEVELOPERS:
             *  If your plugin would like to take over
             *  printing completely when it is in full-screen mode,
             *  set printInfo->pluginPrinted to TRUE and print your
             *  plugin as you see fit.  If your plugin wants Netscape
             *  to handle printing in this case, set
             *  printInfo->pluginPrinted to FALSE (the default) and
             *  do nothing.  If you do want to handle printing
             *  yourself, printOne is true if the print button
             *  (as opposed to the print menu) was clicked.
             *  On the Macintosh, platformPrint is a THPrint; on
             *  Windows, platformPrint is a structure
             *  (defined in npapi.h) containing the printer name, port,
             *  etc.
             */

    /***** Insert NPP_Print code here *****\
            void* platformPrint =
                printInfo->print.fullPrint.platformPrint;
            NPBool printOne =
                printInfo->print.fullPrint.printOne;
    \**************************************/

            /* Do the default*/
            printInfo->print.fullPrint.pluginPrinted = FALSE;
        }
        else {  /* If not fullscreen, we must be embedded */
            /*
             * PLUGIN DEVELOPERS:
             *  If your plugin is embedded, or is full-screen
             *  but you returned false in pluginPrinted above, NPP_Print
             *  will be called with mode == NP_EMBED.  The NPWindow
             *  in the printInfo gives the location and dimensions of
             *  the embedded plugin on the printed page.  On the
             *  Macintosh, platformPrint is the printer port; on
             *  Windows, platformPrint is the handle to the printing
             *  device context.
             */

    /***** Insert NPP_Print code here *****\
            NPWindow* printWindow =
                &(printInfo->print.embedPrint.window);
            void* platformPrint =
                printInfo->print.embedPrint.platformPrint;
    \**************************************/
        }
    }
}

int16 NPP_HandleEvent(NPP instance, void* event)
{
  /*printf("NPP_HandleEvent()\n");*/

  return GtkVNCXHandleEvent(instance, event);
}
