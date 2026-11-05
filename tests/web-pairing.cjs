const {chromium}=require('playwright');const fs=require('node:fs');const assert=require('node:assert/strict');
const fixture=n=>JSON.parse(fs.readFileSync('spec/v1/examples/'+n+'.json','utf8'));
(async()=>{const browser=await chromium.launch({headless:true});try{
 const page=await browser.newPage({viewport:{width:390,height:844},reducedMotion:'reduce'}),errors=[];
 page.on('pageerror',e=>errors.push(e.message));
 let searches=0,connects=0,metaWrites=0,calibrations=0,online=false,demo=false,apiInDemo=0,raw=3000;
 const id='spo-010203040506',plant='01020304-0506-4008-800a-0b0c0d0e0f10',token='b'.repeat(64);
 let metadata={schema_version:1,revision:0,name:'Plant 0506',species_id:null,room:'',setup_stage:0};
 let state={schema_version:1,phase:'idle',remaining_seconds:0,selected_device_id:'',pending:false,security_mode:'first_use_unverified',candidates:[]};
 let config={...fixture('config'),device_id:id,plant_id:plant,plant_name:metadata.name,mode:'manual',revision:0,soil_calibration:null};
 function registry(){return {schema_version:1,nodes:connects?[{device_id:id,plant_id:plant,display_name:metadata.name,room:metadata.room,setup_stage:metadata.setup_stage,online,last_seen_at:null}]:[]};}
 await page.route('**/*',async route=>{
  const req=route.request(),path=new URL(req.url()).pathname;
  const csp=path==='/demo.html'?"default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; connect-src 'none'; form-action 'none'":"default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; connect-src 'self'; form-action 'self'";
  if(path==='/'||path==='/demo.html')return route.fulfill({contentType:'text/html',headers:{'Content-Security-Policy':csp},body:fs.readFileSync(path==='/'?'web/index.html':'web/demo.html','utf8')});
  if(['/network.js','/plant-data.js','/garden.js','/care.js','/home.js','/plants.js','/views.js','/system.js','/updates.js','/api.js','/app.js','/pairing.js','/demo.js','/style.css'].includes(path))return route.fulfill({contentType:path.endsWith('.css')?'text/css':'application/javascript',body:fs.readFileSync('web'+path,'utf8')});
  if(!path.startsWith('/api/v1/'))return route.fulfill({status:404,body:''});
  if(demo)apiInDemo++;
  if(path==='/api/v1/session'&&req.method()==='POST'){
   assert.equal(req.postDataJSON().password,'a'.repeat(64));return route.fulfill({contentType:'application/json',headers:{'Set-Cookie':`sp_session=${token}; Path=/; HttpOnly; Secure; SameSite=Strict`},body:JSON.stringify({schema_version:1,token,expires_in:900})});
  }
  if(!req.headers().authorization&&!req.headers().cookie?.includes('sp_session='))return route.fulfill({status:401,contentType:'application/json',body:'{"schema_version":1,"error":{"code":"unauthorized","message":"unauthorized"}}'});
  let data={},status=200;
  if(path==='/api/v1/nodes')data=registry();
  else if(path==='/api/v1/plants')data={schema_version:1,plants:connects?[{plant_id:plant,device_id:id,name:metadata.name,room:metadata.room,species_id:metadata.species_id,growth_stage:metadata.growth_stage||null,revision:metadata.revision,setup_stage:metadata.setup_stage}]:[]};
  else if(path==='/api/v1/setup/pairing')data=state;
  else if(path==='/api/v1/setup/pairing/search'){
   searches++;assert.deepEqual(req.postDataJSON(),{});state={...state,phase:'searching',remaining_seconds:120,candidates:[{device_id:id,model:'ESP32-C3'}]};status=202;
  }else if(path==='/api/v1/setup/pairing/connect'){
   connects++;assert.equal(req.postDataJSON().device_id,id);state={...state,phase:'connecting',pending:true,selected_device_id:id,candidates:[]};return route.abort('connectionreset');
  }else if(path===`/api/v1/nodes/${id}/device`){data={...fixture('device'),device_id:id,firmware_version:'0.2.0',capabilities:{soil_sensor:'analog',tank_sensor:false,pump:false,water_volume:'estimated',event_history:'local_bounded',hardware_known:true,soil_configured:true,identify:false,detach:true,ota:false,sensors:[{sensor_id:'sensor-0',kind:'soil_index',unit:'percent',max_age_ms:30000,calibration:'soil_two_point'}]}};}
  else if(path===`/api/v1/nodes/${id}/state`){
   data={...fixture('state-clock-unset'),device_id:id,plant:{plant_id:plant,name:metadata.name,species_id:null,room:metadata.room},availability:{online,last_seen_age_ms:online?0:45000},soil:{quality:online?'uncalibrated':'stale',raw_adc:raw,soil_index_pct:null,last_index_pct:online?null:42,sampled_at:null,sample_age_ms:online?0:45000},sensors:[{sensor_id:'sensor-0',kind:'soil_index',unit:'percent',max_age_ms:30000,calibration:'soil_two_point',quality:online?'uncalibrated':'stale',value:null,last_value:online?null:42,sample_age_ms:online?0:45000}]};
  }else if(path===`/api/v1/nodes/${id}/plant`){if(req.method()==='PUT'){const next=req.postDataJSON();assert.equal(next.revision,metadata.revision);metadata={...next,revision:metadata.revision+1};metaWrites++;}data=metadata;}
  else if(path===`/api/v1/nodes/${id}/config`){if(req.method()==='PUT'){const next=req.postDataJSON();assert.equal(req.headers()['if-match'],String(config.revision));assert.deepEqual(next.soil_calibration,{dry_raw:3000,wet_raw:1000});config={...next,revision:config.revision+1};calibrations++;}data={...config,plant_name:metadata.name};}
  else return route.fulfill({status:404,contentType:'application/json',body:'{"error":{"message":"not_found"}}'});
  return route.fulfill({status,contentType:'application/json',body:JSON.stringify(data)});
 });
 await page.goto('https://smartplant.test/');await page.locator('#owner-login').waitFor();
 assert.equal(await page.locator('.pcard').count(),0);assert(!(await page.locator('#view').textContent()).includes('Черри'));
 await page.locator('#owner-key').fill('a'.repeat(64));await page.locator('#owner-login button').click();
 await page.locator('.home-heading [data-action=add]').waitFor();assert.equal(await page.locator('.pcard').count(),0);
 await page.locator('[data-action=add]').first().click();await page.locator('#connect-search').waitFor();assert(await page.locator('#connect-login').isHidden());
 await page.locator('#connect-search').click();await page.locator('#connect-candidates button').click();assert.equal(searches,1);
 await page.waitForFunction(()=>document.querySelector('#connect-message').textContent.includes('Согласуем'));assert.equal(connects,1);
 state={...state,phase:'paired',pending:false};online=true;
 await page.locator('#brief-profile').waitFor();
 await page.locator('#connect-close').click();await page.locator('.pcard').click();await page.locator('[data-action=wizard]').click();await page.locator('#plant-name').fill('Монстера');await page.locator('#plant-room').fill('Гостиная');await page.locator('[data-profile-group=fruit]').click();await page.locator('[data-species=tomato-cherry]').click();await page.locator('[data-growth-stage=flower]').click();assert((await page.locator('#profile-preview').textContent()).includes('Цветение'));await page.locator('[data-profile-group=leaf]').click();await page.locator('[data-species=monstera]').click();assert.equal(await page.locator('[data-growth-stage=fruit]').count(),0);await page.locator('[data-growth-stage=adult]').click();assert.equal(await page.locator('#plant-name').inputValue(),'Монстера');await page.locator('#profile-save').click();
 await page.locator('[data-action=calibrate]').waitFor();assert.equal(metaWrites,1);assert.equal(metadata.species_id,'monstera');assert.equal(metadata.growth_stage,'adult');
 await page.locator('[data-action=calibrate]').click();await page.waitForFunction(()=>document.querySelector('#calibration-raw')?.textContent==='3000');
 await page.locator('[data-action=capture-dry]').click();raw=1000;
 await page.waitForFunction(()=>document.querySelector('#calibration-raw')?.textContent==='1000');await page.locator('[data-action=capture-wet]').click();await page.locator('[data-action=save-calibration]').click();
 await page.waitForFunction(()=>document.querySelector('#sheet-message').textContent.includes('Калибровка сохранена'));assert.equal(calibrations,1);
 await page.locator('#sheet [data-action=close]').click();online=false;
 await page.waitForFunction(()=>document.querySelector('#view').textContent.includes('Данные сейчас не обновляются'));
 await page.locator('[data-action=sensors]').click();assert(await page.locator('[data-action=calibrate]').isDisabled());await page.locator('#sheet [data-action=close]').click();
 await page.locator('#tabbar [data-view=garden]').click();assert((await page.locator('#view').textContent()).includes('Монстера'));assert((await page.locator('#view').textContent()).includes('42%'));assert.equal(await page.locator('.pcard .st-adult').count(),1);assert((await page.locator('.pcard').textContent()).includes('Взрослое'));
 assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
 await page.setViewportSize({width:1440,height:1000});assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
 await page.goto('https://smartplant.test/demo.html');demo=true;await page.locator('.pcard h3').first().waitFor();assert((await page.locator('.demo-banner').textContent()).includes('вымышленными'));assert.equal(apiInDemo,0);
 await page.locator('[data-act=pairdevice]').first().click();await page.locator('#sheet').waitFor();assert.equal(apiInDemo,0);
 demo=false;await page.goto('https://smartplant.test/');await page.locator('.pcard h3').waitFor();assert((await page.locator('#view').textContent()).includes('Монстера'));assert.equal(await page.evaluate(()=>document.cookie.includes('sp_session')),false);assert.equal(await page.evaluate(()=>localStorage.length+sessionStorage.length),0);
 assert.deepEqual(errors,[]);console.log('PASS: empty real garden, isolated demo, discovery without retries, room metadata, live ADC calibration, stale/offline state, responsive layout and cookie restoration');
 }finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
