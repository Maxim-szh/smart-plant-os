// NODE_PATH=<playwright modules> PLAYWRIGHT_BROWSERS_PATH=<cache> node tests/web-setup.cjs
// Browser checks use the actual files and CSP; API responses are mocked, no hardware writes.
const {chromium}=require('playwright');
const fs=require('node:fs');const assert=require('node:assert/strict');
(async()=>{
 const browser=await chromium.launch({headless:true});
 try {
  const page=await browser.newPage({viewport:{width:390,height:844}});const errors=[];let wifi=0,pairs=0,logins=0,scans=0;let status={schema_version:1,firmware_version:'0.1.0',wifi_connected:false,wifi_phase:'idle',ssid:'',ip:'',channel:6,update_method:'usb_app_only',nodes:[]};
  page.on('pageerror',e=>errors.push(e.message));
  await page.route('**/*',async route=>{
   const request=route.request(),path=new URL(request.url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:fs.readFileSync('web/index.html','utf8')});
   if(['/network.js','/plant-data.js','/plants.js','/views.js','/system.js','/updates.js','/api.js'].includes(path))return route.fulfill({contentType:'application/javascript',body:fs.readFileSync('web'+path,'utf8')});
   if(path==='/style.css')return route.fulfill({contentType:'text/css',body:fs.readFileSync('web/style.css','utf8')});
   if(path==='/pairing.js')return route.fulfill({contentType:'application/javascript',body:fs.readFileSync('web/pairing.js','utf8')});
   if(path==='/app.js')return route.fulfill({contentType:'application/javascript',body:fs.readFileSync('web/app.js','utf8')});
   if(path==='/setup')return route.fulfill({contentType:'text/html',headers:{'Content-Security-Policy':"default-src 'self'; script-src 'self'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"},body:fs.readFileSync('web/setup.html','utf8')});
   if(path==='/setup.js')return route.fulfill({contentType:'application/javascript',body:fs.readFileSync('web/setup.js','utf8')});
   let data={},code=200;
   if(path==='/api/v1/session'&&request.method()==='POST'){logins++;assert.equal(request.postDataJSON().password,'a'.repeat(64));data={token:'test-token',expires_in:900};}
   else if(path.startsWith('/api/v1/')) {
    if(!request.headers().authorization)return route.fulfill({status:401,contentType:'application/json',body:JSON.stringify({error:{message:'unauthorized'}})});
    assert.equal(request.headers().authorization,'Bearer test-token');
    if(path==='/api/v1/setup')data=status;
    else if(path==='/api/v1/setup/wifi/scan'){if(request.method()==='POST'){scans++;assert.deepEqual(request.postDataJSON(),{});code=202;}else data={schema_version:1,phase:'completed',available:true,networks:[{ssid:'Example-WiFi',rssi_dbm:-52,channel:6,supported:true}]};}
    else if(path==='/api/v1/setup/wifi'){wifi++;assert.deepEqual(request.postDataJSON(),{ssid:'Example-WiFi',password:'example-password'});if(wifi===2){status={...status,wifi_phase:'connected',wifi_connected:true,ssid:'Example-WiFi',ip:'192.168.1.20'};return route.abort('connectionreset');}status={...status,wifi_phase:'connecting'};code=202;}
    else if(path==='/api/v1/setup/peers'){pairs++;assert.equal(request.postDataJSON().hub_id,'spo-010203040506');status={...status,nodes:[{device_id:'spo-111213141516'}]};code=201;}
    else if(path==='/api/v1/session'&&request.method()==='DELETE')data={schema_version:1};
    else {code=404;data={error:{message:'not_found'}};}
   } else return route.fulfill({status:404,body:''});
   return route.fulfill({status:code,contentType:'application/json',body:JSON.stringify(data)});
  });
  await page.goto('https://smartplant.test/setup');
  assert(await page.locator('#dashboard').isHidden());
  assert.equal(await page.locator('a[href="/"]').count(),0);
  assert(await page.locator('#open-plants').isHidden());
  await page.screenshot({path:'/tmp/smartplant-welcome-mobile.png',fullPage:true});
  await page.locator('#access-file').setInputFiles({name:'access.json',mimeType:'application/json',buffer:Buffer.from(JSON.stringify({admin_password:'a'.repeat(64)}))});
  await page.waitForFunction(()=>document.querySelector('#admin').value.length===64);assert.equal(logins,0);
  await page.locator('#login-button').click();await page.locator('#dashboard').waitFor();assert.equal(logins,1);assert.equal(await page.locator('#admin').inputValue(),'');
  assert(await page.locator('#management').isHidden());
  assert(await page.locator('#network-panel').isVisible());
  await page.locator('#wifi-scan').click();await page.locator('#wifi-network-picker').waitFor();await page.locator('#wifi-networks').selectOption('Example-WiFi');assert.equal(scans,1);assert.equal(await page.locator('#ssid').inputValue(),'Example-WiFi');
  await page.locator('#ssid').fill('Example-WiFi');await page.locator('#wifi-password').fill('example-password');await page.locator('#wifi-button').click();
  await page.waitForFunction(()=>document.querySelector('#network-status').textContent.includes('Проверяем'));assert.equal(wifi,1);assert.equal(await page.locator('#wifi-password').inputValue(),'');
  status={...status,wifi_phase:'failed'};await page.waitForFunction(()=>document.querySelector('#network-status').textContent.includes('Прежняя конфигурация'),null,{timeout:10000});assert.equal(wifi,1);
  await page.locator('#wifi-password').fill('example-password');await page.locator('#wifi-button').click();
  await page.waitForFunction(()=>document.querySelector('#message').textContent.includes('Подключение подтверждено'));
  assert.equal(wifi,2);
  assert(await page.locator('#network-panel').isHidden());
  assert.equal(await page.locator('#open-plants').getAttribute('href'),'https://192.168.1.20/');
  const voucher=JSON.parse(fs.readFileSync('spec/setup/v1/examples/Pairing.json','utf8'));
  await page.locator('#pairing').setInputFiles({name:'node-pairing.json',mimeType:'application/json',buffer:Buffer.from(JSON.stringify(voucher))});await page.locator('#pair-button').click();
  await page.waitForFunction(()=>document.querySelector('#nodes').textContent.includes('spo-111213141516'));assert.equal(pairs,1);
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
  await page.screenshot({path:'/tmp/smartplant-setup-mobile.png',fullPage:true});
  assert.equal(await page.evaluate(()=>localStorage.length+sessionStorage.length),0);
  await page.setViewportSize({width:1440,height:1000});
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
  await page.screenshot({path:'/tmp/smartplant-setup-desktop.png',fullPage:true});
  await page.locator('#logout').click();await page.locator('#login').waitFor();assert.deepEqual(errors,[]);
  await page.goto('https://192.168.4.1/setup');
  await page.locator('#admin').fill('a'.repeat(64));await page.locator('#login-button').click();
  await page.locator('#ready-panel').waitFor();
  assert(await page.locator('#management').isHidden());
  await page.locator('#change-wifi').click();assert(await page.locator('#network-panel').isVisible());
  await page.waitForTimeout(3200);assert(await page.locator('#network-panel').isVisible());
  await page.goto('https://smartplant.test/');
  await page.locator('#owner-login').waitFor();
  assert.equal(await page.locator('meta[name=viewport]').getAttribute('content'),'width=device-width, initial-scale=1, viewport-fit=cover');
  await page.setViewportSize({width:390,height:844});
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
  await page.screenshot({path:'/tmp/smartplant-plants-mobile.png',fullPage:true});
  console.log('PASS: CSP rendering, private access file import, session, single Wi-Fi request, failed connection status, pairing import, mobile layout, logout');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
