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
if (typeof process !== 'undefined' && process.env) {
  process.env.MDXWEB_SKIP_VIS = process.env.MDXWEB_SKIP_VIS || '1';
  process.env.MDXWEB_NO_UW = process.env.MDXWEB_NO_UW || '1';
}
if (typeof process !== 'undefined' && process.env && !process.env.ASAN_OPTIONS) {
  process.env.ASAN_OPTIONS = 'halt_on_error=0:exitcode=0';
}
Module.arguments = Module.arguments || [];
Module.printErr = Module.printErr || function (text) { console.warn(text); };
