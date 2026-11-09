'use strict';
(() => {
  let token = '', timer = null, busy = false, authenticated = false, selectedNew=null, editorDevice=null, configured=[];
  const dialog = document.createElement('dialog');
  dialog.id = 'connect-sheet'; dialog.setAttribute('aria-labelledby','connect-title');
  dialog.innerHTML = `<div class="sheet"><div class="service-header"><h2 id="connect-title" class="disp">Добавить растение</h2><button class="btn quiet" id="connect-close" aria-label="Закрыть">Закрыть</button></div>
    <p class="t2">Включите новый ESP32-C3 рядом с Hub. Подключать C3 к домашнему Wi-Fi не нужно.</p>
    <p id="connect-message" class="banner" role="status" aria-live="polite">Подтвердите вход владельца для добавления устройств.</p>
    <form id="connect-login"><div class="fld"><label for="connect-key">Ключ администратора Hub</label><input class="inp" id="connect-key" type="password" autocomplete="current-password" maxlength="64" required></div><button class="btn pri big">Продолжить</button></form>
    <div id="connect-controls" hidden><button id="connect-search" class="btn pri big">Найти рядом</button><div id="connect-candidates"></div><div id="connect-profile" hidden></div><div id="connect-complete" hidden></div><button id="connect-retry" class="btn" hidden>Продолжить подключение</button><h3 class="sgh">Ваши контроллеры</h3><ul id="connect-nodes"></ul><p class="note">Новые C3 доступны для поиска 2 минуты после включения. Если плата не нашлась, выключите и включите её. Уже привязанные устройства в поиске не появляются.</p><details><summary>Как защищена связь</summary><p class="note">Ключи согласуются автоматически и сохраняются на платах. В этой версии первое подключение не проверяет подлинность устройства по коду. Выполняйте его в доверенном окружении.</p></details></div></div>`;
  document.body.append(dialog);
  const $ = id => document.getElementById(id);
  const message = text => { $('connect-message').textContent = text; };
  function session() { $('connect-login').hidden = authenticated; $('connect-controls').hidden = !authenticated; }
  async function request(path, method='GET', data) {
    if(window.SPApi)return window.SPApi.request(path,method,data);
    const headers={}; if(token) headers.Authorization=`Bearer ${token}`;
    if(data!==undefined) headers['Content-Type']='application/json';
    const controller=new AbortController(), timeout=setTimeout(()=>controller.abort(),8000);
    try {
      const response=await fetch('/api/v1'+path,{method,headers,body:data===undefined?undefined:JSON.stringify(data),cache:'no-store',credentials:'same-origin',signal:controller.signal});
      const body=await response.json();
      if(response.status===401){token='';authenticated=false;session();throw new Error('Введите ключ администратора для продолжения');}
      if(!response.ok)throw new Error(body.error?.message||`HTTP ${response.status}`);
      return body;
    } finally {clearTimeout(timeout);}
  }
  async function poll() {
    clearTimeout(timer);
    if(!authenticated || !dialog.open) return;
    try {
      const state=await request('/setup/pairing');
      const registry=await request('/nodes');
      window.dispatchEvent(new CustomEvent('smartplant:registry',{detail:{nodes:registry.nodes,current:true}}));
      const target=registry.nodes.find(n=>n.device_id===state.selected_device_id);
      const phases={idle:'Нажмите «Найти рядом».',searching:`Ищем новые C3… Осталось ${state.remaining_seconds} с.`,connecting:'Согласуем и сохраняем ключи. Не отключайте платы.',waiting_device:'Ожидаем выбранный C3. Подключите его питание и продолжите подключение.',storage_error:'Не удалось сохранить связь. Проверьте питание плат и попробуйте продолжить.',paired:target?.online?'Растение подключено. Защищённая связь подтверждена.':'Ключи согласованы. Ожидаем связь с растением.',unavailable:'Поиск недоступен. Проверьте версию прошивки Hub.'};
      if(!editorDevice)message(phases[state.phase]||'Проверяем подключение…');
      $('connect-search').disabled=busy||state.pending||state.phase==='searching'||state.phase==='unavailable';
      $('connect-retry').hidden=!['waiting_device','storage_error'].includes(state.phase);
      const candidates=$('connect-candidates');candidates.replaceChildren();
      for(const c of state.candidates){
        const row=document.createElement('div');row.className='row';
        const label=document.createElement('span');label.textContent=`${c.model} · ${c.device_id.slice(-4)}`;
        const button=document.createElement('button');button.className='btn';button.textContent='Подключить';button.disabled=busy;
        button.onclick=()=>{selectedNew=c.device_id;action(()=>request('/setup/pairing/connect','POST',{device_id:c.device_id}));};
        row.append(label,button);candidates.append(row);
      }
      $('connect-nodes').replaceChildren(...registry.nodes.map(n=>{
        const li=document.createElement('li');li.textContent=`${n.display_name || n.device_id} — ${n.online?'на связи':'ожидаем связь'}`;return li;
      }));
      if(target?.online&&state.phase==='paired'&&selectedNew===target.device_id&&editorDevice!==target.device_id)await describe(target);
      if(!registry.nodes.length){const li=document.createElement('li');li.textContent='Пока нет подключённых растений';$('connect-nodes').append(li);}
    } catch(e){message(`Не удалось получить состояние: ${e.message}. Проверяем снова; команда подключения не повторяется.`);}
    if(authenticated && dialog.open) timer=setTimeout(poll,1500);
  }

  async function describe(node){
    editorDevice=node.device_id;message('Связь подтверждена. Теперь задайте имя, комнату и вид растения.');
    try{const saved=await request(`/nodes/${node.device_id}/plant`);if(!dialog.open||selectedNew!==node.device_id)return;const meta={...saved,setup_stage:Math.max(1,saved.setup_stage||0)},host=$('connect-profile');host.hidden=false;$('connect-complete').hidden=true;
      window.SPPlants.mount({host,compact:true,escape:s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])),api:window.SPApi,device:node.device_id,meta,art:window.SPViews.preview,rooms:[...new Set((window.SmartPlantApp?.catalog||[]).map(p=>p.room).filter(Boolean))],continueSetup:false,onSaved:async()=>{
        const name=host.querySelector('#brief-name').value.trim();window.SPDrafts.clean(dialog);host.hidden=true;host.replaceChildren();if(!configured.includes(node.device_id))configured.push(node.device_id);
        message(`Карточка «${name}» сохранена. Датчики и насос можно настроить позже.`);
        const done=$('connect-complete');done.hidden=false;done.innerHTML='<p class="note">Добавлено за этот сеанс: '+configured.length+'</p><div class="live-toolbar"><button class="btn pri" id="connect-add-more">Добавить ещё</button><button class="btn" id="connect-open-plant">Открыть растение</button></div>';
        $('connect-add-more').onclick=nextPlant;$('connect-open-plant').onclick=async()=>{dialog.close();await window.SmartPlantApp.refresh();window.SmartPlantApp.openDevice(node.device_id);};window.dispatchEvent(new Event('smartplant:registry'));
      }});
    }catch(e){editorDevice=null;message('Связь есть, но карточку пока не удалось открыть: '+e.message);}
  }

  async function action(fn){
    if(busy)return;busy=true;
    dialog.querySelectorAll('button:not(#connect-close)').forEach(b=>b.disabled=true);
    try {await fn();}catch(e){message(`Действие не подтверждено: ${e.message}. Перед повтором проверим состояние.`);}
    finally{busy=false;session();if(authenticated) await poll();else dialog.querySelector('form button').disabled=false;}
  }
  $('connect-login').onsubmit=e=>{e.preventDefault();action(async()=>{
    const password=$('connect-key').value;$('connect-key').value='';
    if(window.SPApi)await window.SPApi.login(password);else token=(await request('/session','POST',{password})).token;authenticated=true;
  });};
  function nextPlant(){window.SPDrafts.attempt(dialog,()=>{selectedNew=null;editorDevice=null;$('connect-profile').hidden=true;$('connect-profile').replaceChildren();$('connect-complete').hidden=true;action(()=>request('/setup/pairing/search','POST',{}));});}
  $('connect-search').onclick=nextPlant;
  $('connect-retry').onclick=()=>action(()=>request('/setup/pairing/retry','POST',{}));
  $('connect-close').onclick=()=>dialog.close();
  dialog.addEventListener('close',()=>{clearTimeout(timer);$('connect-key').value='';editorDevice=null;$('connect-profile').hidden=true;$('connect-profile').replaceChildren();if(configured.includes(selectedNew))selectedNew=null;});
  window.SmartPlantConnect={async open(){try{await request('/setup/pairing');authenticated=true;}catch(e){authenticated=false;}session();if(!dialog.open)dialog.showModal();if(authenticated)poll();}};
})();
