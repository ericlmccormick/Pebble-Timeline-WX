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
var transferQueue = [];
var transferBusy = false;
var refreshQueued = false;
var startupCacheFallback = false;
var lastSentFingerprint = null;

function scheduleRetry() {
  requestInProgress = false;
  if (retryCount >= MAX_RETRIES) { if (startupCacheFallback) { startupCacheFallback = false; sendCachedWeather(); } showStatus('Update failed - old data'); return; }
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
function setting(settings, key, name, fallback) {
  return settings[key] !== undefined ? settings[key] : (settings[name] !== undefined ? settings[name] : fallback);
}
function windPreference(settings, units) {
  return settings[keys.WIND_UNIT] || settings['WIND_UNIT'] || (units === 'metric' ? 'kmh' : 'mph');
}
function sendCachedWeather() {
  try {
    var cache = JSON.parse(localStorage.getItem('lastForecast'));
    if (!cache || !cache.hours || !cache.savedAt) return false;
    var settings = currentSettings(), units = setting(settings, keys.TEMP_UNIT, 'TEMP_UNIT', null) || (setting(settings, keys.UNITS, 'UNITS', 'imperial')==='metric'?'C':'F');
    var wind = windPreference(settings, units), time = setting(settings, keys.TIME_FORMAT, 'TIME_FORMAT', '12h');
    if (cache.units !== units || cache.windUnit !== wind || cache.timeFormat !== time) return false;
    var locMode = setting(settings, keys.LOCATION_MODE, 'LOCATION_MODE', 'gps');
    var locQuery = String(setting(settings, keys.LOCATION_QUERY, 'LOCATION_QUERY', '')).trim();
    if ((cache.locationMode || 'gps') !== locMode || (cache.locationQuery || '') !== locQuery) return false;
    var hours = cache.hours.filter(function(h) { return h.dt >= Math.floor(Date.now()/1000) - 3600; });
    if (!hours.length) { showStatus('Forecast expired'); return false; }
    hours.sunrise = cache.sunrise;
    hours.sunset = cache.sunset;
    transmitHours(hours, cache.location, cache.savedAt, true, cache.gustsEnabled);
    return true;
  } catch(e) { console.log('Cache invalid: ' + e); return false; }
}
function sendThemeSetting() {
  var settings = currentSettings();
  var msg = {};
  msg[keys.COLOR_MODE] = setting(settings, keys.COLOR_MODE, 'COLOR_MODE', 'auto');
  Pebble.sendAppMessage(msg, null, function(e) { console.log('Theme update failed: ' + JSON.stringify(e)); });
}
function transmitHours(hours, location, savedAt, cached, gustsEnabled) {
  var settings=currentSettings(), units=setting(settings,keys.TEMP_UNIT,'TEMP_UNIT','F');
  var wind=windPreference(settings,units), count=Math.min(hours.length,48);
  var chunks=[];
  for(var ci=0;ci<Math.ceil(count/6);ci++) chunks.push({});
  var first=chunks[0];
  first[keys.COLOR_MODE]=setting(settings,keys.COLOR_MODE,'COLOR_MODE','auto');
  if(hours.sunrise&&hours.sunset){first[keys.SUNRISE]=hours.sunrise;first[keys.SUNSET]=hours.sunset;}
  first[keys.UNITS]=units==='C'?'metric':'imperial';
  first[keys.WIND_UNIT]=wind;
  first[keys.FORECAST_TIMESTAMP]=Math.floor(savedAt/1000);
  first[keys.FORECAST_CACHED]=cached?1:0;
  first[keys.LOCATION_NAME]=location||'Current location';
  first[keys.GUSTS_ENABLED]=gustsEnabled?1:0;
  first[keys.FORECAST_COUNT]=count;
  for(var i=0;i<count;i++){
    var h=hours[i],t=chunks[Math.floor(i/6)];
    t[keys.COLUMN_TIME+i]=h.time;
    t[keys.COLUMN_TEMP+i]=h.temp;
    t[keys.COLUMN_WIND+i]=h.wind;
    t[keys.COLUMN_RAIN+i]=h.rain;
    t[keys.COLUMN_UV+i]=h.uv;
    t[keys.COLUMN_COND+i]=h.cond;
    t[keys.IS_NIGHT+i]=h.night;
    t[keys.COLUMN_GUST+i]=h.gust;
  }
  for(var j=0;j<chunks.length;j++)chunks[j][keys.TRANSFER_PART]=j+1;
  chunks[0][keys.TRANSFER_COUNT]=count;
  sendWeatherChunks(chunks);
}
function wmoCode(c) {
  if(c===0)return 800;if(c===1)return 801;if(c===2)return 802;
  if(c===3)return 803;if(c>=45&&c<=48)return 701;
  if(c>=51&&c<=57)return 500;if(c>=61&&c<=67)return 502;
  if(c>=71&&c<=77)return 601;if(c>=80&&c<=82)return 520;
  if(c>=95)return 200;return 803;
}
function textCode(t) {
  t=String(t||'').toLowerCase().replace(/_/g,' ');
  if(t.indexOf('thunder')>=0)return 200;
  if(t.indexOf('snow')>=0||t.indexOf('sleet')>=0)return 601;
  if(t.indexOf('rain')>=0||t.indexOf('shower')>=0)return 501;
  if(t.indexOf('fog')>=0)return 701;
  if(t.indexOf('clear')>=0||t.indexOf('sunny')>=0||t.indexOf('fair')>=0)return 800;
  if(t.indexOf('partly')>=0||t.indexOf('fair')>=0)return 801;
  if(t.indexOf('cloud')>=0)return 803;return 803;
}
function httpJson(url, done, headers) {
  var xhr=new XMLHttpRequest();
  xhr.timeout=15000;
  xhr.onload=function() {
    if(xhr.status<200||xhr.status>=300){done(new Error('HTTP '+xhr.status));return;}
    try {done(null,JSON.parse(xhr.responseText));}catch(e){done(e);}
  };
  xhr.onerror=xhr.ontimeout=function(){done(new Error('Network timeout'));};
  xhr.open('GET',url);
  if(headers)Object.keys(headers).forEach(function(k){try{xhr.setRequestHeader(k,headers[k]);}catch(e){}});
  xhr.send();
}
function providerData(name,lat,lon,apiKey,done) {
  var base='lat='+encodeURIComponent(lat)+'&lon='+encodeURIComponent(lon);
  if(name==='openmeteo') {
    httpJson('https://api.open-meteo.com/v1/forecast?latitude='+lat+'&longitude='+lon+
      '&hourly=temperature_2m,dew_point_2m,precipitation_probability,weather_code,wind_speed_10m,wind_gusts_10m,uv_index,is_day'+
      '&daily=sunrise,sunset&timeformat=unixtime&timezone=auto&wind_speed_unit=ms&forecast_days=3',
      function(err,r){
        if(err){done(err);return;}
        try {
          var a=r.hourly, rows=[];
          for(var i=0;i<a.time.length;i++)rows.push({dt:a.time[i],temp:a.temperature_2m[i],
            wind:a.wind_speed_10m[i],gust:a.wind_gusts_10m[i],pop:a.precipitation_probability[i]==null?null:a.precipitation_probability[i]/100,
            uv:a.uv_index[i],cond:wmoCode(a.weather_code[i]),night:a.is_day[i]===0?1:0});
          done(null,{rows:rows,sunrise:r.daily&&r.daily.sunrise,sunset:r.daily&&r.daily.sunset});
        }catch(e){done(e);}
      });return;
  }
  if(name==='openweather') {
    if(!apiKey){done(new Error('OpenWeather API key required'));return;}
    httpJson('https://api.openweathermap.org/data/3.0/onecall?'+base+
      '&exclude=minutely,daily&units=metric&appid='+encodeURIComponent(apiKey),function(err,r){
      if(err){done(err);return;}
      try{done(null,{rows:r.hourly.map(function(h){return {dt:h.dt,temp:h.temp,wind:h.wind_speed,
        gust:h.wind_gust,pop:h.pop,uv:h.uvi,cond:h.weather[0].id,
        night:/n$/.test(h.weather[0].icon)?1:0};}),
        sunrise:r.current&&r.current.sunrise,sunset:r.current&&r.current.sunset});}
      catch(e){done(e);}
    });return;
  }
  if(name==='nws') {
    if(lat<18||lat>72||lon< -180||lon> -60){done(new Error('NWS outside US'));return;}
    httpJson('https://api.weather.gov/points/'+lat.toFixed(4)+','+lon.toFixed(4),function(err,r){
      if(err){done(err);return;}
      var url=r.properties&&r.properties.forecastHourly;
      if(!url){done(new Error('NWS no forecast'));return;}
      httpJson(url,function(err,f){
        if(err){done(err);return;}
        try{done(null,{rows:f.properties.periods.map(function(p){
          var wind=parseFloat(p.windSpeed)||0;
          var mps=/km\/h|kph/i.test(p.windSpeed)?wind/3.6:wind*0.44704;
          return {dt:Date.parse(p.startTime)/1000,temp:p.temperatureUnit==='F'?(p.temperature-32)*5/9:p.temperature,
            wind:mps,gust:null,pop:p.probabilityOfPrecipitation&&p.probabilityOfPrecipitation.value!=null?
            p.probabilityOfPrecipitation.value/100:null,uv:null,cond:textCode(p.shortForecast),
            night:p.isDaytime?0:1};
        })});}catch(e){done(e);}
      });
    });return;
  }
  if(name==='metno') {
    httpJson('https://api.met.no/weatherapi/locationforecast/2.0/compact?lat='+lat.toFixed(4)+'&lon='+lon.toFixed(4),
      function(err,r){
        if(err){done(err);return;}
        try{done(null,{rows:r.properties.timeseries.map(function(p){
          var d=p.data.instant.details;
          var next=p.data.next_1_hours||p.data.next_6_hours||p.data.next_12_hours||{};
          var sym=next.summary&&next.summary.symbol_code||'';
          return {dt:Date.parse(p.time)/1000,temp:d.air_temperature,wind:d.wind_speed,gust:d.wind_speed_of_gust,
            pop:next.details&&next.details.probability_of_precipitation!=null?
              next.details.probability_of_precipitation/100:null,
            uv:d.ultraviolet_index_clear_sky!=null?d.ultraviolet_index_clear_sky:null,
            cond:textCode(sym),night:/night/.test(sym)?1:0};
        })});}catch(e){done(e);}
      },{'User-Agent':'TimelineWX/1.0 (https://github.com/ericlmccormick/Pebble_Hourly_Weather)'});
  }
}
// All provider rows are normalized to SI units and a common time grid.
function validHourlyRows(raw, now) {
  if (!Array.isArray(raw)) return [];
  var seen = {}, rows = [];
  raw.forEach(function(h) {
    if (!h || !isFinite(h.dt) || !isFinite(h.temp) || !isFinite(h.wind) ||
        !isFinite(h.cond) || !isFinite(h.night)) return;
    if (h.dt < now - 3600 || h.dt >= now + 49 * 3600) return;
    var hour = Math.round(h.dt / 3600);
    if (Math.abs(h.dt - hour * 3600) > 1800 || seen[hour]) return;
    if (h.temp < -110 || h.temp > 65 || h.wind < 0 || h.wind > 150) return;
    var row = {dt:h.dt,temp:Number(h.temp),wind:Number(h.wind),
      gust:h.gust==null||!isFinite(h.gust)||h.gust<0?null:Number(h.gust),
      pop:h.pop==null||!isFinite(h.pop)?null:Math.max(0,Math.min(1,Number(h.pop))),
      uv:h.uv==null||!isFinite(h.uv)?null:Math.max(0,Number(h.uv)),
      cond:Number(h.cond),night:Number(h.night)?1:0};
    seen[hour] = true;
    rows.push(row);
  });
  rows.sort(function(a,b){return a.dt-b.dt;});
  var result = [];
  for (var i=0;i<rows.length && result.length<48;i++) {
    if (result.length && Math.abs(rows[i].dt-result[result.length-1].dt-3600)>300) break;
    result.push(rows[i]);
  }
  return result;
}
function fetchWeather() {
  if(requestInProgress){refreshQueued=true;return;}
  requestInProgress=true;
  var s=currentSettings();
  var provider=setting(s,keys.WEATHER_PROVIDER,'WEATHER_PROVIDER','auto');
  var apiKey=setting(s,keys.API_KEY,'API_KEY','');
  var tempUnit=setting(s,keys.TEMP_UNIT,'TEMP_UNIT',null)||
    (setting(s,keys.UNITS,'UNITS','imperial')==='metric'?'C':'F');
  var windUnit=setting(s,keys.WIND_UNIT,'WIND_UNIT','mph');
  if(windUnit==='kph')windUnit='kmh';if(windUnit==='mps')windUnit='ms';
  var timeFormat=setting(s,keys.TIME_FORMAT,'TIME_FORMAT','12h');
  var locationMode=setting(s,keys.LOCATION_MODE,'LOCATION_MODE','gps');
  var locationQuery=String(setting(s,keys.LOCATION_QUERY,'LOCATION_QUERY','')).trim();
  function fail(message){console.log(message);showStatus(message);scheduleRetry();}
  function run(lat,lon,label) {
    var providers=provider==='auto'?(lat>=24&&lat<=50&&lon>=-125&&lon<=-66?
      ['openmeteo','nws','metno']:['openmeteo','metno']):[provider];
    function attempt(i) {
      if(i>=providers.length){fail('All weather sources failed');return;}
      var name=providers[i];
      providerData(name,lat,lon,apiKey,function(err,data) {
        if(err){console.log(name+': '+err);if(provider==='auto')attempt(i+1);else fail(name+' failed: '+err.message);return;}
        var now=Math.floor(Date.now()/1000);
        var rows=validHourlyRows(data.rows,now);
        if(rows.length<48){if(provider==='auto')attempt(i+1);else fail('Incomplete '+name+' forecast');return;}
        var formatted=rows.map(function(h){
          var hr=new Date(h.dt*1000).getHours();
          var t=timeFormat==='24h'?(hr<10?'0':'')+hr:(hr%12||12)+(hr>=12?'p':'a');
          var c=h.temp, v=tempUnit==='F'?c*9/5+32:tempUnit==='K'?c+273.15:
            tempUnit==='R'?(c+273.15)*9/5:c;
          var ws=windUnit==='ms'?h.wind:windUnit==='kmh'?h.wind*3.6:
            windUnit==='knots'?h.wind*1.94384:h.wind*2.23694;
          var gust=h.gust==null?h.wind:h.gust;
          gust=windUnit==='ms'?gust:windUnit==='kmh'?gust*3.6:
            windUnit==='knots'?gust*1.94384:gust*2.23694;
          return {dt:h.dt,time:String(t),temp:String(Math.round(v))+(tempUnit==='F'?'°':tempUnit),
            wind:String(Math.round(ws)),gust:String(Math.round(gust)),
            rain:h.pop==null?'--':String(Math.round(h.pop*100))+'%',
            uv:h.uv==null?-1:Math.round(h.uv),cond:h.cond,night:h.night};
        });
        var sunrise=data.sunrise,sunset=data.sunset;
        if(Array.isArray(sunrise)){
          var day=0;while(day<sunrise.length-1&&sunset[day]<now)day++;
          sunrise=sunrise[day];sunset=sunset[day];
        }
        completeForecast(formatted,label||name,timeFormat,tempUnit,windUnit,
          setting(s,keys.SHOW_GUSTS,'SHOW_GUSTS',false),sunrise,sunset);
        showStatus('Updated: '+name);
      });
    }
    attempt(0);
  }
  if(locationMode==='manual'){
    if(!locationQuery){fail('Enter ZIP or city');return;}
    var zip=/^\d{5}(?:,US)?$/i.test(locationQuery);
    var query=locationQuery.replace(/,US$/i,'');
    var url=zip?'https://api.zippopotam.us/us/'+encodeURIComponent(query):
      'https://geocoding-api.open-meteo.com/v1/search?name='+encodeURIComponent(query)+'&count=1&format=json';
    httpJson(url,function(err,r){
      if(err){fail('Location lookup failed');return;}
      var p=zip&&r.places&&r.places[0];
      var g=!zip&&r.results&&r.results[0];
      if(!p&&!g){fail('Location not found');return;}
      run(Number(p?p.latitude:g.latitude),Number(p?p.longitude:g.longitude),
        zip?query:(g.name||query));
    });
  }else{
    navigator.geolocation.getCurrentPosition(function(p){run(p.coords.latitude,p.coords.longitude,'GPS Fix');},
      function(){fail('GPS unavailable');},{timeout:12000,maximumAge:300000});
  }
}

// ==============================================================================
// TRANSMISSION CHAIN
// ==============================================================================

// Ensures the first 12 hours complete transmission before triggering the remaining 12
function completeForecast(hours, location, timeFormat, units, windUnit, gustsEnabled, sunrise, sunset) {
  requestInProgress = false;
  retryCount = 0;
  startupCacheFallback = false;
  if (refreshQueued) { refreshQueued = false; setTimeout(fetchWeather, 100); }
  if (retryTimer) { clearTimeout(retryTimer); retryTimer = null; }
  var savedAt = Date.now();
  hours.sunrise = sunrise;
  hours.sunset = sunset;
  localStorage.setItem('lastForecast', JSON.stringify({sunrise:sunrise, sunset:sunset, hours:hours, location:location, timeFormat:timeFormat, units:units, windUnit:windUnit, gustsEnabled:gustsEnabled, locationMode:setting(currentSettings(), keys.LOCATION_MODE, 'LOCATION_MODE', 'gps'), locationQuery:String(setting(currentSettings(), keys.LOCATION_QUERY, 'LOCATION_QUERY', '')).trim(), savedAt:savedAt}));
  var fingerprint = JSON.stringify({hours:hours,location:location,units:units,windUnit:windUnit,gustsEnabled:gustsEnabled,sunrise:sunrise,sunset:sunset});
  if (fingerprint === lastSentFingerprint) { console.log('Forecast unchanged; skipping Bluetooth transfer'); return; }
  lastSentFingerprint = fingerprint;
  transmitHours(hours, location, savedAt, false, gustsEnabled);
}
// Serialize whole forecast transactions. Interleaved cached/live transfers
// otherwise overwrite the watch's pending first chunk.
function sendWeatherChunks(chunks) {
  transferQueue.push(chunks);
  if (transferQueue.length > 3) transferQueue.splice(0, transferQueue.length - 2);
  drainTransfers();
}
var awaitingPart = 0;
var ackTimer = null;
var activeTransfer = null;
var transferGeneration = 0;
var sendSequence = 0;
var ACK_TIMEOUT_MS = 10000;
var MAX_PART_RETRIES = 4;

function drainTransfers() {
  if (transferBusy || !transferQueue.length) return;
  transferBusy = true;
  activeTransfer = transferQueue.shift();
  transferGeneration++;
  console.log('Forecast transfer starting: ' + activeTransfer.length + ' parts');
  sendTransferPart(0, 0);
}
function finishTransfer() {
  if (ackTimer) clearTimeout(ackTimer);
  ackTimer = null;
  awaitingPart = 0;
  activeTransfer = null;
  transferBusy = false;
  transferGeneration++;
  drainTransfers();
}
function sendTransferPart(index, attempt) {
  if (!activeTransfer) return;
  if (index >= activeTransfer.length) {
    console.log('Forecast transfer complete');
    finishTransfer();
    return;
  }
  var generation = transferGeneration;
  var sequence = ++sendSequence;
  var part = index + 1;
  awaitingPart = part;
  if (ackTimer) clearTimeout(ackTimer);
  ackTimer = null;
  console.log('Sending forecast part ' + part + '/' + activeTransfer.length + ' attempt ' + (attempt + 1));
  // Arm the timer BEFORE sending: a watch ACK may arrive before the
  // sendAppMessage success callback. Never let an older callback rearm it.
  ackTimer = setTimeout(function() {
    if (generation !== transferGeneration || sequence !== sendSequence || awaitingPart !== part) return;
    ackTimer = null;
    console.log('ACK timeout for part ' + part);
    if (attempt < MAX_PART_RETRIES) sendTransferPart(index, attempt + 1);
    else { showStatus('Forecast transfer timed out'); finishTransfer(); }
  }, ACK_TIMEOUT_MS);
  Pebble.sendAppMessage(activeTransfer[index], function() {
    if (generation !== transferGeneration || sequence !== sendSequence) return;
    console.log('Bluetooth accepted part ' + part + ', waiting for watch ACK');
  }, function(err) {
    if (generation !== transferGeneration || sequence !== sendSequence || awaitingPart !== part) return;
    console.log('Part ' + part + ' send failed: ' + JSON.stringify(err));
    if (ackTimer) clearTimeout(ackTimer);
    ackTimer = null;
    if (attempt < MAX_PART_RETRIES) {
      setTimeout(function() {
        if (generation === transferGeneration && sequence === sendSequence && awaitingPart === part)
          sendTransferPart(index, attempt + 1);
      }, 1000);
    } else { showStatus('Watch transfer failed'); finishTransfer(); }
  });
}
function acknowledgePart(part) {
  if (!activeTransfer) return;
  if (part !== awaitingPart) {
    console.log('Ignoring stale/unexpected ACK ' + part + ', waiting for ' + awaitingPart);
    return;
  }
  console.log('Acknowledged forecast part ' + part + '/' + activeTransfer.length);
  if (ackTimer) clearTimeout(ackTimer);
  ackTimer = null;
  awaitingPart = 0;
  sendTransferPart(part, 0);
}

// ==============================================================================
// LIFECYCLE LISTENERS
// ==============================================================================

// Triggers immediately when the watch app boots up
Pebble.addEventListener('ready', function() {
  sendThemeSetting();
  // Fetch fresh data first. Only send the cached forecast if refresh ultimately fails.
  startupCacheFallback = true;
  fetchWeather();
  var mins=Number(setting(currentSettings(),keys.REFRESH_INTERVAL,'REFRESH_INTERVAL',30));
  REFRESH_MS=Math.max(15,Math.min(120,mins||30))*60000;
  refreshTimer=setInterval(fetchWeather,REFRESH_MS);
});
Pebble.addEventListener('appmessage', function(e) {
  var payload = e && e.payload;
  if (!payload) { console.log('Watch appmessage without payload'); return; }
  // Some Pebble bridges expose incoming keys by symbolic name instead of
  // numeric ID. Support both representations and log the received payload.
  console.log('Watch appmessage: ' + JSON.stringify(payload));
  var raw = payload[keys.REFRESH_REQUEST];
  if (raw === undefined) raw = payload.REFRESH_REQUEST;
  if (raw === undefined) raw = payload[String(keys.REFRESH_REQUEST)];
  if (raw === undefined || raw === null) return;
  var request = Number(raw);
  if (request >= 101 && request <= 108) acknowledgePart(request - 100);
  else if (request === 1) fetchWeather();
  else console.log('Unknown watch request: ' + request);
});

// Triggers when the user closes the Clay Settings menu on their phone
Pebble.addEventListener('webviewclosed', function(e) {
  if (e && e.response) { retryCount=0; sendThemeSetting(); if(refreshTimer)clearInterval(refreshTimer); var mins=Number(setting(currentSettings(),keys.REFRESH_INTERVAL,'REFRESH_INTERVAL',30)); REFRESH_MS=Math.max(15,Math.min(120,mins||30))*60000; refreshTimer=setInterval(fetchWeather,REFRESH_MS); fetchWeather(); } // Refresh data with new user settings
});