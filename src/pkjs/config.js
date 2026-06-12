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
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Watch Face"
      },
      {
        "type": "color",
        "messageKey": "BgFillColor",
        "label": "Background color",
        "description": "Fill color of the watch face area outside the Tama.",
        "defaultValue": "AAAAAA",
        "sunlight": true,
        "allowGray": true
      },
      {
        "type": "color",
        "messageKey": "BgMarkersColor",
        "label": "Hour markers color",
        "description": "Color of the hour markers around the edge of the watchface.",
        "defaultValue": "000000",
        "sunlight": true,
        "allowGray": true
      },
      {
        "type": "select",
        "messageKey": "BgMarkersStyle",
        "label": "Hour markers style",
        "description": "Shape of the markers around the watchface edge.",
        "defaultValue": "0",
        "options": [
          { "label": "Arabic numerals (12, 1, 2...)", "value": "0" },
          { "label": "Roman numerals (XII, I, II...)", "value": "1" },
          { "label": "Ticks (dashes)", "value": "2" }
        ]
      }
    ]
  },

  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Hands"
      },
      {
        "type": "color",
        "messageKey": "HandsColor",
        "label": "Hands color",
        "description": "Inner color of the analog hour and minute hands.",
        "defaultValue": "000000",
        "sunlight": true,
        "allowGray": true
      },
      {
        "type": "color",
        "messageKey": "HandsOutlineColor",
        "label": "Hands outline color",
        "description": "Outline drawn around the hands so they stay visible over any background.",
        "defaultValue": "FFFFFF",
        "sunlight": true,
        "allowGray": true
      },
      {
        "type": "select",
        "messageKey": "HandsThickness",
        "label": "Hands thickness",
        "description": "Stroke width of the analog hands.",
        "defaultValue": "1",
        "options": [
          { "label": "Thick",  "value": "0" },
          { "label": "Medium", "value": "1" },
          { "label": "Thin",   "value": "2" }
        ]
      }
    ]
  },

  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Text"
      },
      {
        "type": "color",
        "messageKey": "TextColor",
        "label": "Text color",
        "description": "Color of the digital time, date, and battery text.",
        "defaultValue": "000000",
        "sunlight": true,
        "allowGray": true
      },
      {
        "type": "toggle",
        "messageKey": "TextOutline",
        "label": "Text outline",
        "description": "Draw a contrasting outline around the time text so it stays readable over any background.",
        "defaultValue": false
      },
      {
        "type": "color",
        "messageKey": "TextOutlineColor",
        "label": "Text outline color",
        "description": "Color of the text outline (when enabled).",
        "defaultValue": "FFFFFF",
        "sunlight": true,
        "allowGray": true
      }
    ]
  },

  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Time & Date"
      },
      {
        "type": "select",
        "messageKey": "TimeFormat",
        "label": "Time format",
        "description": "How the digital time is displayed.",
        "defaultValue": "0",
        "options": [
          { "label": "System default", "value": "0" },
          { "label": "24-hour (14:30)", "value": "1" },
          { "label": "12-hour (2:30)",  "value": "2" }
        ]
      },
      {
        "type": "select",
        "messageKey": "DateFormat",
        "label": "Date format",
        "description": "How the date is displayed.",
        "defaultValue": "0",
        "options": [
          { "label": "European (Fri 12 Jun)", "value": "0" },
          { "label": "American (Fri Jun 12)", "value": "1" },
          { "label": "ISO (2026-06-12)",      "value": "2" }
        ]
      }
    ]
  },

  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Reset"
      },
      {
        "type": "text",
        "defaultValue": "Toggle this ON and hit <b>Save</b> to wipe the Tama's EEPROM and start fresh with a new egg. The toggle resets to OFF automatically after the wipe — flip it again the next time you want to reset."
      },
      {
        "type": "toggle",
        "messageKey": "ResetTama",
        "label": "Reset Tamagotchi",
        "description": "Wipes the saved Tama state. You'll start with a fresh egg next time the app launches.",
        "defaultValue": false
      }
    ]
  },

  {
    "type": "submit",
    "defaultValue": "Save Settings"
  }
];
