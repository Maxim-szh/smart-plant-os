import { readFileSync } from 'node:fs';
import Ajv2020 from 'ajv/dist/2020.js';
import addFormats from 'ajv-formats';

const read = (path) => JSON.parse(readFileSync(path, 'utf8'));
const root = 'spec/v1';
const schema = read(`${root}/schema.json`);
const ajv = new Ajv2020({ allErrors: true, strict: true, strictTypes: false });
addFormats(ajv);
ajv.addSchema(schema);

const examples = [
  ['device.json', 'Device'],
  ['state.json', 'State'],
  ['state-clock-unset.json', 'State'],
  ['config.json', 'Config'],
  ['water-request.json', 'CommandRequest'],
  ['stop-request.json', 'CommandRequest'],
  ['command-result.json', 'CommandResult'],
  ['command-rejected.json', 'CommandResult'],
  ['events.json', 'EventPage'],
  ['registry.json', 'Registry'],
  ['error.json', 'Error'],
];

const validators = new Map();
const docs = new Map();
for (const [file, type] of examples) {
  const validate = validators.get(type) || ajv.getSchema(`${schema.$id}#/$defs/${type}`);
  if (!validate) throw new Error(`No schema for ${type}`);
  validators.set(type, validate);
  const data = read(`${root}/examples/${file}`);
  if (!validate(data)) throw new Error(`${file}: ${ajv.errorsText(validate.errors)}`);
  docs.set(file, data);
}

const device = docs.get('device.json');
const state = docs.get('state.json');
const config = docs.get('config.json');
const water = docs.get('water-request.json');
const result = docs.get('command-result.json');
const events = docs.get('events.json');
const registry = docs.get('registry.json');
const unset = docs.get('state-clock-unset.json');

const assert = (condition, message) => { if (!condition) throw new Error(message); };
for (const doc of [state, config, result, events, unset]) assert(doc.device_id === device.device_id, 'Device IDs diverge');
assert(state.plant.plant_id === config.plant_id && unset.plant.plant_id === config.plant_id, 'Plant IDs diverge');
assert(result.command_id === water.command_id, 'Command IDs diverge');
assert(registry.nodes.some((node) => node.device_id === device.device_id && node.online), 'Registry omits example device');
assert(config.portion_ml <= config.max_daily_ml, 'Portion exceeds daily limit');
assert(config.portion_ml <= config.max_run_seconds * config.pump_calibration_ml_per_second, 'Pump cannot deliver configured portion in time');
assert(config.soil_calibration.dry_raw !== config.soil_calibration.wet_raw, 'Soil calibration endpoints must differ');
assert(unset.clock_state === 'unset' && unset.observed_at === null && unset.last_decision.reason_code === 'clock_unset', 'Unset clock example is inconsistent');

let previous = events.oldest_seq - 1;
for (const event of events.events) {
  assert(event.seq > previous, 'Events are not ordered');
  assert(event.event_id === `${device.device_id}:${event.seq}`, 'Event ID does not match sequence');
  previous = event.seq;
}
assert(events.next_cursor === previous, 'Event cursor does not match last event');

const invalidState = structuredClone(state);
invalidState.soil.soil_index_pct = 101;
assert(!validators.get('State')(invalidState), 'State schema accepted an invalid soil index');
const invalidRequest = structuredClone(water);
invalidRequest.requested_ml = 1000;
assert(!validators.get('CommandRequest')(invalidRequest), 'Command schema accepted an oversized portion');

for (const reason of ['command_expired', 'command_conflict', 'storage_fault', 'watering_no_response']) {
  const rejected = structuredClone(docs.get('command-rejected.json'));
  rejected.reason_code = reason;
  assert(validators.get('CommandResult')(rejected), `Missing firmware reason: ${reason}`);
}
const invalidReserve = structuredClone(state);
invalidReserve.watering.safety_reserved_ml = -1;
assert(!validators.get('State')(invalidReserve), 'Negative safety reserve accepted');

console.log(`✓ контракт v1: ${examples.length} примеров, идентификаторы и инварианты`);
