'use strict';
(()=>{
 const api=window.SPApi,hex=bytes=>Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join('');
 const labels={idle:'Готово к обновлению',uploading:'Загружаем образ на Hub',transferring:'Передаём прошивку растению',rebooting:'Hub перезагружается',rebooting_node:'Проверяем запуск контроллера',completed:'Прошивка запущена и проверена',failed:'Обновление остановлено'};
 let dialog,target='hub',busy=false,poll=null,context=null,wasActive=false,sequence=0,missingSince=0,currentVersion='',releaseSequence=0,releaseController=null;
 const el=id=>dialog.querySelector('#update-'+id),message=text=>{el('message').textContent=text;};
 function modal(){if(dialog)return;dialog=document.createElement('dialog');dialog.id='update-dialog';dialog.className='update-dialog';dialog.setAttribute('aria-labelledby','update-title');
  dialog.innerHTML='<div class="sheet"><header class="service-header"><h2 class="disp" id="update-title">Обновление прошивки</h2><button class="btn quiet" id="update-close">Закрыть</button></header><p id="update-message" role="status">Проверяем устройство…</p><p id="update-version" class="live-help"></p><p class="live-help">Выберите manifest.json и приложение .bin из одного пакета для вашей платы. Настройки и ключи сохранятся.</p><section class="update-releases"><h3>Обновления с GitHub</h3><p class="live-help">Проверьте опубликованный выпуск и скачайте пакет для своей платы. Распакуйте его и выберите manifest.json и .bin ниже.</p><div class="live-toolbar"><button type="button" class="btn" id="update-release-check">Проверить выпуск</button><a class="btn quiet" href="https://github.com/Maxim-szh/smart-plant-os/releases" target="_blank" rel="noopener noreferrer">Все выпуски</a></div><p class="live-help" id="update-release-message" role="status"></p><div class="live-toolbar" id="update-release-assets"></div></section><form id="update-form"><div class="fld"><label for="update-manifest">manifest.json</label><input id="update-manifest" type="file" accept=".json,application/json" required></div><div class="fld"><label for="update-image">Приложение .bin</label><input id="update-image" type="file" accept=".bin,application/octet-stream" required></div><p class="live-help">Отключите питание насосов на время стендового обновления. Для C3 автоматический режим должен быть выключен в настройках датчиков.</p><button class="btn pri" id="update-submit">Проверить и установить</button></form><p id="update-compat" class="live-help" role="status"></p><p id="update-phase" class="live-help"></p><progress id="update-progress" max="100" value="0" aria-label="Ход обновления"></progress><div class="live-toolbar"><button class="btn pri" id="update-resume" hidden>Продолжить загрузку</button><button class="btn" id="update-check">Проверить состояние</button><button class="btn" id="update-abort" hidden>Отменить передачу</button></div><details><summary>Проверка файла</summary><p class="live-help">SHA-256 обнаруживает повреждение файла, но не подтверждает его автора. Устанавливайте образы из доверенного источника. Если новая версия не подтвердит запуск, загрузчик вернёт предыдущую.</p></details></div>';
  document.body.append(dialog);el('form').onsubmit=e=>{e.preventDefault();upload();};el('check').onclick=()=>check();el('close').onclick=()=>dialog.close();dialog.addEventListener('close',()=>{clearTimeout(poll);releaseSequence++;releaseController?.abort();});el('release-check').onclick=checkRelease;el('resume').onclick=()=>resume();el('abort').onclick=()=>abort();for(const id of ['manifest','image'])el(id).addEventListener('change',previewPackage);
 }
 // Public release metadata is requested only after a click, without Hub credentials.
 async function checkRelease(){
  const generation=++releaseSequence,chosen=target;releaseController?.abort();releaseController=new AbortController();
  const controller=releaseController,timeout=setTimeout(()=>controller.abort(),10000);
  el('release-check').disabled=true;el('release-assets').replaceChildren();el('release-message').textContent='Проверяем опубликованный выпуск…';
  try{
   const status=await api.request('/updates'),profile=chosen==='hub'?status.profile:'c3';
   if(!['n8r2','n16r8','c3'].includes(profile))throw Error('Профиль платы не определён. Сначала обновите сведения об устройстве.');
   const response=await fetch('https://api.github.com/repos/Maxim-szh/smart-plant-os/releases/latest',{credentials:'omit',referrerPolicy:'no-referrer',redirect:'error',headers:{Accept:'application/vnd.github+json'},signal:controller.signal});
   if(generation!==releaseSequence||!dialog.open)return;
   if(response.status===404){el('release-message').textContent='Опубликованных стабильных выпусков пока нет. Можно установить проверенный пакет вручную.';return;}
   if(response.status===403||response.status===429)throw Error('GitHub временно ограничил запросы. Попробуйте позже или откройте список выпусков.');
   if(!response.ok)throw Error('Не удалось получить выпуск с GitHub. Можно выбрать пакет вручную.');
   const text=await response.text();if(text.length>500000)throw Error('Описание выпуска слишком большое. Откройте список выпусков.');
   const release=JSON.parse(text);if(generation!==releaseSequence||!dialog.open)return;
   if(release.draft||release.prerelease||typeof release.tag_name!=='string'||release.tag_name.length>80||!Array.isArray(release.assets))throw Error('Нет подходящего стабильного выпуска. Откройте список выпусков.');
   const checks=await api.care(),published=checks.version(release.tag_name.replace(/^v/,'')),installed=checks.version(currentVersion);
   if(generation!==releaseSequence||!dialog.open)return;
   el('release-message').textContent=`Выпуск ${release.tag_name}. ${published===null||installed===null?'Сверьте версию и требования в описании выпуска.':published>installed?'Доступна новая версия.':published===installed?'Эта версия уже установлена.':'На плате установлена более новая версия.'} Профиль: ${profile}.`;
   if(published!==null&&installed!==null&&published<installed)return;
   const assets=release.assets.filter(a=>{
    if(typeof a.name!=='string'||typeof a.browser_download_url!=='string'||a.state!=='uploaded')return false;
    const url=new URL(a.browser_download_url);
    return url.origin==='https://github.com'&&!url.username&&!url.password&&!url.search&&!url.hash&&url.pathname.startsWith('/Maxim-szh/smart-plant-os/releases/download/')&&new RegExp('(?:^|[-_.])'+profile+'(?:[-_.]|$)','i').test(a.name)&&/\.(zip|tar\.gz)$/i.test(a.name);
   }).slice(0,4);
   for(const asset of assets){const a=document.createElement('a');a.className='btn';a.href=asset.browser_download_url;a.target='_blank';a.rel='noopener noreferrer';a.textContent='Скачать '+asset.name;el('release-assets').append(a);}
   if(!assets.length)el('release-message').append(' Готового пакета для этой платы в выпуске нет. Проверьте раздел «Все выпуски».');
  }catch(e){if(generation===releaseSequence&&dialog.open)el('release-message').textContent=e.name==='AbortError'?'GitHub не ответил вовремя. Можно установить пакет вручную.':e instanceof SyntaxError||e instanceof TypeError?'Не удалось прочитать выпуск. Проверьте интернет или откройте список выпусков.':e.message;}
  finally{clearTimeout(timeout);if(generation===releaseSequence)el('release-check').disabled=false;}
 }
 function canResume(s){return !!context&&s.active&&s.phase==='uploading'&&s.session_id===context.session&&s.image_sha256===context.hash&&s.target_device_id===context.target&&s.total_bytes===context.bytes.length&&Number.isInteger(s.received_bytes)&&s.received_bytes>=0&&s.received_bytes<=context.bytes.length;}
 async function check(){clearTimeout(poll);try{
  const s=await api.request('/updates');missingSince=0;
  if(target==='hub')currentVersion=s.firmware_version;
  el('version').textContent=`Установлена: ${currentVersion||'неизвестно'} · Hub ${s.firmware_version} · ${s.profile}`;
  el('submit').disabled=busy||s.active||s.verifying||s.layout!=='ota-v1';el('manifest').disabled=busy||s.active;el('image').disabled=busy||s.active;el('abort').hidden=!s.active;el('resume').hidden=busy||!canResume(s);
  el('progress').value=s.total_bytes?Math.floor(100*s.received_bytes/s.total_bytes):0;
  el('phase').textContent=s.phase==='transferring'?'Образ проверен на Hub. Передача C3 может занять несколько минут. Не выключайте платы.':s.phase==='rebooting_node'?'Файл передан. Ждём запуска и проверки C3.':s.phase==='rebooting'?'Ожидаем возвращения Hub в домашнюю сеть.':'';
  if(s.layout!=='ota-v1')message('Нужна однократная USB-миграция: в программе flash выберите «Обновить прошивку». Настройки сохранятся.');
  else if(s.verifying)message('Новая прошивка проверяет запуск. Подождите около 30 секунд.');
  else if(target==='hub'&&wasActive&&!s.active&&s.validated&&context&&s.phase!=='failed'){
   const own=s.session_id===context.session;
   message(own&&s.last_result==='validated'&&s.firmware_version===context.version?'Hub снова доступен. Запуск новой прошивки подтверждён.':own&&s.last_result==='rolled_back'?'Новая прошивка не подтвердила запуск. Hub вернулся к предыдущей версии.':'Hub снова доступен. Результат этой операции не подтверждён; проверьте установленную версию.');busy=false;wasActive=false;
  }else if(s.phase==='idle'&&s.last_result==='rolled_back')message('Предыдущее обновление не подтвердило запуск. Работает предыдущая прошивка.');
  else {message((labels[s.phase]||s.phase)+(s.error?` · ${api.message({message:s.error})}`:''));if(['completed','failed'].includes(s.phase)){busy=false;wasActive=false;}}
  if(dialog.open&&(s.active||s.verifying||wasActive))poll=setTimeout(check,2000);
  return s;
 }catch(e){
  if(e.status===401){message('Сеанс завершён. Закройте окно, войдите с ключом владельца и проверьте состояние обновления.');busy=false;return;}
  if(e.status===404){el('submit').disabled=true;message('Эта версия не поддерживает веб-обновление. Сначала обновите плату по USB с сохранением настроек.');return;}
  if(wasActive){if(!missingSince)missingSince=Date.now();message('Hub временно недоступен. Ждём перезапуска; файл повторно не отправляется. Если адрес изменился, откройте Hub в домашней сети.');if(dialog.open&&Date.now()-missingSince<300000)poll=setTimeout(check,3000);}
  else message(api.message(e));
 }}
 async function previewPackage(){const mf=el('manifest').files[0],bin=el('image').files[0],chosen=target;el('compat').textContent='';if(!mf||!bin)return;
  try{if(mf.size>16384)throw Error('manifest.json слишком большой');const m=JSON.parse(await mf.text()),s=await api.request('/updates');if(chosen!==target||mf!==el('manifest').files[0]||bin!==el('image').files[0])return;const checks=await api.care(),issue=checks.packageReview(m,bin,s,target,currentVersion);el('compat').textContent=issue||`Пакет ${m.version} для ${m.profile}: предварительная проверка пройдена. При установке дополнительно проверим SHA-256 и заголовок образа.`;}catch(e){el('compat').textContent=api.message(e);}
 }
 async function sendChunks(offset,operation){
  const c=context;for(;offset<c.bytes.length;offset+=768){if(operation!==sequence)return;await api.request('/updates/chunk','POST',{session_id:c.session,offset,data:hex(c.bytes.subarray(offset,offset+768))});el('progress').value=Math.floor(100*Math.min(offset+768,c.bytes.length)/c.bytes.length);message(`Загружаем на Hub: ${el('progress').value}%`);}
  if(operation!==sequence)return;await api.request('/updates/finish','POST',{session_id:c.session});busy=false;await check();
 }
 async function upload(){if(busy)return;busy=true;el('manifest').disabled=true;el('image').disabled=true;el('submit').disabled=true;el('resume').hidden=true;clearTimeout(poll);const operation=++sequence;
  try{
   const mf=el('manifest').files[0],bin=el('image').files[0];if(!mf||!bin||mf.size>16384)throw Error('Выберите manifest.json и образ приложения');
   const m=JSON.parse(await mf.text()),s=await api.request('/updates'),profile=target==='hub'?s.profile:'c3';
   if(s.active||s.verifying)throw Error('Дождитесь завершения текущего обновления');
   const checks=await api.care(),issue=checks.packageReview(m,bin,s,target,currentVersion);if(issue)throw Error(issue);
   message('Проверяем целостность файла…');const bytes=new Uint8Array(await bin.arrayBuffer()),hash=hex(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes)));if(hash!==m.app.sha256)throw Error('SHA-256 не совпадает. Скачайте пакет заново');
   context={bytes,hash,session:api.uuid(),version:m.version,target};
   el('version').textContent=`Установлена: ${currentVersion||s.firmware_version}. Выбрана: ${m.version}.`;
   /* Keep the context even if BEGIN response is lost; resume requires matching server status. */
   wasActive=true;await api.request('/updates/begin','POST',{schema_version:1,device_id:target,profile,size:bin.size,sha256:hash,header:hex(bytes.subarray(0,128)),session_id:context.session});
   await sendChunks(0,operation);
  }catch(e){if(operation!==sequence)return;busy=false;if(e.status>=400&&e.status<500)wasActive=false;message((e.message||api.message(e))+' Повторная отправка автоматически не выполняется. Проверьте состояние перед новой попыткой.');el('submit').disabled=false;if(!wasActive){el('manifest').disabled=false;el('image').disabled=false;}}
 }
 async function resume(){if(busy)return;busy=true;el('resume').hidden=true;clearTimeout(poll);const operation=++sequence;
  try{const s=await api.request('/updates');if(!canResume(s))throw Error('Продолжение недоступно: файл, цель или сессия не совпадают. Проверьте состояние.');await sendChunks(s.received_bytes,operation);}catch(e){busy=false;message(api.message(e)+' Повторная отправка автоматически не выполняется. Проверьте состояние.');}
 }
 async function abort(){const operation=++sequence;clearTimeout(poll);try{const s=await api.request('/updates');if(operation!==sequence)return;if(!s.active){busy=false;wasActive=false;await check();return;}await api.request('/updates/abort','POST',{session_id:s.session_id});busy=false;wasActive=false;await check();}catch(e){busy=false;message(api.message(e));}}
 window.SmartPlantUpdates={open:async(device='hub')=>{
  modal();clearTimeout(poll);releaseSequence++;releaseController?.abort();el('release-check').disabled=false;el('release-message').textContent='';el('release-assets').replaceChildren();
  if(!busy&&!wasActive&&target!==device){context=null;el('form').reset();currentVersion='';target=device;}
  if(!busy&&!wasActive)target=device;
  el('submit').disabled=true;el('compat').textContent='';
  el('title').textContent=target==='hub'?'Обновление Hub':'Обновление контроллера растения';if(!dialog.open)dialog.showModal();
  if(target!=='hub'){try{const d=await api.request(`/nodes/${target}/device`);currentVersion=d.firmware_version;if(!d.capabilities?.ota){el('submit').disabled=true;message('Для этого контроллера сначала требуется обновление по USB.');return;}}catch(e){message(api.message(e));return;}}
  await check();await previewPackage();
 }};
})();

