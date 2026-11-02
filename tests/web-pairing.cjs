const {chromium}=require('playwright');
const fs=require('node:fs');const assert=require('node:assert/strict');
(async()=>{
 const browser=await chromium.launch({headless:true});
 try{
  const page=await browser.newPage({viewport:{width:390,height:844}});const errors=[];let searches=0,connects=0,online=false;
  let state={schema_version:1,phase:'idle',remaining_seconds:0,selected_device_id:'',pending:false,security_mode:'first_use_unverified',candidates:[]};
  page.on('pageerror',e=>errors.push(e.message));
  await page.route('**/*',async route=>{
   const req=route.request(),path=new URL(req.url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',headers:{'Content-Security-Policy':"default-src 'self'; script-src 'self'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"},body:fs.readFileSync('web/index.html','utf8')});
   if(['/app.js','/pairing.js'].includes(path))return route.fulfill({contentType:'application/javascript',body:fs.readFileSync('web'+path,'utf8')});
   let data={},status=200;
   if(path==='/api/v1/session'){assert.equal(req.postDataJSON().password,'a'.repeat(64));return route.fulfill({contentType:'application/json',headers:{'Set-Cookie':'sp_session=owner-cookie; Path=/; HttpOnly; Secure; SameSite=Strict'},body:JSON.stringify({token:'owner-token'})});}
   else if(path.startsWith('/api/v1/')){
    if(!req.headers().authorization && !req.headers().cookie?.includes('sp_session='))return route.fulfill({status:401,contentType:'application/json',body:JSON.stringify({error:{message:'unauthorized'}})});
    if(req.headers().authorization)assert.equal(req.headers().authorization,'Bearer owner-token');
    if(path==='/api/v1/setup/pairing')data=state;
    else if(path==='/api/v1/nodes')data={nodes:connects?[{device_id:'spo-010203040506',display_name:'Plant 0506',online}]:[]};
    else if(path==='/api/v1/setup/pairing/search'){
     searches++;assert.equal(req.method(),'POST');assert.deepEqual(req.postDataJSON(),{});
     state={...state,phase:'searching',remaining_seconds:120,candidates:[{device_id:'spo-010203040506',model:'ESP32-C3'}]};status=202;
    } else if(path==='/api/v1/setup/pairing/connect'){
     connects++;assert.deepEqual(req.postDataJSON(),{device_id:'spo-010203040506'});
     state={...state,phase:'connecting',pending:true,selected_device_id:'spo-010203040506',candidates:[]};
     return route.abort('connectionreset');
    } else return route.fulfill({status:404,body:'{}'});
   } else return route.fulfill({status:404,body:''});
   return route.fulfill({status,contentType:'application/json',body:JSON.stringify(data)});
  });
  await page.goto('https://smartplant.test/');await page.locator('#view .addcard').waitFor();
  await page.locator('#view .addcard').click();await page.locator('#connect-sheet').waitFor();
  assert(await page.locator('#connect-controls').isHidden());
  await page.locator('#connect-key').fill('a'.repeat(64));await page.locator('#connect-login button').click();
  await page.locator('#connect-search').waitFor();assert.equal(await page.locator('#connect-key').inputValue(),'');
  await page.locator('#connect-search').click();await page.locator('#connect-candidates button').waitFor();assert.equal(searches,1);
  assert.equal(await page.evaluate(()=>document.querySelector('#connect-sheet').scrollWidth>innerWidth),false);
  await page.screenshot({path:'/tmp/smartplant-discovery-mobile.png',fullPage:true});
  await page.locator('#connect-candidates button').click();
  await page.waitForFunction(()=>document.querySelector('#connect-message').textContent.includes('Согласуем'));
  assert.equal(connects,1);assert(await page.locator('#connect-search').isDisabled());
  state={...state,phase:'paired',pending:false};
  await page.waitForFunction(()=>document.querySelector('#connect-message').textContent.includes('Ожидаем связь'));
  online=true;await page.waitForFunction(()=>document.querySelector('#connect-message').textContent.includes('Защищённая связь подтверждена'));
  assert.equal(connects,1);assert.equal(await page.evaluate(()=>localStorage.length+sessionStorage.length),0);
  await page.locator('#connect-close').click();await page.locator('#tabbar [data-v=more]').click();
  await page.locator('[data-act=firmwareupdate]').click();assert(await page.locator('#sheet').isVisible());
  assert((await page.locator('#sheet').textContent()).includes('UPDATE'));
  await page.reload();await page.locator('#view .addcard').waitFor();
  await page.waitForFunction(()=>document.querySelector('#view').textContent.includes('Plant 0506'));
  await page.locator('#view .addcard').click();assert(await page.locator('#connect-login').isHidden());
  assert.equal(await page.evaluate(()=>document.cookie.includes('sp_session')),false);
  assert.deepEqual(errors,[]);
  console.log('PASS: real application entry, owner login, discovery selection, single connection on lost response, online confirmation, responsive layout and update instructions');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
