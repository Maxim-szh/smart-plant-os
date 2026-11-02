'use strict';
/* One upload at a time. Retries require an explicit owner action; session and offsets are checked by the Hub. */
(()=>{
 const api=window.SPApi,hex=bytes=>Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join('');
 let dialog,target='hub',busy=false,poll=null,session='',expectedVersion='',beforeVersion='',wasActive=false;
 const labels={idle:'Готово к обновлению',uploading:'Загружаем образ на Hub',transferring:'Передаём прошивку растению',rebooting:'Hub перезагружается',rebooting_node:'Проверяем запуск контроллера',completed:'Прошивка запущена и проверена',failed:'Обновление остановлено'};
 function el(id){return dialog.querySelector('#update-'+id);}
 function message(text){el('message').textContent=text;}
 function modal(){
  if(dialog)return;dialog=document.createElement('dialog');dialog.id='update-dialog';dialog.className='update-dialog';
  dialog.innerHTML='<div class="sheet"><h2 class="disp" id="update-title">Обновление прошивки</h2><p id="update-message" role="status">Проверяем устройство…</p><p class="live-help">Выберите manifest.json и файл приложения .bin из одного официального пакета для вашей платы. Настройки и ключи сохраняются. Проверка SHA-256 обнаруживает повреждения файла; устанавливайте только образы из доверенного источника.</p><form id="update-form"><div class="fld"><label for="update-manifest">manifest.json</label><input id="update-manifest" type="file" accept=".json,application/json" required></div><div class="fld"><label for="update-image">Приложение .bin</label><input id="update-image" type="file" accept=".bin,application/octet-stream" required></div><p class="live-help">Отключите питание насосов на время обновления. Для C3 автоматический режим должен быть выключен в настройках датчиков.</p><button class="btn pri" id="update-submit">Проверить и установить</button></form><progress id="update-progress" max="100" value="0" aria-label="Ход обновления"></progress><p id="update-version" class="live-help"></p><div class="live-toolbar"><button class="btn" id="update-check">Проверить состояние</button><button class="btn" id="update-abort" hidden>Отменить передачу</button><button class="btn" id="update-close">Закрыть</button></div></div>';
  document.body.append(dialog);dialog.setAttribute('aria-labelledby','update-title');
  el('form').onsubmit=e=>{e.preventDefault();upload();};el('check').onclick=()=>check();el('close').onclick=()=>{dialog.close();clearTimeout(poll);};
  el('abort').onclick=async()=>{if(!session)return;try{await api.request('/updates/abort','POST',{session_id:session});busy=false;await check();}catch(e){message(api.message(e));}};
 }
 async function check(){
  clearTimeout(poll);try{
   const s=await api.request('/updates');el('version').textContent=`Hub: ${s.firmware_version} · ${s.profile} · ABI ${s.data_abi}`;
   session=s.session_id||session;
   el('submit').disabled=busy||s.active||s.verifying||s.layout!=='ota-v1';el('abort').hidden=!s.active;
   el('progress').value=s.total_bytes?Math.floor(100*s.received_bytes/s.total_bytes):0;
   if(s.layout!=='ota-v1')message('Нужна однократная USB-миграция: flash → «Обновить прошивку». Она добавит два OTA-раздела с сохранением настроек.');
   else if(s.verifying)message('Новая прошивка проверяет запуск. Подождите около 30 секунд.');
   else if(target==='hub'&&wasActive&&!s.active&&s.validated){
    message(s.last_result==='validated'&&s.firmware_version===expectedVersion?'Hub снова доступен. Запуск новой прошивки подтверждён.':`Hub работает с ${s.firmware_version}. Ожидалась ${expectedVersion}; проверьте, не произошёл ли откат.`);busy=false;wasActive=false;
   }else {message((labels[s.phase]||s.phase)+(s.error?` · ${s.error}`:''));if(['completed','failed'].includes(s.phase)){busy=false;wasActive=false;}}
   if(s.active||s.verifying||wasActive)poll=setTimeout(check,2000);
  }catch(e){
   if(e.status===401){message('Сеанс завершён. Закройте окно, войдите с ключом владельца и проверьте состояние обновления.');busy=false;return;}
   if(wasActive){message('Связь с Hub временно отсутствует. Ждём перезапуска; файл повторно не отправляется. Если изменился адрес, откройте Hub в домашней сети.');poll=setTimeout(check,3000);}
   else message(api.message(e));
  }
 }
 async function upload(){
  if(busy)return;busy=true;el('submit').disabled=true;clearTimeout(poll);
  try{
   const mf=el('manifest').files[0],bin=el('image').files[0];if(!mf||!bin||mf.size>16384)throw Error('Выберите manifest.json и образ приложения');
   const m=JSON.parse(await mf.text()),s=await api.request('/updates');beforeVersion=s.firmware_version;
   const profile=target==='hub'?s.profile:'c3';
   if(m.format!==2||m.layout!=='ota-v1'||m.profile!==profile||m.data_abi!==s.data_abi||m.app?.file!==bin.name||m.app?.size!==bin.size||!Number.isInteger(bin.size)||bin.size<128||bin.size>(profile==='c3'?0x1e0000:0x200000))throw Error('Пакет не подходит к выбранной плате или требует другой версии формата данных');
   message('Проверяем целостность файла…');const bytes=new Uint8Array(await bin.arrayBuffer());
   const digest=hex(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes)));if(digest!==m.app.sha256)throw Error('SHA-256 не совпадает. Скачайте пакет заново');
   expectedVersion=m.version;session=api.uuid();
   await api.request('/updates/begin','POST',{schema_version:1,device_id:target,profile,size:bin.size,sha256:digest,header:hex(bytes.subarray(0,128)),session_id:session});wasActive=true;
   for(let offset=0;offset<bytes.length;offset+=768){await api.request('/updates/chunk','POST',{session_id:session,offset,data:hex(bytes.subarray(offset,offset+768))});el('progress').value=Math.floor(100*Math.min(offset+768,bytes.length)/bytes.length);message(`Загружаем на Hub: ${el('progress').value}%`);}
   await api.request('/updates/finish','POST',{session_id:session});busy=false;await check();
  }catch(e){busy=false;message((e.message||api.message(e))+' Повторная отправка автоматически не выполняется. Проверьте состояние перед новой попыткой.');el('submit').disabled=false;}
 }
 window.SmartPlantUpdates={open:async(device='hub')=>{modal();target=device;busy=false;wasActive=false;session='';el('title').textContent=device==='hub'?'Обновление Hub':'Обновление контроллера растения';dialog.showModal();await check();}};
})();
