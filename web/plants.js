'use strict';
/* Descriptive plant profiles. No watering commands or guessed measurements. */
(()=>{
 const data=window.SPPlantData;
 const get=id=>data.plants.find(p=>p.id===(data.aliases[id]||id))||null;
 const stages=id=>data.stages[get(id)?.group]||[['seed','Росток'],['grow','Рост'],['adult','Взрослое'],['flower','Цветение'],['fruit','Плодоношение'],['rest','Покой']];
 const stageLabel=(id,stage)=>stages(id).find(s=>s[0]===stage)?.[1]||'Стадия не указана';
 const name=id=>get(id)?.name||(id?'Другой вид':'Вид не выбран');

 function mountBrief(o){
  const host=o.host,esc=o.escape,meta=o.meta;let busy=false,blocked=false;
  host.innerHTML=`<form id="brief-profile"><h3 class="disp">Карточка растения</h3><div class="fld"><label for="brief-name">Имя растения</label><input class="inp" id="brief-name" value="${esc(meta.name)}" maxlength="80" required></div><div class="fld"><label for="brief-room">Комната</label><input class="inp" id="brief-room" value="${esc(meta.room||'')}" maxlength="80" list="brief-rooms"><datalist id="brief-rooms">${o.rooms.map(r=>`<option value="${esc(r)}"></option>`).join('')}</datalist></div><div class="fld"><label for="brief-species">Вид растения</label><select class="inp" id="brief-species"><option value="">Укажу позже</option>${data.plants.map(p=>`<option value="${p.id}" ${get(meta.species_id)?.id===p.id?'selected':''}>${esc(p.name)}</option>`).join('')}</select></div><div id="brief-preview" class="brief-preview"></div><p class="note">Стадию роста, датчики и полив можно настроить позже в карточке растения.</p><button class="btn pri big" id="brief-save">Сохранить растение</button><p id="brief-message" class="note" role="status"></p></form>`;
  const $=s=>host.querySelector(s),f=$('#brief-profile'),paint=()=>{$('#brief-preview').innerHTML=o.art($('#brief-species').value,null);};paint();window.SPDrafts.watch(f);
  $('#brief-species').onchange=()=>{if($('#brief-name').value===meta.name&&!meta.species_id)$('#brief-name').value=get($('#brief-species').value)?.name||meta.name;paint();};
  f.onsubmit=async e=>{e.preventDefault();if(busy||blocked)return;const name=$('#brief-name').value.trim(),room=$('#brief-room').value.trim(),species_id=$('#brief-species').value||null;if(!name||new TextEncoder().encode(name).length>80||new TextEncoder().encode(room).length>80){$('#brief-message').textContent='Имя и комната должны занимать не больше 80 байт.';return;}busy=true;window.SPDrafts.pending(f,true);f.querySelectorAll('button,input,select').forEach(x=>x.disabled=true);
   try{await o.api.request(`/nodes/${o.device}/plant`,'PUT',{schema_version:1,revision:meta.revision,name,room,species_id,growth_stage:species_id===meta.species_id?meta.growth_stage||null:null,setup_stage:meta.setup_stage});await o.onSaved();}
   catch(e){blocked=e.status===409||e.uncertain;if($('#brief-message'))$('#brief-message').textContent=e.status===409?'Карточка уже изменена. Откройте её заново перед сохранением.':o.api.message(e);}
   finally{busy=false;window.SPDrafts.pending(f,false);f.querySelectorAll('button,input,select').forEach(x=>x.disabled=false);if($('#brief-save'))$('#brief-save').disabled=blocked;}
  };
 }

 function mount(o){
  if(o.compact)return mountBrief(o);
  const esc=o.escape,host=o.host,meta=o.meta,draft={name:meta.name,room:meta.room||'',species_id:meta.species_id,growth_stage:o.initialStage&&stages(meta.species_id).some(s=>s[0]===o.initialStage)?o.initialStage:meta.growth_stage||null};let group=get(meta.species_id)?.group||'all',saving=false,blocked=false,step=o.initialStage?2:0;
  host.innerHTML=`<form id="profile-form"><nav class="profile-progress" aria-label="Шаги настройки"><span id="profile-step-label" role="status"></span><progress id="profile-step-progress" max="4" value="1" aria-label="Шаг настройки"></progress></nav><section data-profile-step="0"><h3 id="profile-step-name" tabindex="-1">Имя и комната</h3><div class="fld"><label for="plant-name">Имя растения</label><input class="inp" id="plant-name" value="${esc(draft.name)}" maxlength="80" required autocomplete="off"></div><div class="fld"><label for="plant-room">Комната</label><input class="inp" id="plant-room" value="${esc(draft.room)}" maxlength="80" list="profile-rooms"><datalist id="profile-rooms">${o.rooms.map(r=>`<option value="${esc(r)}"></option>`).join('')}</datalist></div></section><section class="grp" data-profile-step="1" hidden><h3 tabindex="-1">Выберите растение</h3><div class="chips" id="profile-groups" role="group" aria-label="Группа растений"></div><div id="profile-picker" class="profile-picker"></div><button type="button" class="chip" data-species="" id="profile-other">Другое растение</button></section><section class="grp" data-profile-step="2" hidden><h3 tabindex="-1">Стадия роста</h3><div class="chips" id="profile-stages" role="group" aria-label="Стадия роста"></div><p class="note">Выберите стадию по внешнему виду растения. Датчики её не определяют.</p></section><section class="grp" data-profile-step="3" hidden><h3 tabindex="-1">Проверьте карточку</h3><div id="profile-preview" aria-live="polite"></div></section><p class="note">Вид и стадия не меняют настройки полива.</p><div class="form-actions profile-navigation"><button type="button" class="btn" id="profile-back">Назад</button><button type="button" class="btn pri" id="profile-next">Дальше</button><button class="btn pri" id="profile-save" hidden>${o.continueSetup?'Сохранить и перейти к датчикам':'Сохранить растение'}</button></div><p class="note" id="profile-message" role="status"></p></form>`;
  const $=s=>host.querySelector(s);
  function showStep(focus=false){
   host.querySelectorAll('[data-profile-step]').forEach(el=>{el.hidden=Number(el.dataset.profileStep)!==step;});
   $('#profile-step-label').textContent=`Шаг ${step+1} из 4`;
   $('#profile-step-progress').value=step+1;$('#profile-back').hidden=step===0;$('#profile-next').hidden=step===3;$('#profile-save').hidden=step!==3;
   if(focus)host.querySelector(`[data-profile-step="${step}"] h3`).focus();
  }
  function advance(){read();if(!$('#plant-name').value.trim()||!$('#plant-name').checkValidity()){step=0;showStep();$('#plant-name').reportValidity();return;}step=Math.min(3,step+1);showStep(true);}
  $('#profile-next').onclick=advance;$('#profile-back').onclick=()=>{step=Math.max(0,step-1);showStep(true);};
  function read(){draft.name=$('#plant-name').value;draft.room=$('#plant-room').value;}
  function preview(){const p=get(draft.species_id);$('#profile-preview').innerHTML=`<div class="profile-preview">${o.art(draft.species_id,draft.growth_stage)}<div><h3 class="disp">${esc(draft.name.trim()||p?.name||'Моё растение')}</h3><p class="t2">${esc(name(draft.species_id))}</p><p class="note">${esc(stageLabel(draft.species_id,draft.growth_stage))}</p><p class="note">${esc(draft.room.trim()||'Без комнаты')}</p></div></div>`;}
  function paint(){
   $('#profile-groups').innerHTML=[['all','Все'],...Object.entries(data.groups)].map(([key,label])=>`<button type="button" class="chip" data-profile-group="${key}" aria-pressed="${group===key}">${label}</button>`).join('');
   $('#profile-picker').innerHTML=data.plants.filter(p=>group==='all'||p.group===group).map(p=>`<button type="button" class="profile-choice" data-species="${p.id}" aria-pressed="${get(draft.species_id)?.id===p.id}"><span class="profile-thumb">${o.art(p.id,null)}</span><span class="profile-label">${esc(p.name)}</span></button>`).join('');
   $('#profile-other').setAttribute('aria-pressed',String(!get(draft.species_id)));$('#profile-other').textContent=draft.species_id&&!get(draft.species_id)?'Другой вид · '+draft.species_id:'Другое растение';
   $('#profile-stages').innerHTML=[[null,'Не указана'],...stages(draft.species_id)].map(([key,label])=>`<button type="button" class="chip" data-growth-stage="${key||''}" aria-pressed="${draft.growth_stage===key}">${label}</button>`).join('');preview();
  }
  host.onclick=e=>{if(saving)return;const b=e.target.closest('button');if(!b)return;
   if(b.hasAttribute('data-profile-group')){read();group=b.dataset.profileGroup;paint();}
   if(b.hasAttribute('data-species')){read();const previous=get(draft.species_id);draft.species_id=b.dataset.species||null;const p=get(draft.species_id);if(!draft.name.trim()||draft.name===previous?.name)draft.name=p?.name||'Моё растение';draft.growth_stage=null;$('#plant-name').value=draft.name;paint();}
   if(b.hasAttribute('data-growth-stage')){read();draft.growth_stage=b.dataset.growthStage||null;paint();}
  };
  for(const id of ['plant-name','plant-room'])$('#'+id).oninput=()=>{read();preview();};
  $('#profile-form').onsubmit=async e=>{e.preventDefault();if(saving||blocked)return;if(step<3){advance();return;}read();const name=draft.name.trim(),room=draft.room.trim();if(!name||new TextEncoder().encode(name).length>80||new TextEncoder().encode(room).length>80){$('#profile-message').textContent='Имя и комната должны занимать не больше 80 байт каждое.';return;}
   saving=true;window.SPDrafts?.pending($('#profile-form'),true);host.querySelectorAll('button,input').forEach(x=>x.disabled=true);$('#profile-message').textContent='Сохраняем на Hub…';
   try{await o.api.request(`/nodes/${o.device}/plant`,'PUT',{schema_version:1,revision:meta.revision,name,room,species_id:draft.species_id,growth_stage:draft.growth_stage,setup_stage:o.continueSetup?2:meta.setup_stage||0});await o.onSaved();}
   catch(err){blocked=err.status===409||err.uncertain;$('#profile-message').textContent=err.code==='revision_conflict'?'Растение уже изменено в другом окне. Закройте редактор и откройте его снова.':o.api.message(err);}
   finally{saving=false;window.SPDrafts?.pending($('#profile-form'),false);host.querySelectorAll('button,input').forEach(x=>x.disabled=false);const saveButton=$('#profile-save');if(saveButton)saveButton.disabled=blocked;}
  };paint();showStep();window.SPDrafts?.watch($('#profile-form'),()=>{read();return JSON.stringify(draft);},JSON.stringify({name:meta.name,room:meta.room||'',species_id:meta.species_id,growth_stage:meta.growth_stage||null}));
 }
 window.SPPlants={get,stages,stageLabel,name,mount};
})();
