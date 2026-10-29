// Лёгкая проверка без зависимостей: JSON базы растений и синтаксис скрипта дизайн-прототипа.
import { readFileSync } from 'node:fs';
import { writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { execFileSync } from 'node:child_process';

let failed = 0;
const fail = (m) => { console.error('✗ ' + m); failed++; };
const ok = (m) => console.log('✓ ' + m);

// 1. База растений
const kb = JSON.parse(readFileSync('data/knowledge-base/plants.json', 'utf8'));
const types = ['fruit', 'flower', 'leaf', 'succ'];
const ids = new Set();
for (const p of kb.plants) {
  const where = p.id || '(без id)';
  if (!/^[a-z0-9-]+$/.test(p.id || '')) fail(`${where}: id должен быть латиницей`);
  if (ids.has(p.id)) fail(`${where}: повторяющийся id`);
  ids.add(p.id);
  if (!types.includes(p.type)) fail(`${where}: неизвестный type ${p.type}`);
  if (!(p.moisture_min >= 0 && p.moisture_max <= 100 && p.moisture_min < p.moisture_max)) fail(`${where}: неверный диапазон влажности`);
  if (!(Array.isArray(p.temp_c) && p.temp_c[0] < p.temp_c[1])) fail(`${where}: неверный диапазон температуры`);
  for (const f of ['name_ru', 'latin', 'tip_ru']) if (!p[f]) fail(`${where}: нет поля ${f}`);
}
if (!failed) ok(`база растений: ${kb.plants.length} записей`);

// 2. Синтаксис скрипта прототипа
const html = readFileSync('web/prototype.html', 'utf8');
const m = html.match(/<script>([\s\S]*)<\/script>/);
if (!m) fail('в web/prototype.html не найден <script>');
else {
  const dir = mkdtempSync(join(tmpdir(), 'spo-'));
  const f = join(dir, 'app.js');
  writeFileSync(f, m[1]);
  try { execFileSync(process.execPath, ['--check', f], { stdio: 'pipe' }); ok('синтаксис скрипта прототипа'); }
  catch (e) { fail('ошибка синтаксиса: ' + String(e.stderr).split('\n').slice(0, 4).join(' ')); }
}

// 3. Дизайн-прототип не должен тянуть внешние сервисы, кроме шрифтов
// Примеры адресов в подсказках полей (placeholder) не считаются зависимостями
const htmlNoHints = html.replace(/placeholder="[^"]*"|placeholder="\$\{[^}]*\}"/g, '').replace(/placeholder="\$\{.*?\}"/g, '');
const ext = [...htmlNoHints.matchAll(/https?:\/\/[^\s"'`)<]+/g)].map((x) => new URL(x[0]).host)
  .filter((h) => !['fonts.googleapis.com', 'fonts.gstatic.com'].includes(h));
if (ext.length) fail('внешние адреса в дизайн-прототипе: ' + [...new Set(ext)].join(', '));
else ok('внешних зависимостей в дизайн-прототипе, кроме шрифтов, нет');

process.exit(failed ? 1 : 0);
