'use strict';
/* Read-only explanations. The controller remains the authority for all watering decisions. */
(()=>{
 const fields=[['dry_threshold_pct','Порог индекса влажности, %',1,95],['portion_ml','Порция, мл',10,300],['max_daily_ml','Максимум за сутки, мл',10,1000],['min_interval_minutes','Минимальная пауза, мин',60,10080],['max_run_seconds','Максимальная работа насоса, с',1,30]];
 function review(c){
  const errors=[],warnings=[];for(const [key,label,lo,hi] of [...fields,['sensor_max_age_seconds','Возраст показания, с',30,900]])if(!Number.isInteger(c[key])||c[key]<lo||c[key]>hi)errors.push(`${label}: от ${lo} до ${hi}, целое число.`);
  if(!['manual','automatic_threshold'].includes(c.mode))errors.push('Режим полива не поддерживается.');
  if(c.max_daily_ml<c.portion_ml)errors.push('Суточный лимит меньше одной порции.');
  const cal=c.soil_calibration,soil=!!cal&&Number.isInteger(cal.dry_raw)&&Number.isInteger(cal.wet_raw)&&cal.dry_raw>=0&&cal.dry_raw<=4095&&cal.wet_raw>=0&&cal.wet_raw<=4095&&Math.abs(cal.dry_raw-cal.wet_raw)>=200;
  if(cal!==null&&!soil)errors.push('Калибровка грунта должна содержать две точки ADC с разницей не меньше 200.');
  const flow=c.pump_calibration_ml_per_second,ul=Math.round(flow*1000),pump=typeof flow==='number'&&Number.isFinite(flow)&&flow>0&&flow<=1000&&Math.abs(flow*1000-ul)<=1e-6;
  if(flow!==null&&!pump)errors.push('Проверьте калибровку насоса: от 0 до 1000 мл/с, не больше трёх знаков после запятой.');
  const run=pump?Math.ceil(c.portion_ml*1000000/ul):null;
  if(pump&&c.portion_ml*1000>c.max_run_seconds*ul)errors.push('Насос не успеет выдать порцию за разрешённое время. Уменьшите порцию или проверьте калибровку и лимит времени.');
  if(c.mode==='automatic_threshold'&&(!soil||!pump))errors.push('Для автоматического режима нужны калибровки грунта и насоса.');
  if(!pump)warnings.push('Насос не откалиброван. Правило можно сохранить в ручном режиме, запуск полива будет заблокирован.');
  if(!soil)warnings.push('Грунт не откалиброван. Автоматический полив пока недоступен.');
  return {errors,warnings,run_ms:run,soil_calibrated:soil,pump_calibrated:pump};
 }
 function readiness(n,r,c,elapsed=0){
  const s=r.state,cap=r.device?.capabilities,online=!!n?.online&&!r.error,checks=[];
  const add=(key,title,ok,detail,action)=>checks.push({key,title,ok:!!ok,detail,action});
  add('link','Связь с контроллером',online,online?'Контроллер отвечает.':'Восстановите питание и связь с C3.','controller');
  add('hardware','Описание оборудования',online&&cap?.hardware_known,cap?.hardware_known?'Получен профиль подключённого оборудования.':'Оборудование ещё не объявлено прошивкой.','sensors');
  add('config','Настройки полива',online&&!!c, c?'Настройки получены с контроллера.':'Не удалось прочитать настройки.','edit-care');
  const v=c?review(c):null;
  add('soil','Датчик грунта и калибровка',online&&cap?.soil_configured&&v?.soil_calibrated,'Нужны настроенный вход и две точки калибровки.','sensors');
  add('measurement','Свежие показания грунта',online&&s?.soil?.quality==='ok'&&Number.isFinite(s.soil.sample_age_ms)&&s.soil.sample_age_ms+elapsed<=(c?.sensor_max_age_seconds||0)*1000,'Ошибка, отсутствие данных и устаревшее значение не считаются сухой землёй.','sensors');
  add('pump','Насос и калибровка подачи',online&&cap?.pump&&v?.pump_calibrated,'Объём без расходомера остаётся расчётным.','sensors');
  add('tank','Вода в баке',online&&cap?.tank_sensor&&s?.tank?.quality==='ok'&&s.tank.water_present===true,'Нужен исправный датчик наличия воды и актуальное подтверждение контроллера.','sensors');
  add('clock','Время контроллера',online&&s?.clock_state==='synced','Время необходимо для учёта суточного лимита.','clock');
  add('limits','Согласованные ограничения',online&&v?.errors.length===0,v?.errors[0]||'Порция, лимит за сутки и максимальное время не противоречат друг другу.','edit-care');
  add('idle','Исполнительная часть свободна',online&&s?.pump?.state==='off'&&s?.watering?.active_command_id===null,'Во время действующего полива нужно дождаться его завершения.');
  add('decision','Последняя проверка защит',online&&['ok','soil_not_dry'].includes(s?.last_decision?.reason_code),'Пауза, суточный лимит и ошибки контроллера могут временно блокировать полив.');
  return checks;
 }
 const version=s=>{const m=typeof s==='string'&&s.match(/^(0|[1-9]\d{0,2})\.(0|[1-9]\d{0,2})\.(0|[1-9]\d{0,2})(?:-abi1)?$/);return m&&m.slice(1).every(x=>Number(x)<=255)?m.slice(1).reduce((a,x)=>a*256+Number(x),0):null;};
 function compatibility(n,d,hub){
  if(!n?.online)return {level:'unknown',title:'Совместимость пока не проверена',detail:'Контроллер не на связи. Сохранённая версия не подтверждает его текущее состояние.'};
  if(!d||!d.capabilities?.hardware_known||version(d.firmware_version)===null||!hub)return {level:'unknown',title:'Недостаточно сведений',detail:'Обновите сведения, когда обе платы будут доступны.'};
  if(d.api_version!=='1.0')return {level:'blocked',title:'Версия API не поддерживается',detail:'Нужна прошивка с API 1.0. Настройки не будут отправлены из редактора.'};
  if(hub.data_abi!==1)return {level:'blocked',title:'Формат данных Hub не поддерживается',detail:'Не устанавливайте пакет другой версии формата данных без инструкции по миграции.'};
  return {level:'ok',title:'Базовые функции доступны',detail:'API 1.0 поддерживается. Разные номера прошивок S3 и C3 сами по себе не означают ошибку. Дополнительные функции определяются возможностями контроллера.'};
 }
 function packageReview(m,bin,s,target,currentVersion){
  if(!m||typeof m!=='object'||Array.isArray(m))return 'manifest.json должен содержать описание пакета прошивки.';
  const profile=target==='hub'?s.profile:'c3',v=version(m.version),hub=version(s.firmware_version),current=version(currentVersion);
  if(m.format!==2||m.layout!=='ota-v1'||m.profile!==profile||m.data_abi!==s.data_abi||s.data_abi!==1||typeof m.version!=='string'||!m.version.endsWith('-abi1')||v===null||m.app?.file!==bin.name||m.app?.size!==bin.size||!Number.isInteger(bin.size)||bin.size<128||bin.size>(profile==='c3'?0x1e0000:0x200000))return 'Пакет не подходит к выбранной плате, файлу или формату данных. Возьмите manifest.json и .bin из одного пакета для этой платы.';
  if(hub===null||(target!=='hub'&&current===null))return 'Не удалось определить установленную версию. Обновите сведения перед установкой.';
  if(v<hub||(current!==null&&v<current))return 'Этот пакет старее установленной прошивки. Обычное OTA не выполняет понижение версии; восстановление после неудачного запуска — отдельный механизм.';
  return null;
 }
 const checks={fields,review,readiness,version,compatibility,packageReview};
 if(typeof module!=='undefined'&&module.exports)module.exports=checks;
 if(typeof window==='undefined')return;
 window.SPCare=checks;

 /* Drafts stay in memory. Never put credentials or unfinished settings into browser storage. */
 const drafts=new Map();let prompt=null,pendingAction=null,pendingDialog=null;
 const formValue=form=>JSON.stringify([...form.querySelectorAll('input,textarea,select')].filter(x=>!['submit','button','file'].includes(x.type)).map(x=>[x.id||x.name,x.type==='checkbox'||x.type==='radio'?x.checked:x.value]));
 function dirty(d){return !!d.open&&!!drafts.get(d)?.some(x=>x.root.isConnected&&(x.pending||x.read()!==x.base));}
 function clean(d){for(const x of drafts.get(d)||[]){if(x.root.isConnected)x.base=x.read();x.pending=false;}if(prompt?.open&&pendingDialog===d){pendingAction=null;prompt.close();}}
 function confirmLeave(action,d){if(!dirty(d)){action();return;}if(pendingAction)return;
  if(!prompt){prompt=document.createElement('dialog');prompt.id='unsaved-dialog';prompt.className='service-dialog';prompt.setAttribute('aria-labelledby','unsaved-title');prompt.innerHTML='<div class="sheet"><h2 class="disp" id="unsaved-title">Есть несохранённые изменения</h2><p>Остаться и продолжить редактирование или выйти без сохранения?</p><p class="live-help" id="unsaved-help"></p><div class="live-toolbar"><button class="btn pri" id="unsaved-stay">Остаться</button><button class="btn" id="unsaved-discard">Выйти без сохранения</button></div></div>';document.body.append(prompt);prompt.querySelector('#unsaved-stay').onclick=()=>{pendingAction=null;prompt.close();};prompt.querySelector('#unsaved-discard').onclick=()=>{const fn=pendingAction;pendingAction=null;clean(pendingDialog);prompt.close();fn?.();};prompt.addEventListener('cancel',()=>{pendingAction=null;});}
  const saving=(drafts.get(d)||[]).some(x=>x.pending);prompt.querySelector('#unsaved-discard').disabled=saving;prompt.querySelector('#unsaved-help').textContent=saving?'Запрос уже отправлен. Дождитесь ответа; закрытие окна не отменяет операцию.':'';pendingAction=action;pendingDialog=d;prompt.showModal();prompt.querySelector('#unsaved-stay').focus();
 }
 function watch(root,read=()=>formValue(root),base){const d=root.closest('dialog');if(!d||d===prompt)return;
  if(!drafts.has(d)){drafts.set(d,[]);const close=d.close.bind(d);d.close=(...args)=>confirmLeave(()=>close(...args),d);d.addEventListener('cancel',e=>{if(dirty(d)){e.preventDefault();confirmLeave(()=>close(),d);}});d.addEventListener('close',()=>drafts.set(d,[]));}
  const rows=(drafts.get(d)||[]).filter(x=>x.root.isConnected&&x.root!==root);rows.push({root,read,base:base===undefined?read():base,pending:false});drafts.set(d,rows);
 }
 function observe(){for(const f of document.querySelectorAll('dialog form')){const d=f.closest('dialog');if(d!==prompt&&!(drafts.get(d)||[]).some(x=>x.root===f))watch(f);}}
 new MutationObserver(observe).observe(document.body,{childList:true,subtree:true});
 window.addEventListener('beforeunload',e=>{if([...drafts.keys()].some(dirty)){e.preventDefault();e.returnValue='';}});
 document.addEventListener('click',e=>{const a=e.target.closest('a[href]');if(!a)return;const d=[...drafts.keys()].find(dirty);if(d){e.preventDefault();e.stopImmediatePropagation();confirmLeave(()=>{window.location.href=a.href;},d);}},true);
 window.SPDrafts={watch,clean,dirty,attempt(d,action){confirmLeave(action,d);},pending(root,value){if(!root)return;const d=root.closest('dialog');for(const x of drafts.get(d)||[])if(x.root===root)x.pending=value;if(prompt?.open){prompt.querySelector('#unsaved-discard').disabled=!!value;}},forget(root){const d=root.closest('dialog');drafts.set(d,(drafts.get(d)||[]).filter(x=>x.root!==root));}};
})();
