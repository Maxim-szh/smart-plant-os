'use strict';
const {chromium}=require('playwright'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {createPreview}=require('./helpers/home-server.cjs');
(async()=>{
 const preview=createPreview(),url=await preview.listen();let browser;
 preview.state.online=false;preview.state.deviceVersion='unknown';
 try{
  browser=await chromium.launch({headless:true,...(process.env.BENCH_BROWSER?{executablePath:process.env.BENCH_BROWSER}:{})});
  const page=await browser.newPage({viewport:{width:390,height:844},colorScheme:'light',reducedMotion:'reduce'}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  const out=process.env.SP_VISUAL_QA;if(out)fs.mkdirSync(out,{recursive:true});
  async function screenshot(name){if(out)await page.screenshot({path:path.join(out,name+'.png')});}
  async function spacing(){
   const field=await page.locator('[data-browser-form] input').boundingBox(),save=await page.locator('[data-browser-save]').boundingBox(),revoke=await page.locator('[data-browser-revoke]').boundingBox();
   assert(save.y-field.y-field.height>=15,'A field must be separated from its actions');
   assert(Math.abs(save.y-revoke.y)<2?revoke.x-save.x-save.width>=11:revoke.y-save.y-save.height>=11,'Actions need an explicit gap');
  }
  const nav=async view=>{const width=await page.evaluate(()=>innerWidth);await page.locator(`${width>=1000?'#rail':'#tabbar'} [data-view=${view}]`).click();};
  await page.goto(url);await page.locator('.home-plant').first().waitFor();await page.evaluate(()=>document.fonts.ready);
  for(const width of [320,390,1440]){
   await page.setViewportSize({width,height:width===1440?900:844});await nav('settings');
   const gap=await page.locator('.settings-session').evaluate(e=>{const a=e.querySelector('p').getBoundingClientRect(),b=e.querySelector('button').getBoundingClientRect();return b.top-a.bottom;});assert(gap>=15);
   await page.locator('[data-system-panel=access]').click();await page.locator('[data-browser-save]').waitFor();assert.equal(await page.locator('#sys-message').innerText(),'','A new panel must not retain another panel’s message');await spacing();
   if(width===390)await screenshot('access-mobile');if(width===1440)await screenshot('access-desktop');
   const input=page.locator('[data-browser-form] input');await input.fill('Ноутбук');await input.press('Enter');await page.getByText('Имя браузера сохранено',{exact:true}).waitFor();assert.equal(preview.state.browsers[0].name,'Ноутбук');
   await input.fill('Несохранённое имя');await page.locator('#browser-refresh').click();await page.locator('#unsaved-stay').click();assert.equal(await input.inputValue(),'Несохранённое имя');await input.fill('Ноутбук');
   await input.fill('  ');const writes=preview.state.writes.filter(w=>w.path.includes('/sessions/')).length;await input.press('Enter');assert.equal(preview.state.writes.filter(w=>w.path.includes('/sessions/')).length,writes);await input.fill('Ноутбук');
   await page.locator('#sys-close').click();assert.equal(await page.locator('#unsaved-dialog[open]').count(),0,'Saved form closes without a dirty prompt');
   await page.locator('[data-system-panel=devices]').click();await page.locator('.service-device h3').first().waitFor();
   assert.equal(await page.locator('.service-device dd').nth(1).innerText(),'Не получена');
   const fonts=await page.locator('.service-facts').first().evaluate(e=>[getComputedStyle(e.querySelector('dt')).fontSize,getComputedStyle(e.querySelector('dd')).fontSize]);assert.deepEqual(fonts,['16px','16px']);
   if(width===390)await screenshot('devices-mobile');await page.locator('#sys-close').click();
   await nav('garden');await page.locator('.home-plant').first().locator('[data-action=diary]').click();await page.locator('[data-diary-add=fertilize]').click();
   assert.equal(await page.locator('[data-diary-add][aria-pressed=true]').count(),1);assert.equal(await page.locator('[data-diary-add=fertilize]').getAttribute('aria-pressed'),'true');
   assert.equal(await page.evaluate(()=>document.activeElement.id),'diary-form-title','Do not summon a keyboard before the user chooses a field');
   const font=await page.locator('#diary-at').evaluate(e=>({size:getComputedStyle(e).fontSize,family:getComputedStyle(e).fontFamily}));assert.equal(font.size,'16px');assert.equal(font.family,await page.locator('#diary-note').evaluate(e=>getComputedStyle(e).fontFamily));
   await page.locator('#diary-note').fill('Удобрение: тестовая запись');await page.locator('[data-diary-add=repot]').click();await page.locator('#unsaved-stay').click();assert.equal(await page.locator('#diary-note').inputValue(),'Удобрение: тестовая запись');assert.equal(await page.locator('[data-diary-add=fertilize]').getAttribute('aria-pressed'),'true');
   if(width===390)await screenshot('diary-mobile');if(width===1440)await screenshot('diary-desktop');
   await page.locator('.diary-form button[type=submit]').click();await page.getByText('Запись сохранена',{exact:true}).waitFor();assert(preview.state.entries.every(e=>e.kind==='fertilize'));
   assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);await page.locator('#diary-close').click();
  }
  await page.emulateMedia({colorScheme:'dark'});await page.setViewportSize({width:390,height:844});await nav('settings');await page.locator('[data-system-panel=access]').click();await page.locator('[data-browser-save]').waitFor();assert.equal(await page.locator('#sys-message').innerText(),'','A new panel must not retain another panel’s message');await spacing();await screenshot('access-dark');await page.locator('#sys-close').click();
  assert.deepEqual(errors,[]);assert(!preview.state.writes.some(w=>w.path.includes('/commands')||w.path.endsWith('/config')));
  console.log('PASS: access form spacing, Enter save, dirty refresh and empty-name rejection; device typography/unknown version; diary selection, date font, draft protection and explicit save at 320/390/1440px, dark theme; no actuator commands');
 }finally{await browser?.close();await preview.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
