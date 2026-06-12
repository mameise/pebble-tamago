// PebbleKit JS bridge for TamaGo. Handles the Clay settings page and
// sends the chosen values down to the watchface as an AppMessage when
// the user hits "Save Settings".

var Clay      = require('@rebble/clay');
var clayConfig = require('./config');
var clay       = new Clay(clayConfig);

// No extra ready / configChanged handlers are needed — Clay handles the
// full configuration round-trip (open page → collect values → send via
// AppMessage) by default. The C side just listens for the message keys
// declared in package.json and applies them on receipt.

Pebble.addEventListener('ready', function () {
  console.log('TamaGo PebbleKit JS ready');
});
