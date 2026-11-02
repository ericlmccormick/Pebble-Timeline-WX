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
  if (retryCount >= MAX_RETRIES) { showStatus('Update failed - old data'); return; }
  retryCount++;
  if (retryTimer) clearTimeout(retryTimer);
  retryTimer = setTimeout(fetchWeather, Math.min(60000 * Math.pow(2, retryCount - 1), 240000));
}

function showStatus(message) {
  var payload = {};
  payload[keys.STATUS_TEXT] = message;
  Pebble.sendAppMessage(payload, null, function(e) { console.log('Status delivery failed'); });
}
function currentSettings() {
  try { return JSON.parse(localStorage.getItem('clay-settings')) || {}; } catch(e) { return {}; }
}
function windPreference(settings, units) {
  return settings[keys.WIND_UNIT] || settings['WIND_UNIT'] || (units === 'metric' ? 'kmh' : 'mph');
}
function sendCachedWeather() {
  try {
    var cache = JSON.parse(localStorage.getItem('lastForecast'));
    if (!cache || !cache.hours || !cache.savedAt) return false;
    var settings = currentSettings(), units = settings[keys.UNITS] || 'imperial';
    var wind = windPreference(settings, units), time = settings[keys.TIME_FORMAT] || '12h';
    if (cache.units !== units || cache.windUnit !== wind || cache.timeFormat !== time) return false;
    var locMode = settings[keys.LOCATION_MODE] || 'gps';
    var locQuery = (settings[keys.LOCATION_QUERY] || '').trim();
    if ((cache.locationMode || 'gps') !== locMode || (cache.locationQuery || '') !== locQuery) return false;
    var hours = cache.hours.filter(function(h) { return h.dt >= Math.floor(Date.now()/1000) - 3600; });
    if (!hours.length) { showStatus('Forecast expired'); return false; }
    transmitHours(hours, cache.location, cache.savedAt, true, cache.gustsEnabled);
    return true;
  } catch(e) { console.log('Cache invalid: ' + e); return false; }
}
function transmitHours(hours, location, savedAt, cached, gustsEnabled) {
  var c1 = {}, c2 = {};
  var settings = currentSettings(), units = settings[keys.UNITS] || 'imperial';
  var wind = windPreference(settings, units);
  c1[keys.COLOR_MODE] = settings[keys.COLOR_MODE] || 'auto';
  c1[keys.UNITS] = units;
  c1[keys.WIND_UNIT] = wind;
  c1[keys.FORECAST_TIMESTAMP] = Math.floor(savedAt / 1000);
  c1[keys.FORECAST_CACHED] = cached ? 1 : 0;
  c1[keys.LOCATION_NAME] = location || 'Current location';
  c1[keys.GUSTS_ENABLED] = gustsEnabled ? 1 : 0;
  c1[keys.FORECAST_COUNT] = Math.min(hours.length, 24);
  var count = Math.min(hours.length, 24);
  for (var i = 0; i < count; i++) {
    var h = hours[i], target = i < 12 ? c1 : c2;
    target[keys.COLUMN_TIME+i] = h.time;
    target[keys.COLUMN_TEMP+i] = h.temp;
    target[keys.COLUMN_WIND+i] = h.wind;
    target[keys.COLUMN_RAIN+i] = h.rain;
    target[keys.COLUMN_UV+i] = h.uv;
    target[keys.COLUMN_COND+i] = h.cond;
    target[keys.IS_NIGHT+i] = h.night;
    target[keys.COLUMN_GUST+i] = h.gust;
  }
  // A complete forecast is published only after both parts arrive.
  c1[keys.TRANSFER_PART] = 1;
  c2[keys.TRANSFER_PART] = 2;
  c2[keys.TRANSFER_COUNT] = count;
  sendWeatherChunks(c1, c2);
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

  if (!apiKey) { requestInProgress = false; showStatus('Set API key in settings'); return; }

  // 2. Request current GPS coordinates from the phone OS
  function weatherAt(lat, lon, manualLabel) {

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
        var hoursData = [];
        var gustsEnabled = settings[keys.SHOW_GUSTS] === true || settings[keys.SHOW_GUSTS] === 'true' || settings[keys.SHOW_GUSTS] === 1;

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
          var target = {};
          
          // Populate the specific key-value pairs utilizing index offsets
          target[keys.COLUMN_TIME + i] = timeStr;
          target[keys.COLUMN_TEMP + i] = Math.round(h.temp) + tempSuffix;
          // OpenWeather reports wind in mph for imperial and m/s for metric.
          var windMs = unitType === 'metric' ? h.wind_speed : h.wind_speed * 0.44704;
          var convertedWind = windUnit === 'ms' ? windMs :
                              windUnit === 'kmh' ? windMs * 3.6 :
                              windUnit === 'knots' ? windMs * 1.94384449 : windMs * 2.23693629;
          target[keys.COLUMN_WIND + i] = Math.round(convertedWind).toString();
          var gustMs = unitType === 'metric' ? (h.wind_gust || h.wind_speed) : (h.wind_gust || h.wind_speed) * 0.44704;
          var gust = windUnit === 'ms' ? gustMs : windUnit === 'kmh' ? gustMs * 3.6 : windUnit === 'knots' ? gustMs * 1.94384449 : gustMs * 2.23693629;
          target[keys.COLUMN_GUST + i] = Math.round(gust).toString();
          target[keys.COLUMN_RAIN + i] = Math.round(h.pop * 100) + "%";
          target[keys.COLUMN_UV + i]   = Math.round(h.uvi);
          
          // Index 0 accesses the primary weather condition array block
          target[keys.COLUMN_COND + i] = h.weather[0].id;
          target[keys.IS_NIGHT + i]    = h.weather[0].icon.includes('n') ? 1 : 0;
          hoursData.push({dt:h.dt,time:target[keys.COLUMN_TIME+i],temp:target[keys.COLUMN_TEMP+i],wind:target[keys.COLUMN_WIND+i],gust:target[keys.COLUMN_GUST+i],rain:target[keys.COLUMN_RAIN+i],uv:target[keys.COLUMN_UV+i],cond:target[keys.COLUMN_COND+i],night:target[keys.IS_NIGHT+i]});
        }

        // ==============================================================================
        // REVERSE GEOCODING & FALLBACK LOGIC
        // ==============================================================================
        
        // Primary fallback: Extract city from generic timezone string (e.g., "America/New_York")
        var fallbackName = manualLabel || "GPS Fix";
        if (json.timezone) {
          var tzParts = json.timezone.split('/');
          if (tzParts.length > 1) {
            fallbackName = tzParts[1].replace('_', ' '); // Converts "New_York" to "New York"
          }
        }

        if (manualLabel) { completeForecast(hoursData, manualLabel, timeFormat, unitType, windUnit, gustsEnabled); return; }
        // Attempt secondary API call for precise local city name
        var geoUrl = 'https://api.openweathermap.org/geo/1.0/reverse?lat=' + lat + '&lon=' + lon + '&limit=1&appid=' + apiKey;
        var geoXhr = new XMLHttpRequest();
        
        geoXhr.onload = function() {
          var locationName = fallbackName;
          if (this.status === 200) {
            var geoJson;
            try { geoJson = JSON.parse(this.responseText); } catch(e) { geoJson = []; }
            if (geoJson && geoJson.length > 0 && geoJson[0].name) {
              locationName = geoJson[0].name; // Successfully pulled true city name
            }
          }
          
          // Insert location into the first chunk and initiate transfer
          completeForecast(hoursData, locationName, timeFormat, unitType, windUnit, gustsEnabled);
        };

        // If the secondary API drops or times out, immediately push the fallback to avoid a stuck UI
        geoXhr.onerror = function() {
          completeForecast(hoursData, fallbackName, timeFormat, unitType, windUnit, gustsEnabled);
        };

        geoXhr.ontimeout = geoXhr.onerror;
        geoXhr.timeout = 10000;
        geoXhr.open('GET', geoUrl);
        geoXhr.send();
      } else { console.log('Weather HTTP ' + this.status); showStatus('Weather API error ' + this.status); scheduleRetry(); }
    };
    xhr.onerror = xhr.ontimeout = function() { console.log('Weather network error'); scheduleRetry(); };
    xhr.timeout = 15000;
    xhr.open('GET', url);
    xhr.send();
  }
  var locationMode = settings[keys.LOCATION_MODE] || 'gps';
  var locationQuery = (settings[keys.LOCATION_QUERY] || '').trim();
  if (locationMode === 'manual') {
    if (!locationQuery) { requestInProgress = false; showStatus('Enter ZIP or city'); return; }
    var geocodeUrl = /^\\d{5}$/.test(locationQuery) ?
      'https://api.openweathermap.org/geo/1.0/zip?zip=' + encodeURIComponent(locationQuery + ',US') + '&appid=' + apiKey :
      'https://api.openweathermap.org/geo/1.0/direct?q=' + encodeURIComponent(locationQuery) + '&limit=1&appid=' + apiKey;
    var geo = new XMLHttpRequest();
    geo.onload = function() {
      try {
        if (geo.status !== 200) throw new Error('Geocoding HTTP ' + geo.status);
        var result = JSON.parse(geo.responseText);
        var place = Array.isArray(result) ? result[0] : result;
        if (!place || typeof place.lat !== 'number' || typeof place.lon !== 'number') throw new Error('Location not found');
        weatherAt(place.lat, place.lon, place.name || locationQuery);
      } catch(e) { console.log(e.message); showStatus('Location not found'); scheduleRetry(); }
    };
    geo.onerror = geo.ontimeout = function() { showStatus('Location lookup failed'); scheduleRetry(); };
    geo.timeout = 12000;
    geo.open('GET', geocodeUrl);
    geo.send();
  } else {
    navigator.geolocation.getCurrentPosition(function(pos) { weatherAt(pos.coords.latitude, pos.coords.longitude, null); },
      function(error) { console.log('GPS error: ' + error.message); requestInProgress = false; showStatus('Location unavailable'); sendCachedWeather(); scheduleRetry(); },
      {timeout: 12000, maximumAge: 300000});
  }
}

// ==============================================================================
// TRANSMISSION CHAIN
// ==============================================================================

// Ensures the first 12 hours complete transmission before triggering the remaining 12
function completeForecast(hours, location, timeFormat, units, windUnit, gustsEnabled) {
  requestInProgress = false;
  retryCount = 0;
  if (retryTimer) { clearTimeout(retryTimer); retryTimer = null; }
  var savedAt = Date.now();
  localStorage.setItem('lastForecast', JSON.stringify({hours:hours, location:location, timeFormat:timeFormat, units:units, windUnit:windUnit, gustsEnabled:gustsEnabled, locationMode:currentSettings()[keys.LOCATION_MODE] || 'gps', locationQuery:(currentSettings()[keys.LOCATION_QUERY] || '').trim(), savedAt:savedAt}));
  transmitHours(hours, location, savedAt, false, gustsEnabled);
}
function sendWeatherChunks(c1, c2, attempt) {
  attempt = attempt || 0;
  Pebble.sendAppMessage(c1, function() {
    Pebble.sendAppMessage(c2, function() { console.log('Forecast delivered'); }, function(e) {
      console.log('Second chunk failed: ' + JSON.stringify(e));
      if (attempt < 2) setTimeout(function() { sendWeatherChunks(c1, c2, attempt + 1); }, 1500 * (attempt + 1)); else showStatus('Watch transfer failed');
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