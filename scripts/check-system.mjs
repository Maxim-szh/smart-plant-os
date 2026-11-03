import {readFileSync} from 'node:fs';
import assert from 'node:assert/strict';
import Ajv from 'ajv';
import formats from 'ajv-formats';
const root='spec/system/v1/',schema=JSON.parse(readFileSync(root+'schema.json'));
const ajv=new Ajv({strict:false,allErrors:true});formats(ajv);ajv.addSchema(schema);
for(const name of Object.keys(schema.$defs)){
 const valid=ajv.compile({$ref:schema.$id+'#/$defs/'+name}),example=JSON.parse(readFileSync(root+'examples/'+name+'.json'));
 assert(valid(example),name+JSON.stringify(valid.errors));assert(!valid({...example,password:'should-not-appear'}));
 if(name==='Backup'){const bad=structuredClone(example);bad.plants[0].device_id='spo-010203040506';assert(!valid(bad));bad.plants=[...Array(21)].map(()=>example.plants[0]);assert(!valid(bad));}
 if(name==='BackupRestoreRequest')assert(!valid({...example,confirm:false}));
}
const manifest=JSON.parse(readFileSync('web/manifest.webmanifest'));assert.equal(manifest.start_url,'/');assert.equal(manifest.display,'standalone');for(const icon of manifest.icons)assert(readFileSync('web'+icon.src).length>100);assert(!readFileSync('web/index.html','utf8').includes('demo.js'));
console.log('PASS: system/backup contracts, unknown-field rejection and local install assets');
