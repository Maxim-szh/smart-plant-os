import {readFileSync} from 'node:fs';
import assert from 'node:assert/strict';
import Ajv from 'ajv';
import formats from 'ajv-formats';
const root='spec/maintenance/v1/',schema=JSON.parse(readFileSync(root+'schema.json'));
const ajv=new Ajv({strict:false});formats(ajv);ajv.addSchema(schema);
for(const type of Object.keys(schema.$defs)){
 const validate=ajv.compile({$ref:schema.$id+'#/$defs/'+type}),example=JSON.parse(readFileSync(root+'examples/'+type+'.json'));
 assert(validate(example),JSON.stringify(validate.errors));assert(!validate({...example,unknown:1}),type+' rejects unknown keys');
 if(type==='UpdateChunk'){assert(!validate({...example,data:'x0'}));assert(!validate({...example,data:'00'.repeat(769)}));assert(!validate({...example,offset:-1}));}
 if(type==='BeginUpdate'){assert(!validate({...example,profile:'s3'}));assert(!validate({...example,size:2097153}));assert(!validate({...example,sha256:'0'}));}
}
console.log('PASS: maintenance/OTA JSON contracts and negative cases');
