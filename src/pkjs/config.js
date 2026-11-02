// ==============================================================================
// TIMELINE WX - CLAY CONFIGURATION
// ==============================================================================
module.exports = [
  {
    "type": "heading",
    "defaultValue": "Timeline WX Settings"
  },
  {
    "type": "section",
    "items": [
      {
        "type": "select",
        "messageKey": "WEATHER_PROVIDER",
        "label": "Weather Data Source",
        "defaultValue": "openmeteo",
        "description": "Note Limitations: Open-Meteo (Global, No API Key needed). NWS (US Only, Lacks UV Data). Met.no (Global, Lacks UV Data). OpenWeather (Global, Requires API Key).",
        "options": [
          { "label": "Open-Meteo (Default)", "value": "openmeteo" },
          { "label": "National Weather Service (US)", "value": "nws" },
          { "label": "Yr.no / Met.no", "value": "metno" },
          { "label": "OpenWeather OneCall 3.0", "value": "openweather" }
        ]
      },
      {
        "type": "input",
        "messageKey": "API_KEY",
        "label": "OpenWeather API Key",
        "description": "Only required if OpenWeather is selected above. Your key is saved if you switch providers."
      }
    ]
  },
  {
    "type": "section",
    "items": [
      {
        "type": "select",
        "messageKey": "TEMP_UNIT",
        "label": "Temperature Unit",
        "defaultValue": "F",
        "options": [
          { "label": "Fahrenheit (°F)", "value": "F" },
          { "label": "Celsius (°C)", "value": "C" },
          { "label": "Kelvin (K)", "value": "K" },
          { "label": "Rankine (°R)", "value": "R" }
        ]
      },
      {
        "type": "select",
        "messageKey": "WIND_UNIT",
        "label": "Wind Speed Unit",
        "defaultValue": "mph",
        "options": [
          { "label": "Miles Per Hour (mph)", "value": "mph" },
          { "label": "Kilometers Per Hour (kph)", "value": "kph" },
          { "label": "Meters Per Second (m/s)", "value": "mps" },
          { "label": "Knots", "value": "knots" }
        ]
      },
      {
        "type": "select",
        "messageKey": "TIME_FORMAT",
        "label": "Time Format",
        "defaultValue": "12h",
        "options": [
          { "label": "12 Hour (AM/PM)", "value": "12h" },
          { "label": "24 Hour (Military)", "value": "24h" }
        ]
      }
    ]
  },
  {
    "type": "section",
    "items": [
      {
        "type": "select",
        "messageKey": "DARK_MODE",
        "label": "Dark Mode",
        "defaultValue": "auto",
        "options": [
          { "label": "Off", "value": "off" },
          { "label": "On", "value": "on" },
          { "label": "Auto (Matches day/night)", "value": "auto" }
        ]
      },
      {
        "type": "select",
        "messageKey": "LOCATION_METHOD",
        "label": "Location Method",
        "defaultValue": "gps",
        "options": [
          { "label": "Use Phone GPS", "value": "gps" },
          { "label": "Use Manual Zip Code", "value": "zip" }
        ]
      },
      {
        "type": "input",
        "messageKey": "ZIP_CODE",
        "label": "Zip Code (if manual selected)",
        "attributes": {
          "placeholder": "e.g., 21044,US"
        }
      },
      {
        "type": "slider",
        "messageKey": "REFRESH_INTERVAL",
        "label": "Background Refresh (Minutes)",
        "defaultValue": 30,
        "min": 15,
        "max": 120,
        "step": 15
      }
    ]
  },
  {
    "type": "submit",
    "defaultValue": "Save Settings"
  }
];