'use strict';
(() => {
 let token='';
 class ApiError extends Error {constructor(message,status=0,uncertain=false,code=""){super(message);this.status=status;this.uncertain=uncertain;this.code=code;}}
 async function request(path,method='GET',body,revision){
  const headers={};if(token)headers.Authorization=`Bearer ${token}`;
  if(body!==undefined)headers['Content-Type']='application/json';
  if(revision!==undefined)headers['If-Match']=String(revision);
  const controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),12000);
  try{
   const r=await fetch('/api/v1'+path,{method,headers,credentials:'same-origin',cache:'no-store',signal:controller.signal,body:body===undefined?undefined:JSON.stringify(body)});
   const json=await r.json();
   if(r.status===401){token='';window.dispatchEvent(new Event('smartplant:unauthorized'));}
   if(!r.ok)throw new ApiError(json.error?.message||`HTTP ${r.status}`,r.status,false,json.error?.code);
   return json;
  }catch(e){if(e instanceof ApiError)throw e;throw new ApiError('Нет ответа от устройства',0,method!=='GET');}
  finally{clearTimeout(timeout);}
 }
 async function login(password){const r=await request('/session','POST',{password});token=r.token;window.dispatchEvent(new Event('smartplant:authorized'));}
 async function logout(){try{await request('/session','DELETE');}finally{token='';window.dispatchEvent(new Event('smartplant:unauthorized'));}}
 const labels={system_busy:'Дождитесь завершения текущей операции',scan_busy:'Поиск занят или запущен недавно. Повторите через 30 секунд',setup_busy:'Дождитесь завершения настройки сети',recovery_unavailable:'Не удалось открыть точку настройки',backup_invalid:'Файл настроек не подходит: проверьте формат и содержимое',backup_preview_expired:'Настройки изменились. Выберите файл ещё раз и проверьте новый предпросмотр',firmware_verifying:'Прошивка проверяет запуск. Подождите около 30 секунд',update_busy:'Дождитесь завершения обновления',incompatible_image:'Образ предназначен другой плате или несовместимой версии данных',unsupported_target:'Контроллер не на связи или требует USB-обновления',node_busy_or_automatic:'Отключите автоматический полив и дождитесь завершения текущего цикла',node_rolled_back:'Контроллер вернулся к предыдущей прошивке',radio_timeout:'Связь при обновлении не подтвердилась. Проверьте питание и доступность контроллера',unauthorized:'Введите ключ владельца',node_offline:'Контроллер не на связи',identity_pending:'Контроллер ещё передаёт сведения',storage_fault:'Не удалось сохранить настройки',revision_conflict:'Настройки изменились. Обновите их перед сохранением',config_invalid:'Проверьте значения настройки',request_outcome_unknown:'Результат команды неизвестен — сначала проверьте состояние',pairing_busy_or_unavailable:'Подключение занято или устройство уже связано',maintenance_busy:'Дождитесь завершения обслуживания',migration_required:'Сначала выполните переход на OTA-разметку по USB',update_incompatible:'Образ не подходит этой плате'};
 function uuid(){const b=crypto.getRandomValues(new Uint8Array(16));b[6]=(b[6]&15)|64;b[8]=(b[8]&63)|128;const s=[...b].map(n=>n.toString(16).padStart(2,'0')).join('');return `${s.slice(0,8)}-${s.slice(8,12)}-${s.slice(12,16)}-${s.slice(16,20)}-${s.slice(20)}`;}
 window.SPApi={request,login,logout,uuid,ApiError,message(e){return labels[e.code||e.message]||e.message;}};
})();
