'use strict';
const $=(s,r=document)=>r.querySelector(s);
const esc=s=>String(s??'').replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',"'":'&#39;'}[c]));
const IC={
 code:'M8 7l-5 5 5 5M16 7l5 5-5 5M14 4l-4 16',
 plug:'M8 3v5M16 3v5M6 8h12v3a6 6 0 01-6 6v4M6 11h12',
 window:'M5 21V10a7 7 0 0114 0v11M5 21h14M12 3v18M5 11h14',
 care:'M4 7h9M17 7h3M4 17h3M11 17h9M13 5v4M7 15v4',
 list:'M8 6h12M8 12h12M8 18h12M4 6h.01M4 12h.01M4 18h.01',
 more:'M6 12h.01M12 12h.01M18 12h.01',
 leaf:'M5 19c0-8 5-14 15-14 0 10-6 15-14 15M5 19c3-5 6-8 10-10',
 gear:'M12 15a3 3 0 100-6 3 3 0 000 6zM19.4 15a1.7 1.7 0 00.3 1.8l.1.1a2 2 0 11-2.8 2.8l-.1-.1a1.7 1.7 0 00-1.8-.3 1.7 1.7 0 00-1 1.5V21a2 2 0 11-4 0v-.1a1.7 1.7 0 00-1-1.5 1.7 1.7 0 00-1.8.3l-.1.1a2 2 0 11-2.8-2.8l.1-.1a1.7 1.7 0 00.3-1.8 1.7 1.7 0 00-1.5-1H3a2 2 0 110-4h.1a1.7 1.7 0 001.5-1 1.7 1.7 0 00-.3-1.8l-.1-.1a2 2 0 112.8-2.8l.1.1a1.7 1.7 0 001.8.3 1.7 1.7 0 001-1.5V3a2 2 0 114 0v.1a1.7 1.7 0 001 1.5 1.7 1.7 0 001.8-.3l.1-.1a2 2 0 112.8 2.8l-.1.1a1.7 1.7 0 00-.3 1.8 1.7 1.7 0 001.5 1H21a2 2 0 110 4h-.1a1.7 1.7 0 00-1.5 1z',
 check:'M5 12.5l4.5 4.5L19 7.5',
 drop:'M12 3c3.5 4.3 6 7.2 6 10.5a6 6 0 01-12 0C6 10.2 8.5 7.3 12 3z',
 alert:'M12 4l9 16H3L12 4zM12 10v4M12 17h.01',
 plus:'M12 5v14M5 12h14',back:'M15 5l-7 7 7 7',chev:'M9 5l7 7-7 7',close:'M6 6l12 12M18 6L6 18',
 sun:'M12 3v2M12 19v2M3 12h2M19 12h2M5.6 5.6L7 7M17 17l1.4 1.4M5.6 18.4L7 17M17 7l1.4-1.4',
 thermo:'M10 14V5a2 2 0 014 0v9a4 4 0 11-4 0z',
 tank:'M7 4h10l-1 16H8L7 4zM7.5 11h9',
 clock:'M12 7v5l3 2',
 eye:'M2.5 12S6 5.5 12 5.5 21.5 12 21.5 12 18 18.5 12 18.5 2.5 12 2.5 12z',
 moon:'M20 14.5A8 8 0 019.5 4 8 8 0 1020 14.5z',
 chip:'M8 8h8v8H8zM10 3v3M14 3v3M10 18v3M14 18v3M3 10h3M3 14h3M18 10h3M18 14h3',
 tfruit:'M12 8.5C8.5 6.5 4 8.5 4 13.5S7.5 21 12 21s8-2.5 8-7.5S15.500 6.500 12 8.500zM12 8.5c0-2.200 1-3.800 3.500-4.800',
 tflower:'M12 10a2 2 0 100 4 2 2 0 000-4zM12 10C10 6.500 11 3 12 3s2 3.500 0 7M14 12c3.500-2 7-1 7 0s-3.500 3-7 0M12 14c2 3.500 1 7 0 7s-2-3.500 0-7M10 12c-3.500 2-7 1-7 0s3.500-3 7 0',
 tsucc:'M9.500 21V7a2.500 2.500 0 015 0v14M9.500 15H7a1.500 1.500 0 01-1.500-1.500V10M14.500 13H17a1.500 1.500 0 001.500-1.500V8M6 21h12'
};
const EX={sun:'<circle cx="12" cy="12" r="4"/>',clock:'<circle cx="12" cy="12" r="9"/>',eye:'<circle cx="12" cy="12" r="3"/>'};
const ic=(n,s=22)=>`<svg width="${s}" height="${s}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="${IC[n]}"/>${EX[n]||''}</svg>`;

const API=window.SPApi;
const R={view:'garden',carePanel:null,detailTab:'now',settingsTab:'main',eventFilter:'all',eventDevice:'all',ui:{theme:'system',units:'C',motion:'auto'},nodes:[],catalog:[],records:new Map(),authorized:false,loading:true,selected:null,error:'',timer:null,refreshing:false,busy:false,calibration:null};
const quality={missing:'Нет данных',uncalibrated:'Нужна калибровка',ok:'Актуальные данные',fault:'Ошибка датчика',stale:'Устаревшие данные'};
const kinds={soil_index:'Влажность грунта',water_present:'Вода в баке',air_temperature:'Температура воздуха',air_humidity:'Влажность воздуха',illuminance:'Освещённость',battery_voltage:'Напряжение батареи',water_flow:'Расход воды'};
const units={percent:'%',boolean:'',degC:'°C',lux:'лк',V:'В','mL/s':'мл/с'};
const age=ms=>ms<2000?'только что':ms<60000?`${Math.floor(ms/1000)} с назад`:ms<3600000?`${Math.floor(ms/60000)} мин назад`:`${Math.floor(ms/3600000)} ч назад`;
function notify(text){$('#live').textContent=text;$('#app-notice').textContent=text;}
function artwork(){return `<div class="arch" aria-hidden="true"><svg width="110" height="155" viewBox="0 0 110 155"><path d="M55 107V42" stroke="var(--leaf-lo)" stroke-width="4"/><path d="M54 77C22 77 20 52 26 42C50 43 58 55 54 77Z" fill="var(--leaf-hi)"/><path d="M56 61C85 59 94 35 85 26C62 29 53 42 56 61Z" fill="var(--leaf-lo)"/><path d="M33 102H77L71 145H39Z" fill="var(--pot)"/><ellipse cx="55" cy="102" rx="23" ry="7" fill="var(--dirt)"/></svg></div>`;}
function nodeRecord(id){return R.records.get(id)||{};}
function nodeLink(n){const rec=nodeRecord(n.device_id);if(!R.authorized||R.error||rec.error)return 'Связь не проверена';return n.online?'На связи':'Нет связи';}
function soilReading(n){const s=nodeRecord(n.device_id).state?.soil;if(!s)return {value:'—',label:n.online?'Ожидаем измерение':'Нет связи',stale:true};const v=s.soil_index_pct??s.last_index_pct,confirmed=n.online&&!R.error&&!nodeRecord(n.device_id).error;return {value:v===null||v===undefined?'—':`${Math.round(v)}%`,label:(!confirmed?'Последнее измерение · ':'')+(quality[s.quality]||s.quality),stale:!confirmed||s.quality!=='ok'};}
function nav(){const items=[['garden','leaf','Растения'],['care','care','Уход'],['events','list','События'],['settings','gear','Настройки']];const current=R.view==='detail'?'garden':R.view;const html=items.map(([key,i,label])=>`<button data-action="nav" data-view="${key}" ${current===key?'aria-current="page"':''}>${ic(i,24)}<span>${label}</span></button>`).join('');$('#tabbar').innerHTML=html;$('#rail').innerHTML=`<div class="logo brand"><i></i></div>`+html;}
function sessionView(){return `<section class="live-session"><h1 class="disp">Ваш сад дома</h1><p class="lead" style="margin:16px 0">Войдите с ключом владельца, чтобы увидеть свои растения и подключить контроллеры.</p><form id="owner-login"><div class="fld"><label for="owner-key">Ключ администратора</label><input class="inp" id="owner-key" type="password" autocomplete="current-password" required maxlength="64"></div><details><summary>Загрузить access.json</summary><input id="owner-file" type="file" accept=".json,application/json"></details><button class="btn pri big" type="submit">Открыть мой сад</button></form><p class="live-help">Ключ создан программой установки. Он не сохраняется в хранилище браузера. Можно посмотреть отдельный пример без подключения к плате.</p></section>`;}
const V=window.SPViews.create({state:R,record:nodeRecord,escape:esc,ic,soil:soilReading,link:nodeLink,quality});
function gardenView(){return V.garden();}
function detailView(){return V.detail();}
function settingsView(){return V.settings();}
function render(){nav();$('#app-notice').textContent=R.error;if(R.loading){$('#view').innerHTML='<section class="live-empty"><h1 class="disp">Открываем ваш сад…</h1></section>';return;}$('#view').innerHTML=!R.authorized?sessionView():R.view==='settings'?settingsView():R.view==='detail'?detailView():R.view==='care'?V.care():R.view==='events'?V.events():gardenView();bindLogin();updateCalibration();}
async function refresh(){if(R.refreshing)return;clearTimeout(R.timer);R.refreshing=true;
 try{const registry=await API.request('/nodes');R.authorized=true;R.error='';const old=R.nodes;R.nodes=registry.nodes;if(!R.nodes.some(n=>n.device_id===R.selected))R.selected=R.nodes[0]?.device_id||null;
  const catalog=await API.request('/plants');R.catalog=catalog.plants;
  for(const n of R.nodes){const rec=nodeRecord(n.device_id);try{
   if(!rec.device||(!old.find(p=>p.device_id===n.device_id)?.online&&n.online)||Date.now()-(rec.deviceAt||0)>30000){rec.device=await API.request(`/nodes/${n.device_id}/device`);rec.deviceAt=Date.now();}
   rec.state=await API.request(`/nodes/${n.device_id}/state`);rec.error='';
   if((R.view==='events'||R.view==='detail'&&R.detailTab==='hist')&&Date.now()-(rec.eventsAt||0)>5000){
    try{const page=await API.request(`/nodes/${n.device_id}/events?after=${rec.eventsCursor||0}&limit=100`);const map=new Map((rec.events||[]).map(e=>[e.event_id,e]));for(const e of page.events)map.set(e.event_id,e);rec.events=[...map.values()].sort((a,b)=>a.seq-b.seq).slice(-100);rec.eventsCursor=page.next_cursor;rec.eventsError='';}
    catch(e){if(e.code==='cursor_expired'){rec.eventsCursor=0;rec.eventsGap=true;}else rec.eventsError=API.message(e);}
    rec.eventsLoaded=true;rec.eventsAt=Date.now();
   }
   if(R.view==='care'&&n.device_id===R.selected&&n.online&&Date.now()-(rec.configAt||0)>15000){try{rec.config=await API.request(`/nodes/${n.device_id}/config`);rec.configAt=Date.now();rec.configError='';}catch(e){rec.configError=API.message(e);}}

  }catch(e){if(e.status===0||e.status===401)throw e;rec.error=API.message(e);}R.records.set(n.device_id,rec);}
 }catch(e){if(e.status===401)R.authorized=false;else R.error=API.message(e);}
 finally{R.loading=false;R.refreshing=false;render();if(R.authorized)R.timer=setTimeout(refresh,document.hidden?15000:3000);}
}
function bindLogin(){const f=$('#owner-login');if(!f)return;f.onsubmit=async e=>{e.preventDefault();const input=$('#owner-key'),password=input.value;input.value='';try{await API.login(password);await refresh();}catch(err){R.error=API.message(err);render();}};$('#owner-file').onchange=async e=>{try{const file=e.target.files[0];if(!file||file.size>4096)throw Error('Выберите access.json');const a=JSON.parse(await file.text());if(typeof a.admin_password!=='string'||![32,64].includes(a.admin_password.length))throw Error('В файле нет ключа');$('#owner-key').value=a.admin_password;}catch(err){notify(err.message);}e.target.value='';};}
function sheet(title,body){const d=$('#sheet');d.innerHTML=`<div class="sheet"><div class="row"><h2 id="shT" class="disp">${esc(title)}</h2><button class="btn quiet" data-action="close">Закрыть</button></div><p id="sheet-message" class="live-help" role="status"></p>${body}</div>`;if(!d.open)d.showModal();}
function sheetMessage(e){const text=typeof e==='string'?e:API.message(e)+(e.uncertain?' Команда не повторяется автоматически. Сначала проверьте состояние.':'');const target=$('#sheet-message');if(target&&$('#sheet').open)target.textContent=text;else notify(text);}
async function mutate(fn){if(R.busy)return;R.busy=true;try{await fn();}catch(e){if($('#sheet').open)sheetMessage(e);else notify(API.message(e));}finally{R.busy=false;}}
async function plantProfile(continueSetup=false,initialStage){const n=R.nodes.find(n=>n.device_id===R.selected);if(!n)return;await mutate(async()=>{const meta=await API.request(`/nodes/${n.device_id}/plant`);sheet(continueSetup?'Настройка растения':'Моё растение','<div id="plant-editor"></div>');window.SPPlants.mount({host:$('#plant-editor'),escape:esc,api:API,device:n.device_id,meta,initialStage,art:window.SPViews.preview,rooms:[...new Set(R.catalog.map(p=>p.room).filter(Boolean))],continueSetup,onSaved:async()=>{$('#sheet').close();await refresh();notify('Сохранено на Hub');if(continueSetup)await sensorPanel();}});});}
async function wizard(){await plantProfile(true);}

async function getConfig(n){const rec=nodeRecord(n.device_id);const c=await API.request(`/nodes/${n.device_id}/config`);rec.config=c;R.records.set(n.device_id,rec);return c;}
async function sensorPanel(){const n=R.nodes.find(n=>n.device_id===R.selected);if(!n)return;const rec=nodeRecord(n.device_id);const descriptors=rec.device?.capabilities?.sensors||[];sheet('Датчики растения',`${!n.online?'<div class="banner">Нет связи. Калибровка недоступна.</div>':''}${descriptors.length?descriptors.map(s=>`<div class="row"><div><b>${esc(kinds[s.kind]||s.kind)}</b><div class="sub">${esc(s.sensor_id)} · ${esc(units[s.unit]??s.unit)}</div></div>${s.calibration==='soil_two_point'?`<button class="btn" data-action="calibrate" ${n.online?'':'disabled'}>Калибровать</button>`:'<span class="live-state">Калибровка не требуется</span>'}</div>`).join(''):'<p>Настроенных датчиков пока нет. Подключите оборудование и задайте проверенный профиль выводов по USB. Приложение не назначает GPIO автоматически.</p>'}<button class="btn" data-action="pause-auto" ${n.online?'':'disabled'}>Отключить автоматический полив</button><button class="btn pri big" data-action="setup-complete">Завершить настройку</button><p class="live-help">Новые типы измерений появятся здесь из описания драйверов устройства.</p>`);}
async function calibration(){const n=R.nodes.find(n=>n.device_id===R.selected);if(!n?.online)return;await mutate(async()=>{await getConfig(n);R.calibration={device:n.device_id,dry:null,wet:null};sheet('Калибровка влажности',`<p>Это индекс по двум точкам, а не физический процент воды в грунте.</p><p class="live-help">Сначала поместите датчик в выбранное сухое состояние и дождитесь стабильного показания. Затем повторите для влажного состояния. Не погружайте электронную часть в воду.</p><div class="live-reading"><small>Текущее измерение ADC</small><div id="calibration-raw" class="live-value">—</div><small id="calibration-quality"></small></div><div class="live-toolbar"><button class="btn" data-action="capture-dry">Запомнить сухую точку</button><button class="btn" data-action="capture-wet">Запомнить влажную точку</button></div><p id="calibration-points" class="live-help"></p><button class="btn pri big" data-action="save-calibration" disabled>Сохранить калибровку</button>`);updateCalibration();});}
function currentRaw(){const c=R.calibration,n=R.nodes.find(n=>n.device_id===c?.device),s=n&&nodeRecord(n.device_id).state?.soil;return n?.online&&s&&['ok','uncalibrated'].includes(s.quality)&&s.raw_adc!==null&&s.sample_age_ms<=5000?s.raw_adc:null;}
function updateCalibration(){if(!R.calibration||!$('#calibration-raw'))return;const raw=currentRaw();$('#calibration-raw').textContent=raw===null?'—':String(raw);$('#calibration-quality').textContent=raw===null?'Нет свежего измерения. Проверьте связь и датчик.':'Свежий исходный сигнал';for(const type of ['dry','wet'])$(`[data-action=capture-${type}]`).disabled=raw===null;const c=R.calibration;$('#calibration-points').textContent=`Сухая точка: ${c.dry??'—'} · влажная: ${c.wet??'—'}`;$('[data-action=save-calibration]').disabled=c.dry===null||c.wet===null||Math.abs(c.dry-c.wet)<200;}
async function completeSetup(){const n=R.nodes.find(n=>n.device_id===R.selected);if(!n)return;await mutate(async()=>{const m=await API.request(`/nodes/${n.device_id}/plant`);await API.request(`/nodes/${n.device_id}/plant`,'PUT',{...m,setup_stage:3});$('#sheet').close();await refresh();});}
async function saveCalibration(){const c=R.calibration,n=R.nodes.find(n=>n.device_id===c?.device);if(!n?.online||c.dry===null||c.wet===null||Math.abs(c.dry-c.wet)<200)return;await mutate(async()=>{const config=await getConfig(n);await API.request(`/nodes/${n.device_id}/config`,'PUT',{...config,soil_calibration:{dry_raw:c.dry,wet_raw:c.wet}},config.revision);sheetMessage('Калибровка сохранена на контроллере');$('[data-action=save-calibration]').disabled=true;for(const type of ['dry','wet'])$(`[data-action=capture-${type}]`).disabled=true;R.calibration=null;await refresh();});}
async function detach(){const n=R.nodes.find(n=>n.device_id===R.selected);if(!n)return;sheet('Отвязка контроллера',`<p>${n.online?'Устройство сначала отключит автоматический полив и сохранит ручной режим. Если насос работает, дождитесь завершения.':'Контроллер недоступен. Удаление на Hub не выключит его насос и автономный полив. Отключите исполнительные устройства физически; для новой привязки потребуется сброс C3.'}</p><label class="row"><input id="detach-ack" type="checkbox">${n.online?'Я понимаю, что связь контроллера будет отключена':'Я понимаю, что старая плата продолжит автономную работу'}</label><button class="btn danger big" data-action="confirm-detach">${n.online?'Отвязать контроллер':'Забыть недоступный контроллер'}</button><p class="live-help">Растение, его имя и комната сохранятся в разделе «Без контроллера».</p>`);}
async function confirmDetach(){if(!$('#detach-ack')?.checked){sheetMessage('Подтвердите действие');return;}const n=R.nodes.find(n=>n.device_id===R.selected);await mutate(async()=>{await API.request(`/nodes/${n.device_id}/${n.online?'detach':'forget'}`,'POST',{schema_version:1,operation_id:API.uuid(),acknowledge_autonomy:true});sheetMessage('Выполняем отвязку…');await pollMaintenance();});}
async function pollMaintenance(){const d=await API.request('/maintenance');if(d.phase==='completed'){$('#sheet').close();R.view='garden';await refresh();return;}if(['failed','unknown','storage_error'].includes(d.phase)){sheetMessage(d.message||'Действие не подтверждено. Проверьте состояние перед повтором.');if(!$('#maintenance-retry')){const b=document.createElement('button');b.id='maintenance-retry';b.className='btn';b.textContent='Продолжить отвязку';b.onclick=()=>mutate(async()=>{await API.request('/maintenance/retry','POST',{});b.remove();await pollMaintenance();});$('#sheet .sheet').append(b);}return;}setTimeout(()=>pollMaintenance().catch(sheetMessage),1500);}
document.addEventListener('click',e=>{const b=e.target.closest('[data-action]');if(!b||b.disabled)return;const a=b.dataset.action;
 if(a==='nav'){R.view=b.dataset.view;if(R.view==='care'&&!R.selected)R.selected=R.nodes[0]?.device_id;render();if(R.view==='care'||R.view==='events')refresh();}
 if(a==='open'){R.selected=b.dataset.id;R.view='detail';R.detailTab='now';R.calibration=null;render();}
 if(a==='close'){$('#sheet').close();R.calibration=null;}
 if(a==='refresh')refresh();
 if(a==='add')window.SmartPlantConnect?.open();
 if(a==='detail-tab'){R.detailTab=b.dataset.tab;render();if(R.detailTab==='hist')refresh();}
 if(a==='settings-tab'){R.settingsTab=b.dataset.tab;render();}
 if(a==='event-filter'){R.eventFilter=b.dataset.filter;render();}
 if(a==='event-device'){R.eventDevice=b.dataset.device;render();}
 if(a==='care-plant'){R.selected=b.dataset.device;R.carePanel=null;render();refresh();}
 if(a==='care-panel'&&['smart','manual','schedule','program'].includes(b.dataset.panel)){R.carePanel=b.dataset.panel;render();}
 if(a==='integrations')sheet('Интеграции',`<p class="live-help">Подключённых интеграций пока нет. Поддержка Home Assistant и внешнего хранения запланирована.</p><p class="note">Когда интеграции появятся, их можно будет подключить здесь.</p>`);
 if(a==='profile')plantProfile(false,b.dataset.growth);
 if(a==='edit-care')editCare();if(a==='appearance')appearance();
 if(a==='wizard')wizard();if(a==='sensors')sensorPanel();if(a==='calibrate')calibration();if(a==='setup-complete')completeSetup();
 if(a==='capture-dry'||a==='capture-wet'){const raw=currentRaw();if(raw!==null)R.calibration[a==='capture-dry'?'dry':'wet']=raw;updateCalibration();}
 if(a==='pause-auto')mutate(async()=>{const n=R.nodes.find(n=>n.device_id===R.selected)||R.nodes[0];if(!n)return;const c=await getConfig(n);await API.request(`/nodes/${n.device_id}/config`,'PUT',{...c,mode:'manual'},c.revision);nodeRecord(n.device_id).configAt=0;await refresh();sheetMessage('Автоматический полив отключён. Лимиты сохранены.');});
 if(a==='save-calibration')saveCalibration();if(a==='detach')detach();if(a==='confirm-detach')confirmDetach();
 if(a==='hub-update'||a==='device-update')window.SmartPlantUpdates?.open(a==='hub-update'?'hub':R.selected);
 if(a==='open-archived'){const p=R.catalog.find(p=>p.plant_id===b.dataset.plant);if(p)sheet(p.name,`<p class="live-help">${esc(p.room||'Без комнаты')} · контроллер не привязан.</p><button class="btn pri" data-action="replace" data-plant="${esc(p.plant_id)}">Привязать контроллер</button>`);}
 if(a==='replace')replacePlant(b.dataset.plant);
 if(a==='clock')mutate(async()=>{await API.request('/time','POST',{schema_version:1,utc_ms:Date.now()});notify('Время синхронизировано');});
 if(a==='logout')API.logout().then(()=>{R.authorized=false;render();}).catch(e=>notify(API.message(e)));
});
window.addEventListener('smartplant:unauthorized',()=>{R.authorized=false;clearTimeout(R.timer);});
window.addEventListener('smartplant:authorized',()=>{R.authorized=true;});
window.addEventListener('smartplant:registry',()=>refresh());
window.SmartPlantApp={refresh,API,openDevice(id){if(!R.nodes.some(n=>n.device_id===id))return;R.selected=id;R.view='detail';R.detailTab='now';R.calibration=null;render();},get nodes(){return R.nodes;},get catalog(){return R.catalog;}};
const notice=document.createElement('p');notice.id='app-notice';notice.className='live-notice';notice.setAttribute('role','status');$('.main').insertBefore(notice,$('#view'));
render();refresh();

function replacePlant(id){const plant=R.catalog.find(p=>p.plant_id===id);if(!plant)return;const fresh=R.nodes.filter(n=>n.setup_stage===0);sheet('Другой контроллер',`<p>Имя, комната и идентичность растения сохранятся. Датчики и насос новой платы нужно настроить заново.</p>${fresh.length?fresh.map(n=>`<button class="row" data-replace-device="${esc(n.device_id)}"><div><b>${esc(n.display_name)}</b><div class="sub">${esc(n.device_id)}</div></div>Выбрать</button>`).join(''):'<p class="live-help">Сначала добавьте новый C3, затем вернитесь сюда, не завершая его настройку как отдельного растения.</p>'}`);$('#sheet').querySelectorAll('[data-replace-device]').forEach(b=>b.onclick=()=>mutate(async()=>{await API.request(`/plants/${plant.plant_id}/controller`,'POST',{schema_version:1,revision:plant.revision,device_id:b.dataset.replaceDevice});$('#sheet').close();await refresh();}));}
document.addEventListener('visibilitychange',()=>{if(!document.hidden&&R.authorized)refresh();});
window.addEventListener('offline',()=>{R.error='Браузер потерял подключение к сети. Команды не ставятся в очередь.';render();});
window.addEventListener('online',()=>{if(R.authorized)refresh();});

function appearance(){sheet('Внешний вид',`<div class="fld"><label for="ui-theme">Тема</label><select class="inp" id="ui-theme">${[['system','Как в системе'],['light','Светлая'],['dark','Тёмная']].map(([value,label])=>`<option value="${value}" ${R.ui.theme===value?'selected':''}>${label}</option>`).join('')}</select></div><div class="fld"><label for="ui-units">Температура</label><select class="inp" id="ui-units"><option value="C" ${R.ui.units==='C'?'selected':''}>Градусы Цельсия, °C</option><option value="F" ${R.ui.units==='F'?'selected':''}>Градусы Фаренгейта, °F</option></select></div><label class="service-check"><input type="checkbox" id="ui-motion" ${R.ui.motion==='off'?'checked':''}>Отключить анимации</label><p class="live-help">Эти настройки сохраняются только в этом браузере.</p>`);for(const id of ['theme','units','motion'])$('#ui-'+id).onchange=()=>{R.ui={theme:$('#ui-theme').value,units:$('#ui-units').value,motion:$('#ui-motion').checked?'off':'auto'};try{localStorage.setItem('sp.ui.v1',JSON.stringify(R.ui));}catch{}applyAppearance();render();};}
function applyAppearance(){const root=document.documentElement;if(R.ui.theme==='system')delete root.dataset.theme;else root.dataset.theme=R.ui.theme;root.dataset.motion=R.ui.motion;}
try{const saved=JSON.parse(localStorage.getItem('sp.ui.v1'));if(saved&&['system','light','dark'].includes(saved.theme)&&['C','F'].includes(saved.units)&&['auto','off'].includes(saved.motion))R.ui=saved;}catch{}applyAppearance();
async function editCare(){const n=R.nodes.find(n=>n.device_id===R.selected);if(!n?.online)return;await mutate(async()=>{const c=await getConfig(n);const fields=[['dry_threshold_pct','Порог индекса влажности, %',1,95],['portion_ml','Порция, мл',10,300],['max_daily_ml','Максимум за сутки, мл',10,1000],['min_interval_minutes','Минимальная пауза, мин',60,10080],['max_run_seconds','Максимальная работа насоса, с',1,30]];sheet('Правило полива',`<form id="care-form">${fields.map(([key,label,min,max])=>`<div class="fld"><label for="care-${key}">${label}</label><input class="inp" type="number" id="care-${key}" value="${c[key]}" min="${min}" max="${max}" step="1" required></div>`).join('')}<p class="live-help">Режим, калибровка и остальные ограничения сохранятся. Контроллер проверит совместимость значений.</p><button class="btn pri big">Сохранить на контроллере</button></form>`);$('#care-form').onsubmit=e=>{e.preventDefault();mutate(async()=>{const next={...c};for(const [key] of fields)next[key]=Number($('#care-'+key).value);await API.request(`/nodes/${n.device_id}/config`,'PUT',next,c.revision);nodeRecord(n.device_id).configAt=0;$('#sheet').close();await refresh();});};});}
