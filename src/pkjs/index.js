// ==============================================================================
// DEPENDENCIES & CONFIGURATION
// ==============================================================================
var Clay = require('pebble-clay');            // Include Clay framework for settings UI
var clayConfig = require('./config');         // Load settings page structure
var keys = require('message_keys');           // Import CloudPebble message key indices
var clay = new Clay(clayConfig);              // Initialize settings

// ==============================================================================
// CORE DATA FETCHING FUNCTION
// ==============================================================================
var refreshTimer = null;
var retryTimer = null;
var retryCount = 0;
var REFRESH_MS = 30 * 60 * 1000;
var MAX_RETRIES = 3;
var requestInProgress = false;

function scheduleRetry() {
  requestInProgress = false;
  if (retryCount >= MAX_RETRIES) return;
  retryCount++;
  if (retryTimer) clearTimeout(retryTimer);
  retryTimer = setTimeout(fetchWeather, Math.min(60000 * Math.pow(2, retryCount - 1), 240000));
}

function sendCachedWeather() {
  try {
    var cache = JSON.parse(localStorage.getItem('lastForecast'));
    if (cache && cache.savedAt && Date.now() - cache.savedAt < 6 * 3600000) {
      var settings = JSON.parse(localStorage.getItem('clay-settings')) || {};
      var selectedUnits = settings[keys.UNITS] || 'imperial';
      var selectedWind = settings[keys.WIND_UNIT] || settings['WIND_UNIT'] || (selectedUnits === 'metric' ? 'kmh' : 'mph');
      var selectedTime = settings[keys.TIME_FORMAT] || '12h';
      if (cache.chunk1[keys.UNITS] !== selectedUnits ||
          cache.chunk1[keys.WIND_UNIT] !== selectedWind ||
          cache.timeFormat !== selectedTime) return false;
      sendWeatherChunks(cache.chunk1, cache.chunk2);
      return true;
    }
  } catch (e) { console.log('Invalid forecast cache: ' + e); }
  return false;
}

function fetchWeather() {
  if (requestInProgress) return;
  requestInProgress = true;
  // 1. Retrieve user preferences from local phone storage
  var settings = {};
  try { settings = JSON.parse(localStorage.getItem('clay-settings')) || {}; } catch (e) { console.log('Invalid settings'); }
  var apiKey = settings[keys.API_KEY] || settings['API_KEY'];
  var unitType = settings[keys.UNITS] || 'imperial';
  var timeFormat = settings[keys.TIME_FORMAT] || '12h';
  // Wind speed is independent of the temperature unit preference.
  var windUnit = settings[keys.WIND_UNIT] || settings['WIND_UNIT'] || (unitType === 'metric' ? 'kmh' : 'mph');
  if (['mph', 'kmh', 'ms', 'knots'].indexOf(windUnit) < 0) windUnit = 'mph';

  if (!apiKey) { console.log('OpenWeather API key missing'); requestInProgress = false; return; }

  // 2. Request current GPS coordinates from the phone OS
  navigator.geolocation.getCurrentPosition(function(pos) {
    var lat = pos.coords.latitude;
    var lon = pos.coords.longitude;

    // Build the OpenWeather 3.0 One Call URL
    var url = 'https://api.openweathermap.org/data/3.0/onecall?lat=' + 
              lat + '&lon=' + lon + '&exclude=minutely,daily&units=' + unitType + '&appid=' + apiKey;

    var xhr = new XMLHttpRequest();
    xhr.onload = function () {
      if (this.status === 200) {
        var json;
        try { json = JSON.parse(this.responseText); } catch (e) { console.log('Invalid weather JSON'); scheduleRetry(); return; }
        var tempSuffix = (unitType === 'metric') ? "C" : "°";
        if (!json.hourly || json.hourly.length < 24) { console.log('Incomplete hourly forecast'); scheduleRetry(); return; }
        
        // Prepare two separate dictionaries to prevent Bluetooth MTU overflow
        var chunk1 = {};
        var chunk2 = {};
        chunk1[keys.UNITS] = unitType;
        chunk1[keys.WIND_UNIT] = windUnit;

        // 3. Process the next 24 hours of forecast data
        for(var i = 0; i < 24; i++) {
          var h = json.hourly[i];
          var date = new Date(h.dt * 1000);
          var hours = date.getHours();
          var timeStr = "";

          // Time formatting logic (12h vs 24h)
          if (timeFormat === '12h') {
            var ampm = hours >= 12 ? 'p' : 'a';
            hours = hours % 12 || 12;
            timeStr = hours + ampm;
          } else {
            timeStr = (hours < 10 ? '0' + hours : hours).toString();
          }

          // Determine which transmission chunk this hour belongs to
          var target = (i < 12) ? chunk1 : chunk2;
          
          // Populate the specific key-value pairs utilizing index offsets
          target[keys.COLUMN_TIME + i] = timeStr;
          target[keys.COLUMN_TEMP + i] = Math.round(h.temp) + tempSuffix;
          // OpenWeather reports wind in mph for imperial and m/s for metric.
          var windMs = unitType === 'metric' ? h.wind_speed : h.wind_speed * 0.44704;
          var convertedWind = windUnit === 'ms' ? windMs :
                              windUnit === 'kmh' ? windMs * 3.6 :
                              windUnit === 'knots' ? windMs * 1.94384449 : windMs * 2.23693629;
          target[keys.COLUMN_WIND + i] = Math.round(convertedWind).toString();
          target[keys.COLUMN_RAIN + i] = Math.round(h.pop * 100) + "%";
          target[keys.COLUMN_UV + i]   = Math.round(h.uvi);
          
          // Index 0 accesses the primary weather condition array block
          target[keys.COLUMN_COND + i] = h.weather[0].id;
          target[keys.IS_NIGHT + i]    = h.weather[0].icon.includes('n') ? 1 : 0;
        }

        // ==============================================================================
        // REVERSE GEOCODING & FALLBACK LOGIC
        // ==============================================================================
        
        // Primary fallback: Extract city from generic timezone string (e.g., "America/New_York")
        var fallbackName = "GPS Fix";
        if (json.timezone) {
          var tzParts = json.timezone.split('/');
          if (tzParts.length > 1) {
            fallbackName = tzParts[1].replace('_', ' '); // Converts "New_York" to "New York"
          }
        }

        // Attempt secondary API call for precise local city name
        var geoUrl = 'https://api.openweathermap.org/geo/1.0/reverse?lat=' + lat + '&lon=' + lon + '&limit=1&appid=' + apiKey;
        var geoXhr = new XMLHttpRequest();
        
        geoXhr.onload = function() {
          var locationName = fallbackName;
          if (this.status === 200) {
            var geoJson = JSON.parse(this.responseText);
            if (geoJson && geoJson.length > 0 && geoJson[0].name) {
              locationName = geoJson[0].name; // Successfully pulled true city name
            }
          }
          
          // Insert location into the first chunk and initiate transfer
          chunk1[keys.LOCATION_NAME] = locationName;
          completeForecast(chunk1, chunk2, timeFormat);
        };

        // If the secondary API drops or times out, immediately push the fallback to avoid a stuck UI
        geoXhr.onerror = function() {
          chunk1[keys.LOCATION_NAME] = fallbackName;
          completeForecast(chunk1, chunk2);
        };

        geoXhr.ontimeout = geoXhr.onerror;
        geoXhr.timeout = 10000;
        geoXhr.open('GET', geoUrl);
        geoXhr.send();
      } else { console.log('Weather HTTP ' + this.status); scheduleRetry(); }
    };
    xhr.onerror = xhr.ontimeout = function() { console.log('Weather network error'); scheduleRetry(); };
    xhr.timeout = 15000;
    xhr.open('GET', url);
    xhr.send();
  }, function(error) { console.log('GPS error: ' + error.message); requestInProgress = false; sendCachedWeather(); scheduleRetry(); }, {timeout: 12000, maximumAge: 300000});
}

// ==============================================================================
// TRANSMISSION CHAIN
// ==============================================================================

// Ensures the first 12 hours complete transmission before triggering the remaining 12
function completeForecast(c1, c2, timeFormat) {
  requestInProgress = false;
  retryCount = 0;
  localStorage.setItem('lastForecast', JSON.stringify({chunk1:c1, chunk2:c2, timeFormat:timeFormat, savedAt:Date.now()}));
  sendWeatherChunks(c1, c2);
}

function sendWeatherChunks(c1, c2, attempt) {
  attempt = attempt || 0;
  Pebble.sendAppMessage(c1, function() {
    Pebble.sendAppMessage(c2, function() { console.log('Forecast delivered'); }, function(e) {
      console.log('Second chunk failed: ' + JSON.stringify(e));
      if (attempt < 2) setTimeout(function() { sendWeatherChunks(c1, c2, attempt + 1); }, 1500 * (attempt + 1));
    });
  }, function(e) {
    console.log('First chunk failed: ' + JSON.stringify(e));
    if (attempt < 2) setTimeout(function() { sendWeatherChunks(c1, c2, attempt + 1); }, 1500 * (attempt + 1));
  });
}

// ==============================================================================
// LIFECYCLE LISTENERS
// ==============================================================================

// Triggers immediately when the watch app boots up
Pebble.addEventListener('ready', function() {
  sendCachedWeather();
  fetchWeather();
  refreshTimer = setInterval(fetchWeather, REFRESH_MS);
});
Pebble.addEventListener('appmessage', function(e) {
  if (e.payload && e.payload[keys.REFRESH_REQUEST]) fetchWeather();
});

// Triggers when the user closes the Clay Settings menu on their phone
Pebble.addEventListener('webviewclosed', function(e) {
  if (e && e.response) { retryCount = 0; fetchWeather(); } // Refresh data with new user settings
});