'use strict';
// Rasterize the source vector with the existing browser dependency, no external fonts/network.
const fs=require('node:fs'),path=require('node:path'),{chromium}=require('playwright');
(async()=>{const root=path.resolve(__dirname,'..'),svg=fs.readFileSync(path.join(__dirname,'assets/app-icon.svg'),'utf8');
 const browser=await chromium.launch({headless:true,...(process.env.BENCH_BROWSER?{executablePath:process.env.BENCH_BROWSER}:{})});
 try{const page=await browser.newPage({deviceScaleFactor:1});
  for(const [size,name]of [[180,'apple-touch-icon.png'],[192,'icon-192.png'],[512,'icon-512.png']]){
   await page.setViewportSize({width:size,height:size});await page.setContent('<style>html,body{margin:0;padding:0}svg{display:block;width:100vw;height:100vh}</style>'+svg);
   await page.locator('svg').screenshot({path:path.join(root,'web',name),omitBackground:false});
  }
  const inner=svg.slice(svg.indexOf('>')+1,svg.lastIndexOf('</svg>')).replace(/<title>[\s\S]*?<\/title>/,'').trim();
  const index=path.join(root,'web/index.html'),html=fs.readFileSync(index,'utf8');
  const mark='<svg class="app-mark" width="28" height="28" viewBox="0 0 512 512" aria-hidden="true" focusable="false">'+inner+'</svg>';
  fs.writeFileSync(index,html.replace(/<img src="\/icon-192\.png"[^>]*>|<svg class="app-mark"[\s\S]*?<\/svg>/,mark));
  console.log('Generated opaque 180/192/512px app icons from tools/assets/app-icon.svg');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
