import {readFileSync} from 'node:fs';
import Ajv2020 from 'ajv/dist/2020.js';
import addFormats from 'ajv-formats';
const schema=JSON.parse(readFileSync('spec/v1/schema.json','utf8'));
const ajv=new Ajv2020({allErrors:true,strict:true,strictTypes:false});addFormats(ajv);ajv.addSchema(schema);
const path=process.argv[2] || 'spec/v1/examples/firmware-output.jsonl';
let count=0;
for(const line of readFileSync(path,'utf8').trim().split('\n')) {
  const {type,data}=JSON.parse(line);const check=ajv.getSchema(`${schema.$id}#/$defs/${type}`);
  if(!check || !check(data))throw new Error(`${type}: ${ajv.errorsText(check?.errors)}`);
  count++;
}
console.log(`✓ firmware JSON: ${count} replies match public schemas`);
