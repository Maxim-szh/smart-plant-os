'use strict';
// Real UI with an isolated API fixture; never sends commands to a physical board.
const {chromium}=require('playwright'),assert=require('node:assert/strict'),fs=require('node:fs');
const {createPreview}=require('./helpers/home-server.cjs');
(async()=>{
 const preview=createPreview(),url=await preview.listen();let browser;
 preview.state.plants[0].name=preview.state.plants[0].device_id;
 try{
  browser=await chromium.launch({headless:true});
  const page=await browser.newPage({viewport:{width:390,height:844},isMobile:true,hasTouch:true,colorScheme:'light',reducedMotion:'reduce'}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.goto(url);await page.locator('.home-plant').first().waitFor();await page.evaluate(()=>document.fonts.ready);
  const nav=async view=>page.locator(`${await page.evaluate(()=>innerWidth)>=1000?'#rail':'#tabbar'} [data-view=${view}]`).click();
  assert((await page.locator('.home-plant').first().innerText()).includes('Растение 1'));
  assert.equal(await page.locator('#view [data-action=diary],.plant-utilities').count(),0);
  assert(await page.locator('#tabbar [data-view=diary]').isVisible());
  assert.equal(await page.locator('.home-plant [data-home=quick]').count(),3);
  const controls=await page.locator('.home-plant [data-home=quick]').first().evaluate(e=>({select:getComputedStyle(e).userSelect,tap:getComputedStyle(e).webkitTapHighlightColor}));
  assert.equal(controls.select,'none');assert.equal(controls.tap,'rgba(0, 0, 0, 0)');
  assert.equal(await page.evaluate(()=>getComputedStyle(document.documentElement).scrollbarWidth),'none');
  assert(await page.evaluate(()=>{scrollTo(0,300);return scrollY>0;}),'Hiding the scrollbar must preserve scrolling');
  for(const width of [390,768,1280]){
   await page.setViewportSize({width,height:844});
   const boxes=await page.locator('.garden-summary-panel').evaluateAll(es=>es.map(e=>{const r=e.getBoundingClientRect();return {x:r.x,y:r.y,width:r.width};}));
   if(width<=600){assert(boxes[1].y>boxes[0].y);assert(boxes[2].y>boxes[1].y);}
   else{assert(Math.abs(boxes[0].y-boxes[1].y)<2);assert(boxes[1].x>boxes[0].x);assert(boxes[2].y>boxes[0].y);assert(boxes[2].width>boxes[0].width*1.9);}
   assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth+1),false);
  }
  await page.setViewportSize({width:390,height:844});await nav('settings');await page.locator('[data-system-panel=devices]').click();await page.locator('.service-device').first().waitFor();
  const first=page.locator('.service-device').first();
  assert.equal(await first.locator('h3').innerText(),'Растение 1');
  assert(!(await first.innerText()).includes(preview.state.plants[0].device_id));
  await first.locator('.technical-details summary').click();
  assert((await first.innerText()).includes(preview.state.plants[0].device_id));
  assert.equal(await first.locator('.technical-details').evaluate(e=>getComputedStyle(e).userSelect),'text');
  await page.locator('#sys-close').click();await nav('garden');await page.locator('.plant-open').first().click();
  assert.equal(await page.locator('#view [data-action=diary]').count(),0);
  assert(await page.locator('#view [data-home=move]').isVisible());assert(await page.locator('#view [data-home=service]').isVisible());
  await page.locator('.plant-title').click();await page.locator('#plant-name').waitFor();
  assert.equal(await page.locator('#plant-name').evaluate(e=>getComputedStyle(e).userSelect),'text');await page.locator('#sheet [data-action=close]').click();
  const manifest=JSON.parse(fs.readFileSync('web/manifest.webmanifest'));
  assert.deepEqual([manifest.id,manifest.start_url,manifest.scope,manifest.display],['/','/','/','standalone']);
  for(const [name,size]of [['apple-touch-icon.png',180],['icon-192.png',192],['icon-512.png',512]]){
   const png=fs.readFileSync('web/'+name);assert.equal(png.readUInt32BE(16),size);assert.equal(png.readUInt32BE(20),size);
  }
  assert.equal(await page.locator('link[rel=apple-touch-icon]').getAttribute('sizes'),'180x180');
  assert(await page.locator('.brand svg.app-mark').isVisible());
  assert(!(await page.locator('meta[name=viewport]').getAttribute('content')).includes('user-scalable=no'));
  await page.emulateMedia({colorScheme:'dark'});
  await page.waitForFunction(()=>document.querySelector('meta[name="theme-color"]').content===getComputedStyle(document.documentElement).getPropertyValue('--bg').trim());
  assert.equal(await page.locator('meta[name=theme-color]').getAttribute('content'),await page.evaluate(()=>getComputedStyle(document.documentElement).getPropertyValue('--bg').trim()));
  assert.deepEqual(errors,[]);assert.equal(preview.state.writes.length,0);
  console.log('PASS: PWA icons, mobile scrolling/selection, editable inputs, human device names with expandable IDs, diary navigation, detail actions and responsive two-plus-one summary; no mutations');
 }finally{await browser?.close();await preview.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
