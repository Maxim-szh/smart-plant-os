'use strict';
/* Serial script loading fits the Hub's TLS connection budget. Retry only failed GETs. */
(()=>{
 const setup=document.currentScript?.dataset.app==='setup';
 const modules=setup?[['api.js','SPApi'],['care.js','SPDrafts'],['updates.js','SmartPlantUpdates'],['network.js','SPWifi'],['setup.js',null]]:[
  ['api.js','SPApi'],['care.js','SPDrafts'],['plant-data.js','SPPlantData'],['plants.js','SPPlants'],['views.js','SPViews'],['network.js','SPWifi'],['system.js','SmartPlantSystem'],['updates.js','SmartPlantUpdates'],['garden.js','SPGarden'],['home.js','SPHome'],['pairing.js','SmartPlantConnect'],['app.js','SmartPlantApp']
 ];
 function load(file,exported){return new Promise((resolve,reject)=>{
  const script=document.createElement('script');script.src='/'+file;script.async=false;
  const timer=setTimeout(()=>finish(new Error('Загрузка приложения заняла слишком много времени.')),10000);
  const runtimeError=e=>{if(e.filename===script.src)finish(new Error('Не удалось запустить приложение.'));};
  function finish(error,retry=false){clearTimeout(timer);window.removeEventListener('error',runtimeError);script.onload=script.onerror=null;script.remove();if(error){error.retry=retry;reject(error);}else resolve();}
  script.onload=()=>finish(exported&&!window[exported]?new Error('Не удалось подготовить приложение.'):null);
  script.onerror=()=>finish(new Error('Соединение с контроллером прервалось.'),true);
  window.addEventListener('error',runtimeError);document.head.append(script);
 });}
 function failed(){
  const host=document.querySelector('#view')||document.querySelector('main');if(!host)return;
  const section=document.createElement('section');section.className='empty';section.setAttribute('role','alert');
  const title=document.createElement('h1');title.className='disp';title.textContent='Не удалось открыть приложение';
  const help=document.createElement('p');help.className='note';help.textContent='Часть приложения не загрузилась. Проверьте связь с главным контроллером и повторите загрузку.';
  const button=document.createElement('button');button.className='btn pri';button.textContent='Повторить загрузку';button.onclick=()=>location.reload();section.append(title,help,button);host.replaceChildren(section);
 }
 (async()=>{for(const [file,exported]of modules){
  const note=document.querySelector('.app-loading .note');if(note)note.textContent='Подготавливаем приложение…';
  try{await load(file,exported);}catch(e){if(!e.retry)throw e;await new Promise(resolve=>setTimeout(resolve,200));await load(file,exported);}
 }})().catch(failed);
})();
