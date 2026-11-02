const {chromium}=require('playwright');const fs=require('node:fs');const assert=require('node:assert/strict');const crypto=require('node:crypto');
(async()=>{const browser=await chromium.launch({headless:true});try{
 const page=await browser.newPage({viewport:{width:390,height:844}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
 let begins=0,chunks=0,finishes=0,offset=0,failChunk=false;
 const bytes=Buffer.alloc(2048,0x37),hash=crypto.createHash('sha256').update(bytes).digest('hex');
 const manifest={format:2,layout:'ota-v1',data_abi:1,profile:'n8r2',version:'0.2.1-abi1',app:{file:'hub.bin',size:bytes.length,sha256:hash}};
 let status={schema_version:1,layout:'ota-v1',data_abi:1,profile:'n8r2',firmware_version:'0.2.0-abi1',phase:'idle',active:false,verifying:false,validated:true,received_bytes:0,total_bytes:0,last_result:'none',session_id:'00000000-0000-0000-0000-000000000000'};
 await page.route('**/*',async route=>{const path=new URL(route.request().url()).pathname;let result={schema_version:1};
  if(path==='/')return route.fulfill({contentType:'text/html',body:'<!doctype html><html lang="ru"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><link rel="stylesheet" href="style.css"><script src="api.js" defer></script><script src="updates.js" defer></script><body></body></html>'});
  if(['/api.js','/updates.js','/style.css'].includes(path))return route.fulfill({contentType:path.endsWith('css')?'text/css':'application/javascript',body:fs.readFileSync('web'+path,'utf8')});
  if(path==='/api/v1/updates')result=status;
  else if(path==='/api/v1/updates/begin'){begins++;const b=route.request().postDataJSON();assert.equal(b.device_id,'hub');assert.equal(b.sha256,hash);assert.equal(b.header,bytes.subarray(0,128).toString('hex'));status={...status,active:true,phase:'uploading',session_id:b.session_id,total_bytes:bytes.length};}
  else if(path==='/api/v1/updates/chunk'){chunks++;if(failChunk)return route.abort('connectionreset');const b=route.request().postDataJSON(),data=Buffer.from(b.data,'hex');assert.equal(b.offset,offset);assert(data.equals(bytes.subarray(offset,offset+data.length)));offset+=data.length;status.received_bytes=offset;}
  else if(path==='/api/v1/updates/finish'){finishes++;assert.equal(offset,bytes.length);status={...status,phase:'rebooting',active:true};setTimeout(()=>{status={...status,active:false,phase:'idle',last_result:'validated',firmware_version:'0.2.1-abi1'};},250);}
  else return route.fulfill({status:404,body:'{}'});
  return route.fulfill({contentType:'application/json',body:JSON.stringify(result)});
 });
 await page.goto('https://smartplant.test/');await page.waitForFunction(()=>!!window.SmartPlantUpdates);await page.evaluate(()=>SmartPlantUpdates.open('hub'));
 const setManifest=async m=>page.locator('#update-manifest').setInputFiles({name:'manifest.json',mimeType:'application/json',buffer:Buffer.from(JSON.stringify(m))});
 await page.locator('#update-image').setInputFiles({name:'hub.bin',mimeType:'application/octet-stream',buffer:bytes});await setManifest({...manifest,profile:'c3'});await page.locator('#update-submit').click();await page.waitForFunction(()=>document.querySelector('#update-message').textContent.includes('не подходит'));assert.equal(begins,0);
 await setManifest({...manifest,app:{...manifest.app,sha256:'0'.repeat(64)}});await page.locator('#update-submit').click();await page.waitForFunction(()=>document.querySelector('#update-message').textContent.includes('SHA-256'));assert.equal(begins,0);
 await setManifest(manifest);await page.locator('#update-submit').click();await page.waitForFunction(()=>document.querySelector('#update-message').textContent.includes('Запуск новой прошивки подтверждён'));assert.equal(begins,1);assert.equal(chunks,3);assert.equal(finishes,1);
 assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);await page.screenshot({path:'/tmp/smartplant-update-mobile.png',fullPage:true});
 await page.locator('#update-close').click();status={...status,active:false,phase:'idle'};offset=0;failChunk=true;await page.evaluate(()=>SmartPlantUpdates.open('hub'));await page.locator('#update-submit').click();await page.waitForFunction(()=>document.querySelector('#update-message').textContent.includes('автоматически не выполняется'));const count=chunks;await page.waitForTimeout(1400);assert.equal(chunks,count);assert.equal(finishes,1);assert.deepEqual(errors,[]);
 console.log('PASS: OTA profile/hash rejection, bounded upload, confirmed reboot, mobile dialog and no automatic mutation retry');
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
