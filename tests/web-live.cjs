// Usage: NODE_PATH=<path containing playwright> SP_BROWSER=<Chromium binary> node tests/web-live.cjs
// Run `python -m http.server 8765 --bind 127.0.0.1 --directory web` first. No hardware is contacted.
const {chromium}=require('playwright');
const fs=require('node:fs');
const assert=require('node:assert/strict');
(async()=>{
 const fixtures=fs.readFileSync('spec/v1/examples/firmware-output.jsonl','utf8').trim().split('\n').map(JSON.parse);
 const state=fixtures.find(x=>x.type==='State').data,config=fixtures.find(x=>x.type==='Config').data;
 const browser=await chromium.launch({headless:true,...(process.env.SP_BROWSER?{executablePath:process.env.SP_BROWSER}:{})});
 try {
  const page=await browser.newPage();const errors=[];let waters=0, queries=0;
  page.on('pageerror',e=>errors.push(e.message));page.on('dialog',d=>d.accept());
  await page.route('**/api/v1/**',async route=>{
   const p=new URL(route.request().url()).pathname;let data,status=200;
   if(p.endsWith('/session'))data={token:'test-token',expires_in:900};
   else if(p==='/api/v1/nodes')data={nodes:[{device_id:state.device_id,display_name:'Тестовое растение',online:true},{device_id:'spo-000000000002',display_name:'Нет связи',online:false}]};
   else if(p.endsWith('/state'))data=state;
   else if(p.endsWith('/config'))data=config;
   else if(p.endsWith('/commands/water')){waters++;status=503;data={error:{message:'request_outcome_unknown'}};}
   else if(p.includes('/commands/')){queries++;data={status:'stopped'};}
   else data={};
   await route.fulfill({status,contentType:'application/json',body:JSON.stringify(data)});
  });
  await page.goto('http://127.0.0.1:8765/live.html');
  await page.locator('#password').fill('test');await page.locator('#login button').click();
  await page.locator('#plants article').first().waitFor();assert.equal(await page.locator('#plants article').count(),2);
  assert.equal(await page.getByRole('button',{name:'Открыть',exact:true}).last().isDisabled(),true);
  await page.getByRole('button',{name:'Открыть',exact:true}).first().click();await page.locator('#water').waitFor();
  assert.deepEqual(await page.locator('#detail .value').allTextContents(),['-12.5']);
  await page.locator('#water').click();await page.waitForFunction(()=>document.querySelector('#status').textContent.includes('request_outcome_unknown'));
  assert.equal(waters,1);assert.equal(await page.evaluate(()=>Object.keys(sessionStorage).length),1);
  await page.locator('#query').click();await page.waitForFunction(()=>document.querySelector('#status').textContent.includes('stopped'));
  assert.equal(queries,1);assert.equal(waters,1);assert.deepEqual(errors,[]);
  await page.setViewportSize({width:390,height:844});assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
  await page.screenshot({path:'/tmp/smartplant-live-mobile.png',fullPage:true});
  await page.locator('#logout').click();await page.locator('#login').waitFor();
  assert.equal(await page.locator('#password').inputValue(),'');
  console.log('PASS: login, online/offline, generic sensor, timeout retains ID, query without duplicate Water, mobile layout, logout');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
