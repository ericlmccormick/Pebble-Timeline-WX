var Clay = require('pebble-clay');
var clayConfig = require('./config');
var keys = require('message_keys');
var clay = new Clay(clayConfig);

var cachedWeatherData = null;

// ==============================================================================
// ERROR HANDLING & APP MESSAGES
// ==============================================================================
function sendErrorToWatch(errorMsg) {
  Pebble.sendAppMessage({ [keys.ERROR_MSG]: errorMsg });
}

function initializeWatchData(timeFormat, locationName, refreshInterval, isDark, provider) {
  var initPayload = {};
  initPayload[keys.TIME_FORMAT] = timeFormat; 
  initPayload[keys.LOCATION_NAME] = locationName;
  initPayload[keys.REFRESH_INTERVAL] = parseInt(refreshInterval);
  initPayload[keys.DARK_MODE] = isDark;
  initPayload[keys.WEATHER_PROVIDER] = provider;

  var settings = JSON.parse(localStorage.getItem('clay-settings')) || {};
  if(settings['TEMP_UNIT']) initPayload[keys.TEMP_UNIT] = settings['TEMP_UNIT'];
  if(settings['WIND_UNIT']) initPayload[keys.WIND_UNIT] = settings['WIND_UNIT'];

  Pebble.sendAppMessage(initPayload, function() {
      sendChunk(0);
      setTimeout(function() { sendChunk(1); }, 500); 
  });
}

// ==============================================================================
// NORMALIZED DATA PIPELINE
// ==============================================================================
function sendChunk(chunkIndex) {
  if (!cachedWeatherData || !cachedWeatherData.hourly) return;
  
  var settings = JSON.parse(localStorage.getItem('clay-settings')) || {};
  var tempUnit = settings['TEMP_UNIT'] || 'F';
  var windUnit = settings['WIND_UNIT'] || 'mph';
  
  var startIdx = chunkIndex * 6;
  var chunkRows = [];
  
  for (var i = 0; i < 6; i++) {
    var globalIdx = startIdx + i;
    if (globalIdx >= 48 || !cachedWeatherData.hourly[globalIdx]) break; 
    
    var h = cachedWeatherData.hourly[globalIdx];
    var date = new Date(h.dt * 1000); 
    
    var dayBreakFlag = 0;
    if (globalIdx > 0) {
      var prevDate = new Date(cachedWeatherData.hourly[globalIdx - 1].dt * 1000);
      if (date.getDate() !== prevDate.getDate()) dayBreakFlag = 1; 
    }
    
    var finalTemp = h.temp;
    if (tempUnit === 'F') finalTemp = (h.temp * 9/5) + 32;
    else if (tempUnit === 'K') finalTemp = h.temp + 273.15;
    else if (tempUnit === 'R') finalTemp = (h.temp + 273.15) * 9/5;
    
    var finalWind = h.wind_speed;
    if (windUnit === 'mph') finalWind = h.wind_speed * 2.23694;
    else if (windUnit === 'kph') finalWind = h.wind_speed * 3.6;
    else if (windUnit === 'knots') finalWind = h.wind_speed * 1.94384;

    var hazard = 0;
    var tempF = (h.temp * 9/5) + 32; 
    var windMph = h.wind_speed * 2.23694;
    var dewF = (h.dew_point * 9/5) + 32;

    if (tempF >= 100 || tempF <= 15 || windMph >= 40 || h.uvi >= 10 || h.weather_id == 781 || h.weather_id == 504) hazard = 3;
    else if (tempF >= 95 || windMph >= 25 || h.uvi >= 6 || (h.weather_id >= 200 && h.weather_id <= 232)) hazard = 2;
    else if (tempF >= 85 || tempF <= 32 || dewF >= 65) hazard = 1;

    var rowStr = [
      h.dt,
      Math.round(finalTemp),
      Math.round(finalWind),
      Math.round(h.pop * 100),
      Math.round(h.uvi),
      h.weather_id,
      h.is_night,
      dayBreakFlag,
      hazard
    ].join(',');

    chunkRows.push(rowStr);
  }

  var payload = {};
  payload[keys.CHUNK_INDEX] = chunkIndex;
  payload[keys.CHUNK_DATA] = chunkRows.join(';');
  Pebble.sendAppMessage(payload);
}

// ==============================================================================
// PROVIDER TRANSLATION MAPS
// ==============================================================================
function mapWmoToOpenWeather(code) {
  if (code === 0) return 800;
  if (code === 1) return 801;
  if (code === 2) return 802;
  if (code === 3) return 803;
  if (code >= 45 && code <= 48) return 701;
  if (code >= 51 && code <= 55) return 500;
  if (code >= 61 && code <= 65) return 502;
  if (code >= 71 && code <= 75) return 601;
  if (code >= 80 && code <= 82) return 520;
  if (code >= 95) return 200;
  return 803;
}

function mapNwsToOpenWeather(str) {
  str = str.toLowerCase();
  if (str.indexOf('thunder') !== -1) return 200;
  if (str.indexOf('snow') !== -1) return 601;
  if (str.indexOf('rain') !== -1 || str.indexOf('shower') !== -1) return 501;
  if (str.indexOf('fog') !== -1) return 701;
  if (str.indexOf('clear') !== -1 || str.indexOf('sunny') !== -1) return 800;
  return 803; 
}

function mapMetNoToOpenWeather(str) {
  if (!str) return 803;
  if (str.indexOf('thunder') !== -1) return 200;
  if (str.indexOf('snow') !== -1 || str.indexOf('sleet') !== -1) return 601;
  if (str.indexOf('rain') !== -1) return 501;
  if (str.indexOf('fog') !== -1) return 701;
  if (str.indexOf('clearsky') !== -1 || str.indexOf('fair') !== -1) return 800;
  if (str.indexOf('partlycloudy') !== -1) return 801;
  return 803;
}

// ==============================================================================
// WEATHER FETCHERS
// ==============================================================================
function fetchOpenWeather(lat, lon, apiKey, timeFormat, locationName, refreshInterval, isDark, provider) {
  if (!apiKey || apiKey.trim() === '') { sendErrorToWatch("Missing API Key"); return; }
  var url = 'https://api.openweathermap.org/data/3.0/onecall?lat=' + lat + '&lon=' + lon + '&exclude=minutely,daily&units=metric&appid=' + apiKey;

  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    if (this.status === 200) { 
      var raw = JSON.parse(this.responseText); 
      cachedWeatherData = { hourly: [] };
      for (var i = 0; i < 48; i++) {
        var h = raw.hourly[i];
        cachedWeatherData.hourly.push({
          dt: h.dt,
          temp: h.temp,
          wind_speed: h.wind_speed,
          dew_point: h.dew_point,
          pop: h.pop,
          uvi: h.uvi,
          weather_id: h.weather[0].id,
          is_night: h.weather[0].icon.includes('n') ? 1 : 0
        });
      }
      initializeWatchData(timeFormat, locationName, refreshInterval, isDark, provider);
    } else { sendErrorToWatch("API Error: " + this.status); }
  };
  xhr.onerror = function() { sendErrorToWatch("Network Offline"); };
  xhr.open('GET', url);
  xhr.send();
}

function fetchOpenMeteo(lat, lon, timeFormat, locationName, refreshInterval, isDark, provider) {
  var url = 'https://api.open-meteo.com/v1/forecast?latitude=' + lat + '&longitude=' + lon + '&hourly=temperature_2m,relativehumidity_2m,dewpoint_2m,precipitation_probability,weathercode,windspeed_10m,uv_index,is_day&windspeed_unit=ms&timeformat=unixtime&forecast_days=3';
  
  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    if (this.status === 200) {
      var raw = JSON.parse(this.responseText);
      cachedWeatherData = { hourly: [] };
      
      var nowEpoch = Date.now() / 1000;
      var startIndex = 0;
      for (var j = 0; j < raw.hourly.time.length; j++) {
        if (raw.hourly.time[j] >= nowEpoch - 3600) {
          startIndex = j;
          break;
        }
      }
      
      for (var i = 0; i < 48; i++) {
        var idx = startIndex + i;
        if (idx >= raw.hourly.time.length) break;
        cachedWeatherData.hourly.push({
          dt: raw.hourly.time[idx],
          temp: raw.hourly.temperature_2m[idx],
          wind_speed: raw.hourly.windspeed_10m[idx],
          dew_point: raw.hourly.dewpoint_2m[idx],
          pop: raw.hourly.precipitation_probability[idx] / 100,
          uvi: raw.hourly.uv_index[idx],
          weather_id: mapWmoToOpenWeather(raw.hourly.weathercode[idx]),
          is_night: raw.hourly.is_day[idx] === 0 ? 1 : 0
        });
      }
      initializeWatchData(timeFormat, locationName, refreshInterval, isDark, provider);
    } else { sendErrorToWatch("Meteo API Error"); }
  };
  xhr.onerror = function() { sendErrorToWatch("Network Offline"); };
  xhr.open('GET', url);
  xhr.send();
}

function fetchNWS(lat, lon, timeFormat, locationName, refreshInterval, isDark, provider) {
  var pointsUrl = 'https://api.weather.gov/points/' + lat + ',' + lon;
  
  var xhr1 = new XMLHttpRequest();
  xhr1.onload = function() {
    if (this.status === 200) {
      var forecastUrl = JSON.parse(this.responseText).properties.forecastHourly;
      var xhr2 = new XMLHttpRequest();
      xhr2.onload = function() {
        if (this.status === 200) {
          var raw = JSON.parse(this.responseText).properties.periods;
          cachedWeatherData = { hourly: [] };
          
          var nowEpoch = Date.now() / 1000;
          var startIndex = 0;
          for (var j = 0; j < raw.length; j++) {
            var periodEpoch = new Date(raw[j].startTime).getTime() / 1000;
            if (periodEpoch >= nowEpoch - 3600) {
              startIndex = j;
              break;
            }
          }
          
          for (var i = 0; i < 48; i++) {
            var idx = startIndex + i;
            if (idx >= raw.length) break;
            var p = raw[idx];
            var wStr = p.windSpeed.split(' ')[0];
            var windMps = (parseInt(wStr) || 0) * 0.44704;
            var tempC = p.temperatureUnit === 'F' ? (p.temperature - 32) * 5/9 : p.temperature;
            
            var popVal = (p.probabilityOfPrecipitation && p.probabilityOfPrecipitation.value != null) ? p.probabilityOfPrecipitation.value : 0;
            
            cachedWeatherData.hourly.push({
              dt: new Date(p.startTime).getTime() / 1000,
              temp: tempC,
              wind_speed: windMps,
              dew_point: tempC - 2, 
              pop: popVal / 100,
              uvi: 0, 
              weather_id: mapNwsToOpenWeather(p.shortForecast),
              is_night: !p.isDaytime ? 1 : 0
            });
          }
          initializeWatchData(timeFormat, locationName, refreshInterval, isDark, provider);
        } else { sendErrorToWatch("NWS Forecast Failed"); }
      };
      xhr2.open('GET', forecastUrl);
      xhr2.send();
    } else { sendErrorToWatch("NWS Region Error"); }
  };
  xhr1.onerror = function() { sendErrorToWatch("NWS Offline"); };
  xhr1.open('GET', pointsUrl);
  xhr1.send();
}

function fetchMetNo(lat, lon, timeFormat, locationName, refreshInterval, isDark, provider) {
  var url = 'https://api.met.no/weatherapi/locationforecast/2.0/complete?lat=' + lat + '&lon=' + lon;
  var xhr = new XMLHttpRequest();
  xhr.onload = function() {
    if (this.status === 200) {
      var raw = JSON.parse(this.responseText).properties.timeseries;
      cachedWeatherData = { hourly: [] };
      
      var nowEpoch = Date.now() / 1000;
      var startIndex = 0;
      for (var j = 0; j < raw.length; j++) {
        var periodEpoch = new Date(raw[j].time).getTime() / 1000;
        if (periodEpoch >= nowEpoch - 3600) {
          startIndex = j;
          break;
        }
      }
      
      for (var i = 0; i < 48; i++) {
        var idx = startIndex + i;
        if (idx >= raw.length) break;
        var p = raw[idx];
        var d = p.data.instant.details;
        
        var sym = "";
        if (p.data.next_1_hours) sym = p.data.next_1_hours.summary.symbol_code;
        else if (p.data.next_6_hours) sym = p.data.next_6_hours.summary.symbol_code;
        else if (p.data.next_12_hours) sym = p.data.next_12_hours.summary.symbol_code;
        
        var popVal = 0;
        var precipAmount = 0;
        
        if (p.data.next_1_hours && p.data.next_1_hours.details) {
          if (p.data.next_1_hours.details.probability_of_precipitation != null) popVal = p.data.next_1_hours.details.probability_of_precipitation;
          if (p.data.next_1_hours.details.precipitation_amount != null) precipAmount = p.data.next_1_hours.details.precipitation_amount;
        } else if (p.data.next_6_hours && p.data.next_6_hours.details) {
          if (p.data.next_6_hours.details.probability_of_precipitation != null) popVal = p.data.next_6_hours.details.probability_of_precipitation;
          if (p.data.next_6_hours.details.precipitation_amount != null) precipAmount = p.data.next_6_hours.details.precipitation_amount;
        }
        
        if (popVal === 0) {
          if (precipAmount > 0) {
            if (precipAmount < 0.25) popVal = 25;       
            else if (precipAmount < 1.0) popVal = 50;   
            else if (precipAmount < 2.5) popVal = 75;   
            else popVal = 100;                          
          } else if (sym.indexOf('rain') !== -1 || sym.indexOf('shower') !== -1 || sym.indexOf('snow') !== -1 || sym.indexOf('thunder') !== -1 || sym.indexOf('sleet') !== -1) {
            popVal = 100;
          }
        }
        
        cachedWeatherData.hourly.push({
          dt: new Date(p.time).getTime() / 1000,
          temp: d.air_temperature,
          wind_speed: d.wind_speed,
          dew_point: d.dew_point_temperature,
          pop: popVal / 100,
          uvi: d.ultraviolet_index_clear_sky || 0,
          weather_id: mapMetNoToOpenWeather(sym),
          is_night: sym.indexOf('night') !== -1 ? 1 : 0
        });
      }
      initializeWatchData(timeFormat, locationName, refreshInterval, isDark, provider);
    } else { sendErrorToWatch("Met.no API Error"); }
  };
  xhr.onerror = function() { sendErrorToWatch("Network Offline"); };
  xhr.open('GET', url);
  xhr.setRequestHeader("User-Agent", "PebbleApp/TimelineWX github.com");
  xhr.send();
}

// ==============================================================================
// LOCATION & LIFECYCLE
// ==============================================================================
function fetchWeather() {
  var settings = JSON.parse(localStorage.getItem('clay-settings')) || {};
  var provider = settings['WEATHER_PROVIDER'] || 'openmeteo';
  var apiKey = settings['API_KEY'];
  var timeFormat = settings['TIME_FORMAT'] || '12h';
  var locMethod = settings['LOCATION_METHOD'] || 'gps';
  var zipCode = settings['ZIP_CODE'] || '';
  var refreshInterval = settings['REFRESH_INTERVAL'] || 30;
  
  // Safely intercept older boolean cache values
  var isDark = settings['DARK_MODE'];
  if (isDark === true) isDark = "on";
  else if (isDark === false) isDark = "off";
  else if (!isDark) isDark = "auto";

  function routeProvider(lat, lon, locationName) {
    if (provider === 'openweather') fetchOpenWeather(lat, lon, apiKey, timeFormat, locationName, refreshInterval, isDark, provider);
    else if (provider === 'nws') fetchNWS(lat, lon, timeFormat, locationName, refreshInterval, isDark, provider);
    else if (provider === 'metno') fetchMetNo(lat, lon, timeFormat, locationName, refreshInterval, isDark, provider);
    else fetchOpenMeteo(lat, lon, timeFormat, locationName, refreshInterval, isDark, provider);
  }

  if (locMethod === 'zip' && zipCode !== '') {
    var geoUrl = 'https://geocoding-api.open-meteo.com/v1/search?name=' + zipCode + '&count=1&format=json';
    var geoXhr = new XMLHttpRequest();
    geoXhr.onload = function() {
      if (this.status === 200 && JSON.parse(this.responseText).results) {
        var geo = JSON.parse(this.responseText).results[0];
        routeProvider(geo.latitude, geo.longitude, geo.name);
      } else { sendErrorToWatch("Invalid Zip Code"); }
    };
    geoXhr.open('GET', geoUrl);
    geoXhr.send();
  } else {
    navigator.geolocation.getCurrentPosition(function(pos) {
      routeProvider(pos.coords.latitude, pos.coords.longitude, "GPS Fix");
    }, function(err) { sendErrorToWatch("GPS Failed"); }, {timeout: 15000, maximumAge: 60000});
  }
}

Pebble.addEventListener('ready', fetchWeather);
Pebble.addEventListener('webviewclosed', function(e) { if (e && e.response) fetchWeather(); });
Pebble.addEventListener('appmessage', function(e) {
  if (e.payload['SETTINGS_SYNC'] !== undefined) {
      var settings = JSON.parse(localStorage.getItem('clay-settings')) || {};
      if (e.payload['TEMP_UNIT']) settings['TEMP_UNIT'] = e.payload['TEMP_UNIT'];
      if (e.payload['WIND_UNIT']) settings['WIND_UNIT'] = e.payload['WIND_UNIT'];
      if (e.payload['DARK_MODE']) settings['DARK_MODE'] = e.payload['DARK_MODE'];
      if (e.payload['TIME_FORMAT']) settings['TIME_FORMAT'] = e.payload['TIME_FORMAT'];
      if (e.payload['WEATHER_PROVIDER']) settings['WEATHER_PROVIDER'] = e.payload['WEATHER_PROVIDER'];
      localStorage.setItem('clay-settings', JSON.stringify(settings));
      fetchWeather(); 
  } else if (e.payload['REQUEST_CHUNK'] !== undefined) {
      sendChunk(e.payload['REQUEST_CHUNK']);
  } else if (e.payload['REFRESH_REQUESTED'] !== undefined) {
      fetchWeather(); 
  }
});