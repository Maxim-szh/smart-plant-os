'use strict';
const $ = selector => document.querySelector(selector);
let token = '', timer = null, busy = false, pendingSsid = null, changingWifi = false;
const viaSetupAP = location.hostname === '192.168.4.1';
const phase = {idle:'Домашняя сеть ещё не настроена',connecting:'Проверяем новую сеть…',connected:'Подключено',failed:'Новая сеть недоступна. Прежняя конфигурация сохранена.',storage_error:'Не удалось сохранить настройки. Прежняя конфигурация восстановлена.'};
const message = text => { $('#message').textContent = text; };
function session() {
  if (!token) {
    for (const id of ['network-panel','ready-panel','management']) $(`#${id}`).hidden = true;
    setStep('login');
  } $('#login').hidden = !!token; $('#dashboard').hidden = !token; if (!token) {clearTimeout(timer); timer = null;} }
function setStep(step) {
  for (const id of ['login','wifi','ready']) {
    const el = $(`#step-${id}`);
    if (id === step) el.setAttribute('aria-current','step'); else el.removeAttribute('aria-current');
  }
}
async function request(path, method = 'GET', body) {
  const headers = {}; if (token) headers.Authorization = `Bearer ${token}`;
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  const controller = new AbortController(), timeout = setTimeout(() => controller.abort(), 9000);
  try {
    const response = await fetch(`/api/v1${path}`, {method,headers,body:body === undefined ? undefined : JSON.stringify(body),cache:'no-store',signal:controller.signal});
    const data = await response.json();
    if (response.status === 401) {token = ''; session();}
    if (!response.ok) throw Object.assign(new Error(data.error?.message || `HTTP ${response.status}`), {status:response.status});
    return data;
  } finally {clearTimeout(timeout);}
}
async function refresh() {
  clearTimeout(timer);
  if (!token) return;
  try {
    const data = await request('/setup');
    const ready = data.wifi_connected && data.wifi_phase === 'connected' &&
      /^(\d{1,3}\.){3}\d{1,3}$/.test(data.ip) && data.ip.split('.').every(n => +n <= 255);
    $('#network-panel').hidden = ready && !changingWifi;
    $('#ready-panel').hidden = !ready || changingWifi;
    $('#management').hidden = !ready || viaSetupAP || changingWifi;
    setStep(ready && !changingWifi ? 'ready' : 'wifi');
    const open = $('#open-plants'); open.hidden = !ready;
    if (ready) {open.href = `https://${data.ip}/`; $('#home-address').textContent = `https://${data.ip}/`;}
    else {open.removeAttribute('href'); $('#home-address').textContent = '';}
    $('#network-status').textContent = phase[data.wifi_phase] || data.wifi_phase;
    if (pendingSsid !== null) {
      if (data.wifi_phase === 'failed' || data.wifi_phase === 'storage_error') {message(phase[data.wifi_phase]);pendingSsid = null;}
      else if (data.wifi_connected && data.ssid === pendingSsid && data.wifi_phase === 'connected') {message('Подключение подтверждено. Подключите телефон к домашней сети и откройте адрес Hub ниже. Точка SmartPlantOS скоро выключится.');pendingSsid = null;}
    }
    $('#ssid-value').textContent = data.ssid || '—';
    $('#channel-value').textContent = data.channel || '—';
    $('#ap-value').textContent = data.setup_ap_enabled === undefined ? '—' : data.setup_ap_enabled ? 'Включена' : 'Выключена';
    const ip = $('#ip-value'); ip.replaceChildren();
    if (data.wifi_connected && /^\d{1,3}(\.\d{1,3}){3}$/.test(data.ip)) {
      const a = document.createElement('a'); a.href = `https://${data.ip}/`; a.textContent = data.ip; ip.append(a);
    } else ip.textContent = '—';
    $('#wifi-button').disabled = busy || data.wifi_phase === 'connecting';$('#wifi-scan').disabled=busy||data.wifi_phase==='connecting';
    $('#nodes').replaceChildren(...data.nodes.map(node => {const li = document.createElement('li');li.textContent = node.device_id;return li;}));
  } catch(e) {if (token) message('Нет ответа от Hub. Если меняли сеть, подключитесь к SmartPlantOS заново. Повторное сохранение не выполняется автоматически.');}
  if (token) timer = setTimeout(refresh, 3000);
}
async function action(button, fn) {
  if (busy) return; busy = true; button.disabled = true;
  try {await fn();} catch(e) {message(`Не удалось завершить действие: ${e.message}. Проверьте состояние перед повтором.`);}
  finally {busy = false; button.disabled = false; session();}
}
$('#access-file').onchange = async () => {
  try {
    const file = $('#access-file').files[0];if (!file || file.size > 4096) throw new Error('Неверный файл доступа');
    const access = JSON.parse(await file.text());
    if (typeof access.admin_password !== 'string' || ![32,64].includes(access.admin_password.length)) throw new Error('В файле нет ключа администратора');
    $('#admin').value = access.admin_password;message('Ключ загружен локально. Нажмите «Продолжить».');
  } catch(e) {message(e.message);} finally {$('#access-file').value = '';}
};
$('#login-form').onsubmit = e => {e.preventDefault();action($('#login-button'), async () => {
  const password = $('#admin').value; $('#admin').value = '';
  token = (await request('/session','POST',{password})).token; session();message('Устройство подтверждено. Проверяем настройки сети…');await refresh();
});};
$('#wifi-form').onsubmit = e => {e.preventDefault();action($('#wifi-button'), async () => {
  const ssid = $('#ssid').value, password = $('#wifi-password').value;
  if (new TextEncoder().encode(ssid).length > 32) throw new Error('Название сети должно занимать не более 32 байт');
  if (!/^[\x20-\x7e]{8,63}$/.test(password)) throw new Error('Пароль: 8–63 печатных ASCII-символа');
  $('#wifi-password').value = '';
  message('Подключаемся. При отключении телефона повторно выберите SmartPlantOS.');
  changingWifi = false;
  pendingSsid = ssid;
  try {await request('/setup/wifi','POST',{ssid,password});}
  catch(e) {
    if (e.status) {pendingSsid = null;throw e;}
    message('Соединение изменилось во время настройки. Проверяем состояние; повторно отправлять пароль не нужно. При необходимости подключитесь к SmartPlantOS заново.');
  }
  await refresh();
});};
$('#passport').onclick = () => action($('#passport'), async () => {
  const data = await request('/setup/passport');
  const url = URL.createObjectURL(new Blob([JSON.stringify(data,null,2)],{type:'application/json'}));
  const link = document.createElement('a');link.href=url;link.download='hub-passport.json';document.body.append(link);link.click();link.remove();setTimeout(()=>URL.revokeObjectURL(url),1000);
});
$('#pair-form').onsubmit = e => {e.preventDefault();action($('#pair-button'),async () => {
  const file = $('#pairing').files[0];if (!file || file.size > 1024) throw new Error('Выберите файл привязки до 1 КиБ');
  const data = JSON.parse(await file.text());$('#pairing').value = '';
  await request('/setup/peers','POST',data);message('Контроллер добавлен. Его связь и состояние доступны в API Hub.');await refresh();
});};
$('#change-wifi').onclick = () => {changingWifi = true; $('#network-panel').hidden = false; $('#ready-panel').hidden = true; $('#management').hidden = true; setStep('wifi'); $('#ssid').focus();};
$('#logout').onclick = () => action($('#logout'),async () => {try {await request('/session','DELETE');} finally {token='';session();message('Сеанс завершён.');}});
session();

$("#open-updates").onclick=()=>window.SmartPlantUpdates.open("hub");

$('#wifi-scan').onclick=async()=>{if(busy)return;await action($('#wifi-scan'),async()=>{
 $('#wifi-button').disabled=true;message('Ищем сети 2,4 ГГц…');const networks=await window.SPWifi.scan({request,active:()=>!!token});if(!networks)return;const picker=$('#wifi-networks');picker.replaceChildren();const first=document.createElement('option');first.value='';first.textContent='Выберите сеть';picker.append(first);for(const network of networks){const option=document.createElement('option');option.value=network.ssid;option.textContent=network.ssid+' · '+network.rssi_dbm+' dBm'+(network.supported?'':' · не поддерживается');option.disabled=!network.supported;picker.append(option);}$('#wifi-network-picker').hidden=false;message(networks.length?'Выберите сеть или введите название вручную.':'Сети не найдены. Введите название вручную.');});$('#wifi-button').disabled=false;};
$('#wifi-networks').onchange=e=>{if(e.target.value){$('#ssid').value=e.target.value;$('#wifi-password').focus();}};
