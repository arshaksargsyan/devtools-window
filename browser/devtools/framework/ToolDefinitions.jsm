/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

this.EXPORTED_SYMBOLS = [ "defaultTools" ];

Components.utils.import("resource:///modules/devtools/InspectorDefinition.jsm");
const debuggerProps = "chrome://browser/locale/devtools/debugger.properties";
const styleEditorProps = "chrome://browser/locale/devtools/styleeditor.properties";
const webConsoleProps = "chrome://browser/locale/devtools/webconsole.properties";
// Panels
XPCOMUtils.defineLazyModuleGetter(this, "WebConsolePanel",
  "resource:///modules/WebConsolePanel.jsm");

XPCOMUtils.defineLazyModuleGetter(this, "DebuggerPanel",
  "resource:///modules/devtools/DebuggerPanel.jsm");

XPCOMUtils.defineLazyModuleGetter(this, "StyleEditorPanel",
  "resource:///modules/devtools/StyleEditorPanel.jsm");

// Strings
XPCOMUtils.defineLazyGetter(this, "webConsoleStrings",
  function() Services.strings.createBundle(webConsoleProps));

XPCOMUtils.defineLazyGetter(this, "debuggerStrings",
  function() Services.strings.createBundle(debuggerProps));

XPCOMUtils.defineLazyGetter(this, "styleEditorStrings",
  function() Services.strings.createBundle(styleEditorProps));

// Definitions
let webConsoleDefinition = {
  id: "webconsole",
  key: l10n("cmd.commandkey", webConsoleStrings),
  accesskey: l10n("webConsoleCmd.accesskey", webConsoleStrings),
  modifiers: Services.appinfo.OS == "Darwin" ? "accel,alt" : "accel,shift",
  ordinal: 0,
  icon: "chrome://browser/skin/devtools/webconsole-tool-icon.png",
  url: "chrome://browser/content/devtools/webconsole.xul",
  label: l10n("ToolboxWebconsole.label", webConsoleStrings),
  isTargetSupported: function(target) {
    return true;
  },
  build: function(iframeWindow, toolbox) {
    return new WebConsolePanel(iframeWindow, toolbox);
  }
};

let debuggerDefinition = {
  id: "jsdebugger",
  key: l10n("open.commandkey", debuggerStrings),
  accesskey: l10n("debuggerMenu.accesskey", debuggerStrings),
  modifiers: osString == "Darwin" ? "accel,alt" : "accel,shift",
  ordinal: 1,
  killswitch: "devtools.debugger.enabled",
  icon: "chrome://browser/skin/devtools/tools-icons-small.png",
  url: "chrome://browser/content/debugger.xul",
  label: l10n("ToolboxDebugger.label", debuggerStrings),

  isTargetSupported: function(target) {
    return true;
  },

  build: function(iframeWindow, toolbox) {
    return new DebuggerPanel(iframeWindow, toolbox);
  }
};


let styleEditorDefinition = {
  id: "styleeditor",
  key: l10n("open.commandkey", styleEditorStrings),
  ordinal: 3,
  accesskey: l10n("open.accesskey", styleEditorStrings),
  modifiers: "shift",
  label: l10n("ToolboxStyleEditor.label", styleEditorStrings),
  url: "chrome://browser/content/styleeditor.xul",

  isTargetSupported: function(target) {
    return !target.isRemote && !target.isChrome;
  },

  build: function(iframeWindow, toolbox) {
    return new StyleEditorPanel(iframeWindow, toolbox);
  }
};

this.defaultTools = [
  styleEditorDefinition,
  InspectorDefinition,
  webConsoleDefinition,
  debuggerDefinition,
];
