'use strict';
/* One explicit scan request, bounded read-only polling; shared by setup and settings. */
(()=>{async function scan({request=window.SPApi.request,active=()=>true}={}){
 await request('/setup/wifi/scan','POST',{});const until=Date.now()+22000;
 while(active()&&Date.now()<until){await new Promise(r=>setTimeout(r,700));if(!active())return null;const result=await request('/setup/wifi/scan');if(!active())return null;if(result.phase==='scanning')continue;if(result.phase!=='completed')throw Error('Поиск не завершился. Можно ввести название сети вручную.');return result.networks;}
 if(!active())return null;throw Error('Время поиска истекло. Введите название сети вручную.');
}window.SPWifi={scan};})();
