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
   if(!r.ok)throw new ApiError(json.error?.message||`HTTP ${r.status}`,r.status,method!=='GET'&&['request_outcome_unknown','invalid_node_response'].includes(json.error?.code),json.error?.code);
   return json;
  }catch(e){if(e instanceof ApiError)throw e;throw new ApiError('Нет ответа от устройства',0,method!=='GET');}
  finally{clearTimeout(timeout);}
 }
 async function login(password,remember=true){const device_name=/iPhone|iPad|Android/i.test(navigator.userAgent)?'Мой телефон':'Мой компьютер';const r=await request('/session','POST',{password,remember,device_name});token=r.token||'';window.dispatchEvent(new Event('smartplant:authorized'));}
 async function logout(){await request('/session','DELETE');token='';window.dispatchEvent(new Event('smartplant:unauthorized'));}
 const labels={care_service_active:'Сначала завершите обслуживание. Автополив сейчас отключён',care_service_unavailable:'Не удалось прочитать или сохранить обслуживание. Настройки не сбрасываются',care_service_invalid:'Проверьте запрос обслуживания',care_service_busy:'Дождитесь завершения полива или текущей операции',care_service_unverified:'Ручной режим ещё не подтверждён. Обновите состояние контроллера',care_invalid:'Проверьте запись, дату и длину заметки',entry_conflict:'Запись уже существует с другим содержимым. Обновите дневник',entry_not_found:'Запись уже удалена. Обновите дневник',device_not_found:'Устройство уже удалено из списка',login_rate_limit:'Подождите минуту перед следующей попыткой входа',sessions_unavailable:'Не удалось запомнить устройство. Проверьте список доверенных устройств или выберите временный вход',device_name_invalid:'Название устройства должно занимать от 1 до 48 байт',care_full:'Дневник заполнен. Сохраните экспорт и удалите ненужные записи',care_unavailable:'Дневник временно недоступен',history_unavailable:'История пока недоступна',system_busy:'Дождитесь завершения текущей операции',scan_busy:'Поиск занят или запущен недавно. Повторите через 30 секунд',setup_busy:'Дождитесь завершения настройки сети',recovery_unavailable:'Не удалось открыть точку настройки',backup_invalid:'Файл настроек не подходит: проверьте формат и содержимое',backup_preview_expired:'Настройки изменились. Выберите файл ещё раз и проверьте новый предпросмотр',firmware_verifying:'Прошивка проверяет запуск. Подождите около 30 секунд',update_busy:'Дождитесь завершения обновления',incompatible_image:'Образ предназначен другой плате или несовместимой версии данных',unsupported_target:'Контроллер не на связи или требует USB-обновления',node_busy_or_automatic:'Отключите автоматический полив и дождитесь завершения текущего цикла',node_rolled_back:'Контроллер вернулся к предыдущей прошивке',radio_timeout:'Связь при обновлении не подтвердилась. Проверьте питание и доступность контроллера',unauthorized:'Введите ключ владельца',node_offline:'Контроллер не на связи',identity_pending:'Контроллер ещё передаёт сведения',storage_fault:'Не удалось сохранить настройки',revision_conflict:'Настройки изменились в другом окне. Загрузите актуальные перед сохранением',config_rejected:'Контроллер отклонил настройки. Проверьте актуальную версию и ограничения',config_invalid:'Проверьте значения настройки',request_outcome_unknown:'Результат команды неизвестен — сначала проверьте состояние',pairing_busy_or_unavailable:'Подключение занято или устройство уже связано',maintenance_busy:'Дождитесь завершения обслуживания',migration_required:'Сначала выполните переход на OTA-разметку по USB',update_incompatible:'Образ не подходит этой плате'};
 function uuid(){const b=crypto.getRandomValues(new Uint8Array(16));b[6]=(b[6]&15)|64;b[8]=(b[8]&63)|128;const s=[...b].map(n=>n.toString(16).padStart(2,'0')).join('');return `${s.slice(0,8)}-${s.slice(8,12)}-${s.slice(12,16)}-${s.slice(16,20)}-${s.slice(20)}`;}
 window.SPApi={request,login,logout,uuid,ApiError,message(e){return labels[e.code||e.message]||e.message;}};
})();
