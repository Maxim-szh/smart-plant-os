'use strict';
const {chromium}=require('playwright'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {createPreview}=require('./helpers/home-server.cjs');
(async()=>{
 const preview=createPreview(),url=await preview.listen();let browser;
 preview.state.online=false;preview.state.stateUnavailable=true;
 try{
  browser=await chromium.launch({headless:true,...(process.env.BENCH_BROWSER?{executablePath:process.env.BENCH_BROWSER}:{})});
  const page=await browser.newPage({viewport:{width:390,height:844},colorScheme:'light',reducedMotion:'reduce'}),errors=[],calls=[];
  page.on('pageerror',e=>errors.push(e.message));
  let releaseRegistry,releaseEvents,registryHeld=true,mode='hold',onEvents=null;
  const registryGate=new Promise(r=>releaseRegistry=r);let eventsGate=new Promise(r=>releaseEvents=r);
  await page.route('**/api/v1/**',async route=>{
   const p=new URL(route.request().url()).pathname;calls.push(p);
   if(p==='/api/v1/nodes'&&registryHeld)await registryGate;
   if(p.endsWith('/events')){
    if(mode==='hold'){onEvents?.();await eventsGate;}
    if(mode==='error')return route.fulfill({status:503,contentType:'application/json',body:JSON.stringify({error:{code:'history_unavailable',message:'history_unavailable'}})});
   }
   return route.continue();
  });
  const out=process.env.SP_VISUAL_QA;if(out)fs.mkdirSync(out,{recursive:true});
  await page.goto(url,{waitUntil:'domcontentloaded'});await page.locator('.app-loading').waitFor();await page.evaluate(()=>document.fonts.ready);
  assert.equal(await page.locator('.app-loading').getAttribute('aria-busy'),'true');
  assert((await page.locator('.app-loading h1').evaluate(e=>getComputedStyle(e).fontFamily)).includes('Literata'));
  if(out)await page.screenshot({path:path.join(out,'loading-mobile.png')});
  await page.setViewportSize({width:1440,height:900});if(out)await page.screenshot({path:path.join(out,'loading-desktop.png')});
  registryHeld=false;releaseRegistry();await page.locator('.home-plant').first().waitFor();await page.evaluate(()=>SmartPlantApp.refresh());
  assert.equal(await page.locator('.app-loading').count(),0);
  await page.setViewportSize({width:390,height:844});await page.locator('#tabbar [data-view=events]').click();
  await page.getByText('Загружаем события…',{exact:true}).waitFor();
  await page.waitForFunction(()=>document.querySelector('#view').textContent.includes('Загружаем события'));
  mode='empty';releaseEvents();await page.getByText('Событий нет',{exact:true}).waitFor();
  assert(calls.some(p=>p.endsWith('/events')),'Read Hub history despite unavailable live C3 state');
  assert.equal(await page.getByText('Загружаем события…',{exact:true}).count(),0);
  if(out)await page.screenshot({path:path.join(out,'events-empty.png')});
  await page.clock.install();mode='hold';eventsGate=new Promise(r=>releaseEvents=r);const entered=new Promise(r=>onEvents=r);
  await page.clock.fastForward(6000);await page.evaluate(()=>{window.emptyPoll=SmartPlantApp.refresh(true);});await entered;
  assert.equal(await page.getByText('Загружаем события…',{exact:true}).count(),0,'Confirmed empty history remains visible during a background read');
  assert.equal(await page.getByText('Событий нет',{exact:true}).count(),1);
  mode='error';releaseEvents();await page.evaluate(()=>emptyPoll);
  await page.getByText('События недоступны',{exact:true}).waitFor();assert.equal(await page.getByText('Событий нет',{exact:true}).count(),0,'Failed history is not empty history');
  assert.equal(await page.getByText('Загружаем события…',{exact:true}).count(),0);
  mode='empty';await page.locator('[data-action=refresh-events]').click();await page.getByText('Событий нет',{exact:true}).waitFor();
  await page.locator('#tabbar [data-view=garden]').click();await page.locator('.plant-open').first().click();await page.locator('#plant-tab-hist').click();await page.getByText('Событий нет',{exact:true}).waitFor();
  assert.equal(await page.getByText('Загружаем события…',{exact:true}).count(),0);
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);assert.deepEqual(errors,[]);
  assert(!preview.state.writes.some(w=>w.path.includes('/commands')||w.path.endsWith('/config')));
  console.log('PASS: styled initial loader, offline C3 state does not block Hub events, actual in-flight state, confirmed empty history, failure distinct from empty, explicit recovery and plant history; no actuator commands');
 }finally{await browser?.close();await preview.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
