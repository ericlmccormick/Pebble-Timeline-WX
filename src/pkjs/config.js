module.exports = [
  {type:'heading',defaultValue:'Timeline WX Settings'},
  {type:'section',items:[
    {type:'heading',defaultValue:'Weather source'},
    {type:'select',messageKey:'WEATHER_PROVIDER',label:'Provider',defaultValue:'auto',options:[
      {label:'Automatic (no API key)',value:'auto'},
      {label:'Open-Meteo (global, no key)',value:'openmeteo'},
      {label:'NWS (US, no key)',value:'nws'},
      {label:'Met.no (global, no key)',value:'metno'},
      {label:'OpenWeather OneCall 3.0 (key)',value:'openweather'}]},
    {type:'input',messageKey:'API_KEY',label:'OpenWeather API key (optional)'}
  ]},
  {type:'section',items:[
    {type:'heading',defaultValue:'Display'},
    {type:'select',messageKey:'TEMP_UNIT',label:'Temperature',defaultValue:'F',options:[
      {label:'Fahrenheit',value:'F'},{label:'Celsius',value:'C'},{label:'Kelvin',value:'K'},{label:'Rankine',value:'R'}]},
    {type:'select',messageKey:'WIND_UNIT',label:'Wind',defaultValue:'mph',options:[
      {label:'mph',value:'mph'},{label:'km/h',value:'kmh'},{label:'m/s',value:'ms'},{label:'knots',value:'knots'}]},
    {type:'select',messageKey:'TIME_FORMAT',label:'Time',defaultValue:'12h',options:[
      {label:'12 hour',value:'12h'},{label:'24 hour',value:'24h'}]},
    {type:'select',messageKey:'COLOR_MODE',label:'Appearance',defaultValue:'auto',options:[
      {label:'Auto (sunrise/sunset)',value:'auto'},{label:'Day',value:'day'},{label:'Night',value:'night'}]},
    {type:'toggle',messageKey:'SHOW_GUSTS',label:'Show gusts on select',defaultValue:false},
    {type:'select',messageKey:'MESSAGE_POLICY',label:'Notifications',defaultValue:'all',options:[
      {label:'All messages',value:'all'},
      {label:'Critical only',value:'critical'}]}
  ]},
  {type:'section',items:[
    {type:'heading',defaultValue:'Location'},
    {type:'select',messageKey:'LOCATION_MODE',label:'Source',defaultValue:'gps',options:[
      {label:'Phone GPS',value:'gps'},{label:'ZIP or city',value:'manual'}]},
    {type:'input',messageKey:'LOCATION_QUERY',label:'ZIP or city (manual only)',defaultValue:''}
  ]},
  {type:'section',items:[
    {type:'heading',defaultValue:'Updates'},
    {type:'slider',messageKey:'REFRESH_INTERVAL',label:'Refresh minutes',defaultValue:30,min:15,max:120,step:15}
  ]},
  {type:'submit',defaultValue:'Save'}
];