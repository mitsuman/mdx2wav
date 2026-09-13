// Runs before the Emscripten runtime initializes.
//
// This file is also evaluated in pthread workers, where `document` does not
// exist, so the canvas lookup has to be guarded.
if (typeof Module === 'undefined') {
  var Module = {};
}
if (typeof document !== 'undefined' && typeof document.getElementById === 'function') {
  Module.canvas = document.getElementById('screen');
}
Module.noInitialRun = true;

Module.arguments = Module.arguments || [];
Module.printErr = Module.printErr || function (text) { console.warn(text); };
