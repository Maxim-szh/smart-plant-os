'use strict';
// Real UI and local assets; isolated API fixture, never a physical controller.
const {chromium}=require('playwright');
const assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {createPreview}=require('./helpers/home-server.cjs');

(async()=>{
 const preview=createPreview(),url=await preview.listen();let browser;
 try{
  browser=await chromium.launch({headless:true,...(process.env.BENCH_BROWSER?{executablePath:process.env.BENCH_BROWSER}:{})});
  const page=await browser.newPage({viewport:{width:1440,height:900},colorScheme:'light',reducedMotion:'reduce'}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  // Valid long metadata, including a word without spaces, stresses intrinsic sizing.
  preview.state.plants[0].name='Монстера у большого окна в гостиной';
  preview.state.plants[0].room=preview.state.plants[1].room='Гостиная с большим окном и балконом';
  preview.state.plants[2].room='Комната'.repeat(5);
  const out=process.env.SP_VISUAL_QA;if(out)fs.mkdirSync(out,{recursive:true});
  const shot=async name=>{if(out)await page.screenshot({path:path.join(out,name+'.png')});};
  async function layout(label){
   const result=await page.evaluate(()=>{
    const root=document.documentElement,w=innerWidth;
    const clipped=[...document.querySelectorAll('#view button,#view input,#view select,#view textarea,dialog[open] button,dialog[open] input,dialog[open] select,dialog[open] textarea')]
     .filter(e=>e.getClientRects().length&&getComputedStyle(e).visibility!=='hidden')
     .filter(e=>{const r=e.getBoundingClientRect();return r.left< -1||r.right>w+1;})
     .map(e=>e.id||e.textContent.trim().slice(0,60));
    return {overflow:root.scrollWidth>w+1,clipped};
   });
   assert.equal(result.overflow,false,label+': page overflow');
   assert.deepEqual(result.clipped,[],label+': controls outside viewport');
  }
  const nav=async view=>{const width=await page.evaluate(()=>innerWidth);await page.locator(`${width>=1000?'#rail':'#tabbar'} [data-view=${view}]`).click();};
  await page.goto(url);await page.locator('.home-plant').first().waitFor();await page.evaluate(()=>document.fonts.ready);
  for(const width of [320,390,768,1024,1440]){
   await page.setViewportSize({width,height:width<600?844:900});
   for(const view of ['garden','diary','care','events','settings']){
    await nav(view);await layout(width+' '+view);
    if(width===390&&view==='care'||width===1440&&view==='garden'){await page.evaluate(()=>window.scrollTo(0,0));await shot(width+'-'+view);}
   }
   await nav('garden');
   const card=page.locator('.home-plant').first();
   if(await card.locator('.plant-utilities').getAttribute('open')!==null)await card.locator('.plant-utilities summary').click();
   assert.equal(await card.locator('[data-home=move]').isVisible(),false);
   await card.locator('.plant-utilities summary').click();
   assert(await card.locator('[data-home=move]').isVisible());
   if(width===390){preview.state.online=false;await page.evaluate(()=>SmartPlantApp.refresh());assert(await card.locator('[data-home=move]').isVisible(),'Data refresh retains expanded actions');preview.state.online=true;await page.evaluate(()=>SmartPlantApp.refresh());}
   await layout(width+' card actions');
   await card.locator('[data-home=move]').click();
   await page.locator('#home-dialog').waitFor({state:'visible'});await layout(width+' move dialog');
   await page.locator('#home-close').click();
   await card.locator('.plant-open').click();await layout(width+' plant detail');
   assert.equal(await page.locator('#view .cta').count(),0,'No fixed disabled action covers readings');
   if(width===390){
    assert(await page.locator('#plant-tab-now').isVisible());
    await shot('390-detail');
   }
   await page.locator('#plant-tab-now').press('ArrowRight');
   assert.equal(await page.locator('#plant-tab-hist').getAttribute('aria-selected'),'true');
   assert.equal(await page.locator('#plant-panel').getAttribute('aria-labelledby'),'plant-tab-hist');
   await page.evaluate(()=>SmartPlantApp.refresh());
   assert.equal(await page.evaluate(()=>document.activeElement.id),'plant-tab-hist');
   await page.locator('#plant-tab-hist').press('End');
   assert.equal(await page.locator('#plant-tab-about').getAttribute('aria-selected'),'true');
   await page.locator('#plant-tab-about').press('Home');
   await page.locator('.plant-title').click();await page.locator('#sheet').waitFor({state:'visible'});
   await layout(width+' profile dialog');
   assert(await page.locator('#sheet [data-action=close]').isVisible());
   assert.equal(await page.evaluate(()=>getComputedStyle(document.body).overflow),'hidden');
   if(width===390)await shot('390-profile');
   await page.locator('#sheet [data-action=close]').click();
  }
  // A small landscape / keyboard-sized viewport must keep a long form navigable.
  await page.setViewportSize({width:844,height:390});await nav('garden');
  await page.locator('.plant-open').first().click();await page.locator('.plant-title').click();
  await page.locator('#sheet').waitFor({state:'visible'});
  const dialog=page.locator('#sheet');
  assert(await dialog.evaluate(e=>e.getBoundingClientRect().height<=innerHeight-23));
  await page.locator('#sheet input').last().focus();
  await layout('short viewport');await page.locator('#sheet [data-action=close]').click();
  // Switching sections opens the new section at its own position; returning restores context.
  await page.setViewportSize({width:390,height:844});await nav('garden');
  const y=await page.evaluate(()=>{window.scrollTo(0,400);return scrollY;});assert(y>0);
  await nav('settings');assert.equal(await page.evaluate(()=>scrollY),0);
  await nav('garden');assert(Math.abs(await page.evaluate(()=>scrollY)-y)<=2);
  await page.emulateMedia({colorScheme:'dark'});await layout('dark garden');await shot('390-dark');
  // The setup page uses the same local fonts, and must fit narrow / landscape windows.
  await page.route('**/api/v1/setup',r=>r.fulfill({status:401,contentType:'application/json',body:JSON.stringify({error:{code:'unauthorized'}})}));
  await page.goto(url+'/setup.html');await page.locator('#admin').waitFor();
  for(const [width,height] of [[320,700],[390,844],[844,390],[1440,900]]){
   await page.setViewportSize({width,height});
   assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth+1),false,'setup '+width);
   assert.equal(await page.locator('#remember-browser').evaluate(e=>e.getBoundingClientRect().height),20);
  }
  // Touch interaction uses a real mobile browser context, not only a narrow desktop window.
  preview.state.plants[0].name='Монстера';preview.state.plants[0].room=preview.state.plants[1].room='Гостиная';preview.state.plants[2].room='Кухня';
  const mobile=await browser.newContext({viewport:{width:390,height:844},screen:{width:390,height:844},isMobile:true,hasTouch:true,colorScheme:'light',reducedMotion:'reduce'});
  const touch=await mobile.newPage();touch.on('pageerror',e=>errors.push(e.message));await touch.goto(url);await touch.locator('.plant-open').first().waitFor();await touch.evaluate(()=>document.fonts.ready);
  await touch.locator('.plant-open').first().tap();await touch.locator('#plant-tab-now').waitFor();
  assert.equal(await touch.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
  if(out)await touch.screenshot({path:path.join(out,'mobile-final.png')});
  await touch.locator('#plant-tab-about').tap();assert.equal(await touch.locator('#plant-tab-about').getAttribute('aria-selected'),'true');await mobile.close();
  const desktop=await browser.newPage({viewport:{width:1440,height:900},colorScheme:'light',reducedMotion:'reduce'});await desktop.goto(url);await desktop.locator('.plant-open').first().waitFor();await desktop.evaluate(()=>document.fonts.ready);
  if(out)await desktop.screenshot({path:path.join(out,'desktop-final.png')});await desktop.close();
  assert.deepEqual(errors,[]);assert(!preview.state.writes.some(w=>w.path.includes('/commands')||w.path.endsWith('/config')));
  console.log('PASS: five live sections at 320/390/768/1024/1440px, long metadata, secondary actions, dialogs, short viewport, keyboard tabs, scroll restoration, dark theme and setup layout; no actuator commands');
 }finally{await browser?.close();await preview.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
