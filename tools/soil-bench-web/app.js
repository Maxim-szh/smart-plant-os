'use strict';
const $ = id => document.getElementById(id);
let port, active, latest, history = [], last, runs = [], finished;
const csvCell = v => '"' + String(v ?? '').replace(/"/g, '""') + '"';
function download(run, json = false) {
  const rows = [['sensor','sample','measured_gross_g','note','utc','elapsed_ms','seq','device_ms','read_index','raw_adc','flag']];
  for (const f of run.frames) f.raw.forEach((v,i) => rows.push([run.sensor,run.sample,run.mass,run.note,f.utc,f.elapsed_ms,f.seq,f.device_ms,i,v,v === null ? 'adc_error' : v === 0 || v === 4095 ? 'rail' : '']));
  const data = json ? JSON.stringify(run,null,2) : '\uFEFF' + rows.map(r=>r.map(csvCell).join(',')).join('\r\n');
  const url = URL.createObjectURL(new Blob([data], {type:json?'application/json':'text/csv;charset=utf-8'}));
  const a=document.createElement('a'); a.href=url; a.download=`soil-${run.id}.${json?'json':'csv'}`; a.click(); setTimeout(()=>URL.revokeObjectURL(url),1000);
}
function finish(reason) {
  if (!active) return;
  const run=active; active=null; run.outcome=reason; run.finished_utc=new Date().toISOString();
  finished=run; runs.push(run);
  for(const id of ['sensor','sample','mass','note']) $(id).disabled=false;
  $('stop').disabled=true; $('save').disabled=false; $('json').disabled=false;
  const values=run.frames.flatMap(f=>f.raw).filter(Number.isInteger).sort((a,b)=>a-b);
  $('progress').textContent=reason==='complete'?'Минута записана':'Запись прервана: '+reason;
  $('summary').textContent=values.length?`Чтений: ${values.length}; минимум ${values[0]}, максимум ${values.at(-1)}, медиана ${values[Math.floor(values.length/2)]}. Замечаний о связи: ${run.events.length}.`:'Данных нет — проверьте подключение.';
  const li=document.createElement('li'); li.textContent=`${run.sensor} · ${run.sample} · ${reason} `;
  for(const isJson of [false,true]) {const b=document.createElement('button'); b.textContent=isJson?'JSON':'CSV'; b.onclick=()=>download(run,isJson); li.append(b);}
  $('runs').append(li); download(run);
}
function accept(f) {
  if(f.kind!=='soil_bench'||f.v!==1||f.gpio!==3||f.firmware!=='bench-1'||!Number.isSafeInteger(f.seq)||f.seq<0||!Number.isSafeInteger(f.device_ms)||f.device_ms<0||!Array.isArray(f.raw)||f.raw.length!==9||!f.raw.every(v=>v===null||(Number.isInteger(v)&&v>=0&&v<=4095))) return;
  const now=performance.now(); latest=now;
  if(active) {
    if(last && (f.seq<=last.seq || f.device_ms<last.device_ms)) {active.events.push('device_restart');finish('перезапуск платы');}
    else {if(last && f.seq!==last.seq+1) active.events.push(`sequence_gap:${last.seq}->${f.seq}`);
      active.frames.push({...f,utc:new Date().toISOString(),elapsed_ms:Math.round(now-active.started)});}
  }
  last=f;
  const a=f.raw.filter(Number.isInteger).sort((a,b)=>a-b); const v=a.length?a[Math.floor(a.length/2)]:null;
  $('value').textContent=v??'Ошибка ADC';history.push(v);history=history.slice(-120);
  const ctx=$('graph').getContext('2d');ctx.clearRect(0,0,760,150);ctx.strokeStyle='#40855a';ctx.beginPath();
  history.forEach((v,i)=>{if(v!==null)ctx.lineTo(i*760/119,145-v*140/4095);else ctx.moveTo(i*760/119,145);});ctx.stroke();
}
$('connect').onclick=async()=>{
  try {
    if(!navigator.serial) throw Error('Нужен Chrome или Edge на компьютере, адрес localhost.');
    if(port) throw Error('Порт уже открыт. Для нового подключения обновите страницу после сохранения опытов.');
    port=await navigator.serial.requestPort();await port.open({baudRate:115200});
    $('status').textContent='USB подключён. Ожидаю диагностическую прошивку…';$('connect').disabled=true;
    const reader=port.readable.getReader();let buffer='';const decoder=new TextDecoder();
    try {for(;;){const {value,done}=await reader.read();if(done)break;buffer+=decoder.decode(value,{stream:true});let p;
      while((p=buffer.indexOf('\n'))>=0){const line=buffer.slice(0,p).trim();buffer=buffer.slice(p+1);try{accept(JSON.parse(line));}catch{}}
      if(buffer.length>4096)buffer='';
    }} finally {reader.releaseLock();finish('USB отключён');try{await port.close();}catch{}port=null;$('connect').disabled=false;$('status').textContent='USB отключён';latest=null;last=null;}
  } catch(e){$('status').textContent=e.message;if(port&&!port.readable)port=null;}
};
$('start').onclick=()=>{
  if(!latest||performance.now()-latest>2000||active)return;
  const mass=$('mass').value;if(mass!==''&&(!Number.isFinite(Number(mass))||Number(mass)<0))return;
  active={schema:1,id:new Date().toISOString().replace(/[:.]/g,'-'),sensor:$('sensor').value,sample:$('sample').value,mass:mass===''?null:Number(mass),note:$('note').value,gpio:3,adc_bits:12,attenuation:'12dB',burst_reads:9,period_ms:500,started:performance.now(),started_utc:new Date().toISOString(),frames:[],events:[]};
  for(const id of ['sensor','sample','mass','note'])$(id).disabled=true;
  $('stop').disabled=false;$('summary').textContent='';
};
$('stop').onclick=()=>finish('остановлено пользователем');$('save').onclick=()=>finished&&download(finished);$('json').onclick=()=>finished&&download(finished,true);
setInterval(()=>{
  const fresh=latest!==undefined&&latest!==null&&performance.now()-latest<2000;
  $('start').disabled=!fresh||!!active;
  if(fresh)$('status').textContent='C3 на связи · GPIO3 · bench-1';
  else if(port)$('status').textContent='Нет свежих данных. Проверьте плату и прошивку.';
  if(active){const elapsed=performance.now()-active.started;$('progress').textContent=`Запись: ${Math.min(60,Math.floor(elapsed/1000))} / 60 с · пакетов ${active.frames.length}`;
    if(!fresh){active.events.push('stream_timeout');finish('пропала связь');}
    else if(elapsed>=60000)finish('complete');}
},200);
window.addEventListener('beforeunload',e=>{if(active||runs.length){e.preventDefault();e.returnValue='';}});
