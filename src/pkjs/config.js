module.exports = [
  {
    "type": "heading",
    "defaultValue": "TamaGo Settings"
  },
  {
    "type": "text",
    "defaultValue": "Tama-Go emulator watchface for Pebble Time 2. Step A settings — more options will be added soon."
  },

  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Notifications"
      },
      {
        "type": "toggle",
        "messageKey": "VibrationEnabled",
        "label": "Vibrate on attention",
        "description": "Watch vibrates once when the Tama needs attention (hungry, sick, ...).",
        "defaultValue": true
      }
    ]
  },

  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Tama Display"
      },
      {
        "type": "color",
        "messageKey": "TamaPixelColor",
        "label": "Tama pixel color",
        "description": "Color of the darkest LCD pixels. The 3 lighter shades are derived automatically by mixing toward white.",
        "defaultValue": "000000",
        "sunlight": true,
        "allowGray": true
      },
      {
        "type": "toggle",
        "messageKey": "TamaFrameEnabled",
        "label": "Show device frame",
        "description": "Draw a rounded panel behind the Tama when an icon is selected. Off = LCD floats freely on the watchface.",
        "defaultValue": true
      },
      {
        "type": "color",
        "messageKey": "TamaFrameColor",
        "label": "Frame color",
        "description": "Background color of the device frame around the Tama.",
        "defaultValue": "FFFFFF",
        "sunlight": true,
        "allowGray": true
      }
    ]
  },

  {
    "type": "submit",
    "defaultValue": "Save Settings"
  }
];
