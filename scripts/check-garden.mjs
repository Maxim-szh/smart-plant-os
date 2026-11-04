import {readFileSync} from 'node:fs';
import assert from 'node:assert/strict';
import Ajv from 'ajv/dist/2020.js';import formats from 'ajv-formats';
const root='spec/garden/v1/',schema=JSON.parse(readFileSync(root+'schema.json'));const ajv=new Ajv({strict:false,allErrors:true});formats(ajv);ajv.addSchema(schema);
for(const type of Object.keys(schema.$defs)){const valid=ajv.getSchema(schema.$id+'#/$defs/'+type),data=JSON.parse(readFileSync(root+'examples/'+type+'.json'));assert(valid(data),type+JSON.stringify(valid.errors));assert(!valid({...data,admin_password:'must-not-be-accepted'}));if(type==='NewDiaryEntry'){assert(!valid({...data,remind_at_ms:null}));assert(!valid({...data,note:'x'.repeat(161)}));}}
assert(!readFileSync('web/plants.js','utf8').includes('p.latin'));assert(!readFileSync('web/views.js','utf8').includes('p.latin'));
console.log('PASS: diary/history/browser contracts, unknown fields, reminder constraints and Russian-only plant labels');
