'use strict';
// The transport owns authentication; views never construct radio packets or GPIO commands.
class HubSource {
  token = '';
  async request(path, method = 'GET', body, revision) {
    const headers = {};
    if (this.token) headers.Authorization = `Bearer ${this.token}`;
    if (body !== undefined) headers['Content-Type'] = 'application/json';
    if (revision !== undefined) headers['If-Match'] = String(revision);
    const response = await fetch(`/api/v1${path}`, {method, headers, body: body === undefined ? undefined : JSON.stringify(body), signal: AbortSignal.timeout(8000), cache: 'no-store'});
    const data = await response.json();
    if (response.status === 401) this.token = '';
    if (!response.ok) throw Object.assign(new Error(data.error?.message || data.reason_code || `HTTP ${response.status}`), {status: response.status, data});
    return data;
  }
  async login(password) { this.token = (await this.request('/session', 'POST', {password})).token; }
  node(id, resource, method, body, revision) {return this.request(`/nodes/${encodeURIComponent(id)}/${resource}`, method, body, revision);}
}
const api = new HubSource();
const $ = s => document.querySelector(s);
const escapeHTML = value => String(value ?? '—').replace(/[&<>"']/g, x => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[x]));
const quality = {ok:'достоверно',missing:'нет данных',uncalibrated:'нужна калибровка',fault:'ошибка',stale:'устарело'};
let selected = null, config = null, refreshing = false;
function notice(message) {$('#status').textContent = message;}
function session() {const active = !!api.token;$('#login').hidden = active;$('#dashboard').hidden = !active;$('#logout').hidden = !active;}
async function run(action) {try {await action();} catch(e) {notice(`Ошибка: ${e.message}. Команда автоматически не повторяется.`);} finally {session();}}
function retained(id) {try {return JSON.parse(sessionStorage.getItem(`sp-command:${id}`));} catch {return null;}}
async function refresh() {
  if (refreshing || !api.token) return;
  refreshing = true;
  try {
    const registry = await api.request('/nodes');
    const cards = [];
    for (const node of registry.nodes) {
      let state = null, failure = null;
      if (node.online) {try {state = await api.node(node.device_id, 'state');} catch(e) {failure = e.message;}}
      cards.push(`<article class="card"><small>${escapeHTML(node.device_id)}</small><h2>${escapeHTML(node.display_name)}</h2>${state ? `<div class="value">${state.soil.soil_index_pct === null ? '—' : escapeHTML(state.soil.soil_index_pct) + '%'}</div><small>Индекс почвы · ${escapeHTML(quality[state.soil.quality])}</small><p>Бак: ${state.tank.water_present === null ? 'нет данных' : state.tank.water_present ? 'вода есть' : 'пуст'}<br>Насос: ${state.pump.state === 'running' ? 'работает' : 'выключен'}<br>Решение: ${escapeHTML(state.last_decision.reason_code || state.last_decision.status)}</p>` : `<p class="offline">${escapeHTML(failure || 'Нет связи')}</p>`}<button data-open="${escapeHTML(node.device_id)}" ${node.online ? '' : 'disabled'}>Открыть</button></article>`);
    }
    $('#plants').innerHTML = cards.join('') || '<p>В этой установке нет зарегистрированных растений.</p>';
    $('#plants').querySelectorAll('[data-open]').forEach(b => b.onclick = () => run(() => open(b.dataset.open)));
  } finally {refreshing = false;}
}
const fields = {dry_threshold_pct:'Порог индекса, %',portion_ml:'Порция, мл',max_daily_ml:'Суточный предел, мл',max_run_seconds:'Максимальное время, с',min_interval_minutes:'Минимальный интервал, мин',sensor_max_age_seconds:'Допустимый возраст измерения, с',pump_calibration_ml_per_second:'Калибровка насоса, мл/с'};
async function open(id) {
  selected = id;
  const state = await api.node(id, 'state');
  config = await api.node(id, 'config');
  const remembered = retained(id);
  const target = state.watering.active_command_id || remembered?.command_id;
  $('#detail').hidden = false;
  $('#detail').innerHTML = `<h2>${escapeHTML(state.plant.name)}</h2><small>${escapeHTML(id)} · версия настроек ${config.revision}</small><div class="grid">${(state.sensors || []).map(s => `<div><small>${escapeHTML(s.kind)} · ${escapeHTML(s.sensor_id)}</small><div class="value">${s.value === null ? '—' : escapeHTML(s.value)}</div><small>${escapeHTML(s.unit)} · ${escapeHTML(quality[s.quality])}</small></div>`).join('')}</div><label>Разовый полив, мл<input id="volume" type="number" min="10" max="300" step="1" value="${config.portion_ml}"></label><div class="actions"><button id="water" class="primary">Полить</button><button id="stop" ${target ? '' : 'disabled'}>Остановить последний полив</button><button id="query" ${remembered ? '' : 'disabled'}>Проверить последнюю команду</button></div><details><summary>Настройки и калибровка</summary><p>Проверяйте значения на своём стенде. Калибровка почвы — индекс, а не физическая влажность.</p><form id="settings"><label>Режим<select name="mode"><option value="manual">Вручную</option><option value="automatic_threshold">По порогу</option></select></label>${Object.entries(fields).map(([key,label]) => `<label>${label}<input name="${key}" type="number" step="${key.startsWith('pump') ? '0.001' : '1'}" value="${config[key] ?? ''}"></label>`).join('')}<label>Сухая почва, ADC<input name="dry_raw" type="number" min="0" max="4095" value="${config.soil_calibration?.dry_raw ?? ''}"></label><label>Влажная почва, ADC<input name="wet_raw" type="number" min="0" max="4095" value="${config.soil_calibration?.wet_raw ?? ''}"></label><button class="primary">Сохранить настройки</button></form></details><details><summary>История устройства</summary><button id="history">Загрузить события</button><pre id="events"></pre></details>`;
  $('#settings').elements.mode.value = config.mode;
  $('#water').onclick = () => run(async () => {
    const requested_ml = Number($('#volume').value);
    if (!Number.isInteger(requested_ml) || requested_ml < 10 || requested_ml > 300) throw new Error('Допустимо 10–300 мл');
    if (!confirm(`Запросить полив ${requested_ml} мл?`)) return;
    const command = {schema_version:1,command_id:crypto.randomUUID(),requested_ml};
    // Persist the ID before sending. A timeout must never create a second watering command.
    sessionStorage.setItem(`sp-command:${id}`, JSON.stringify(command));
    $('#water').disabled = true;
    $('#stop').disabled = false;$('#query').disabled = false;
    notice(`Команда ${command.command_id}: отправляется. При потере ответа используйте проверку или остановку.`);
    const result = await api.node(id,'commands/water','POST',command);
    notice(`${result.status}: ${result.reason_code || 'команда принята'}. Объём оценочный. ID ${command.command_id}`);
  });
  $('#query').onclick = () => run(async () => {const previous = retained(id);if (!previous) return;const result = await api.node(id,`commands/${previous.command_id}`);notice(JSON.stringify(result,null,2));});
  $('#stop').onclick = () => run(async () => {
    const target_command_id = state.watering.active_command_id || retained(id)?.command_id || target;
    if (!target_command_id) return;
    const result = await api.node(id,'commands/stop','POST',{schema_version:1,command_id:crypto.randomUUID(),target_command_id});notice(JSON.stringify(result,null,2));
  });
  $('#settings').onsubmit = e => {e.preventDefault();run(async () => {
    const values = new FormData(e.target), next = {...config,mode:values.get('mode')};
    for (const key of Object.keys(fields)) next[key] = values.get(key) === '' && key.startsWith('pump') ? null : Number(values.get(key));
    next.soil_calibration = values.get('dry_raw') === '' && values.get('wet_raw') === '' ? null : {dry_raw:Number(values.get('dry_raw')),wet_raw:Number(values.get('wet_raw'))};
    if (!confirm('Сохранить настройки и ограничения полива? Изменение режима может разрешить автоматический полив.')) return;
    config = await api.node(id,'config','PUT',next,config.revision);notice(`Сохранено на ESP32, revision ${config.revision}`);
  });};
  $('#history').onclick = () => run(async () => {
    try {const events = await api.node(id,'events?after=0&limit=50');$('#events').textContent = JSON.stringify(events,null,2);}
    catch(e) {if(e.status!==410) throw e; const first=e.data.error.oldest_seq;const events=await api.node(id,`events?after=${first-1}&limit=50`);$('#events').textContent='Ранние события уже удалены из ограниченной истории.\n'+JSON.stringify(events,null,2);}
  });
}
$('#login').onsubmit = e => {e.preventDefault();const password=$('#password').value;$('#password').value='';run(async () => {await api.login(password);session();notice('Подключено. Перед поливом синхронизируйте время и проверьте настройки.');await refresh();});};
$('#logout').onclick = () => run(async () => {try {await api.request('/session','DELETE');} finally {api.token='';$('#detail').hidden=true;notice('Сессия завершена.');}});
$('#refresh').onclick = () => run(refresh);
$('#time').onclick = () => run(async () => {await api.request('/time','POST',{schema_version:1,utc_ms:Date.now()});notice('Время принято Hub; дождитесь передачи на ESP32.');});
setInterval(() => {if(api.token) run(refresh);},10000);
