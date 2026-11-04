'use strict';
/* Descriptive plant profiles. No watering commands or guessed measurements. */
(()=>{
 const data=window.SPPlantData;
 const get=id=>data.plants.find(p=>p.id===(data.aliases[id]||id))||null;
 const stages=id=>data.stages[get(id)?.group]||[['seed','Росток'],['grow','Рост'],['adult','Взрослое'],['flower','Цветение'],['fruit','Плодоношение'],['rest','Покой']];
 const stageLabel=(id,stage)=>stages(id).find(s=>s[0]===stage)?.[1]||'Стадия не указана';
 const name=id=>get(id)?.name||(id?'Другой вид':'Вид не выбран');
 function mount(o){
  const esc=o.escape,host=o.host,meta=o.meta,draft={name:meta.name,room:meta.room||'',species_id:meta.species_id,growth_stage:o.initialStage&&stages(meta.species_id).some(s=>s[0]===o.initialStage)?o.initialStage:meta.growth_stage||null};let group=get(meta.species_id)?.group||'all',saving=false;
  host.innerHTML=`<form id="profile-form"><div class="fld"><label for="plant-name">Имя растения</label><input class="inp" id="plant-name" value="${esc(draft.name)}" maxlength="80" required autocomplete="off"></div><div class="fld"><label for="plant-room">Комната</label><input class="inp" id="plant-room" value="${esc(draft.room)}" maxlength="80" list="profile-rooms"><datalist id="profile-rooms">${o.rooms.map(r=>`<option value="${esc(r)}"></option>`).join('')}</datalist></div><section class="grp"><h2>Выберите растение</h2><div class="chips" id="profile-groups" role="group" aria-label="Группа растений"></div><div id="profile-picker" class="profile-picker"></div><button type="button" class="chip" data-species="" id="profile-other">Другое растение</button></section><section class="grp"><h2>Стадия роста</h2><div class="chips" id="profile-stages" role="group" aria-label="Стадия роста"></div><p class="note">Выберите стадию по внешнему виду растения. Датчики её не определяют.</p></section><section class="grp"><h2>Так будет выглядеть карточка</h2><div id="profile-preview" aria-live="polite"></div></section><p class="note">Вид и стадия не меняют настройки полива.</p><button class="btn pri big" id="profile-save">${o.continueSetup?'Сохранить и перейти к датчикам':'Сохранить растение'}</button><p class="note" id="profile-message" role="status"></p></form>`;
  const $=s=>host.querySelector(s);
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
  $('#profile-form').onsubmit=async e=>{e.preventDefault();if(saving)return;read();const name=draft.name.trim(),room=draft.room.trim();if(!name||new TextEncoder().encode(name).length>80||new TextEncoder().encode(room).length>80){$('#profile-message').textContent='Имя и комната должны занимать не больше 80 байт каждое.';return;}
   saving=true;host.querySelectorAll('button,input').forEach(x=>x.disabled=true);$('#profile-message').textContent='Сохраняем на Hub…';
   try{await o.api.request(`/nodes/${o.device}/plant`,'PUT',{schema_version:1,revision:meta.revision,name,room,species_id:draft.species_id,growth_stage:draft.growth_stage,setup_stage:o.continueSetup?2:meta.setup_stage||0});await o.onSaved();}
   catch(err){$('#profile-message').textContent=err.code==='revision_conflict'?'Растение уже изменено в другом окне. Закройте редактор и откройте его снова.':o.api.message(err);}
   finally{saving=false;host.querySelectorAll('button,input').forEach(x=>x.disabled=false);}
  };paint();
 }
 window.SPPlants={get,stages,stageLabel,name,mount};
})();
