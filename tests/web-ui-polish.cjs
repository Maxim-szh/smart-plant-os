'use strict';
const {chromium}=require('playwright'),assert=require('node:assert/strict'),fs=require('node:fs');
const {createPreview}=require('./helpers/home-server.cjs');
(async()=>{
 const preview=createPreview(),url=await preview.listen();let browser;
 try{
  browser=await chromium.launch({headless:true});const page=await browser.newPage({viewport:{width:1440,height:900},reducedMotion:'reduce'}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  let careReads=0,releaseReads=0,releaseStatus=200,externalHeaders;
  await page.route('**/care.js',route=>++careReads===1?route.abort():route.continue());
  await page.route('https://api.github.com/**',route=>{
   releaseReads++;externalHeaders=route.request().headers();
   return route.fulfill({status:releaseStatus,contentType:'application/json',body:JSON.stringify({tag_name:'v0.7.0-abi1',draft:false,prerelease:false,assets:[
    {state:'uploaded',name:'smartplantos-n8r2.zip',browser_download_url:'https://github.com/Maxim-szh/smart-plant-os/releases/download/v0.7.0/smartplantos-n8r2.zip'},
    {state:'uploaded',name:'smartplantos-n16r8.zip',browser_download_url:'https://github.com/Maxim-szh/smart-plant-os/releases/download/v0.7.0/smartplantos-n16r8.zip'},
    {state:'uploaded',name:'other-n8r2.zip',browser_download_url:'https://example.com/other-n8r2.zip'}
   ]})});
  });
  const diagnostics=JSON.parse(fs.readFileSync('spec/system/v1/examples/Diagnostics.json'));
  diagnostics.devices=[{device_id:preview.state.plants[0].device_id,online:false,last_telemetry_age_ms:null,firmware_version:'unknown',rssi_dbm:null,rssi_age_ms:null,health:null}];
  await page.route('**/api/v1/diagnostics',route=>route.fulfill({contentType:'application/json',body:JSON.stringify(diagnostics)}));
  await page.goto(url);await page.locator('.home-plant').first().waitFor();await page.evaluate(()=>SmartPlantApp.refresh());assert.equal(careReads,2,'One failed care script load must recover before the app opens');
  assert.equal(await page.locator('#view [data-action=add]').count(),1);assert.equal(await page.locator('.room-order,#home-diary-preview,.home-journal').count(),0);assert.equal(await page.locator('.garden-summary-panel').count(),3);assert.equal(releaseReads,0,'Opening the app does not contact GitHub');
  const room=page.locator('.room-group').first(),toggle=room.locator('.room-disclosure');await toggle.click();assert.equal(await room.locator('.room-shelf').isVisible(),false);await toggle.click();assert(await room.locator('.room-shelf').isVisible());
  const nav=async view=>page.locator(`${await page.evaluate(()=>innerWidth)>=1000?'#rail':'#tabbar'} [data-view=${view}]`).click();
  for(const width of [320,1440]){
   await page.setViewportSize({width,height:900});await page.locator('.home-plant .plant-open').first().click();await page.locator('.plant-title').click();await page.locator('#sheet[open] #profile-step-label').waitFor();
   assert.equal(await page.locator('#profile-step-label').innerText(),'Шаг 1 из 4');assert.equal(await page.locator('#profile-picker').isVisible(),false);
   await page.locator('#plant-name').fill('Монстера у окна');const writes=preview.state.writes.length;
   await page.locator('#profile-next').click();assert(await page.locator('#profile-picker').isVisible());await page.locator('[data-species=monstera]').click();await page.locator('#profile-next').click();await page.locator('[data-growth-stage=adult]').click();await page.locator('#profile-next').click();
   assert.equal(preview.state.writes.length,writes,'Moving through the wizard does not write settings');assert(await page.locator('#profile-preview').isVisible());
   assert.equal(await page.locator('#profile-back').isVisible(),true);await page.locator('#profile-back').click();assert.equal(await page.locator('[data-growth-stage=adult]').getAttribute('aria-pressed'),'true');await page.locator('#profile-next').click();
   const overflow=await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth+1);assert.equal(overflow,false);
   await page.locator('#profile-save').click();await page.waitForFunction(()=>!document.querySelector('#sheet').open&&document.querySelector('#app-notice')?.textContent==='Сохранено на Hub');await nav('garden');
  }
  await nav('settings');await page.locator('[data-action=settings-tab][data-tab=advanced]').click();await page.locator('[data-system-panel=diagnostics]').click();await page.locator('#sys-body .service-device').waitFor();
  const diagnosticText=await page.locator('#sys-body').innerText();assert(!diagnosticText.includes('unknown')&&!diagnosticText.includes('Нет данных назад'));assert(diagnosticText.includes('Ещё не получали'));await page.locator('#sys-close').click();await page.locator('[data-action=settings-tab][data-tab=main]').click();
  await page.locator('[data-action=firmware-compatibility]').click();await page.locator('#shT').getByText('Совместимость прошивок').waitFor();assert(!(await page.locator('#sheet').innerText()).includes('Cannot read'));await page.locator('#sheet [data-action=close]').click();
  await page.locator('#view [data-action=hub-update]').click();const close=await page.locator('#update-close').boundingBox(),title=await page.locator('#update-title').boundingBox();assert(Math.abs(close.y-title.y)<30,'Close belongs in the header');
  await page.locator('#update-release-check').click();await page.getByText(/Доступна новая версия/).waitFor();assert.equal(await page.locator('#update-release-assets a').count(),1);assert.equal(externalHeaders.authorization,undefined);assert.equal(externalHeaders.cookie,undefined);assert.equal(externalHeaders.referer,undefined);assert(!preview.state.writes.some(w=>w.path.startsWith('/api/v1/updates/')));
  releaseStatus=404;await page.locator('#update-release-check').click();await page.getByText(/Опубликованных стабильных выпусков пока нет/).waitFor();assert.equal(await page.locator('#update-release-assets a').count(),0);
  releaseStatus=429;await page.locator('#update-release-check').click();await page.getByText(/GitHub временно ограничил запросы/).waitFor();assert(await page.locator('#update-release-check').isEnabled());await page.locator('#update-close').click();
  preview.state.online=false;await page.evaluate(()=>SmartPlantApp.refresh());await nav('garden');assert((await page.locator('.garden-summary').innerText()).includes('Контроллер не на связи'));assert(!(await page.locator('.garden-summary').innerText()).includes('Срочных действий'));
  const names=await page.evaluate(()=>[
   SPApi.browserName('Mozilla/5.0 (iPhone) AppleWebKit Safari/604.1'),SPApi.browserName('Mozilla/5.0 (Windows NT 10.0) Chrome/130 YaBrowser/24.10'),SPApi.browserName('Mozilla/5.0 (Macintosh) Firefox/130')
  ]);assert.deepEqual(names,['Safari · iPhone','Яндекс Браузер · Windows','Firefox · Mac']);assert.deepEqual(errors,[]);
  const failed=await browser.newPage();const failedErrors=[];failed.on('pageerror',e=>failedErrors.push(e.message));
  await failed.route('**/care.js',route=>route.abort());await failed.goto(url);await failed.getByRole('heading',{name:'Не удалось открыть приложение'}).waitFor();assert(await failed.getByRole('button',{name:'Повторить загрузку'}).isVisible());assert.equal(await failed.locator('#owner-login').count(),0);assert.deepEqual(failedErrors,[]);await failed.close();
  console.log('PASS: failed care module recovery, one add action, working room disclosure, truthful home sections, responsive staged profiles, OTA header and explicit credential-free GitHub lookup with profile filtering/404/rate limits, browser names');
 }finally{await browser?.close();await preview.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
