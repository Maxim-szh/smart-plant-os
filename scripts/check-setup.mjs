import {readFileSync} from 'node:fs';
import assert from 'node:assert/strict';
import Ajv from 'ajv';
const root='spec/setup/v1/';
const schema=JSON.parse(readFileSync(root+'schema.json','utf8'));
const ajv=new Ajv({strict:false});ajv.addSchema(schema);
for(const type of Object.keys(schema.$defs)) {
  const validate=ajv.compile({$ref:schema.$id+'#/$defs/'+type});
  const example=JSON.parse(readFileSync(root+'examples/'+type+'.json','utf8'));
  assert(validate(example),JSON.stringify(validate.errors));
  assert(!validate({...example,unexpected:true}),type+' must reject unknown keys');
}
const pairing=ajv.compile({$ref:schema.$id+'#/$defs/Pairing'});
const p=JSON.parse(readFileSync(root+'examples/Pairing.json','utf8'));
assert(!pairing({...p,lmk:'short'}));assert(!pairing({...p,format:2}));
console.log('PASS: setup contracts, examples, unknown fields and malformed key rejection');
